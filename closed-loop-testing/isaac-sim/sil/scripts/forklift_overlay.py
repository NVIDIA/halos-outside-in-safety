# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL forklift overlay — authors forklift prims from robots.yaml.

Adding a forklift used to mean editing the scene USD: duplicate the prim, retype
the payload, retype the transform. This module generates a small USD layer that
sublayers the scene and adds one prim per robot carrying a `spawn:` block, so the
fleet is described in `robots.yaml` and the scene describes only the warehouse.

    robots:
      - name: forklift_b2
        articulation_prim: /World/forklift_b2
        spawn:
          asset_path: https://.../Isaac/6.0/Isaac/Robots/IsaacSim/ForkliftB/forklift_b.usd
          position: [2.0, -21.63, 0.0]   # metres, world space
          yaw_deg: 180.0                 # optional, default 0 — rotation about Z
          scale: 1.0                     # optional, default 1 — scalar or [x, y, z]

`yaw_deg` rather than a quaternion: every truck in every scene so far sits flat on
the floor and differs only in heading, and `(6.123234e-17, 0, 0, 1)` is not a
number anyone should have to type or recognise. A truck that genuinely needs to be
tilted has no way to say so here — that is a deliberate limit, not an oversight,
and the day it arrives an `orientation_wxyz` escape hatch belongs next to `yaw_deg`.

Unlike `camera_loader.py` and `indicator_loader.py`, this does NOT author into the
live stage. Not for pathfinding reasons — the scene's navmesh settings carry
`excludeRigidBodies`, so a forklift is skipped by the bake whether or not it is
there when it runs, and a point under a parked truck measures as walkable. The
reasons are that the static USD tooling (`scene_scan.py`, the waypoint
generator) reads files rather than the live stage, that a definition in a file
can be reviewed and diffed while a session layer dies with the process, and that
PhysX then sees exactly the shape the baked scene used to give it.

Idempotency needs no marker attribute: the layer is rewritten whole on every run.
A robot whose prim path is ALREADY authored in the scene is an error — the baked
truck and the YAML both claim the path, and silently letting the overlay win would
hide a half-finished migration.

Generated, never hand-edited, and gitignored. It goes in `scenes/generated/` rather
than beside the scene because the container runs as UID 1234 (Dockerfile
`USER 1234:1234`) while the checkout belongs to the host user, so `scenes/` is not
writable to it. `generated/` holds no tracked file, which is what lets setup.sh hand
it to 1234 — write permission on a directory is permission to unlink what is in it,
and `scenes/` holds the scene USDs. The sublayer stays relative, now `../<scene>`.
"""

from __future__ import annotations

import datetime
import math
import os

_OVERLAY_SUFFIX = ".overlay.usda"

_GENERATED_DIR = "generated"

_SPAWN_KEYS = ("asset_path", "position", "yaw_deg", "scale")


def _validate_spawn(robot: dict) -> tuple[str, str, tuple[float, float, float], float,
                                          tuple[float, float, float]]:
    """(prim_path, asset_path, position, yaw_deg, scale) for one robot, validated.

    Unknown keys are rejected, not ignored. A pose is all defaults and no required
    fields beyond the asset, so a misspelled key does not fail — it silently takes
    the default: `yaw:` instead of `yaw_deg:` spawns the truck facing 0 deg, which
    for these scenes is 180 deg wrong, and nothing anywhere says so.
    """
    from action_graphs.forklift_common import is_number

    name = robot.get("name", "?")
    cfg = robot["spawn"]
    if not isinstance(cfg, dict):
        raise ValueError(f"robots.yaml: '{name}'.spawn must be a mapping, got {cfg!r}")

    unknown = set(cfg) - set(_SPAWN_KEYS)
    if unknown:
        raise ValueError(
            f"robots.yaml: '{name}'.spawn has unknown keys: {sorted(unknown)} — "
            f"valid keys are {list(_SPAWN_KEYS)}. The prim path comes from "
            f"articulation_prim, one level up."
        )

    prim_path = robot.get("articulation_prim")
    if not isinstance(prim_path, str) or not prim_path.startswith("/"):
        raise ValueError(
            f"robots.yaml: '{name}'.articulation_prim must be an absolute prim path "
            f"to spawn this robot, got {prim_path!r}"
        )

    asset_path = cfg.get("asset_path")
    if not isinstance(asset_path, str) or not asset_path:
        raise ValueError(f"robots.yaml: '{name}'.spawn.asset_path is required")

    position = cfg.get("position")
    if not isinstance(position, (list, tuple)) or len(position) != 3:
        raise ValueError(
            f"robots.yaml: '{name}'.spawn.position must be [x, y, z], got {position!r}"
        )
    for component in position:
        if not is_number(component):
            raise ValueError(
                f"robots.yaml: '{name}'.spawn.position components must be numbers, "
                f"got {position!r}"
            )

    yaw_deg = cfg.get("yaw_deg", 0.0)
    if not is_number(yaw_deg):
        raise ValueError(f"robots.yaml: '{name}'.spawn.yaw_deg must be a number, got {yaw_deg!r}")

    # Every component is checked, and checked before float() so a string names the
    # robot instead of raising a bare "could not convert string to float". A zero or
    # negative scale is rejected here because the symptom lands elsewhere: the disc
    # loader reports "Parent ... has zero scale", pointing at the indicator rather
    # than at the truck it was collapsed with.
    scale = cfg.get("scale", 1.0)
    if is_number(scale):
        components = (scale,) * 3
    elif isinstance(scale, (list, tuple)) and len(scale) == 3 and all(
            is_number(s) for s in scale):
        components = tuple(scale)
    else:
        raise ValueError(
            f"robots.yaml: '{name}'.spawn.scale must be a number or [x, y, z] of "
            f"numbers, got {scale!r}"
        )
    if any(s <= 0.0 for s in components):
        raise ValueError(
            f"robots.yaml: '{name}'.spawn.scale must be > 0 in every axis, got {scale!r}"
        )
    scale = tuple(float(s) for s in components)

    return (prim_path, asset_path,
            tuple(float(c) for c in position), float(yaw_deg), scale)


def _yaw_to_quat(yaw_deg: float):
    """Quaternion for a rotation about Z, as (w, x, y, z)."""
    half = math.radians(yaw_deg) / 2.0
    w, z = math.cos(half), math.sin(half)
    # cos(pi/2) is 6.1e-17, not 0. Snapping it keeps the generated file readable
    # without changing the pose: the two differ far below single-precision.
    snap = lambda v: 0.0 if abs(v) < 1e-12 else v  # noqa: E731
    return snap(w), 0.0, 0.0, snap(z)


def _author_robot(layer, prim_path: str, asset_path: str, position, yaw_deg: float, scale) -> None:
    from pxr import Gf, Sdf

    prim = Sdf.CreatePrimInLayer(layer, Sdf.Path(prim_path))
    # Ancestors stay `over` (CreatePrimInLayer's default), so the overlay adds the
    # truck to the scene's /World without redefining it.
    prim.specifier = Sdf.SpecifierDef
    prim.payloadList.prependedItems.append(Sdf.Payload(asset_path))

    w, x, y, z = _yaw_to_quat(yaw_deg)

    def attr(name, typename, value, uniform=False):
        spec = Sdf.AttributeSpec(
            prim, name, typename,
            Sdf.VariabilityUniform if uniform else Sdf.VariabilityVarying,
        )
        spec.default = value

    attr("xformOp:translate", Sdf.ValueTypeNames.Double3, Gf.Vec3d(*position))
    attr("xformOp:orient", Sdf.ValueTypeNames.Quatf, Gf.Quatf(w, Gf.Vec3f(x, y, z)))
    attr("xformOp:scale", Sdf.ValueTypeNames.Float3, Gf.Vec3f(*scale))
    # Same op order the scene authored by hand. Without it the ops are ignored and
    # the truck lands at the origin, which reads as a bad position rather than a
    # missing token list.
    attr("xformOpOrder", Sdf.ValueTypeNames.TokenArray,
         ["xformOp:translate", "xformOp:orient", "xformOp:scale"], uniform=True)


def _assert_not_already_in_scene(base_stage_path: str, claims: dict[str, str]) -> None:
    """Fail if the scene already authors a prim the YAML wants to spawn."""
    from pxr import Usd

    # LoadNone: composition without pulling payloads. The check needs the prim to
    # exist, not its contents, and the 40x20 payloads are hundreds of megabytes.
    stage = Usd.Stage.Open(base_stage_path, load=Usd.Stage.LoadNone)
    if stage is None:
        raise RuntimeError(
            f"[forklift-overlay] Could not open the base stage to check for "
            f"conflicting prims: {base_stage_path}"
        )
    for prim_path, name in claims.items():
        if stage.GetPrimAtPath(prim_path):
            raise RuntimeError(
                f"[forklift-overlay] '{name}' declares spawn: but {prim_path} is "
                f"already authored in {base_stage_path}. Two definitions of one "
                f"truck — delete the prim from the scene USD, or drop the spawn: "
                f"block to keep using the baked one."
            )


# Set from the sublayer list we build ourselves; copying the scene's would make
# the overlay sublayer whatever the scene sublayers, not the scene.
_LAYER_METADATA_NOT_COPIED = {"subLayers", "subLayerOffsets"}


def _copy_root_layer_metadata(base_stage_path: str, layer) -> None:
    """Repeat the scene's layer-level metadata on the overlay.

    Prim composition flows up through sublayers; a good deal of LAYER metadata
    does not, and is read off the root layer — which is now the overlay rather
    than the scene. Rather than guess which readers care, everything the scene
    declares at layer level is repeated verbatim, so the overlay presents the
    same face to Isaac that the scene did.

    Two of these are load-bearing here, and both fail quietly:

    `customLayerData` holds `navmeshSettings` — the agent radius, step height
    and slope the navmesh bakes with. Without it IRA burns its whole 100-frame
    budget on a bake that never starts and reports "check whether the stage has
    a valid NavmeshVolume" while the volume sits there intact.

    `defaultPrim` is root-layer-only by definition (`UsdStage.GetDefaultPrim`
    reads the root layer, not the composed stage). Losing it leaves the stage
    with no nominated root, and the navmesh then bakes to nothing: the volumes
    and floor are all still there, but characters spawn at the origin and every
    MoveTo fails, with the navmesh itself reported as present and healthy.
    """
    from pxr import Sdf

    base_layer = Sdf.Layer.FindOrOpen(base_stage_path)
    if base_layer is None:
        raise RuntimeError(
            f"[forklift-overlay] Could not open the base stage as a layer: {base_stage_path}"
        )
    for key in base_layer.pseudoRoot.ListInfoKeys():
        if key in _LAYER_METADATA_NOT_COPIED:
            continue
        layer.pseudoRoot.SetInfo(key, base_layer.pseudoRoot.GetInfo(key))


def _assert_writable(out_dir: str) -> None:
    """Fail here, naming the remedy, rather than in Sdf.Layer.Export.

    On a fresh checkout this directory does not exist yet, and once setup.sh makes
    it the owner is 1234 rather than whoever cloned. Export's own error says only
    "Insufficient permissions", half a minute into a launch that then unwinds
    through IRA — far from the one command that fixes it.
    """
    if os.access(out_dir, os.W_OK):
        return
    raise RuntimeError(
        f"[forklift-overlay] {out_dir} is missing or not writable by uid "
        f"{os.geteuid()}. Run closed-loop-testing/scripts/setup.sh <profile>: it "
        f"creates the directory and chowns it to the isaac-sim container user 1234."
    )


def generate_overlay(robots_config_path: str, base_stage_path: str) -> str | None:
    """Write the overlay for every robot with a `spawn:` block.

    Returns the overlay path, or None when no robot declares one — in which case
    the caller keeps loading the scene directly, which is what makes the migration
    incremental: a scene with baked trucks needs no overlay and gets none.
    """
    from pxr import Sdf

    from action_graphs.forklift_common import load_and_validate_robots_yaml

    robots, _ = load_and_validate_robots_yaml(robots_config_path)
    # Key presence, not truthiness: `spawn:` with an empty body parses as None, and a
    # truthiness test would drop that robot here and load the scene without it. The
    # launch then dies three minutes later in verify_prim_exists, whose message names
    # the scene — sending the reader to the wrong file. Presence routes it into
    # _validate_spawn, which says what is actually wrong.
    spawned = [r for r in robots if "spawn" in r]
    if not spawned:
        return None

    claims: dict[str, str] = {}
    specs = []
    for robot in spawned:
        prim_path, asset_path, position, yaw_deg, scale = _validate_spawn(robot)
        if prim_path in claims:
            raise ValueError(
                f"robots.yaml: {prim_path} is claimed by both {claims[prim_path]} and "
                f"{robot['name']} — two trucks cannot share one prim"
            )
        claims[prim_path] = robot["name"]
        specs.append((prim_path, asset_path, position, yaw_deg, scale))

    _assert_not_already_in_scene(base_stage_path, claims)

    scene_dir = os.path.dirname(os.path.abspath(base_stage_path))
    scene_file = os.path.basename(base_stage_path)
    out_dir = os.path.join(scene_dir, _GENERATED_DIR)
    out_path = os.path.join(
        out_dir, os.path.splitext(scene_file)[0] + _OVERLAY_SUFFIX
    )
    _assert_writable(out_dir)

    layer = Sdf.Layer.CreateAnonymous(".usda")
    # Relative, so the pair stays movable and a reader sees which scene this is
    # an overlay OF without resolving an absolute path from another machine.
    layer.subLayerPaths.append("../" + scene_file)
    _copy_root_layer_metadata(base_stage_path, layer)
    layer.comment = (
        "GENERATED by forklift_overlay.py from "
        f"{os.path.basename(robots_config_path)} at "
        f"{datetime.datetime.now().isoformat(timespec='seconds')} — do not edit, "
        "every launch overwrites this file. Trucks are declared in the spawn: "
        "blocks of that config."
    )

    for prim_path, asset_path, position, yaw_deg, scale in specs:
        _author_robot(layer, prim_path, asset_path, position, yaw_deg, scale)

    layer.Export(out_path)
    print(f"[forklift-overlay] {len(specs)} forklift(s) from "
          f"{os.path.basename(robots_config_path)} -> {out_path}", flush=True)
    for prim_path, _, position, yaw_deg, scale in specs:
        print(f"[forklift-overlay]   {prim_path} at {position} yaw={yaw_deg}deg "
              f"scale={scale}", flush=True)
    return out_path
