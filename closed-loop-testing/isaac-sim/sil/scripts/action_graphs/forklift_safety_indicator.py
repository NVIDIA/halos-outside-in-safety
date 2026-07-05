# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL forklift safety indicator Action Graph builder.

Replaces the baked Safety_indicator_Graph: std_msgs/Bool /safety/is_muted
-> indicator disk display color (green when muted, red/orange otherwise).

Builds /World/<name>_SafetyGraph per robot in robots.yaml.
Shared helpers live in forklift_common.py.
"""

from __future__ import annotations

import os

from .forklift_common import (
    DEFAULT_ROBOTS_YAML,
    clear_existing_graph,
    ensure_extensions_enabled,
    load_and_validate_robots_yaml,
    verify_prim_exists,
)

# ScriptNode body for the safety indicator. INDICATOR_PRIM is prepended
# by the builder. Mirrors Safety_indicator_Graph/script_node_indicator.
_SAFETY_SCRIPT_BODY = '''
from pxr import UsdGeom, Gf
import omni.graph.core as og
import omni.usd


def compute(db):
    # Read the subscriber output directly each tick. ROS2Subscriber creates
    # outputs:data dynamically at runtime, and a USD-authored connection to a
    # dynamic attribute is not reliably bound by OmniGraph (the script input
    # then stays at its default False forever). Polling the attribute value
    # sidesteps the binding problem entirely.
    try:
        is_muted = bool(og.Controller.get(og.Controller.attribute(SUB_DATA_ATTR)))
    except Exception:
        is_muted = bool(db.inputs.is_muted)
    stage = omni.usd.get_context().get_stage()
    disk_prim = stage.GetPrimAtPath(INDICATOR_PRIM)
    if disk_prim.IsValid():
        mesh = UsdGeom.Mesh(disk_prim)
        color_attr = mesh.GetDisplayColorAttr()
        if is_muted:
            # GREEN - safety muted (loading allowed)
            color_attr.Set([Gf.Vec3f(0.0, 1.0, 0.0)])
        else:
            # RED/ORANGE - alarm active
            color_attr.Set([Gf.Vec3f(1.0, 0.3, 0.0)])
    return True
'''


def _build_one_safety_graph(robot: dict) -> None:
    import omni.graph.core as og
    import omni.usd

    cfg = robot.get("safety_indicator", {}) or {}
    if not cfg.get("enabled", True):
        return

    name = robot["name"]
    graph_path = f"/World/{name}_SafetyGraph"
    muted_topic = cfg.get("muted_topic", "/safety/is_muted")
    indicator_prim = cfg.get(
        "indicator_prim", f"{robot['articulation_prim']}/body/body/safety_indicator"
    )

    stage = omni.usd.get_context().get_stage()
    verify_prim_exists(stage, indicator_prim, "safety indicator")
    clear_existing_graph(stage, graph_path)

    sub_data_attr = f"{graph_path}/SubscribeIsMuted.outputs:data"
    script = (
        f"INDICATOR_PRIM = {indicator_prim!r}\n"
        f"SUB_DATA_ATTR = {sub_data_attr!r}\n" + _SAFETY_SCRIPT_BODY
    )

    keys = og.Controller.Keys
    og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: [
                ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
                ("ROS2Context", "isaacsim.ros2.bridge.ROS2Context"),
                ("SubscribeIsMuted", "isaacsim.ros2.bridge.ROS2Subscriber"),
                ("Indicator", "omni.graph.scriptnode.ScriptNode"),
            ],
            keys.CREATE_ATTRIBUTES: [
                ("Indicator.inputs:is_muted", "bool"),
            ],
            keys.SET_VALUES: [
                ("SubscribeIsMuted.inputs:messageName", "Bool"),
                ("SubscribeIsMuted.inputs:messagePackage", "std_msgs"),
                ("SubscribeIsMuted.inputs:topicName", muted_topic),
                ("Indicator.inputs:script", script),
                ("Indicator.inputs:usePath", False),
            ],
            keys.CONNECT: [
                ("OnPlaybackTick.outputs:tick", "SubscribeIsMuted.inputs:execIn"),
                ("ROS2Context.outputs:context", "SubscribeIsMuted.inputs:context"),
                ("SubscribeIsMuted.outputs:execOut", "Indicator.inputs:execIn"),
            ],
        },
    )

    # ROS2Subscriber.outputs:data is a dynamically-typed ("any") output that
    # og.Controller.connect() refuses to wire to a static bool input, so the
    # edge is authored as a plain USD connection instead. Crucially, the baked
    # USD also persisted the resolved type of the dynamic attribute
    # (`custom bool outputs:data`) — without pre-authoring that bool attribute
    # OmniGraph never binds the connection at runtime and the input stays at
    # its default (False), freezing the indicator on the unmuted color.
    from pxr import Sdf

    sub_prim = stage.GetPrimAtPath(f"{graph_path}/SubscribeIsMuted")
    if not sub_prim.GetAttribute("outputs:data"):
        sub_prim.CreateAttribute("outputs:data", Sdf.ValueTypeNames.Bool)

    is_muted_attr = stage.GetAttributeAtPath(f"{graph_path}/Indicator.inputs:is_muted")
    is_muted_attr.AddConnection(Sdf.Path(f"{graph_path}/SubscribeIsMuted.outputs:data"))

    print(f"[forklift-safety] Safety graph built at {graph_path} (disk={indicator_prim})")


def build_safety_graph(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Build the /safety/is_muted -> indicator color graph per robot."""
    robots, _ = load_and_validate_robots_yaml(config_path)
    ensure_extensions_enabled()
    for robot in robots:
        _build_one_safety_graph(robot)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_safety_graph(cfg)
