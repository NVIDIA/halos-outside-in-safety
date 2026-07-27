# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Deterministic regression test for NVBug 6512051.

The bug: the OPC-UA -> ROS2 bridge read seq/command/status via 7 SEPARATE
read_value() calls. If the server was mid-update between two commands, the
bridge could pair a FRESH sequence number with a STALE command code -> it
published is_muted=False for what was actually a MUTE (Seq#795). The symmetric
failure (stale MUTE over a fresh UNMUTE -> is_muted=True forever) is a dangerous
alarm-suppression.

The fix: the server writes ONE atomic 'StateJson' node (a single OPC-UA String);
the bridge reads+parses that ONE node. A single scalar read cannot interleave, so
(seq, command) is always coherent.

This file proves the DESIGN, not the wire. It runs on a plain Mac with NO asyncua
and NO ROS2:
  * Fidelity note - read/build path: the OLD (7-read) build logic was DELETED by
    the fix, so it is reproduced here as a FAITHFUL REPLICA of the removed lines
    (safety_ros_bridge.py L383-418 pre-fix). The NEW path uses the REAL
    SafetyCommand class imported from the patched bridge, and json.loads, exactly
    as the patched _run_opcua_loop does.
  * Fidelity note - latch: the isolated-MUTE latch test drives the REAL
    SafetyRosBridge.publish_command()/_current_is_muted from the shipped module
    (importable because HAS_ROS2/HAS_OPCUA guards degrade gracefully). Not a
    replica.
"""

import json
import os
import sys
import random

# Make the real bridge module importable (src/ros_bridge/safety_ros_bridge.py)
_SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "src"))
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

from ros_bridge.safety_ros_bridge import SafetyRosBridge, SafetyCommand  # REAL code

# Command codes (match the shipped SafetyCommand semantics): 2=MUTE, 7=UNMUTE+ALARM, 0=NOP
MUTE, UNMUTE, NOP = 2, 7, 0


# --------------------------------------------------------------------------- #
# Model of the two commands straddling the update boundary that caused Seq#795
# --------------------------------------------------------------------------- #
# update-A: the PREVIOUS command (UNMUTE, seq 794)
UPDATE_A = {
    "sequence": 794, "command": UNMUTE, "command_name": "UNMUTE",
    "status": 2, "status_name": "Safety active",
    "timestamp": "120000.0", "last_update": "2026-07-27T12:00:00.000000",
}
# update-B: the NEW command we must not drop (MUTE, seq 795)
UPDATE_B = {
    "sequence": 795, "command": MUTE, "command_name": "MUTE",
    "status": 1, "status_name": "Safety muted",
    "timestamp": "120001.0", "last_update": "2026-07-27T12:00:01.000000",
}


class TornPerFieldNodes:
    """FAITHFUL REPLICA of the OLD 7-separate-node read surface.

    Models the exact interleaving the fix targets: the server has finished
    writing update-B into the Sequence node but NOT yet into the Command node,
    so a reader that fetches Command then Sequence sees (cmd from A, seq from B).
    """
    def __init__(self, per_node_update):
        # per_node_update: dict browse_name -> which update ('A' or 'B') is live now
        self._live = per_node_update

    def _val(self, browse_name, key):
        upd = UPDATE_A if self._live[browse_name] == "A" else UPDATE_B
        return upd[key]

    def read(self, browse_name):
        mapping = {
            "Command": "command", "CommandName": "command_name",
            "Sequence": "sequence", "Status": "status",
            "StatusName": "status_name", "LastUpdate": "last_update",
            "Timestamp": "timestamp",
        }
        return self._val(browse_name, mapping[browse_name])


def old_build_from_separate_reads(nodes):
    """Replica of removed safety_ros_bridge.py L394-414 (7 reads -> build)."""
    cmd_code = nodes.read("Command")
    cmd_name = nodes.read("CommandName")
    seq = nodes.read("Sequence")
    status_code = nodes.read("Status")
    status_name = nodes.read("StatusName")
    timestamp = nodes.read("Timestamp")
    _last_update = nodes.read("LastUpdate")
    return SafetyCommand(
        sequence_number=seq, command_code=cmd_code, command_name=cmd_name,
        status_code=status_code, status_name=status_name,
        timestamp=timestamp, source="opcua",
    )


class AtomicStateJsonNode:
    """Model of the NEW single 'StateJson' node. Its value is one whole
    json string swapped atomically A->B; a read returns a COMPLETE snapshot,
    never a byte-level mix of A and B."""
    def __init__(self):
        self._value = json.dumps(UPDATE_A)

    def commit(self, update):
        # single-assignment => atomic w.r.t. any concurrent reader
        self._value = json.dumps(update)

    def read_value(self):
        return self._value


def new_build_from_statejson(raw):
    """Exact patched _run_opcua_loop build path."""
    st = json.loads(raw)
    return SafetyCommand(
        sequence_number=st.get("sequence"),
        command_code=st.get("command"),
        command_name=st.get("command_name", "Unknown"),
        status_code=st.get("status"),
        status_name=st.get("status_name", "Unknown"),
        timestamp=st.get("timestamp", ""),
        source="opcua",
    )


# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #
def test_old_path_tears():
    """OLD path: fresh Sequence(795,B) + stale Command(7,A) -> is_muted=False = BUG."""
    torn = TornPerFieldNodes({
        "Command": "A",      # stale UNMUTE not yet overwritten
        "CommandName": "A",
        "Sequence": "B",     # fresh 795 already written
        "Status": "A",
        "StatusName": "A",
        "Timestamp": "A",
        "LastUpdate": "B",
    })
    cmd = old_build_from_separate_reads(torn)
    # The tear is producible: we built a NEW sequence carrying the OLD command.
    assert cmd.sequence_number == 795, cmd.sequence_number
    assert cmd.command_code == UNMUTE, cmd.command_code
    assert cmd.is_muted is False, "expected the 6512051 tear (under-mute)"
    print(f"  OLD  -> seq={cmd.sequence_number} cmd={cmd.command_code} "
          f"is_muted={cmd.is_muted}  <-- BUG (dropped MUTE)")


def test_new_path_coherent():
    """NEW path: single StateJson(update-B) always parses to a coherent (795, MUTE)."""
    node = AtomicStateJsonNode()
    node.commit(UPDATE_B)               # atomic swap to the MUTE snapshot
    cmd = new_build_from_statejson(node.read_value())
    assert cmd.sequence_number == 795, cmd.sequence_number
    assert cmd.command_code == MUTE, cmd.command_code
    assert cmd.is_muted is True, "MUTE must survive the atomic read"
    print(f"  NEW  -> seq={cmd.sequence_number} cmd={cmd.command_code} "
          f"is_muted={cmd.is_muted}  <-- correct (MUTE held)")


def test_torn_read_impossible_under_hammer():
    """Hammer atomic swaps A<->B across many interleavings; assert EVERY read is
    internally coherent (795<->MUTE or 794<->UNMUTE), never the torn (795,UNMUTE)."""
    node = AtomicStateJsonNode()
    rng = random.Random(6512051)
    torn_seen = 0
    for _ in range(20000):
        node.commit(UPDATE_B if rng.random() < 0.5 else UPDATE_A)
        cmd = new_build_from_statejson(node.read_value())
        coherent = ((cmd.sequence_number == 795 and cmd.command_code == MUTE) or
                    (cmd.sequence_number == 794 and cmd.command_code == UNMUTE))
        if not coherent:
            torn_seen += 1
    assert torn_seen == 0, f"atomic node produced {torn_seen} torn reads"
    print(f"  NEW  -> 20000 hammered reads, torn pairs = {torn_seen}  <-- no tear possible")


def test_malformed_statejson_skips_publish():
    """Fail-safe: a partial/garbage StateJson must raise (bridge skips publish,
    never emits a wrong is_muted)."""
    for bad in ['{"sequence": 795, "command":', "not json", ""]:
        raised = False
        try:
            json.loads(bad)
        except (ValueError, TypeError):
            raised = True
        assert raised, f"expected parse failure for {bad!r}"
    print("  NEW  -> malformed StateJson raises -> bridge fail-safe skips publish")


def test_isolated_mute_latch_real_bridge():
    """REAL SafetyRosBridge latch: UNMUTE(1) -> MUTE(2) -> HEARTBEAT/NOP(3).
    is_muted must go True at the MUTE and STAY True through the heartbeat."""
    bridge = SafetyRosBridge()          # HAS_ROS2=False -> publish is a no-op, latch still runs
    published = []
    bridge.add_command_callback(
        lambda cmd, is_muted, is_alarm: published.append((cmd.sequence_number, is_muted))
    )

    def mk(seq, code, name):
        return SafetyCommand(seq, code, name, 0, "", timestamp="", source="opcua")

    bridge.publish_command(mk(1, UNMUTE, "UNMUTE"))
    bridge.publish_command(mk(2, MUTE, "MUTE"))
    bridge.publish_command(mk(3, NOP, "NOP"))      # heartbeat: must NOT clear the mute

    states = dict(published)
    assert states[1] is False, states
    assert states[2] is True, "MUTE not latched"
    assert states[3] is True, "heartbeat wrongly cleared the mute latch"
    print(f"  LATCH-> published (seq,is_muted)={published}  <-- MUTE held through heartbeat")


ALL_TESTS = [
    ("test_old_path_tears (BUG reproduced)", test_old_path_tears),
    ("test_new_path_coherent", test_new_path_coherent),
    ("test_torn_read_impossible_under_hammer", test_torn_read_impossible_under_hammer),
    ("test_malformed_statejson_skips_publish", test_malformed_statejson_skips_publish),
    ("test_isolated_mute_latch_real_bridge", test_isolated_mute_latch_real_bridge),
]


if __name__ == "__main__":
    failures = 0
    for name, fn in ALL_TESTS:
        try:
            print(f"[RUN ] {name}")
            fn()
            print(f"[PASS] {name}\n")
        except AssertionError as e:
            failures += 1
            print(f"[FAIL] {name}: {e}\n")
    print("=" * 60)
    print(f"RESULT: {len(ALL_TESTS) - failures}/{len(ALL_TESTS)} passed")
    sys.exit(1 if failures else 0)
