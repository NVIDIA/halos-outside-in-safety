# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Shared helpers for the per-robot forklift OmniGraph builders.

The forklift logic that used to be baked into the scene USD
(ROS_Forklift_Control_Graph / Odometry_Graph / Safety_indicator_Graph)
is split across three sibling builder modules, all driven by robots.yaml:

  - forklift_control.py            -> build_control_graph
  - forklift_odometry.py           -> build_odometry_graph
  - forklift_safety_indicator.py   -> build_safety_graph

This module holds the bits they all share (YAML load/validate, extension
enable, prim verify, idempotent graph clear, relationship-target set), plus
`strip_baked_scene_graphs()` and the `build_forklift_graphs()` orchestrator.

YAML schema: see sil/configs/robots.yaml.
"""

from __future__ import annotations

import os

DEFAULT_ROBOTS_YAML = "/isaac-sim/sil/configs/robots.yaml"

# Baked OmniGraph prims this refactor replaces. The production scene
# (indicator_warehouse_20x20_layout_overflow_test.usd) baked
# everything into a single /World/ActionGraph; the older split-graph
# scene used the named graphs below. strip_baked_scene_graphs() removes
# whichever are present so the Python builders are the single source of
# truth and the logic does not run twice (baked + rebuilt).
#
# NOTE: this strips the scene's own `def OmniGraph` prims only. It does
# NOT touch `over "Nova_Carter_ROS"` (sensor/IMU/lidar/diff-drive graphs
# baked INTO the referenced robot asset) - those are asset-coupled and
# out of scope for this refactor.
_BAKED_GRAPH_PATHS = (
    "/World/ActionGraph",
    "/World/ROS_Forklift_Control_Graph",
    "/World/Safety_indicator_Graph",
    "/World/Old_Playback_Graph",
    "/World/Odometry_Graph",
    "/World/Clock_Publisher_Graph",
)


def load_and_validate_robots_yaml(yaml_path: str) -> tuple[list[dict], dict]:
    """Parse robots.yaml. Returns (robots_list, clock_cfg). Raises with a
    descriptive error on schema mismatch.
    """
    try:
        import yaml
    except ImportError as e:
        raise RuntimeError(
            "pyyaml not available; ensure the Isaac Sim python env has it"
        ) from e

    if not os.path.isfile(yaml_path):
        raise FileNotFoundError(f"robots yaml not found: {yaml_path}")

    with open(yaml_path) as f:
        cfg = yaml.safe_load(f)

    if not isinstance(cfg, dict) or "robots" not in cfg:
        raise ValueError(f"{yaml_path}: missing top-level 'robots' list")

    robots = cfg["robots"]
    if not isinstance(robots, list) or not robots:
        raise ValueError(f"{yaml_path}: 'robots' must be a non-empty list")

    seen_names: set[str] = set()
    for i, r in enumerate(robots):
        for field in ("name", "articulation_prim"):
            if field not in r:
                raise ValueError(f"{yaml_path}: robots[{i}] missing field '{field}'")
        name = r["name"]
        if name in seen_names:
            raise ValueError(f"{yaml_path}: duplicate robot name '{name}'")
        seen_names.add(name)
        if not str(r["articulation_prim"]).startswith("/"):
            raise ValueError(
                f"{yaml_path}: robots[{i}].articulation_prim must be an absolute prim path"
            )

    clock_cfg = cfg.get("clock", {}) or {}
    return robots, clock_cfg


def ensure_extensions_enabled() -> None:
    """Enable isaacsim.core.nodes + isaacsim.ros2.bridge if not yet on.
    Idempotent.
    """
    import omni.kit.app

    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    for ext_id in ("isaacsim.core.nodes", "isaacsim.ros2.bridge"):
        if not ext_mgr.is_extension_enabled(ext_id):
            ext_mgr.set_extension_enabled_immediate(ext_id, True)
            print(f"[forklift] Enabled extension: {ext_id}")


def verify_prim_exists(stage, prim_path: str, what: str) -> None:
    prim = stage.GetPrimAtPath(prim_path)
    if not prim or not prim.IsValid():
        raise RuntimeError(
            f"[forklift] {what} prim not found on stage: {prim_path}\n"
            f"Load the matching scene, or edit robots.yaml to match actual prim paths."
        )


def clear_existing_graph(stage, graph_path: str) -> None:
    """Idempotency guard - delete any prior graph at graph_path."""
    prim = stage.GetPrimAtPath(graph_path)
    if prim and prim.IsValid():
        stage.RemovePrim(graph_path)
        print(f"[forklift] Cleared existing graph at {graph_path}")


def set_rel_target(stage, node_path: str, rel_name: str, target_path: str) -> None:
    """Set a relationship target on a built OG node (targetPrim / chassisPrim).
    og.Controller.SET_VALUES does not handle relationships, so do it via USD.
    """
    from pxr import Sdf

    prim = stage.GetPrimAtPath(node_path)
    rel = prim.GetRelationship(rel_name)
    if not rel:
        rel = prim.CreateRelationship(rel_name, custom=True)
    rel.SetTargets([Sdf.Path(target_path)])


def strip_baked_scene_graphs(extra_paths: tuple[str, ...] = ()) -> list[str]:
    """Remove the baked OmniGraph prims this refactor replaces, so the
    Python builders are authoritative and logic does not double-run.

    Headless/SSH-safe alternative to deleting the prim in the OmniGraph
    editor and re-saving the (36k-line) scene USD. Idempotent: prims
    already absent are skipped. Returns the list of paths actually removed.

    Call BEFORE the per-robot builders.
    """
    import omni.usd

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Load a scene first.")

    removed: list[str] = []
    for path in (*_BAKED_GRAPH_PATHS, *extra_paths):
        prim = stage.GetPrimAtPath(path)
        if prim and prim.IsValid():
            stage.RemovePrim(path)
            removed.append(path)
            print(f"[forklift] Stripped baked graph: {path}")
    if not removed:
        print("[forklift] No baked scene graphs found to strip (already clean)")
    return removed


def build_forklift_graphs(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Strip the baked scene graph, then build control + odometry + safety
    graphs for every robot in the config (each gated by its per-block
    `enabled` flag). Call this from run_actor_sdg.py.
    """
    # Deferred imports avoid a circular import at module load (the builder
    # modules import this one).
    from .forklift_control import build_control_graph
    from .forklift_odometry import build_odometry_graph
    from .forklift_safety_indicator import build_safety_graph

    ensure_extensions_enabled()
    # Remove the baked /World/ActionGraph (and legacy named graphs) first
    # so the rebuilt Python graphs are the only ones running.
    strip_baked_scene_graphs()
    build_control_graph(config_path)
    build_odometry_graph(config_path)
    build_safety_graph(config_path)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_forklift_graphs(cfg)
