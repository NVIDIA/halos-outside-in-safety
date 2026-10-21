# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""NVBug 6966743: the ATL safe hold must reach /safety/is_muted.

The SDM enters its safe hold with UNMUTE followed about 22 us later by
SW_ERROR. The bridge reads only the latest StateJson, at 10 Hz, so it sees the
SW_ERROR alone; it used to keep is_muted=True from the MUTE before. The OPC
server tests need asyncua and are skipped without it; run everything in the
comm-layer image:

    docker run --rm --network none -v $PWD/tests:/tests -v $PWD/src:/src \
      --entrypoint bash comm-layer:latest -c 'cd /src && python3 -m pytest -q /tests'
"""

import json
import os
import sys
from queue import Queue

import pytest

_SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "src"))
for path in (_SRC, os.path.join(_SRC, "comm_layer")):
    if path not in sys.path:
        sys.path.insert(0, path)

from common.safety_commands import CommandCode as C  # noqa: E402
from common.safety_commands import SafetyCommand as RxCommand  # noqa: E402
from common.safety_commands import SafetyStatus  # noqa: E402
from ros_bridge.safety_ros_bridge import SafetyRosBridge  # noqa: E402

ATL_NODES = ("command", "command_name", "sequence", "status", "status_name",
             "timestamp", "is_alarm", "is_muted", "last_update", "state_json",
             "zone2_is_muted", "zone2_state_json")


# ----------------------------------------------------------------- OPC server

class FakeNode:
    def __init__(self):
        self.value = None

    def write_value(self, variant):
        self.value = variant.Value


def make_server():
    pytest.importorskip("asyncua")
    from opc_ua.safety_opc_server import SafetyOpcUaServer

    srv = SafetyOpcUaServer(input_queue=Queue())
    srv._nodes = {k: FakeNode() for k in ATL_NODES}
    srv._running = True
    return srv


def rx(seq, code):
    return RxCommand(sequence_number=seq, command=code,
                     status=SafetyStatus.from_command(code),
                     timestamp="06:40:17", microseconds=0)


def snapshot(srv, node="state_json"):
    return json.loads(srv._nodes[node].value)


def test_opc_snapshot_of_the_sw_error_carries_the_safe_hold():
    srv = make_server()
    for seq, code in ((106, C.MUTE), (107, C.UNMUTE), (108, C.SW_ERROR)):
        srv.update_command(rx(seq, code))
    st = snapshot(srv)
    assert st["sequence"] == 108 and st["command"] == int(C.SW_ERROR)
    assert st["is_muted"] is False and st["is_alarm"] is True
    assert srv._nodes["is_muted"].value is False
    assert srv._nodes["is_alarm"].value is True


@pytest.mark.parametrize("error", [C.SW_ERROR, C.HW_ERROR])
def test_opc_an_error_alone_unmutes(error):
    srv = make_server()
    srv.update_command(rx(1, C.MUTE))
    srv.update_command(rx(2, error))
    assert snapshot(srv)["is_muted"] is False
    assert srv._nodes["is_muted"].value is False


@pytest.mark.parametrize("decision,muted", [(C.MUTE, True), (C.UNMUTE, False)])
def test_opc_heartbeat_keeps_the_last_decision(decision, muted):
    srv = make_server()
    srv.update_command(rx(1, decision))
    srv.update_command(rx(2, C.HEARTBEAT))
    st = snapshot(srv)
    assert st["command"] == int(C.HEARTBEAT) and st["is_muted"] is muted
    assert srv._nodes["is_muted"].value is muted


def test_opc_zone2_state_is_its_own():
    srv = make_server()
    srv.update_command(rx(1, C.MUTE), zone=1)
    for seq, code in ((1, C.MUTE), (2, C.UNMUTE), (3, C.SW_ERROR)):
        srv.update_command(rx(seq, code), zone=2)
    assert snapshot(srv, "zone2_state_json")["is_muted"] is False
    assert srv._nodes["zone2_is_muted"].value is False
    assert snapshot(srv)["is_muted"] is True


# --------------------------------------------------------------------- bridge

def payload(seq, command, tracked=None):
    st = {"sequence": seq, "command": int(command), "command_name": command.name,
          "status": int(SafetyStatus.from_command(command)), "status_name": "",
          "timestamp": "", "last_update": f"t{seq}"}
    if tracked is not None:
        st["is_muted"], st["is_alarm"] = tracked
    return json.dumps(st)


def bridge_and_log(**kw):
    bridge = SafetyRosBridge(**kw)
    log = []
    bridge.add_command_callback(
        lambda cmd, muted, alarm: log.append((cmd.sequence_number, muted, alarm)))
    return bridge, log


def test_bridge_polling_past_the_unmute_still_unmutes():
    bridge, log = bridge_and_log()
    seen = {1: (-1, ""), 2: (-1, "")}
    bridge._publish_snapshot(payload(106, C.MUTE, (True, False)), "StateJson", 1, seen)
    # 107 UNMUTE was overwritten before the next poll.
    bridge._publish_snapshot(payload(108, C.SW_ERROR, (False, True)), "StateJson", 1, seen)
    bridge._publish_snapshot(payload(109, C.HEARTBEAT, (False, True)), "StateJson", 1, seen)
    assert log == [(106, True, False), (108, False, True), (109, False, True)]


@pytest.mark.parametrize("error", [C.SW_ERROR, C.HW_ERROR])
def test_bridge_counts_an_error_as_safe_hold_without_tracked_state(error):
    bridge, log = bridge_and_log()
    seen = {1: (-1, ""), 2: (-1, "")}
    bridge._publish_snapshot(payload(1, C.MUTE), "StateJson", 1, seen)
    bridge._publish_snapshot(payload(2, error), "StateJson", 1, seen)
    assert log[-1] == (2, False, True)


def test_bridge_heartbeat_keeps_the_mute_without_tracked_state():
    bridge, log = bridge_and_log()
    seen = {1: (-1, ""), 2: (-1, "")}
    bridge._publish_snapshot(payload(1, C.MUTE), "StateJson", 1, seen)
    bridge._publish_snapshot(payload(2, C.HEARTBEAT), "StateJson", 1, seen)
    assert log[-1] == (2, True, False)


def test_bridge_zone2_robot_follows_its_safe_hold():
    bridge, _ = bridge_and_log(robot_zones={"forklift_b": 1, "forklift_b2": 2})
    seen = {1: (-1, ""), 2: (-1, "")}
    bridge._publish_snapshot(payload(1, C.MUTE, (True, False)), "StateJson", 1, seen)
    bridge._publish_snapshot(payload(1, C.MUTE, (True, False)), "Zone2StateJson", 2, seen)
    bridge._publish_snapshot(payload(3, C.SW_ERROR, (False, True)), "Zone2StateJson", 2, seen)
    assert not bridge.robot_is_muted("forklift_b2")
    assert bridge.robot_is_muted("forklift_b")
