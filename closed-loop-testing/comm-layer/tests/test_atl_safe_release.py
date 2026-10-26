# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""ATL safe-release: the PLC side of the ATL SDM's latched safe state.

The ATL SDM latches (UNMUTE then SW_ERROR) on a fault such as a person exit at
count 0 and holds it until the PLC sends CMD_SAFE_RELEASE_REQUEST (0x08); it
answers SAFE_RELEASE_ACK (0x09) and resets its state, or SAFE_RELEASE_DENIED
(0x0A) while a cause is live. The OPC server tests need asyncua and are skipped
without it; run everything in the comm-layer image:

    docker run --rm --network none -v $PWD/tests:/tests -v $PWD/src:/src \
      --entrypoint bash comm-layer:latest -c 'cd /src && python3 -m pytest -q /tests'
"""

import os
import sys
from queue import Queue

import pytest

_SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "src"))
for path in (_SRC, os.path.join(_SRC, "comm_layer")):
    if path not in sys.path:
        sys.path.insert(0, path)

from common.proximity_commands import ProximityCmdPacket  # noqa: E402
from common.proximity_commands import ProximityCommandCode as P  # noqa: E402
from common.safety_commands import ATL_PACKET_IDENTIFIER, CmdPacket, SafetyCommand  # noqa: E402
from common.safety_commands import CommandCode as C  # noqa: E402
from udp_receiver.safety_receiver import SafetyReceiver  # noqa: E402

SDM = ("127.0.0.1", 40001)


class FakeSocket:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((bytes(data), addr))


def make_receiver(**kw):
    rx = SafetyReceiver(port=0, **kw)
    rx._socket = FakeSocket()
    return rx


def feed(rx, seq, code, addr=SDM):
    pkt = CmdPacket.unpack(CmdPacket.now(seq=seq, command=code).pack())
    rx._process_command(SafetyCommand.from_packet(pkt, source_ip=addr[0]), addr, pkt)


def requests(rx):
    out = []
    for data, addr in rx._socket.sent:
        pkt = CmdPacket.unpack(data)
        if pkt is not None and pkt.command == C.SAFE_RELEASE_REQUEST:
            out.append((pkt, addr))
    return out


def test_handshake_opcodes_decode_as_themselves():
    for code in (C.SAFE_RELEASE_REQUEST, C.SAFE_RELEASE_ACK, C.SAFE_RELEASE_DENIED):
        back = CmdPacket.unpack(CmdPacket.now(seq=7, command=code).pack())
        assert back.command is code


def test_latch_edges():
    rx = make_receiver()
    feed(rx, 1, C.UNMUTE)
    assert not rx.atl_safe_state_latched
    feed(rx, 2, C.SW_ERROR)
    assert rx.atl_safe_state_latched and rx.stats.atl_safe_state_latched
    feed(rx, 3, C.HEARTBEAT)
    feed(rx, 4, C.UNMUTE)
    assert rx.atl_safe_state_latched
    feed(rx, 5, C.SAFE_RELEASE_DENIED)
    assert rx.atl_safe_state_latched and rx.stats.atl_safe_release_denied == 1
    feed(rx, 6, C.SAFE_RELEASE_ACK)
    assert not rx.atl_safe_state_latched and rx.stats.atl_safe_release_acked == 1
    feed(rx, 7, C.SW_ERROR)
    feed(rx, 8, C.MUTE)
    assert not rx.atl_safe_state_latched


def test_release_refused_without_a_reported_latch():
    rx = make_receiver()
    assert rx.request_atl_safe_release() is False  # no SDM seen yet
    feed(rx, 1, C.UNMUTE)
    assert rx.request_atl_safe_release() is False
    assert requests(rx) == [] and rx.stats.atl_safe_release_sent == 0


def test_release_goes_to_the_sdm_as_an_atl_packet():
    rx = make_receiver()
    feed(rx, 1, C.UNMUTE)
    feed(rx, 2, C.SW_ERROR)
    assert rx.request_atl_safe_release() is True
    assert rx.request_atl_safe_release() is True
    sent = requests(rx)
    assert [addr for _, addr in sent] == [SDM, SDM]
    assert all(pkt.identifier == ATL_PACKET_IDENTIFIER for pkt, _ in sent)
    assert sent[1][0].seq == (sent[0][0].seq + 1) & 0xFFFF
    assert rx.stats.atl_safe_release_sent == 2


def test_force_sends_without_a_reported_latch():
    rx = make_receiver()
    feed(rx, 1, C.UNMUTE)
    assert rx.request_atl_safe_release(force=True) is True
    assert len(requests(rx)) == 1


def test_no_automatic_release():
    rx = make_receiver(startup_safe_release=True)
    rx._first_contact_mono = -1e9
    for seq in range(1, 6):
        feed(rx, seq, C.SW_ERROR)
    assert requests(rx) == [] and rx.stats.atl_safe_release_sent == 0


def test_atl_and_proximity_latches_are_separate():
    rx = make_receiver()
    rx._process_proximity(ProximityCmdPacket.now(seq=1, command=P.SW_ERROR).pack(), SDM)
    assert rx.safe_state_latched and not rx.atl_safe_state_latched
    assert rx.request_atl_safe_release() is False

    rx = make_receiver()
    feed(rx, 1, C.SW_ERROR)
    assert rx.atl_safe_state_latched and not rx.safe_state_latched
    assert rx.request_safe_release() is False


# ----------------------------------------------------------------- OPC server

class FakeNode:
    def __init__(self, value=False):
        self.value = value

    def read_value(self):
        return self.value

    def write_value(self, value, _vtype=None):
        self.value = value


def make_server(handlers):
    pytest.importorskip("asyncua")
    from opc_ua.safety_opc_server import SafetyOpcUaServer

    srv = SafetyOpcUaServer(input_queue=Queue(), zone2_queue=Queue(),
                            atl_safe_release_handlers=handlers)
    srv._nodes = {"atl_safe_release_request": FakeNode(),
                  "zone2_atl_safe_release_request": FakeNode()}
    return srv


def test_each_zone_node_calls_its_own_handler_once():
    calls = []
    srv = make_server({1: lambda: calls.append(1), 2: lambda: calls.append(2)})
    assert srv._poll_atl_safe_release_requests() is False

    srv._nodes["zone2_atl_safe_release_request"].value = True
    assert srv._poll_atl_safe_release_requests() is True
    assert calls == [2] and srv._nodes["zone2_atl_safe_release_request"].value is False

    srv._nodes["atl_safe_release_request"].value = True
    srv._poll_atl_safe_release_requests()
    srv._poll_atl_safe_release_requests()
    assert calls == [2, 1]


def test_a_failing_handler_does_not_leave_the_node_armed():
    def boom():
        raise RuntimeError("socket gone")

    srv = make_server({1: boom})
    srv._nodes["atl_safe_release_request"].value = True
    assert srv._poll_atl_safe_release_requests() is True
    assert srv._nodes["atl_safe_release_request"].value is False
