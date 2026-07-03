# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos scene component isolation runtime patch (debug bisection scaffold).

Drop-in extension to `halos_runtime_patches.apply_halos_runtime_patches()`.
Add ONE line at the end of that function:

    deactivate_optional_scene_components(stage)

Then control behavior via env vars (set before launching `run_actor_sdg.py`):

  HALOS_DEACTIVATE_FORKLIFT=1   Deactivate forklift prims (PhysX rigid bodies +
                                /World/ActionGraph script_node_SwivelIK).
  HALOS_DEACTIVATE_ROS=1        Deactivate Nova_Carter_ROS* (ROS2-controlled
                                differential drives). Does NOT unload the
                                isaacsim.ros2.bridge extension; that has to
                                happen at extension-load time.
  HALOS_DEACTIVATE_PATROL=1     Deactivate PatrolHumaniods + their ActionGraphs.

Each toggle is independent. Set multiple at once for additive isolation.

"""

from __future__ import annotations
import os


# Prim paths discovered from
# `indicator_warehouse_20x20_layout_overflow_test.usd`.
_FORKLIFT_PRIMS = [
    "/World/SM_Forklift_A01_Blue_01",
    "/World/SM_Forklift_A01_Blue_01_physics",
    "/World/SM_Forklift_B01_Red_01_physics",
    "/World/SM_HeavyDutyPalletTruck_A01_01",
    "/World/ActionGraph",  # script_node_SwivelIK + MakeJointNames (forklift control)
    # NOTE: /World/forklift_b and /World/forklift_c are the LIVE physics-active
    # forklifts referenced by /World/ActionGraph/articulation_controller. We do NOT
    # deactivate them here because doing so triggers omni.physx.tensors.plugin
    # "Pattern did not match any articulations" errors at every render tick, which
    # makes frame duplication worse, not better, so they are left active.
]
_ROS_CONTROLLED_ROBOTS = [
    "/World/Nova_Carter_ROS",
    "/World/Nova_Carter_ROS_01",
]
_PATROL_HUMANOIDS = [
    "/World/PatrolHumaniods",  # parent prim — covers both AgilityDigit_Patrol and GR1_T2_Patrol
]


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")


def _deactivate_prims(stage, paths, label: str) -> int:
    count = 0
    for path in paths:
        prim = stage.GetPrimAtPath(path)
        if prim and prim.IsValid():
            prim.SetActive(False)
            print(f"[scene-isolation/{label}] Deactivated {path}")
            count += 1
        else:
            print(f"[scene-isolation/{label}] WARN: prim not found: {path}")
    return count


def _remove_prims(stage, paths, label: str) -> int:
    """USD-level removal (stronger than SetActive — also tears down OmniGraph refs)."""
    count = 0
    for path in paths:
        prim = stage.GetPrimAtPath(path)
        if prim and prim.IsValid():
            ok = stage.RemovePrim(path)
            mark = "Removed" if ok else "RemovePrim returned False for"
            print(f"[scene-isolation/{label}] {mark} {path}")
            if ok:
                count += 1
        else:
            print(f"[scene-isolation/{label}] WARN: prim not found: {path}")
    return count


def _deactivate_prims_by_name_substring(stage, substrs, label: str, exclude_paths=None) -> int:
    """Walk stage, deactivate any prim whose path contains one of substrs (case-insensitive).
    Useful when USD composition relocates physics prims to non-canonical paths.
    """
    exclude_paths = set(exclude_paths or [])
    count = 0
    seen = set()
    for prim in stage.Traverse():
        path_str = str(prim.GetPath())
        if path_str in exclude_paths or path_str in seen:
            continue
        low = path_str.lower()
        if any(s.lower() in low for s in substrs):
            if prim.IsActive():
                prim.SetActive(False)
                print(f"[scene-isolation/{label}/wildcard] Deactivated {path_str}")
                count += 1
                seen.add(path_str)
    return count


def deactivate_optional_scene_components(stage) -> None:
    """Read HALOS_DEACTIVATE_* env vars and deactivate matching prims."""
    all_flags = [
        "HALOS_DEACTIVATE_FORKLIFT", "HALOS_DEACTIVATE_ROS", "HALOS_DEACTIVATE_PATROL",
        "HALOS_REMOVE_FORKLIFT_FULL", "HALOS_REMOVE_ALL_DYNAMIC",
        "HALOS_DEACTIVATE_DOME", "HALOS_DEACTIVATE_RECT", "HALOS_DEACTIVATE_RECT01",
        "HALOS_REMOVE_MATERIALS", "HALOS_REMOVE_LIGHTS_ALL", "HALOS_REMOVE_MESHES",
        "HALOS_REMOVE_LOADING_ZONE", "HALOS_REMOVE_BAKED_CAMS", "HALOS_REMOVE_NAVMESH",
        "HALOS_REMOVE_PHYSICS_SCENE", "HALOS_REMOVE_RENDER_SETTINGS",
        "HALOS_REMOVE_VIEWPORT_MEASURE", "HALOS_FORCE_TIMECODES_60", "HALOS_FORCE_TIMECODES_30",
        "HALOS_DEBUG_DUMP",
    ]
    if not any(_env_flag(f) for f in all_flags):
        return  # quiet no-op when no toggle set

    # Light bisection toggles
    if _env_flag("HALOS_DEACTIVATE_DOME"):
        _deactivate_prims(stage, ["/World/DomeLight"], "dome_light")
    if _env_flag("HALOS_DEACTIVATE_RECT"):
        _deactivate_prims(stage, ["/World/RectLight"], "rect_light")
    if _env_flag("HALOS_DEACTIVATE_RECT01"):
        _deactivate_prims(stage, ["/World/RectLight_01"], "rect_light_01")

    # Phase L tests (narrow scene-specific trigger)
    if _env_flag("HALOS_REMOVE_MATERIALS"):
        _remove_prims(stage, ["/World/Looks"], "materials_remove")
    if _env_flag("HALOS_REMOVE_LIGHTS_ALL"):
        _remove_prims(stage, ["/World/DomeLight", "/World/RectLight", "/World/RectLight_01"], "lights_remove")
    if _env_flag("HALOS_REMOVE_MESHES"):
        _remove_prims(stage, [
            "/World/warehouse",  # NEW: top-level warehouse parent (contains all geom)
            "/World/warehouse_h10m_straight_90",
            "/World/sm_warehouse_a11_h10m_straight_01",
        ], "meshes_remove")
    if _env_flag("HALOS_REMOVE_LOADING_ZONE"):
        _remove_prims(stage, [
            "/World/Loading_Zone",
            "/World/Loading_Zone_Objects",
            "/World/Loading_Zone_Objects_01",
            "/World/Objects_In_Trailer",
        ], "loading_zone_remove")
    if _env_flag("HALOS_REMOVE_BAKED_CAMS"):
        _remove_prims(stage, [
            "/World/Cameras/Camera_03",
            "/World/Cameras/Camera_04",
            "/World/Cameras/Camera_05",
            "/World/Cameras/Camera_06",
        ], "baked_cams_remove")
    if _env_flag("HALOS_REMOVE_NAVMESH"):
        _remove_prims(stage, ["/World/Navmesh"], "navmesh_remove")
    if _env_flag("HALOS_REMOVE_PHYSICS_SCENE"):
        # Collect-then-remove (iterator invalidation safety)
        paths = [str(p.GetPath()) for p in stage.Traverse() if "PhysicsScene" in str(p.GetTypeName())]
        for p in paths:
            stage.RemovePrim(p)
            print(f"[scene-isolation/physics_scene_remove] Removed {p}")

    if _env_flag("HALOS_REMOVE_RENDER_SETTINGS"):
        paths = [str(p.GetPath()) for p in stage.Traverse() if "RenderSettings" in str(p.GetTypeName())]
        for p in paths:
            stage.RemovePrim(p)
            print(f"[scene-isolation/render_settings_remove] Removed {p}")

    if _env_flag("HALOS_REMOVE_VIEWPORT_MEASURE"):
        _remove_prims(stage, ["/Viewport_Measure"], "viewport_measure_remove")

    if _env_flag("HALOS_FORCE_TIMECODES_60"):
        old = stage.GetTimeCodesPerSecond()
        stage.SetTimeCodesPerSecond(60)
        new = stage.GetTimeCodesPerSecond()
        print(f"[scene-isolation/timecodes] Set timeCodesPerSecond: {old} -> {new}")

    if _env_flag("HALOS_FORCE_TIMECODES_30"):
        old = stage.GetTimeCodesPerSecond()
        stage.SetTimeCodesPerSecond(30)
        new = stage.GetTimeCodesPerSecond()
        print(f"[scene-isolation/timecodes] Set timeCodesPerSecond: {old} -> {new}")
        print("[scene-debug] === Remaining /World children ===")
        w = stage.GetPrimAtPath("/World")
        if w and w.IsValid():
            for c in w.GetChildren():
                active = "OK" if c.IsActive() else "OFF"
                print(f"[scene-debug]   [{active}] {c.GetPath()} ({c.GetTypeName()})")
        print("[scene-debug] === /Render children ===")
        r = stage.GetPrimAtPath("/Render")
        if r and r.IsValid():
            for c in r.GetChildren():
                active = "OK" if c.IsActive() else "OFF"
                print(f"[scene-debug]   [{active}] {c.GetPath()} ({c.GetTypeName()})")
        print("[scene-debug] === Root (/) top-level prims ===")
        root = stage.GetPseudoRoot()
        for c in root.GetChildren():
            print(f"[scene-debug]   {c.GetPath()} ({c.GetTypeName()})")
        # USD layer stack
        print("[scene-debug] === USD layer stack ===")
        for layer in stage.GetLayerStack():
            print(f"[scene-debug]   {layer.identifier} (sub: {len(layer.subLayerPaths)} subs)")

    # K12: full forklift removal (USD RemovePrim, not just SetActive).
    if _env_flag("HALOS_REMOVE_FORKLIFT_FULL"):
        full_kill_paths = [
            "/World/forklift_b",
            "/World/forklift_c",
            "/World/ActionGraph",
            "/World/SM_Forklift_A01_Blue_01",
            "/World/SM_Forklift_A01_Blue_01_physics",
            "/World/SM_Forklift_B01_Red_01_physics",
            "/World/SM_HeavyDutyPalletTruck_A01_01",
        ]
        total = _remove_prims(stage, full_kill_paths, "forklift_full_remove")
        print(f"[scene-isolation] HALOS_REMOVE_FORKLIFT_FULL: removed {total} prims via RemovePrim")

    # K13: scene USD bare bones — RemovePrim on EVERYTHING dynamic.
    if _env_flag("HALOS_REMOVE_ALL_DYNAMIC"):
        full_kill_paths = [
            "/World/forklift_b",
            "/World/forklift_c",
            "/World/ActionGraph",
            "/World/SM_Forklift_A01_Blue_01",
            "/World/SM_Forklift_A01_Blue_01_physics",
            "/World/SM_Forklift_B01_Red_01_physics",
            "/World/SM_HeavyDutyPalletTruck_A01_01",
            "/World/Nova_Carter_ROS",
            "/World/Nova_Carter_ROS_01",
            "/World/PatrolHumaniods",
        ]
        total = _remove_prims(stage, full_kill_paths, "all_dynamic_remove")
        print(f"[scene-isolation] HALOS_REMOVE_ALL_DYNAMIC: removed {total} prims via RemovePrim")

    # Debug dump — print remaining scene structure AFTER all toggles applied
    if _env_flag("HALOS_DEBUG_DUMP"):
        print("[scene-debug] === Remaining /World children ===")
        w = stage.GetPrimAtPath("/World")
        if w and w.IsValid():
            for c in w.GetChildren():
                active = "OK" if c.IsActive() else "OFF"
                print(f"[scene-debug]   [{active}] {c.GetPath()} ({c.GetTypeName()})")
        print("[scene-debug] === /Render children ===")
        r = stage.GetPrimAtPath("/Render")
        if r and r.IsValid():
            for c in r.GetChildren():
                active = "OK" if c.IsActive() else "OFF"
                print(f"[scene-debug]   [{active}] {c.GetPath()} ({c.GetTypeName()})")
        print("[scene-debug] === Root (/) top-level prims ===")
        root = stage.GetPseudoRoot()
        for c in root.GetChildren():
            print(f"[scene-debug]   {c.GetPath()} ({c.GetTypeName()})")

    total = 0
    if _env_flag("HALOS_DEACTIVATE_FORKLIFT"):
        total += _deactivate_prims(stage, _FORKLIFT_PRIMS, "forklift")
        # Wildcard intentionally omitted: catches forklift_b/forklift_c which we
        # cannot safely deactivate (breaks articulation_controller — see K1b/c).
    if _env_flag("HALOS_DEACTIVATE_ROS"):
        total += _deactivate_prims(stage, _ROS_CONTROLLED_ROBOTS, "ros")
        total += _deactivate_prims_by_name_substring(
            stage,
            ("Nova_Carter", "differential_drive"),
            "ros",
            exclude_paths=set(_ROS_CONTROLLED_ROBOTS),
        )
    if _env_flag("HALOS_DEACTIVATE_PATROL"):
        total += _deactivate_prims(stage, _PATROL_HUMANOIDS, "patrol")
        total += _deactivate_prims_by_name_substring(
            stage,
            ("Patrol", "AgilityDigit", "GR1_T2"),
            "patrol",
            exclude_paths=set(_PATROL_HUMANOIDS),
        )

    print(f"[scene-isolation] Total prims deactivated: {total}")
