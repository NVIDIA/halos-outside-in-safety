# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL camera loader — spawns Camera prims from cameras.yaml.

Cameras used to be baked into the scene USD. This loader creates them
at scene-init time from the same `cameras.yaml` that already drives the
RTSP Action Graph (rtsp_cameras.py) and VST registration
(vst_sensor_manager.py), so camera placement becomes a config edit
instead of USD surgery.

Per-camera `spawn:` block (all fields required when present):

    cameras:
      - name: Camera
        camera_prim: /World/Cameras/Camera
        port: 8554
        mount_path: /camera
        spawn:
          position: [0.5953581, -15.3361188, 3.1996765]   # world-space, meters
          rotation_yxz_deg: [51.569855, 0.0, -92.38173]   # xformOp:rotateYXZ (baked-cam convention)
          focal_length: 8.0
          horizontal_aperture: 20.955
          vertical_aperture: 15.2908

`position` and `rotation_yxz_deg` are WORLD-space and authored directly
as the camera's local ops, so the parent chain (e.g. /World/Cameras and
/World) must be an identity transform. The loader fail-fasts if a
parent's world transform is non-identity.

Cameras without a `spawn:` block are left untouched (assumed baked in
the scene USD) — the loader is a no-op for them, which keeps this file
backward-compatible during migration.

Invoked from `run_actor_sdg.py`'s `SET_UP_SIMULATION_DONE_EVENT`
callback BEFORE `build_rtsp_graph()` (the RTSP builder fail-fasts on
missing camera prims, so spawn must run first).

Idempotency: spawned prims carry a `halos:spawnedBy` marker attribute.
On re-run (UI setup re-entry) a marked prim is removed and re-created.
An EXISTING UNMARKED prim at the target path is an error — it means the
baked camera is still in the scene USD while a `spawn:` block claims the
same path; delete the baked def (or drop the `spawn:` block) instead of
letting two sources of truth fight.
"""

from __future__ import annotations

import os

# Same container-canonical default as rtsp_cameras.py.
DEFAULT_CAMERAS_YAML = "/isaac-sim/sil/configs/cameras.yaml"

_MARKER_ATTR = "halos:spawnedBy"
_MARKER_VALUE = "camera_loader"

_REQUIRED_SPAWN_FIELDS = (
    "position",
    "rotation_yxz_deg",
    "focal_length",
    "horizontal_aperture",
    "vertical_aperture",
)

# Match the baked production cameras (scene USD): near clip 1 stage unit,
# effectively-infinite far clip.
_CLIPPING_RANGE = (1.0, 10000000.0)


def _load_cameras_yaml(yaml_path: str) -> list[dict]:
    import yaml

    if not os.path.isfile(yaml_path):
        raise FileNotFoundError(f"cameras yaml not found: {yaml_path}")
    with open(yaml_path) as f:
        cfg = yaml.safe_load(f)
    cameras = (cfg or {}).get("cameras")
    if not isinstance(cameras, list) or not cameras:
        raise ValueError(f"{yaml_path}: 'cameras' must be a non-empty list")
    return cameras


def _is_number(x) -> bool:
    # bool is a subclass of int; reject it so `true`/`false` don't pass as numbers.
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _validate_spawn_block(name: str, spawn: dict) -> None:
    if not isinstance(spawn, dict):
        raise ValueError(f"cameras.yaml: '{name}'.spawn must be a mapping")
    missing = [f for f in _REQUIRED_SPAWN_FIELDS if f not in spawn]
    if missing:
        raise ValueError(f"cameras.yaml: '{name}'.spawn missing fields: {missing}")
    for field in ("position", "rotation_yxz_deg"):
        val = spawn[field]
        if not isinstance(val, (list, tuple)) or len(val) != 3 or not all(_is_number(v) for v in val):
            raise ValueError(f"cameras.yaml: '{name}'.spawn.{field} must be 3 numbers [x, y, z]")
    for field in ("focal_length", "horizontal_aperture", "vertical_aperture"):
        if not _is_number(spawn[field]):
            raise ValueError(f"cameras.yaml: '{name}'.spawn.{field} must be a number, got {spawn[field]!r}")


def _spawn_one(stage, cam: dict) -> str:
    """Create (or re-create) one Camera prim from its spawn block.
    Returns the prim path."""
    from pxr import Gf, Sdf, UsdGeom

    if "camera_prim" not in cam:
        raise ValueError(f"cameras.yaml: camera '{cam.get('name', '?')}' missing 'camera_prim'")
    path = cam["camera_prim"]
    spawn = cam["spawn"]

    existing = stage.GetPrimAtPath(path)
    if existing and existing.IsValid():
        marker = existing.GetAttribute(_MARKER_ATTR)
        if marker and marker.IsValid() and marker.Get() == _MARKER_VALUE:
            stage.RemovePrim(path)
            print(f"[camera-loader] Re-creating previously spawned camera at {path}")
        else:
            raise RuntimeError(
                f"[camera-loader] Prim already exists at {path} and was NOT "
                f"spawned by this loader (baked camera still in the scene "
                f"USD?). Delete the baked def or remove the 'spawn:' block "
                f"for '{cam.get('name', '?')}'."
            )

    camera = UsdGeom.Camera.Define(stage, path)
    prim = camera.GetPrim()

    # spawn.position/rotation are world-space and authored directly as the
    # camera's local ops below, which is only correct when the parent chain
    # is identity (assumption documented in the module docstring). Fail loud
    # if it isn't, rather than silently placing the camera at the wrong world
    # pose and breaking calibration.
    parent_world = UsdGeom.XformCache().GetLocalToWorldTransform(prim.GetParent())
    identity = Gf.Matrix4d(1.0)
    if not all(Gf.IsClose(parent_world.GetRow(i), identity.GetRow(i), 1e-6) for i in range(4)):
        raise RuntimeError(
            f"[camera-loader] Parent of {path} has a non-identity transform; "
            f"spawn.position/rotation are world-space and assume an identity "
            f"parent. Author the camera under an identity Xform, or convert the "
            f"pose to local first."
        )

    camera.GetFocalLengthAttr().Set(float(spawn["focal_length"]))
    camera.GetHorizontalApertureAttr().Set(float(spawn["horizontal_aperture"]))
    camera.GetVerticalApertureAttr().Set(float(spawn["vertical_aperture"]))
    camera.GetClippingRangeAttr().Set(Gf.Vec2f(*_CLIPPING_RANGE))
    camera.GetProjectionAttr().Set(UsdGeom.Tokens.perspective)

    # Author xform ops in the exact baked-camera convention so the
    # existing calibration (extrinsics) stays valid:
    #   xformOpOrder = [translate, rotateYXZ, scale]
    xformable = UsdGeom.Xformable(prim)
    xformable.ClearXformOpOrder()
    xformable.AddTranslateOp().Set(Gf.Vec3d(*[float(v) for v in spawn["position"]]))
    xformable.AddRotateYXZOp().Set(Gf.Vec3f(*[float(v) for v in spawn["rotation_yxz_deg"]]))
    xformable.AddScaleOp().Set(Gf.Vec3f(1.0, 1.0, 1.0))

    prim.CreateAttribute(_MARKER_ATTR, Sdf.ValueTypeNames.String).Set(_MARKER_VALUE)
    # Kit has no per-prim transform lock authored on the USD (viewport camera
    # lock is session-only). `no_delete` is the closest travels-with-scene
    # guard: it stops accidental deletion in the GUI (no-op headless).
    prim.SetMetadata("no_delete", True)

    return path


def spawn_cameras(config_path: str = DEFAULT_CAMERAS_YAML) -> list[str]:
    """Spawn every camera in cameras.yaml that carries a `spawn:` block.

    Returns the list of spawned prim paths (empty when no camera has a
    spawn block — the loader is then a config-driven no-op).
    """
    import omni.usd

    cameras = _load_cameras_yaml(config_path)

    to_spawn = [c for c in cameras if "spawn" in c]
    if not to_spawn:
        print("[camera-loader] No 'spawn:' blocks in cameras.yaml — nothing to do")
        return []

    for cam in to_spawn:
        _validate_spawn_block(cam.get("name", "?"), cam["spawn"])

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Load a scene first.")

    spawned = [_spawn_one(stage, cam) for cam in to_spawn]
    print(f"[camera-loader] Spawned {len(spawned)} cameras: {spawned}")
    return spawned


if __name__ == "__main__":
    # Allow paste-into-Script-Editor invocation for manual testing.
    cfg = os.environ.get("ISAAC_RTSP_CAMERAS_YAML", DEFAULT_CAMERAS_YAML)
    spawn_cameras(cfg)
