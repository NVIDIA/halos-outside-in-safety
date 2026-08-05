# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL indicator loader — creates safety-indicator discs from robots.yaml.

The disc was a hand-authored Mesh inside the scene USD, which made "add a
forklift" a USD edit: robot number three had no disc, and the safety graph
builder is fail-fast, so the only way to launch was to disable the indicator.
This loader authors the geometry from config instead, following the contract
`camera_loader.py` established for cameras.

Per-robot `safety_indicator.mesh:` block, every field optional:

    robots:
      - name: forklift_b
        safety_indicator:
          enabled: true
          indicator_prim: /World/forklift_b/body/body/safety_indicator
          mesh:
            radius: 1.5          # metres, final world-space size
            segments: 32         # triangle-fan segments
            height_offset: 0.0   # metres above the parent origin

The disc is created AT `indicator_prim`, so that one path stays the single
place the geometry and the Action Graph agree on. Its parent must already
exist — for a ForkliftB that is `body/body` inside the asset payload, and a
different forklift model has a different internal path, which is exactly why
the path is spelled out in the config rather than derived.

`radius` is the final size: the 33 points are authored at that radius rather
than as a 0.5-radius disc plus `xformOp:scale = (3, 3, 1)` the way the baked
scene expressed it. A uniform parent scale is divided out so the number in
the config is what you measure in the viewport; a NON-uniform parent scale is
rejected, because it would draw an ellipse and look like a rendering fault
rather than a config one.

Colours are deliberately absent. The ScriptNode in
`action_graphs/forklift_safety_indicator.py` still holds the muted and alarm
colours, so a `color_*` key here would be silently overwritten on the first
state change. They move together, in the change that puts them on node inputs.

Robots without a `mesh:` block are left untouched (disc assumed baked in the
scene USD) — the loader is a no-op for them, which is what keeps the 20x20
scenes working unchanged.

Invoked from `run_actor_sdg.py` BEFORE `build_forklift_graphs()`, whose
`verify_prim_exists` fail-fasts on a missing disc.

Idempotency: created prims carry a `halos:spawnedBy` marker. On re-run a
marked prim is removed and re-created. An EXISTING UNMARKED prim at the target
path is an error — the baked disc is still in the scene USD while the config
claims the same path; delete the baked def or drop the `mesh:` block.
"""

from __future__ import annotations

import math
import os

DEFAULT_ROBOTS_YAML = "/isaac-sim/sil/configs/robots.yaml"

_MARKER_ATTR = "halos:spawnedBy"
_MARKER_VALUE = "indicator_loader"

# Defaults reproduce the disc the 20x20 scene authored by hand: a 32-segment
# fan of final radius 1.5 m sitting on the parent origin.
_DEFAULT_RADIUS = 1.5
_DEFAULT_SEGMENTS = 32
_DEFAULT_HEIGHT_OFFSET = 0.0

# Matches the authored alarm colour in the scene and the ScriptNode's
# unmuted branch, so a disc that is never written looks the same as before.
_INITIAL_COLOR = (1.0, 0.3, 0.0)


def _load_robots_yaml(yaml_path: str) -> list[dict]:
    import yaml

    if not os.path.isfile(yaml_path):
        raise FileNotFoundError(f"robots yaml not found: {yaml_path}")
    with open(yaml_path) as f:
        cfg = yaml.safe_load(f)
    robots = (cfg or {}).get("robots")
    if not isinstance(robots, list) or not robots:
        raise ValueError(f"{yaml_path}: 'robots' must be a non-empty list")
    return robots


def _is_number(x) -> bool:
    # bool is a subclass of int; reject it so `true`/`false` don't pass.
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _validate_mesh_block(name: str, mesh: dict) -> None:
    if not isinstance(mesh, dict):
        raise ValueError(f"robots.yaml: '{name}'.safety_indicator.mesh must be a mapping")
    unknown = set(mesh) - {"radius", "segments", "height_offset"}
    if unknown:
        raise ValueError(
            f"robots.yaml: '{name}'.safety_indicator.mesh has unknown keys: "
            f"{sorted(unknown)} (colours live in the Action Graph for now)"
        )
    for field in ("radius", "height_offset"):
        if field in mesh and not _is_number(mesh[field]):
            raise ValueError(
                f"robots.yaml: '{name}'.safety_indicator.mesh.{field} must be a number, "
                f"got {mesh[field]!r}"
            )
    if "radius" in mesh and float(mesh["radius"]) <= 0.0:
        raise ValueError(f"robots.yaml: '{name}'.safety_indicator.mesh.radius must be > 0")
    if "segments" in mesh:
        segments = mesh["segments"]
        if not isinstance(segments, int) or isinstance(segments, bool) or segments < 3:
            raise ValueError(
                f"robots.yaml: '{name}'.safety_indicator.mesh.segments must be an "
                f"integer >= 3, got {segments!r}"
            )


def _disc_topology(segments: int, radius: float):
    """Triangle fan in the local XY plane: rim points then the centre."""
    points = [
        (radius * math.cos(2.0 * math.pi * i / segments),
         radius * math.sin(2.0 * math.pi * i / segments),
         0.0)
        for i in range(segments)
    ]
    points.append((0.0, 0.0, 0.0))
    centre = segments
    face_counts = [3] * segments
    face_indices = []
    for i in range(segments):
        face_indices += [i, (i + 1) % segments, centre]
    return points, face_counts, face_indices


def _parent_uniform_scale(stage, disc_path: str) -> float:
    """Uniform scale of the disc's parent chain; raises if non-uniform."""
    from pxr import Gf, UsdGeom

    parent_path = disc_path.rsplit("/", 1)[0]
    parent = stage.GetPrimAtPath(parent_path)
    world = UsdGeom.XformCache().GetLocalToWorldTransform(parent)
    scale = Gf.Transform(world).GetScale()
    if not (Gf.IsClose(scale[0], scale[1], 1e-6) and Gf.IsClose(scale[1], scale[2], 1e-6)):
        raise RuntimeError(
            f"[indicator-loader] Parent {parent_path} has a non-uniform scale "
            f"{tuple(scale)}; a disc authored under it would render as an ellipse. "
            f"Author the disc under an intermediate identity Xform, or fix the scene."
        )
    if scale[0] == 0.0:
        raise RuntimeError(f"[indicator-loader] Parent {parent_path} has zero scale")
    return float(scale[0])


def _create_one(stage, robot: dict) -> str:
    from pxr import Gf, Sdf, UsdGeom, Vt

    from action_graphs.forklift_common import verify_prim_exists

    name = robot.get("name", "?")
    cfg = robot["safety_indicator"]
    mesh_cfg = cfg["mesh"] or {}
    path = cfg.get("indicator_prim")
    if not path:
        raise ValueError(
            f"robots.yaml: '{name}'.safety_indicator needs 'indicator_prim' to say "
            f"where the disc goes"
        )

    verify_prim_exists(stage, path.rsplit("/", 1)[0], f"{name} indicator parent")

    existing = stage.GetPrimAtPath(path)
    if existing and existing.IsValid():
        marker = existing.GetAttribute(_MARKER_ATTR)
        if marker and marker.IsValid() and marker.Get() == _MARKER_VALUE:
            stage.RemovePrim(path)
            print(f"[indicator-loader] Re-creating previously spawned disc at {path}")
        else:
            raise RuntimeError(
                f"[indicator-loader] Prim already exists at {path} and was NOT "
                f"created by this loader (baked disc still in the scene USD?). "
                f"Delete the baked def, or remove the 'mesh:' block for '{name}'."
            )

    radius = float(mesh_cfg.get("radius", _DEFAULT_RADIUS))
    segments = int(mesh_cfg.get("segments", _DEFAULT_SEGMENTS))
    height = float(mesh_cfg.get("height_offset", _DEFAULT_HEIGHT_OFFSET))

    # radius is world-space, so cancel the parent chain's uniform scale to keep
    # the config number and the measured disc the same thing.
    parent_scale = _parent_uniform_scale(stage, path)
    local_radius = radius / parent_scale
    if not Gf.IsClose(parent_scale, 1.0, 1e-6):
        print(f"[indicator-loader] {path}: parent scale {parent_scale:.4f}, authoring "
              f"local radius {local_radius:.4f} for a world radius of {radius}",
              flush=True)

    points, face_counts, face_indices = _disc_topology(segments, local_radius)

    disc = UsdGeom.Mesh.Define(stage, path)
    prim = disc.GetPrim()
    disc.GetPointsAttr().Set(Vt.Vec3fArray([Gf.Vec3f(*p) for p in points]))
    disc.GetFaceVertexCountsAttr().Set(Vt.IntArray(face_counts))
    disc.GetFaceVertexIndicesAttr().Set(Vt.IntArray(face_indices))
    disc.GetExtentAttr().Set(Vt.Vec3fArray([
        Gf.Vec3f(-local_radius, -local_radius, 0.0),
        Gf.Vec3f(local_radius, local_radius, 0.0),
    ]))
    disc.GetSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    # Constant interpolation: one colour for the whole disc, which is what the
    # ScriptNode overwrites on each state change.
    disc.GetDisplayColorAttr().Set(Vt.Vec3fArray([Gf.Vec3f(*_INITIAL_COLOR)]))
    disc.GetNormalsAttr().Set(Vt.Vec3fArray([Gf.Vec3f(0.0, 0.0, 1.0)] * len(face_indices)))
    disc.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)

    xformable = UsdGeom.Xformable(prim)
    xformable.ClearXformOpOrder()
    xformable.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, height))

    prim.CreateAttribute(_MARKER_ATTR, Sdf.ValueTypeNames.String).Set(_MARKER_VALUE)
    # Closest travels-with-scene guard against an accidental GUI delete
    # (no-op headless), same as camera_loader.
    prim.SetMetadata("no_delete", True)

    return path


def spawn_indicators(config_path: str = DEFAULT_ROBOTS_YAML) -> list[str]:
    """Create a disc for every robot whose safety_indicator carries `mesh:`.

    Returns the created prim paths (empty when no robot declares a mesh block —
    the loader is then a config-driven no-op).
    """
    import omni.usd

    robots = _load_robots_yaml(config_path)

    to_create = []
    for robot in robots:
        cfg = robot.get("safety_indicator", {}) or {}
        if not cfg.get("enabled", True) or "mesh" not in cfg:
            continue
        _validate_mesh_block(robot.get("name", "?"), cfg["mesh"] or {})
        to_create.append(robot)

    if not to_create:
        print("[indicator-loader] No 'mesh:' blocks in robots.yaml — nothing to do",
              flush=True)
        return []

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Load a scene first.")

    created = [_create_one(stage, robot) for robot in to_create]
    print(f"[indicator-loader] Created {len(created)} indicator disc(s): {created}",
          flush=True)
    return created


if __name__ == "__main__":
    # Allow paste-into-Script-Editor invocation for manual testing.
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    spawn_indicators(cfg)
