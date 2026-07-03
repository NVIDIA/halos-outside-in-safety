# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL OmniGraph ActionGraph builders.

Each public function in this package builds a single OmniGraph
(ActionGraph) on the stage from a YAML config. Invoked from
`run_actor_sdg.py`'s `SET_UP_SIMULATION_DONE_EVENT` callback after
`setup_simulation()` completes but before timeline.play().

Modules:

  - rtsp_cameras.py  -> build_rtsp_graph(yaml_path)
       N-camera RTSP streaming AG (1 OnPlaybackTick shared, N pairs
       of IsaacCreateRenderProduct + RTSPCameraHelper).
  - forklift_common.py -> build_forklift_graphs(yaml_path),
       strip_baked_scene_graphs(). Shared helpers + orchestrator for the
       three per-robot forklift builders below. Driven by robots.yaml.
  - forklift_control.py -> build_control_graph(yaml_path)
       Per-robot cmd_vel -> swivel-IK -> articulation AG, replacing the
       baked ROS_Forklift_Control_Graph.
  - forklift_odometry.py -> build_odometry_graph(yaml_path)
       Per-robot IsaacComputeOdometry -> ROS2 odom/tf AG, replacing the
       baked Odometry_Graph.
  - forklift_safety_indicator.py -> build_safety_graph(yaml_path)
       Per-robot /safety/is_muted -> indicator color AG, replacing the
       baked Safety_indicator_Graph.
  - clock.py -> build_clock_graph(yaml_path)
       ROS 2 /clock publisher AG, replacing the baked
       Clock_Publisher_Graph. Driven by robots.yaml `clock:` block.
  - (future) <name>.py -> build_<name>_graph(...)

For non-graph stage tweaks (deactivate prims, set xform, etc),
see the sibling `runtime_patches/` package.

Adding a new graph (recipe):

  1. Copy `rtsp_cameras.py` as a template into `<your_graph>.py`.
  2. Rename the public entry to `build_<name>_graph(config_path: str,
     *, graph_path: str = "/World/<Name>Graph") -> og.Graph`.
  3. Implement the single `og.Controller.edit({...}, {CREATE_NODES +
     SET_VALUES + CONNECT})` call. Keep it declarative.
  4. Add an idempotency guard before the edit:
        prim = stage.GetPrimAtPath(graph_path)
        if prim and prim.IsValid(): stage.RemovePrim(graph_path)
  5. Export the entry from `__init__.py` and add the name to `__all__`.
  6. Wire a CLI flag (`--no-<name>`) + an invocation in
     `run_actor_sdg.py`'s setup-done callback.

Design notes:

  - Flat module-scope functions. No factory class hierarchy. NVIDIA's
    own production code (isaacsim.ros2.ui/.../og_utils.py) uses this
    shape for 5+ helper graphs.
  - All builders default to a graph_path under /World/ so the graph
    is greppable in the Stage panel.
  - Each builder is idempotent. UI re-runs of setup do not collide.
  - omni.* imports live INSIDE function bodies because run_actor_sdg.py
    imports this package before SimulationApp is constructed.
  - YAML-driven config. Per-scenario shape (camera count, topic list)
    lives in sil/configs/*.yaml, not in Python.
"""

from .rtsp_cameras import build_rtsp_graph
from .forklift_common import build_forklift_graphs, strip_baked_scene_graphs
from .forklift_control import build_control_graph
from .forklift_odometry import build_odometry_graph
from .forklift_safety_indicator import build_safety_graph
from .clock import build_clock_graph

__all__ = [
    "build_rtsp_graph",
    "build_control_graph",
    "build_odometry_graph",
    "build_safety_graph",
    "build_forklift_graphs",
    "strip_baked_scene_graphs",
    "build_clock_graph",
]
