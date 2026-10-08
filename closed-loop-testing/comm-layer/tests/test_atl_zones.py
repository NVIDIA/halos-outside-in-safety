# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""Two ATL zones (atl_dual): each robot's is_muted mirror carries its own zone's mute.

Drives the real SafetyRosBridge with ROS2 and asyncua absent, as test_no_tear.py does.
"""
import json
import os
import sys

import pytest

_HERE = os.path.dirname(__file__)
_SRC = os.path.abspath(os.path.join(_HERE, "..", "src"))
_ISAAC_SCRIPTS = os.path.abspath(os.path.join(_HERE, "..", "..", "isaac-sim", "sil", "scripts"))
for path in (_SRC, _ISAAC_SCRIPTS):
    if path not in sys.path:
        sys.path.insert(0, path)

from ros_bridge.safety_ros_bridge import SafetyRosBridge, SafetyCommand  # noqa: E402
from action_graphs.forklift_common import mute_mirror_zones, resolve_safety_zone  # noqa: E402

MUTE, UNMUTE = 2, 7


def _command(seq, code):
    return SafetyCommand(sequence_number=seq, command_code=code,
                         command_name="MUTE" if code == MUTE else "UNMUTE",
                         status_code=1 if code == MUTE else 2, status_name="")


def _snapshot(seq, code, stamp):
    return json.dumps({"sequence": seq, "command": code, "command_name": "",
                       "status": 1, "status_name": "", "timestamp": "",
                       "last_update": stamp})


def _bridge():
    return SafetyRosBridge(robot_zones={"forklift_b": 1, "forklift_b2": 2})


def test_zone2_mute_reaches_only_its_robot():
    bridge = _bridge()
    bridge.publish_command(_command(1, MUTE), zone=2)
    assert bridge.robot_is_muted("forklift_b2")
    assert not bridge.robot_is_muted("forklift_b")
    assert not bridge._current_is_muted  # /safety/is_muted is zone 1's


def test_zone1_mute_leaves_zone2_robot_alone():
    bridge = _bridge()
    bridge.publish_command(_command(1, MUTE), zone=2)
    bridge.publish_command(_command(1, UNMUTE), zone=1)
    bridge.publish_command(_command(2, MUTE), zone=1)
    bridge.publish_command(_command(2, UNMUTE), zone=2)
    assert bridge.robot_is_muted("forklift_b")
    assert not bridge.robot_is_muted("forklift_b2")


def test_without_zone2_server_its_robots_follow_zone1():
    bridge = _bridge()
    bridge.follow_zone1_without_zone2()
    bridge.publish_command(_command(1, MUTE), zone=1)
    assert bridge.robot_zones == {"forklift_b": 1, "forklift_b2": 1}
    assert bridge.robot_is_muted("forklift_b2")


def test_robot_ids_alone_means_zone1():
    assert SafetyRosBridge(robot_ids=["forklift_b"]).robot_zones == {"forklift_b": 1}


def test_snapshots_are_deduplicated_per_zone():
    bridge = _bridge()
    seen = {1: (-1, ""), 2: (-1, "")}
    published = []
    bridge.publish_command = lambda command, zone=1: published.append((zone, command.sequence_number))
    for _ in range(2):
        bridge._publish_snapshot(_snapshot(5, MUTE, "t1"), "Zone2StateJson", 2, seen)
        bridge._publish_snapshot(_snapshot(5, MUTE, "t1"), "StateJson", 1, seen)
    bridge._publish_snapshot("not json", "Zone2StateJson", 2, seen)
    assert published == [(2, 5), (1, 5)]


def _robot(name, **indicator):
    return {"name": name, "safety_indicator": {"enabled": True, **indicator}}


def test_fleet_zone_defaults_to_1():
    assert mute_mirror_zones([_robot("forklift_b"), _robot("forklift_b2", zone=2)]) == {
        "forklift_b": 1, "forklift_b2": 2}


@pytest.mark.parametrize("zone", [0, 3, "2", True])
def test_fleet_zone_rejects_unknown_values(zone):
    with pytest.raises(ValueError):
        resolve_safety_zone(_robot("forklift_b", zone=zone))
