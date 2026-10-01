# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""What a PSF proximity decision does to this truck's speed.

PSF (`--app pxc`) issues one of three motion levels per decision: normal,
reduce_speed, stop. comm-layer republishes each on /safety/proximity/pair. Two
properties of that stream decide the shape of this class:

- It is lossy. The ROS bridge samples one OPC UA snapshot at 10 Hz, and the SDM
  merges a batch last-writer-wins, so a STOP can be followed by a NORMAL from
  the same instant or be skipped outright. A STOP therefore holds for
  `stop_hold_s` after the last one seen, and a REDUCE for `reduce_hold_s`.
- It is silent when nothing is paired: no person in view means no decision at
  all. Silence keeps the last level; it is not a NORMAL.

A fault opcode (HW_ERROR / SW_ERROR) carries no level and is taken as a stop.
"""

from __future__ import annotations

import math
import time
from typing import Callable, Optional

NORMAL = "normal"
REDUCE = "reduce_speed"
STOP = "stop"
LEVELS = (NORMAL, REDUCE, STOP)


class ProximityGate:
    def __init__(self, reduce_speed: float, stop_hold_s: float, reduce_hold_s: float,
                 clock: Callable[[], float] = time.monotonic):
        self.reduce_speed = float(reduce_speed)
        self.stop_hold_s = float(stop_hold_s)
        self.reduce_hold_s = float(reduce_hold_s)
        self._clock = clock
        self._latest: Optional[str] = None
        self._last_stop = -math.inf
        self._last_reduce = -math.inf
        self.separation_m: Optional[float] = None
        self.fault: Optional[str] = None

    def on_decision(self, decision: dict) -> None:
        """One /safety/proximity/pair message, already JSON-decoded."""
        mode = decision.get("mode")
        now = self._clock()
        if mode in LEVELS:
            self.fault = None
        elif decision.get("safety_critical"):
            self.fault = decision.get("command_name") or "fault"
            mode = STOP
        else:
            # Heartbeat or safe-release handshake: says nothing about motion.
            return
        self._latest = mode
        sep = decision.get("separation_m")
        self.separation_m = float(sep) if isinstance(sep, (int, float)) else None
        if mode == STOP:
            self._last_stop = now
        elif mode == REDUCE:
            self._last_reduce = now

    @property
    def active(self) -> bool:
        """Whether PSF has said anything yet. False on an atl run."""
        return self._latest is not None

    def level(self) -> str:
        now = self._clock()
        if self._latest == STOP or now - self._last_stop < self.stop_hold_s:
            return STOP
        if self._latest == REDUCE or now - self._last_reduce < self.reduce_hold_s:
            return REDUCE
        return NORMAL

    def limit(self, linear: float, angular: float) -> tuple[float, float]:
        """(linear, angular) after the current level, keeping the path's curvature.

        Scaling both by one ratio leaves the turning radius unchanged, so a truck
        slowed mid-corner stays on the corner it was driving.
        """
        level = self.level()
        if level == STOP:
            return 0.0, 0.0
        if level == REDUCE and abs(linear) > self.reduce_speed:
            ratio = self.reduce_speed / abs(linear)
            return linear * ratio, angular * ratio
        return linear, angular
