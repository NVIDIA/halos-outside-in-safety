# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Halos SIL forklift odometry Action Graph builder.

Replaces the baked Odometry_Graph: IsaacComputeOdometry -> ROS2PublishOdometry
+ ROS2PublishRawTransformTree.

Builds /World/<name>_OdometryGraph per robot in robots.yaml.
Shared helpers live in forklift_common.py.
"""

from __future__ import annotations

import os

from .forklift_common import (
    DEFAULT_ROBOTS_YAML,
    clear_existing_graph,
    ensure_extensions_enabled,
    load_and_validate_robots_yaml,
    resolve_odom_frames,
    set_rel_target,
    verify_prim_exists,
)


def _build_one_odometry_graph(robot: dict) -> None:
    import omni.graph.core as og
    import omni.usd

    cfg = robot.get("odometry", {}) or {}
    if not cfg.get("enabled", True):
        return

    name = robot["name"]
    robot_prim = robot["articulation_prim"]
    graph_path = f"/World/{name}_OdometryGraph"

    robot_front = cfg.get("robot_front", [-1.0, 0.0, 0.0])
    odom_topic = cfg.get("odom_topic")
    tf_topic = cfg.get("tf_topic")
    odom_frame, base_frame = resolve_odom_frames(robot)

    stage = omni.usd.get_context().get_stage()
    verify_prim_exists(stage, robot_prim, "chassis")
    clear_existing_graph(stage, graph_path)

    set_values = [
        ("PublishOdometry.inputs:robotFront", tuple(robot_front)),
        # Both nodes describe the same edge of the tree, so both must be told
        # the same pair. Leaving them at the node defaults (odom -> base_link)
        # made every robot publish the same edge onto one /tf.
        ("PublishOdometry.inputs:odomFrameId", odom_frame),
        ("PublishOdometry.inputs:chassisFrameId", base_frame),
        ("PublishRawTF.inputs:parentFrameId", odom_frame),
        ("PublishRawTF.inputs:childFrameId", base_frame),
    ]
    if odom_topic:
        set_values.append(("PublishOdometry.inputs:topicName", odom_topic))
    if tf_topic:
        set_values.append(("PublishRawTF.inputs:topicName", tf_topic))

    keys = og.Controller.Keys
    og.Controller.edit(
        {"graph_path": graph_path, "evaluator_name": "execution"},
        {
            keys.CREATE_NODES: [
                ("OnPlaybackTick", "omni.graph.action.OnPlaybackTick"),
                ("ROS2Context", "isaacsim.ros2.bridge.ROS2Context"),
                ("ComputeOdometry", "isaacsim.core.nodes.IsaacComputeOdometry"),
                ("PublishOdometry", "isaacsim.ros2.bridge.ROS2PublishOdometry"),
                ("PublishRawTF", "isaacsim.ros2.bridge.ROS2PublishRawTransformTree"),
            ],
            keys.SET_VALUES: set_values,
            keys.CONNECT: [
                ("OnPlaybackTick.outputs:tick", "ComputeOdometry.inputs:execIn"),
                ("ComputeOdometry.outputs:execOut", "PublishOdometry.inputs:execIn"),
                ("ROS2Context.outputs:context", "PublishOdometry.inputs:context"),
                ("ComputeOdometry.outputs:angularVelocity", "PublishOdometry.inputs:angularVelocity"),
                ("ComputeOdometry.outputs:linearVelocity", "PublishOdometry.inputs:linearVelocity"),
                ("ComputeOdometry.outputs:orientation", "PublishOdometry.inputs:orientation"),
                ("ComputeOdometry.outputs:position", "PublishOdometry.inputs:position"),
                ("OnPlaybackTick.outputs:time", "PublishOdometry.inputs:timeStamp"),
                ("OnPlaybackTick.outputs:tick", "PublishRawTF.inputs:execIn"),
                ("ROS2Context.outputs:context", "PublishRawTF.inputs:context"),
                ("ComputeOdometry.outputs:orientation", "PublishRawTF.inputs:rotation"),
                ("ComputeOdometry.outputs:position", "PublishRawTF.inputs:translation"),
                ("OnPlaybackTick.outputs:time", "PublishRawTF.inputs:timeStamp"),
            ],
        },
    )

    # chassisPrim is a relationship.
    set_rel_target(
        stage, f"{graph_path}/ComputeOdometry", "inputs:chassisPrim", robot_prim
    )

    print(f"[forklift-odometry] Odometry graph built at {graph_path} "
          f"(robot={robot_prim}, tf={odom_frame}->{base_frame})", flush=True)


def build_odometry_graph(config_path: str = DEFAULT_ROBOTS_YAML) -> None:
    """Build the IsaacComputeOdometry -> ROS2 publishers graph per robot."""
    robots, _ = load_and_validate_robots_yaml(config_path)

    # Two robots on one frame pair is the bug this phase exists to remove, and
    # it is silent: /tf carries both edges and consumers see the pose flicker
    # between two trucks. Derived frames cannot collide (robot names are unique
    # and validated), so this only catches a hand-written pair.
    claims: dict[tuple[str, str], str] = {}
    for robot in robots:
        if not (robot.get("odometry", {}) or {}).get("enabled", True):
            continue
        frames = resolve_odom_frames(robot)
        if frames in claims:
            raise RuntimeError(
                f"TF frames {frames[0]}->{frames[1]} are claimed by both "
                f"{claims[frames]} and {robot['name']} — each robot needs its own pair"
            )
        claims[frames] = robot["name"]

    ensure_extensions_enabled()
    for robot in robots:
        _build_one_odometry_graph(robot)


if __name__ == "__main__":
    cfg = os.environ.get("ISAAC_ROBOTS_YAML", DEFAULT_ROBOTS_YAML)
    build_odometry_graph(cfg)
