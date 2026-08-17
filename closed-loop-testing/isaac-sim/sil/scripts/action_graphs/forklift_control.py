# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL forklift control Action Graph builder.

Replaces the baked ROS_Forklift_Control_Graph: cmd_vel (Twist) -> swivel-IK
-> IsaacArticulationController. The break-vector / divide / construct-array
plumbing from the baked graph is folded into one ScriptNode (same math,
fewer fragile node-to-node connections).

Builds /World/<name>_ControlGraph per robot in robots.yaml.
Shared helpers live in forklift_common.py.
"""

from __future__ import annotations

import math
import os

from .forklift_common import (
    DEFAULT_ROBOTS_YAML,
    clear_existing_graph,
    ensure_extensions_enabled,
    is_section_enabled,
    load_and_validate_robots_yaml,
    resolve_cmd_vel_topic,
    resolve_drive_type,
    set_rel_target,
    verify_prim_exists,
)

# ScriptNode body for the swivel-drive IK.
#
# Every per-robot value arrives on `db.inputs`, never as a name defined at
# script level. OgnScriptNode runs `exec(code_object)` without a globals dict
# and then does `compute_fn.__globals__.update(script_context)`, so script-level
# names are shared by every ScriptNode in the process: the kinematics of the
# graph built last would overwrite every earlier robot's. That stayed invisible
# only while all robots were the same ForkliftB with identical values.
#
# Mirrors the original ROS_Forklift_Control_Graph/script_node_SwivelIK math
# exactly, then folds in the divide-by-wheel-radius and the ConstructArray
# plumbing that followed it.
_CONTROL_SCRIPT_BODY = '''
import math


def compute(db):
    min_cos = 1e-3
    min_speed = 1e-4

    lin = db.inputs.linearVelocity
    ang = db.inputs.angularVelocity

    # Frame-convention sign flips (vehicle frame vs ROS frame), driven by
    # control.reverse_logic.* in robots.yaml. The original SwivelIK negated
    # both inputs (both flags default true).
    linear_x = -lin[0] if db.inputs.flipLinearX else lin[0]
    angular_z = -ang[2] if db.inputs.flipAngularZ else ang[2]

    max_steer = db.inputs.maxSteer
    heading_speed = max(abs(linear_x), min_speed)
    steer = math.atan2(angular_z * db.inputs.wheelbase, heading_speed)
    steer = max(min(steer, max_steer), -max_steer)

    if linear_x < 0.0:
        # Reverse: optionally flip steer; drive negative.
        steer = 0.0 if abs(angular_z) < min_speed else -steer
        drive = -heading_speed / max(math.cos(steer), min_cos)
    else:
        drive = heading_speed / max(math.cos(steer), min_cos)

    # divide node: linear wheel speed -> wheel angular velocity.
    wheel_ang = drive / db.inputs.wheelRadius

    # ConstructArray plumbing: joint[0]=drive (velocity), joint[1]=swivel (position).
    db.outputs.jointNames = [db.inputs.driveJoint, db.inputs.swivelJoint]
    db.outputs.positionCommand = [0.0, steer]
    db.outputs.velocityCommand = [wheel_ang, 0.0]
    return True
'''


def _build_one_swivel_control_graph(robot: dict) -> None:
    """cmd_vel -> bicycle IK -> articulation, for a truck that steers one wheel.

    The topology, not just the numbers, is specific to that layout: the graph
    drives exactly two joints and computes a steer angle for one of them. A
    differential-drive robot needs a different builder, which is why the choice
    is made by `drive_type` rather than by leaving these joints unset.
    """
    import omni.graph.core as og
    import omni.usd

    cfg = robot.get("control", {}) or {}
    if not is_section_enabled(robot, "control"):
        return

    name = robot["name"]
    robot_prim = robot["articulation_prim"]
    graph_path = f"/World/{name}_ControlGraph"

    drive_joint = cfg.get("drive_joint", "back_wheel_drive")
    swivel_joint = cfg.get("swivel_joint", "back_wheel_swivel")
    wheelbase = float(cfg.get("wheelbase", 1.49))
    wheel_radius = float(cfg.get("wheel_radius", 0.15))
    max_steer = math.radians(float(cfg.get("max_steer_deg", 45.0)))
    cmd_vel_topic = resolve_cmd_vel_topic(robot)

    rev = cfg.get("reverse_logic", {}) or {}
    flip_linear_x = bool(rev.get("flip_linear_x", True))
    flip_angular_z = bool(rev.get("flip_angular_z", True))

    stage = omni.usd.get_context().get_stage()
    verify_prim_exists(stage, robot_prim, "articulation")
    clear_existing_graph(stage, graph_path)

    keys = og.Controller.Keys
    og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: [
                ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
                ("ROS2Context", "isaacsim.ros2.bridge.ROS2Context"),
                ("SubscribeTwist", "isaacsim.ros2.bridge.ROS2SubscribeTwist"),
                ("SwivelIK", "omni.graph.scriptnode.ScriptNode"),
                ("ArticulationController", "isaacsim.core.nodes.IsaacArticulationController"),
            ],
            keys.CREATE_ATTRIBUTES: [
                ("SwivelIK.inputs:linearVelocity", "vectord[3]"),
                ("SwivelIK.inputs:angularVelocity", "vectord[3]"),
                # Per-robot kinematics as node inputs, one set per graph.
                ("SwivelIK.inputs:driveJoint", "string"),
                ("SwivelIK.inputs:swivelJoint", "string"),
                ("SwivelIK.inputs:wheelbase", "double"),
                ("SwivelIK.inputs:wheelRadius", "double"),
                ("SwivelIK.inputs:maxSteer", "double"),
                ("SwivelIK.inputs:flipLinearX", "bool"),
                ("SwivelIK.inputs:flipAngularZ", "bool"),
                ("SwivelIK.outputs:jointNames", "token[]"),
                ("SwivelIK.outputs:positionCommand", "double[]"),
                ("SwivelIK.outputs:velocityCommand", "double[]"),
            ],
            keys.SET_VALUES: [
                ("SubscribeTwist.inputs:topicName", cmd_vel_topic),
                ("SwivelIK.inputs:script", _CONTROL_SCRIPT_BODY),
                ("SwivelIK.inputs:usePath", False),
                ("SwivelIK.inputs:driveJoint", drive_joint),
                ("SwivelIK.inputs:swivelJoint", swivel_joint),
                ("SwivelIK.inputs:wheelbase", wheelbase),
                ("SwivelIK.inputs:wheelRadius", wheel_radius),
                ("SwivelIK.inputs:maxSteer", max_steer),
                ("SwivelIK.inputs:flipLinearX", flip_linear_x),
                ("SwivelIK.inputs:flipAngularZ", flip_angular_z),
            ],
            keys.CONNECT: [
                ("OnPlaybackTick.outputs:tick", "SubscribeTwist.inputs:execIn"),
                ("ROS2Context.outputs:context", "SubscribeTwist.inputs:context"),
                ("SubscribeTwist.outputs:execOut", "SwivelIK.inputs:execIn"),
                ("SubscribeTwist.outputs:linearVelocity", "SwivelIK.inputs:linearVelocity"),
                ("SubscribeTwist.outputs:angularVelocity", "SwivelIK.inputs:angularVelocity"),
                ("SwivelIK.outputs:execOut", "ArticulationController.inputs:execIn"),
                ("SwivelIK.outputs:jointNames", "ArticulationController.inputs:jointNames"),
                ("SwivelIK.outputs:positionCommand", "ArticulationController.inputs:positionCommand"),
                ("SwivelIK.outputs:velocityCommand", "ArticulationController.inputs:velocityCommand"),
            ],
        },
    )

    # targetPrim is a relationship, not a settable value.
    set_rel_target(
        stage, f"{graph_path}/ArticulationController", "inputs:targetPrim", robot_prim
    )

    # Kinematics are logged per graph on purpose: a swapped parameter set is
    # otherwise silent, since a robot driving with another robot's wheelbase
    # still reports every node healthy.
    print(f"[forklift-control] Control graph built at {graph_path} (robot={robot_prim}, "
          f"joints={drive_joint}/{swivel_joint}, wheelbase={wheelbase}, "
          f"wheel_radius={wheel_radius}, max_steer={math.degrees(max_steer):.1f}deg)",
          flush=True)


# One entry per kinematic class. A `drive_type` that is not a key here is an
# error rather than a fallback to swivel: the fallback would build a steering
# graph for a robot with no steering joint, and the articulation controller
# would then quietly drive nothing. The truck stands still, every graph reports
# healthy, and the config that caused it looks reasonable.
_CONTROL_BUILDERS = {
    "swivel": _build_one_swivel_control_graph,
}


def build_control_graph(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Build the cmd_vel -> articulation control graph for every robot."""
    robots, _ = load_and_validate_robots_yaml(config_path)

    # Two robots on one articulation means one truck is driven by both graphs
    # while the other never moves — check before building anything. Two robots on
    # one cmd_vel is the same class of silent fault seen from the other end: both
    # trucks obey every message either controller sends.
    claims: dict[str, str] = {}
    topics: dict[str, str] = {}
    for robot in robots:
        if not is_section_enabled(robot, "control"):
            continue
        prim = robot["articulation_prim"]
        if prim in claims:
            raise RuntimeError(
                f"articulation prim {prim} is claimed by both {claims[prim]} and "
                f"{robot['name']} — each robot needs its own articulation"
            )
        claims[prim] = robot["name"]

        topic = resolve_cmd_vel_topic(robot)
        if topic in topics:
            raise RuntimeError(
                f"cmd_vel topic {topic!r} is claimed by both {topics[topic]} and "
                f"{robot['name']} — both trucks would answer every command sent to it"
            )
        topics[topic] = robot["name"]

    # Resolve every builder up front. The loader has already rejected an
    # unknown drive_type; what is caught here is a value that is known but has
    # no builder registered, which is a gap in this file rather than in the
    # config — and finding it after half the fleet is built leaves a stage that
    # is neither the old state nor the new one.
    builders = []
    for robot in robots:
        drive_type = resolve_drive_type(robot)
        builder = _CONTROL_BUILDERS.get(drive_type)
        if builder is None:
            raise RuntimeError(
                f"'{robot['name']}' asks for drive_type {drive_type!r}, which has no "
                f"builder in _CONTROL_BUILDERS (registered: "
                f"{', '.join(sorted(_CONTROL_BUILDERS))})"
            )
        builders.append((robot, builder))

    ensure_extensions_enabled()
    for robot, builder in builders:
        builder(robot)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_control_graph(cfg)
