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
    load_and_validate_robots_yaml,
    set_rel_target,
    verify_prim_exists,
)

# ScriptNode body for the swivel-drive IK. Build-time constants
# (joint names, wheelbase, wheel radius, max steer) are prepended by the
# builder as module-level names so this body stays brace-free and static.
# Mirrors the original ROS_Forklift_Control_Graph/script_node_SwivelIK
# math exactly, then folds in the divide-by-wheel-radius and the
# ConstructArray plumbing that followed it.
_CONTROL_SCRIPT_BODY = '''
import math

MIN_COS = 1e-3
MIN_SPEED = 1e-4


def compute(db):
    lin = db.inputs.linearVelocity
    ang = db.inputs.angularVelocity

    # Frame-convention sign flips (vehicle frame vs ROS frame), driven by
    # control.reverse_logic.* in robots.yaml. The original SwivelIK negated
    # both inputs (both flags default true).
    linear_x = -lin[0] if FLIP_LINEAR_X else lin[0]
    angular_z = -ang[2] if FLIP_ANGULAR_Z else ang[2]

    heading_speed = max(abs(linear_x), MIN_SPEED)
    steer = math.atan2(angular_z * WHEELBASE, heading_speed)
    steer = max(min(steer, MAX_STEER), -MAX_STEER)

    if linear_x < 0.0:
        # Reverse: optionally flip steer; drive negative.
        steer = 0.0 if abs(angular_z) < MIN_SPEED else -steer
        drive = -heading_speed / max(math.cos(steer), MIN_COS)
    else:
        drive = heading_speed / max(math.cos(steer), MIN_COS)

    # divide node: linear wheel speed -> wheel angular velocity.
    wheel_ang = drive / WHEEL_RADIUS

    # ConstructArray plumbing: joint[0]=drive (velocity), joint[1]=swivel (position).
    db.outputs.jointNames = [DRIVE_JOINT, SWIVEL_JOINT]
    db.outputs.positionCommand = [0.0, steer]
    db.outputs.velocityCommand = [wheel_ang, 0.0]
    return True
'''


def _build_one_control_graph(robot: dict) -> None:
    import omni.graph.core as og
    import omni.usd

    cfg = robot.get("control", {}) or {}
    if not cfg.get("enabled", True):
        return

    name = robot["name"]
    robot_prim = robot["articulation_prim"]
    graph_path = f"/World/{name}_ControlGraph"

    drive_joint = cfg.get("drive_joint", "back_wheel_drive")
    swivel_joint = cfg.get("swivel_joint", "back_wheel_swivel")
    wheelbase = float(cfg.get("wheelbase", 1.49))
    wheel_radius = float(cfg.get("wheel_radius", 0.15))
    max_steer = math.radians(float(cfg.get("max_steer_deg", 45.0)))
    cmd_vel_topic = cfg.get("cmd_vel_topic", "cmd_vel")

    rev = cfg.get("reverse_logic", {}) or {}
    flip_linear_x = bool(rev.get("flip_linear_x", True))
    flip_angular_z = bool(rev.get("flip_angular_z", True))

    stage = omni.usd.get_context().get_stage()
    verify_prim_exists(stage, robot_prim, "articulation")
    clear_existing_graph(stage, graph_path)

    # Prepend build-time constants so the static body can use them.
    script = (
        f"DRIVE_JOINT = {drive_joint!r}\n"
        f"SWIVEL_JOINT = {swivel_joint!r}\n"
        f"WHEELBASE = {wheelbase!r}\n"
        f"WHEEL_RADIUS = {wheel_radius!r}\n"
        f"MAX_STEER = {max_steer!r}\n"
        f"FLIP_LINEAR_X = {flip_linear_x!r}\n"
        f"FLIP_ANGULAR_Z = {flip_angular_z!r}\n"
        + _CONTROL_SCRIPT_BODY
    )

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
                ("SwivelIK.outputs:jointNames", "token[]"),
                ("SwivelIK.outputs:positionCommand", "double[]"),
                ("SwivelIK.outputs:velocityCommand", "double[]"),
            ],
            keys.SET_VALUES: [
                ("SubscribeTwist.inputs:topicName", cmd_vel_topic),
                ("SwivelIK.inputs:script", script),
                ("SwivelIK.inputs:usePath", False),
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

    print(f"[forklift-control] Control graph built at {graph_path} (robot={robot_prim})")


def build_control_graph(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Build the cmd_vel -> articulation control graph for every robot."""
    robots, _ = load_and_validate_robots_yaml(config_path)
    ensure_extensions_enabled()
    for robot in robots:
        _build_one_control_graph(robot)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_control_graph(cfg)
