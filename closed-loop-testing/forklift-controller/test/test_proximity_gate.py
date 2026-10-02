# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""ProximityGate on a fake clock. No ROS: python3 -m pytest test/test_proximity_gate.py"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from proximity_gate import LINK_STALE, ProximityGate  # noqa: E402

HEARTBEAT = {"mode": None, "safety_critical": False, "command_name": "HEARTBEAT"}


def make_gate():
    clock = [0.0]
    return ProximityGate(0.5, 2.0, 1.0, stale_s=10.0, clock=lambda: clock[0]), clock


def test_normal_until_psf_speaks():
    gate, _ = make_gate()
    assert not gate.active
    assert gate.level() == "normal"
    assert gate.limit(1.5, 0.3) == (1.5, 0.3)


def test_reduce_caps_speed_and_keeps_curvature():
    gate, _ = make_gate()
    gate.on_decision({"mode": "reduce_speed", "separation_m": 3.0})
    linear, angular = gate.limit(1.5, 0.3)
    assert abs(linear - 0.5) < 1e-9 and abs(angular - 0.1) < 1e-9
    assert gate.limit(-1.5, 0.3)[0] == -0.5
    assert gate.limit(0.3, 0.2) == (0.3, 0.2)


def test_stop_outlives_a_following_normal_by_the_hold():
    gate, clock = make_gate()
    gate.on_decision({"mode": "stop", "separation_m": 1.2})
    assert gate.limit(1.5, 0.3) == (0.0, 0.0)
    clock[0] = 0.5
    gate.on_decision({"mode": "normal"})
    clock[0] = 1.9
    assert gate.level() == "stop"
    clock[0] = 2.1
    assert gate.level() == "normal"


def test_reduce_hold():
    gate, clock = make_gate()
    gate.on_decision({"mode": "reduce_speed"})
    clock[0] = 0.1
    gate.on_decision({"mode": "normal"})
    assert gate.level() == "reduce_speed"
    clock[0] = 1.1
    assert gate.level() == "normal"


def test_silence_with_heartbeats_keeps_the_last_level():
    gate, clock = make_gate()
    gate.on_decision({"mode": "reduce_speed"})
    for t in range(5, 100, 5):
        clock[0] = float(t)
        gate.on_decision(HEARTBEAT)
        assert gate.level() == "reduce_speed"
    assert not gate.stale and gate.fault is None


def test_heartbeat_is_not_a_level_and_a_fault_stops():
    gate, clock = make_gate()
    gate.on_decision(HEARTBEAT)
    assert not gate.active
    gate.on_decision({"mode": None, "safety_critical": True, "command_name": "SOFTWARE ERROR"})
    assert gate.level() == "stop" and gate.fault == "SOFTWARE ERROR"
    clock[0] = 5.0
    assert gate.level() == "stop"
    gate.on_decision({"mode": "normal"})
    clock[0] = 7.1
    assert gate.level() == "normal" and gate.fault is None


def test_a_dead_link_stops_and_latches_until_a_new_decision():
    gate, clock = make_gate()
    gate.on_decision({"sequence": 1, "mode": "normal"})
    clock[0] = 9.9
    assert gate.level() == "normal"
    clock[0] = 10.1
    assert gate.level() == "stop" and gate.stale and gate.fault == LINK_STALE
    # A heartbeat proves the link, not that the held NORMAL is still current.
    gate.on_decision(HEARTBEAT)
    assert gate.level() == "stop"
    # Nor does the same decision republished.
    gate.on_decision({"sequence": 1, "mode": "normal"})
    assert gate.level() == "stop"
    gate.on_decision({"sequence": 2, "mode": "normal"})
    assert gate.level() == "normal" and not gate.stale and gate.fault is None


def test_no_watchdog_before_psf_speaks():
    gate, clock = make_gate()
    clock[0] = 1000.0
    assert gate.level() == "normal" and not gate.stale


def test_note_heard_keeps_the_link_alive():
    gate, clock = make_gate()
    gate.on_decision({"sequence": 1, "mode": "normal"})
    for t in range(5, 60, 5):
        clock[0] = float(t)
        gate.note_heard()
    assert gate.level() == "normal"
