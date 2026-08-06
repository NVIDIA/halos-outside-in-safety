# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL forklift safety indicator Action Graph builder.

Replaces the baked Safety_indicator_Graph: a std_msgs/Bool mute topic
-> indicator disk display color (green when muted, red/orange otherwise).

The topic defaults to the per-robot `/<name>/safety/is_muted` that comm-layer
mirrors; scenes still on the single global `/safety/is_muted` name it explicitly.

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
    resolve_indicator_colors,
    resolve_indicator_prim,
    resolve_muted_topic,
    verify_prim_exists,
)

# ScriptNode body for the safety indicator, one instance per robot.
# Mirrors Safety_indicator_Graph/script_node_indicator.
#
# Every value it needs must come from `db` or from the node's own prim path,
# never from a script-level global. OgnScriptNode runs `exec(code_object)`
# without a globals dict, so the user `compute` ends up with the extension
# module's globals as its __globals__, and the node then does
# `compute_fn.__globals__.update(script_context)`. Script-level names are
# therefore shared by every ScriptNode in the process: with two forklifts the
# graph built last overwrote the first one's prim path, and both nodes
# recoloured the same disk while the other stayed on its authored colour.
_SAFETY_SCRIPT_BODY = '''
from pxr import UsdGeom, Gf
import omni.graph.core as og
import omni.usd


def compute(db):
    indicator_prim = db.inputs.indicator_prim
    if not indicator_prim:
        return False
    # /World/<name>_SafetyGraph/Indicator -> /World/<name>_SafetyGraph
    graph_path = db.node.get_prim_path().rsplit("/", 1)[0]
    sub_data_attr = graph_path + "/SubscribeIsMuted.outputs:data"

    # Read the subscriber output directly each tick. ROS2Subscriber creates
    # outputs:data dynamically at runtime, and a USD-authored connection to a
    # dynamic attribute is not reliably bound by OmniGraph (the script input
    # then stays at its default False forever). Polling the attribute value
    # sidesteps the binding problem entirely.
    try:
        is_muted = bool(og.Controller.get(og.Controller.attribute(sub_data_attr)))
    except Exception:
        is_muted = bool(db.inputs.is_muted)

    stage = omni.usd.get_context().get_stage()
    disk_prim = stage.GetPrimAtPath(indicator_prim)
    if not disk_prim.IsValid():
        return False

    color_attr = UsdGeom.Mesh(disk_prim).GetDisplayColorAttr()
    # Muted means loading is allowed; the alarm colour is the resting state.
    # Both arrive as node inputs so this body stays identical for every robot.
    rgb = db.inputs.color_muted if is_muted else db.inputs.color_alarm
    target = Gf.Vec3f(float(rgb[0]), float(rgb[1]), float(rgb[2]))

    # Write only on a transition. USD is the state store, so no per-instance
    # bookkeeping is needed (a script-level dict would be shared by every
    # ScriptNode in the process — the bug this builder exists to avoid), and
    # the log line below then marks real state changes instead of every tick.
    current = color_attr.Get()
    if current and len(current) and Gf.IsClose(Gf.Vec3f(current[0]), target, 1e-6):
        return True

    color_attr.Set([target])
    # flush: run_sdg.sh redirects stdout to a file, so without this the
    # transition lines sit in the block buffer and never reach the log.
    # The colour is printed rather than named: it is configurable now, so
    # "green" would be a guess about what the config says.
    print("[forklift-safety] " + graph_path + " -> "
          + ("MUTED" if is_muted else "ALARM")
          + " rgb=" + str(tuple(round(float(c), 3) for c in target))
          + " disk=" + indicator_prim, flush=True)
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
    muted_topic = resolve_muted_topic(robot)
    indicator_prim = resolve_indicator_prim(robot)
    color_muted, color_alarm = resolve_indicator_colors(robot)

    stage = omni.usd.get_context().get_stage()
    verify_prim_exists(stage, indicator_prim, "safety indicator")
    clear_existing_graph(stage, graph_path)

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
                ("Indicator.inputs:indicator_prim", "string"),
                ("Indicator.inputs:color_muted", "colorf[3]"),
                ("Indicator.inputs:color_alarm", "colorf[3]"),
            ],
            keys.SET_VALUES: [
                ("SubscribeIsMuted.inputs:messageName", "Bool"),
                ("SubscribeIsMuted.inputs:messagePackage", "std_msgs"),
                ("SubscribeIsMuted.inputs:topicName", muted_topic),
                ("Indicator.inputs:script", _SAFETY_SCRIPT_BODY),
                ("Indicator.inputs:indicator_prim", indicator_prim),
                ("Indicator.inputs:color_muted", color_muted),
                ("Indicator.inputs:color_alarm", color_alarm),
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

    # The topic is in the log because it is now derived rather than written in
    # the config: this line is what you compare against `ros2 topic list` when a
    # disk stays on its alarm colour because nobody publishes what it listens to.
    print(f"[forklift-safety] Safety graph built at {graph_path} "
          f"(disk={indicator_prim}, topic={muted_topic})", flush=True)


def build_safety_graph(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Build the mute-topic -> indicator color graph per robot."""
    robots, _ = load_and_validate_robots_yaml(config_path)

    # Two robots resolving to the same disk is the signature of the bug this
    # builder was rewritten to avoid, and it is invisible at runtime: both
    # graphs tick happily while one truck never changes colour. Check before
    # building anything so the launch fails with the paths named.
    claims: dict[str, str] = {}
    for robot in robots:
        if not (robot.get("safety_indicator", {}) or {}).get("enabled", True):
            continue
        # Parse the colours and the topic here too, so a malformed palette or a
        # relative topic name fails before any graph is built rather than on the
        # robot that happens to be last.
        resolve_indicator_colors(robot)
        resolve_muted_topic(robot)
        prim = resolve_indicator_prim(robot)
        if prim in claims:
            raise RuntimeError(
                f"safety indicator prim {prim} is claimed by both "
                f"{claims[prim]} and {robot['name']} — each robot needs its own disk"
            )
        claims[prim] = robot["name"]

    ensure_extensions_enabled()
    for robot in robots:
        _build_one_safety_graph(robot)
    print(f"[forklift-safety] {len(claims)} safety graph(s) on distinct disks: "
          + ", ".join(f"{n}->{p}" for p, n in claims.items()), flush=True)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_safety_graph(cfg)
