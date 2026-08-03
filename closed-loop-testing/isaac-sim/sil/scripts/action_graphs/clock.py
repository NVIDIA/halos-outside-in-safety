# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL ROS 2 clock publisher Action Graph builder.

Replaces the baked Clock_Publisher_Graph (OnPlaybackTick -> ROS2Context
-> ROS2PublishClock) with a Python builder. The clock is global (not
tied to any articulation), so it lives in its own module rather than
forklift_control.py.

Reads the optional top-level `clock:` block of robots.yaml:

    clock:
      enabled: true
      clock_topic: clock   # ROS2PublishClock.topicName (USD: default)

Fired from run_actor_sdg.py after setup_simulation(), before play().
Idempotent.
"""

from __future__ import annotations

import os

DEFAULT_ROBOTS_YAML = "/isaac-sim/sil/configs/robots.yaml"
DEFAULT_GRAPH_PATH = "/World/ClockPublisherGraph"


def _load_clock_cfg(yaml_path: str) -> dict:
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

    return (cfg or {}).get("clock", {}) or {}


def _ensure_extensions_enabled() -> None:
    import omni.kit.app

    ext_mgr = omni.kit.app.get_app().get_extension_manager()
    if not ext_mgr.is_extension_enabled("isaacsim.ros2.bridge"):
        ext_mgr.set_extension_enabled_immediate("isaacsim.ros2.bridge", True)
        print("[clock] Enabled extension: isaacsim.ros2.bridge")


def build_clock_graph(
    config_path: str = DEFAULT_ROBOTS_YAML,
    *,
    graph_path: str = DEFAULT_GRAPH_PATH,
) -> None:
    """Build the /clock publisher graph. No-op if clock.enabled is false."""
    import omni.graph.core as og
    import omni.usd

    cfg = _load_clock_cfg(config_path)
    if not cfg.get("enabled", True):
        print("[clock] clock.enabled is false; skipping clock graph")
        return

    clock_topic = cfg.get("clock_topic")

    _ensure_extensions_enabled()

    stage = omni.usd.get_context().get_stage()
    if stage is None:
        raise RuntimeError("No USD stage open. Load a scene first.")

    prim = stage.GetPrimAtPath(graph_path)
    if prim and prim.IsValid():
        stage.RemovePrim(graph_path)
        print(f"[clock] Cleared existing graph at {graph_path}")

    set_values = []
    if clock_topic:
        set_values.append(("PublishClock.inputs:topicName", clock_topic))

    keys = og.Controller.Keys
    og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: [
                ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
                ("ROS2Context", "isaacsim.ros2.bridge.ROS2Context"),
                ("PublishClock", "isaacsim.ros2.bridge.ROS2PublishClock"),
            ],
            keys.SET_VALUES: set_values,
            keys.CONNECT: [
                ("OnPlaybackTick.outputs:tick", "PublishClock.inputs:execIn"),
                ("ROS2Context.outputs:context", "PublishClock.inputs:context"),
                ("OnPlaybackTick.outputs:time", "PublishClock.inputs:timeStamp"),
            ],
        },
    )

    print(f"[clock] Clock graph built at {graph_path}")


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_clock_graph(cfg)
