# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""What a PSF proximity decision does to this truck's speed.

PSF (`--app pxc`) issues one of three motion levels per decision: normal,
reduce_speed, stop. comm-layer republishes each on /safety/proximity/pair. Three
properties of that stream decide the shape of this class:

- It is lossy. The SDM merges a batch last-writer-wins and the ROS bridge
  samples one OPC UA snapshot at 10 Hz. comm-layer holds the worst decision
  visible for several polls, but a STOP still holds here for `stop_hold_s`
  after the last one seen, and a REDUCE for `reduce_hold_s`.
- It is silent when nothing is paired: no person in view means no decision at
  all. Silence keeps the last level; it is not a NORMAL.
- Its link can die. Heartbeats (every 5 s from the SDM) keep the message stream
  going while no decision changes, so once PSF has spoken, nothing at all for
  `stale_s` means the link is gone: STOP, with fault `psf_link_stale`, until a
  new decision arrives.

A fault opcode (HW_ERROR / SW_ERROR, or one the decoder does not know) carries
no level and is taken as a stop.
"""

from __future__ import annotations

import math
import time
from typing import Callable, Optional

NORMAL = "normal"
REDUCE = "reduce_speed"
STOP = "stop"
LEVELS = (NORMAL, REDUCE, STOP)
# Published instead of a level: PSF has said nothing yet, or the gate is off.
INACTIVE = "inactive"
DISABLED = "disabled"
LINK_STALE = "psf_link_stale"


class ProximityGate:
    def __init__(self, reduce_speed: float, stop_hold_s: float, reduce_hold_s: float,
                 stale_s: float, clock: Callable[[], float] = time.monotonic):
        self.reduce_speed = float(reduce_speed)
        self.stop_hold_s = float(stop_hold_s)
        self.reduce_hold_s = float(reduce_hold_s)
        self.stale_s = float(stale_s)
        self._clock = clock
        self._latest: Optional[str] = None
        self._latest_seq = None
        self._last_stop = -math.inf
        self._last_reduce = -math.inf
        self._last_heard = -math.inf
        self._stale = False
        self.separation_m: Optional[float] = None
        self.fault: Optional[str] = None

    def note_heard(self) -> None:
        """A message arrived on the link, whatever it says."""
        self._last_heard = self._clock()

    def on_decision(self, decision: dict) -> None:
        """One /safety/proximity/pair message, already JSON-decoded."""
        self.note_heard()
        now = self._last_heard
        seq = decision.get("sequence")
        mode = decision.get("mode")
        if seq is not None and seq == self._latest_seq:
            # The same decision republished because a heartbeat arrived: liveness only.
            return
        if mode in LEVELS:
            fault = None
        elif decision.get("safety_critical"):
            fault = decision.get("command_name") or "fault"
            mode = STOP
        else:
            # Heartbeat or safe-release handshake: says nothing about motion.
            return
        self.fault = fault
        self._stale = False
        self._latest, self._latest_seq = mode, seq
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

    @property
    def stale(self) -> bool:
        return self._stale

    def level(self) -> str:
        now = self._clock()
        if self.active and not self._stale and now - self._last_heard > self.stale_s:
            # Latched until a new decision: one more heartbeat proves the link,
            # not that the decision it held is still current.
            self._stale = True
            self.fault = LINK_STALE
        if self._stale:
            return STOP
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
