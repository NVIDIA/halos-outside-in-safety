# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""RobotController topic names with ROS stubbed out: python3 -m pytest test/test_robot_controller_topics.py"""

import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class _Logger:
    def __getattr__(self, _name):
        return lambda *args, **kwargs: None


class _Node:
    def __init__(self, name):
        self.publishers = {}
        self.subscriptions = {}

    def get_logger(self):
        return _Logger()

    def create_publisher(self, _msg_type, topic, _qos):
        self.publishers[topic] = types.SimpleNamespace(topic_name=topic)
        return self.publishers[topic]

    def create_subscription(self, _msg_type, topic, _callback, _qos):
        self.subscriptions[topic] = types.SimpleNamespace(topic_name=topic)
        return self.subscriptions[topic]

    def create_timer(self, _period, _callback):
        return object()


def _stub_ros():
    def module(name, **attrs):
        mod = types.ModuleType(name)
        mod.__dict__.update(attrs)
        sys.modules.setdefault(name, mod)

    msg = type("Msg", (), {})
    module("rclpy")
    module("rclpy.node", Node=_Node)
    module("rclpy.qos", QoSProfile=lambda **kwargs: None,
           ReliabilityPolicy=types.SimpleNamespace(RELIABLE=1),
           DurabilityPolicy=types.SimpleNamespace(TRANSIENT_LOCAL=1))
    module("geometry_msgs")
    module("geometry_msgs.msg", Twist=msg, PoseStamped=msg)
    module("nav_msgs")
    module("nav_msgs.msg", Odometry=msg, Path=msg)
    module("std_msgs")
    module("std_msgs.msg", String=msg, ColorRGBA=msg)
    module("visualization_msgs")
    module("visualization_msgs.msg", Marker=msg, MarkerArray=msg)
    module("rosgraph_msgs")
    module("rosgraph_msgs.msg", Clock=msg)


_stub_ros()
from robot_controller import RobotController  # noqa: E402

PROXIMITY = {"enabled": True, "reduce_speed": 0.5, "stop_hold_s": 2.0,
             "reduce_hold_s": 1.0, "stale_s": 10.0}


def test_defaults_follow_the_fleet_file_fallback():
    node = RobotController(proximity=PROXIMITY)
    assert "/forklift_b/cmd_vel" in node.publishers
    assert "/forklift_b/odom" in node.subscriptions
    assert "/safety/proximity/pair" in node.subscriptions
    assert "/forklift_b/proximity/state" in node.publishers


def test_partial_topic_map_keeps_the_rest_on_the_fallback():
    node = RobotController(robot_id="forklift_a", topics={"cmd_vel": "/a/cmd"})
    assert "/a/cmd" in node.publishers
    assert "/forklift_a/proximity/state" in node.publishers
    assert "/safety/proximity/pair" not in node.subscriptions


def test_fleet_file_names_win():
    topics = {"proximity_pair": "/forklift_a/proximity/pair",
              "proximity_state": "/forklift_a/pxc"}
    node = RobotController(robot_id="forklift_a", topics=topics, proximity=PROXIMITY)
    assert "/forklift_a/proximity/pair" in node.subscriptions
    assert "/forklift_a/pxc" in node.publishers
