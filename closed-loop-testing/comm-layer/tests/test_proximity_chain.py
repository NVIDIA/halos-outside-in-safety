# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""The 0xA5 proximity path, from bytes to the bridge, with no PSF, ROS or network.

What this guards is one argument: 0x02 means MUTE for ATL and STOP for
proximity, so a 0xA5 packet must never reach the ATL path, and nothing on the
proximity path may ever read as "muted". The OPC server tests need asyncua and
are skipped without it; run everything in the comm-layer image:

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

from common.proximity_commands import (  # noqa: E402
    FAULT_SEVERITY,
    NO_DECISION,
    PROXIMITY_PACKET_IDENTIFIER,
    ObjectClass,
    ProximityCmdPacket,
    ProximityCommandCode as P,
    ProximityHold,
    packet_severity,
)
from common.safety_commands import (  # noqa: E402
    ATL_PACKET_IDENTIFIER,
    CmdPacket,
    CommandCode,
    ObjectRecord,
)
from udp_receiver.safety_receiver import SafetyReceiver  # noqa: E402

SDM = ("127.0.0.1", 40000)
MACHINE = ObjectRecord(object_id=40, x=7.8, y=-13.4, z=0.0, metadata=ObjectClass.OBJECT)
PERSON = ObjectRecord(object_id=13, x=9.2, y=-12.1, z=0.0, metadata=ObjectClass.PERSON)


def pxc(seq, command, objects=(MACHINE, PERSON)) -> ProximityCmdPacket:
    return ProximityCmdPacket.now(seq=seq, command=command, objects=list(objects))


class FakeSocket:
    def __init__(self, inbox=()):
        self.sent = []
        self._inbox = list(inbox)

    def sendto(self, data, addr):
        self.sent.append((bytes(data), addr))

    def recvfrom(self, _size):
        if self._inbox:
            return self._inbox.pop(0), SDM
        raise OSError("closed")


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


def make_receiver(**kw):
    seen = []
    rx = SafetyReceiver(port=0, proximity_callback=seen.append, **kw)
    rx._socket = FakeSocket()
    return rx, seen


def sent_packets(rx):
    return [ProximityCmdPacket.unpack(data) for data, _ in rx._socket.sent]


# ----------------------------------------------------------------- wire format

def test_pack_unpack_round_trip():
    pkt = pxc(4242, P.REDUCE)
    data = pkt.pack()
    assert len(data) == 64 and data[0] == PROXIMITY_PACKET_IDENTIFIER
    back = ProximityCmdPacket.unpack(data)
    assert back.seq == 4242 and back.command is P.REDUCE
    assert back.machine.object_id == 40 and back.person.object_id == 13
    assert abs(back.separation_m - ((7.8 - 9.2) ** 2 + (-13.4 + 12.1) ** 2) ** 0.5) < 1e-4


def test_bad_crc_is_rejected():
    data = bytearray(pxc(1, P.STOP).pack())
    data[30] ^= 0xFF
    assert ProximityCmdPacket.unpack(bytes(data)) is None


def test_unknown_opcode_is_kept_raw_and_counts_as_a_fault():
    back = ProximityCmdPacket.unpack(ProximityCmdPacket(seq=1, command=0x04).pack())
    assert back.command == 0x04 and not isinstance(back.command, P)
    assert packet_severity(back) == FAULT_SEVERITY


def test_each_decoder_rejects_the_other_identifier():
    atl = CmdPacket.now(seq=1, command=CommandCode.MUTE).pack()
    prox = pxc(1, P.STOP).pack()
    assert atl[0] == ATL_PACKET_IDENTIFIER
    assert ProximityCmdPacket.unpack(atl) is None
    assert CmdPacket.unpack(prox) is None


def test_empty_slot_gives_no_separation():
    pkt = pxc(1, P.STOP, objects=(MACHINE, ObjectRecord()))
    assert ProximityCmdPacket.unpack(pkt.pack()).separation_m is None


# -------------------------------------------------------------------- receiver

def test_a_proximity_stop_never_reaches_the_atl_path():
    rx, seen = make_receiver()
    rx._socket = FakeSocket(inbox=[pxc(7, P.STOP).pack()])
    rx._running = True
    rx._receive_loop()  # one packet, then the fake socket "closes"

    assert rx._queue.empty()
    assert rx.stats.packets_received == 0 and rx.stats.proximity_received == 1
    assert [p.command for p in seen] == [P.STOP]
    (ack,) = sent_packets(rx)
    assert ack is not None and ack.identifier == PROXIMITY_PACKET_IDENTIFIER
    assert ack.seq == 7 and ack.command is P.STOP


def test_heartbeat_and_unknown_opcode_are_not_acked():
    rx, seen = make_receiver()
    rx._process_proximity(pxc(1, P.HEARTBEAT).pack(), SDM)
    rx._process_proximity(ProximityCmdPacket(seq=2, command=0x04).pack(), SDM)
    assert rx._socket.sent == []
    # Both still reach the consumer: one as liveness, one as a fault.
    assert [p.seq for p in seen] == [1, 2]
    assert rx.stats.proximity_unknown_opcode == 1


def test_bad_crc_is_counted_and_dropped():
    rx, seen = make_receiver()
    data = bytearray(pxc(1, P.STOP).pack())
    data[21] ^= 0xFF
    rx._process_proximity(bytes(data), SDM)
    assert seen == [] and rx._socket.sent == []
    assert rx.stats.proximity_invalid_crc == 1


def test_latch_edges():
    rx, _ = make_receiver()
    rx._process_proximity(pxc(1, P.SW_ERROR).pack(), SDM)
    assert rx.stats.safe_state_latched
    rx._process_proximity(pxc(2, P.SAFE_RELEASE_DENIED).pack(), SDM)
    assert rx.stats.safe_state_latched and rx.stats.safe_release_denied == 1
    rx._process_proximity(pxc(3, P.NORMAL).pack(), SDM)
    assert not rx.stats.safe_state_latched


def test_no_startup_release_by_default():
    rx, _ = make_receiver()
    rx._first_contact_mono = -1e9  # long past the settle time
    rx._process_proximity(pxc(1, P.SW_ERROR).pack(), SDM)
    assert all(p.command is not P.SAFE_RELEASE_REQUEST for p in sent_packets(rx))
    assert rx.stats.safe_release_sent == 0


def test_startup_release_only_for_a_reported_latch():
    rx, _ = make_receiver(startup_safe_release=True)
    rx._process_proximity(pxc(1, P.STOP).pack(), SDM)
    rx._first_contact_mono = -1e9
    rx._process_proximity(pxc(2, P.STOP).pack(), SDM)
    assert rx.stats.safe_release_sent == 0  # a healthy link: nothing to release

    rx._process_proximity(pxc(3, P.SW_ERROR).pack(), SDM)
    rx._process_proximity(pxc(4, P.SW_ERROR).pack(), SDM)
    requests = [p for p in sent_packets(rx) if p.command is P.SAFE_RELEASE_REQUEST]
    assert len(requests) == 1 and rx.stats.safe_release_sent == 1


def test_manual_release_refused_without_a_latch():
    rx, _ = make_receiver()
    rx._process_proximity(pxc(1, P.NORMAL).pack(), SDM)
    assert rx.request_safe_release() is False
    rx._process_proximity(pxc(2, P.SW_ERROR).pack(), SDM)
    assert rx.request_safe_release() is True


# ---------------------------------------------------------------- decision hold

def test_hold_keeps_a_stop_visible_past_a_following_normal():
    clock = FakeClock()
    hold = ProximityHold(min_visible_s=0.3, clock=clock)
    assert hold.offer(pxc(1, P.STOP))
    clock.t = 0.05
    assert not hold.offer(pxc(2, P.NORMAL))
    assert hold.shown.command is P.STOP
    clock.t = 0.2
    assert not hold.tick()
    clock.t = 0.31
    assert hold.tick() and hold.shown.command is P.NORMAL
    assert not hold.tick()


def test_hold_escalates_at_once_and_ignores_liveness():
    clock = FakeClock()
    hold = ProximityHold(min_visible_s=0.3, clock=clock)
    assert not hold.offer(pxc(1, P.HEARTBEAT)) and hold.shown is None
    hold.offer(pxc(2, P.NORMAL))
    clock.t = 0.01
    assert hold.offer(pxc(3, P.REDUCE))
    assert hold.offer(pxc(4, P.SW_ERROR))
    assert packet_severity(pxc(5, P.SAFE_RELEASE_ACK)) == NO_DECISION
    assert not hold.offer(pxc(5, P.SAFE_RELEASE_ACK))
    assert hold.shown.seq == 4


# ----------------------------------------------------------------- OPC server

class FakeNode:
    def __init__(self):
        self.value = None

    def write_value(self, variant):
        self.value = variant.Value


def make_server():
    pytest.importorskip("asyncua")
    from opc_ua.safety_opc_server import SafetyOpcUaServer

    clock = FakeClock()
    srv = SafetyOpcUaServer(proximity_queue=Queue())
    srv._pxc_hold = ProximityHold(clock=clock)
    srv._nodes = {k: FakeNode() for k in (
        "proximity_command", "proximity_command_name", "proximity_mode",
        "proximity_separation", "proximity_state_json",
        "is_muted", "is_alarm", "state_json")}
    srv._running = True
    return srv, clock


def state(srv):
    return json.loads(srv._nodes["proximity_state_json"].value)


def test_opc_stop_is_never_muted_and_survives_a_heartbeat():
    srv, clock = make_server()
    srv.update_proximity(pxc(10, P.STOP))
    st = state(srv)
    assert st["mode"] == "stop" and st["sequence"] == 10 and st["heard_sequence"] == 10
    clock.t = 0.05
    srv.update_proximity(pxc(11, P.HEARTBEAT))
    st = state(srv)
    assert st["mode"] == "stop" and st["sequence"] == 10 and st["heard_sequence"] == 11
    for key in ("is_muted", "is_alarm", "state_json"):
        assert srv._nodes[key].value is None


def test_opc_publishes_an_unknown_opcode_as_a_fault():
    srv, _ = make_server()
    srv.update_proximity(ProximityCmdPacket.unpack(
        ProximityCmdPacket(seq=3, command=0x04).pack()))
    st = state(srv)
    assert st["mode"] is None and st["safety_critical"] is True
    assert st["command_name"] == "UNKNOWN (0x04)"


def test_opc_writes_nothing_before_the_first_decision():
    srv, _ = make_server()
    srv.update_proximity(pxc(1, P.HEARTBEAT))
    assert srv._nodes["proximity_state_json"].value is None


# --------------------------------------------------------------------- bridge

class FakeOpcNode:
    def __init__(self, value):
        self.value = value

    def read_value(self):
        return self.value


def test_bridge_proximity_stop_does_not_touch_the_mute():
    from ros_bridge.safety_ros_bridge import SafetyRosBridge

    bridge = SafetyRosBridge()
    seen = []
    bridge._proximity_callbacks.append(seen.append)
    muted_before = bridge._current_is_muted
    payload = {"sequence": 5, "command": int(P.STOP), "command_name": "STOP",
               "mode": "stop", "separation_m": 1.5, "safety_critical": True,
               "objects": [], "timestamp": "t", "heard_sequence": 6,
               "heard_at": "h", "last_update": "u1"}
    node = FakeOpcNode(json.dumps(payload))
    bridge._poll_proximity({"ProximityStateJson": node})
    bridge._poll_proximity({"ProximityStateJson": node})  # unchanged: not republished
    assert len(seen) == 1
    assert seen[0].mode == "stop" and seen[0].heard_sequence == 6
    assert bridge._current_is_muted == muted_before

    node.value = json.dumps(dict(payload, heard_sequence=7, last_update="u2"))
    bridge._poll_proximity({"ProximityStateJson": node})
    assert len(seen) == 2 and seen[1].sequence_number == 5


def test_bridge_skips_malformed_payloads():
    from ros_bridge.safety_ros_bridge import SafetyRosBridge

    bridge = SafetyRosBridge()
    seen = []
    bridge._proximity_callbacks.append(seen.append)
    for raw in ("not json", json.dumps({"mode": "stop"})):
        bridge._poll_proximity({"ProximityStateJson": FakeOpcNode(raw)})
    assert seen == []
