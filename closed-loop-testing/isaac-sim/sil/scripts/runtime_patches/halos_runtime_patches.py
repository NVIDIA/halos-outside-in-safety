# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos post-setup_simulation runtime stage modifications.

Fired from `run_actor_sdg.py`'s `SET_UP_SIMULATION_DONE_EVENT` callback,
after IRA has finished `setup_simulation()` (environment + chars + robots
+ sensors loaded; timeline still stopped).

What it does:
  1. Deactivates 3 legacy 5.1 baked Character prims that exist in the
     warehouse USD for backwards compatibility. They have no Behavior
     Tree wired up in the 6.0 stack and would otherwise sit static in
     the scene cluttering it.
  2. Moves the IRA-spawned characters
     (`/World/Characters/<group>/<group>_0`) from their NavMesh-randomized
     positions to the canonical baked positions taken from the Halos
     scenario specification:
       inspect_workers_0 -> (2.26, -10.30, 0.00)
       gather_workers_0  -> (0.29, -18.00, 0.00)
       pickup_workers_0  -> (-2.86, -15.28, 0.00)
     This is a USD-transform-only move (frame-0 placement). It is NOT the
     cause of the recurring `MoveTo failed: status=2` seen on 6.0 -- the
     suspected cause is the behavior.core auto-avoidance layer
     (enableAutoAvoidance, default-on in 6.0) treating the moving forklift_b as
     a dynamic threat (see run_actor_sdg.py enableAutoAvoidance note for the
     trackThreat log evidence). Offline navmesh queries confirmed every waypoint
     is on-navmesh and every A<->B segment has a valid path, so navmesh geometry
     is healthy.

Why this exists (not just a YAML config):
  IRA 6.0 has no deterministic-spawn config path today. `spawn_areas` is
  NavMesh-area-name only, and the Character prim's xformOp:translate is
  unconditionally overwritten by `character_loader.py:set_prim_pos` at
  setup time. The only way to pin spawn to (2.26, -10.30, 0) etc. is to
  move chars AFTER setup.

Once IRA exposes runtime spawn position via USD attributes, retire this
module + replace with a few lines in cameras.yaml / character.yaml.
"""

from __future__ import annotations

# Canonical baked spawn positions for the 3 Halos SIL character groups
# in indicator_warehouse_20x20_layout_overflow_test.usd. Positions
# are also the "endpoint B" of each group's behavior tree loop, so the
# first BT MoveTo node takes the character toward endpoint A.
_HALOS_CHAR_SPAWN_TARGETS = [
    ("/World/Characters/inspect_workers/inspect_workers_0", (2.26, -10.30, 0.00)),
    ("/World/Characters/gather_workers/gather_workers_0",   (0.29, -18.00, 0.00)),
    ("/World/Characters/pickup_workers/pickup_workers_0",   (-2.86, -15.28, 0.00)),
]

# Legacy 5.1 baked Character prims that we deactivate so the scene
# doesn't show 6 chars (3 baked + 3 IRA-spawned) at the same time.
# Biped_Setup is the 5.1 animation rig shared by the 3 baked chars; its
# Biped_Setup.usd payload 404s on the 6.0 S3 (only the 5.1 path resolves),
# so it's deactivated as a side-effect cleanup too.
_LEGACY_BAKED_CHARS = [
    "/World/Characters/Character",
    "/World/Characters/Character_01",
    "/World/Characters/Character_02",
    "/World/Characters/Biped_Setup",
]

# Camera render tick rate (`omni:sensor:tickRate`) is intentionally NOT
# authored here. Since the tc=60 fix (commit 595534a) the production value
# is 0.0 (no cap), which is already the sensor default — the dup-DTS
# collision it used to work around was a USD timeCodesPerSecond
# quantization bug, not a capture-rate problem. The 30 Hz divider now
# lives in the RTSP graph's IsaacSimulationGate (see action_graphs/
# rtsp_cameras.py). To deliberately throttle capture, set a positive
# `omni:sensor:tickRate` on the camera in cameras.yaml / camera_loader.

# Articulation roots whose PhysX TGS solver iterations must be re-balanced for
# Isaac Sim 6.0 (PhysX SDK 5.3). The forklift asset (Isaac 5.1 ForkliftB) was
# authored for the PhysX 5.2 TGS solver, which SILENTLY converted any velocity
# iterations in excess of 4 into position iterations. PhysX 5.3 no longer does
# this (CHANGELOG: TGS now honors the requested velocity-iteration count like
# PGS), so the same asset solves differently in 6.0 and logs:
#   [omni.physx.plugin] Detected an articulation at /World/forklift_b with more
#   than 4 velocity iterations being added to a TGS scene...
# To keep the forklift dynamics matching the 5.1 baseline, apply NVIDIA's
# recommended migration: clamp velocity iterations to 4 and move the excess
# onto the position-iteration count (old_pos + (old_vel - 4)). Values are read
# live from the loaded payload (the asset is a remote S3 payload, so we can't
# author them statically in the scene USD).
# (articulation root path, required). Fallback list for callers that don't
# pass a robots config (e.g. Script Editor use). The normal launch path
# derives the list from the same robots yaml the graph builders consume
# (see _articulations_from_robots_config), so adding a robot stays a
# yaml-only change. required=False marks prims that exist only in the
# opt-in two-forklift scene (configs/robots-2fl.yaml); their absence from
# the default single-forklift scene is expected, not an error.
_HALOS_FORKLIFT_ARTICULATIONS = [
    ("/World/forklift_b", True),
    ("/World/forklift_b2", False),  # only in the 2FL scene variant
]


def _articulations_from_robots_config(robots_config_path):
    """Derive the articulation-root list from the robots config yaml (the
    same file build_forklift_graphs consumes). Entries are optional
    (required=False): the graph builders that run right after this patch
    are the authoritative missing-prim fail-fast, so the patch just skips
    absent prims with an info line. Falls back to the built-in list on any
    read/parse problem.
    """
    import yaml

    try:
        with open(robots_config_path) as f:
            cfg = yaml.safe_load(f)
        roots = [
            (robot["articulation_prim"], False)
            for robot in (cfg or {}).get("robots", [])
            if robot.get("articulation_prim")
        ]
        if roots:
            return roots
        print(
            f"[halos-runtime-patches] WARN: no robots in {robots_config_path}, "
            f"using built-in articulation list"
        )
    except Exception as exc:
        print(
            f"[halos-runtime-patches] WARN: could not read {robots_config_path} "
            f"({exc}), using built-in articulation list"
        )
    return _HALOS_FORKLIFT_ARTICULATIONS
_PHYSX_MAX_TGS_VELOCITY_ITERS = 4
_PHYSX_POS_ITER_ATTR = "physxArticulation:solverPositionIterationCount"
_PHYSX_VEL_ITER_ATTR = "physxArticulation:solverVelocityIterationCount"


def _rebalance_tgs_iterations(stage, articulations) -> None:
    """Clamp TGS velocity iterations to 4 on the forklift articulation(s),
    moving the excess onto the position-iteration count to preserve the PhysX
    5.2 (Isaac Sim 5.1) effective solver behavior. Idempotent.
    """
    from pxr import Usd, Sdf

    for root_path, required in articulations:
        root = stage.GetPrimAtPath(root_path)
        if not root or not root.IsValid():
            if required:
                print(f"[halos-runtime-patches] WARN: forklift root not found: {root_path}")
            else:
                print(
                    f"[halos-runtime-patches] {root_path}: not in scene "
                    f"(single-forklift default), skipping"
                )
            continue

        # The PhysxArticulationAPI attrs live on whichever prim in the subtree
        # carries the articulation root (the payload's default prim or a child).
        fixed_any = False
        for prim in Usd.PrimRange(root):
            vel_attr = prim.GetAttribute(_PHYSX_VEL_ITER_ATTR)
            if not vel_attr or not vel_attr.IsValid() or not vel_attr.HasAuthoredValue():
                continue
            vel = vel_attr.Get()
            if vel is None or vel <= _PHYSX_MAX_TGS_VELOCITY_ITERS:
                continue
            pos_attr = prim.GetAttribute(_PHYSX_POS_ITER_ATTR)
            pos = pos_attr.Get() if (pos_attr and pos_attr.IsValid()) else None
            if pos is None:
                pos = 0
            new_pos = int(pos) + (int(vel) - _PHYSX_MAX_TGS_VELOCITY_ITERS)
            if pos_attr and pos_attr.IsValid():
                pos_attr.Set(new_pos)
            else:
                pos_attr = prim.CreateAttribute(_PHYSX_POS_ITER_ATTR, Sdf.ValueTypeNames.Int)
                pos_attr.Set(new_pos)
            vel_attr.Set(_PHYSX_MAX_TGS_VELOCITY_ITERS)
            fixed_any = True
            print(
                f"[halos-runtime-patches] Rebalanced TGS iters on {prim.GetPath()}: "
                f"pos {pos}->{new_pos}, vel {vel}->{_PHYSX_MAX_TGS_VELOCITY_ITERS}"
            )
        if not fixed_any:
            print(
                f"[halos-runtime-patches] {root_path}: no TGS velocity iters >4 "
                f"to rebalance (already 5.1-compatible)"
            )


def apply_halos_runtime_patches(robots_config_path=None) -> None:
    """Apply post-setup Halos stage modifications. Safe to call multiple
    times (idempotent).

    robots_config_path: the robots yaml the launch passed via --robots-config
    (or its default). When given, the TGS solver rebalance covers exactly the
    robots listed there; when None (e.g. Script Editor), the built-in
    _HALOS_FORKLIFT_ARTICULATIONS list is used.
    """
    # Late imports: this module is imported from `run_actor_sdg.py` at
    # the top of file, before SimulationApp is instantiated. omni.usd
    # and pxr can only be imported AFTER SimulationApp init, so they
    # live inside the function body.
    import omni.usd
    from pxr import UsdGeom, Gf, Sdf

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        print("[halos-runtime-patches] WARN: no USD stage open, skipping")
        return

    # 1) Deactivate legacy baked Character prims so they don't render.
    for legacy_path in _LEGACY_BAKED_CHARS:
        prim = stage.GetPrimAtPath(legacy_path)
        if prim and prim.IsValid():
            prim.SetActive(False)
            print(f"[halos-runtime-patches] Deactivated {legacy_path}")

    # 2) Move each IRA-spawned char to its baked position.
    for prim_path, target in _HALOS_CHAR_SPAWN_TARGETS:
        prim = stage.GetPrimAtPath(prim_path)
        if not prim or not prim.IsValid():
            print(f"[halos-runtime-patches] WARN: IRA prim not found: {prim_path}")
            continue
        xformable = UsdGeom.Xformable(prim)
        # Clear any existing xformOp:translate authored by IRA's
        # set_prim_pos and re-add a typed TranslateOp pointing at the
        # baked position. Avoids the "Empty typeName" pitfall hit
        # earlier with DH_Characters_Extended USDs.
        xformable.ClearXformOpOrder()
        xformable.AddTranslateOp().Set(Gf.Vec3d(*target))
        print(
            f"[halos-runtime-patches] Moved {prim_path} -> "
            f"({target[0]:.2f}, {target[1]:.2f}, {target[2]:.2f})"
        )

    # 3) Re-balance forklift TGS solver iterations to match the 5.1 baseline
    #    (PhysX 5.3 no longer auto-converts velocity iters >4 to position iters).
    articulations = (
        _articulations_from_robots_config(robots_config_path)
        if robots_config_path
        else _HALOS_FORKLIFT_ARTICULATIONS
    )
    _rebalance_tgs_iterations(stage, articulations)

    # Scene-component isolation toggles (env-var driven, no-op when no HALOS_DEACTIVATE_* set)
    from .scene_component_isolation import deactivate_optional_scene_components
    deactivate_optional_scene_components(stage)


if __name__ == "__main__":
    # Allow paste-into-Script-Editor invocation for manual testing.
    apply_halos_runtime_patches()
