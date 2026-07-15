#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Focused tracker for /gt/character_1/tf — confirms whether Char_01 reaches
expected East endpoint (6.28, -16.37) at least once.

Logs:
  - every NEW max-x (so you see progression as character walks East)
  - every full-cycle reset (when x dips back below half of max)
  - elapsed time + total messages

Run from host with ROS Humble sourced:
  source /opt/ros/humble/setup.bash
  python3 scenarios/host-scripts/track_char1_east.py

Stop with Ctrl-C — final summary on shutdown.
"""
import math
import os
import sys
import time

import rclpy
from rclpy.node import Node
from tf2_msgs.msg import TFMessage

EAST_TARGET = (6.28, -16.37)
WEST_SPAWN  = (0.29, -18.00)
REACH_THRESHOLD_M = 0.30        # within 30cm of East target counts as "reached"


class Tracker(Node):
    def __init__(self):
        super().__init__("char1_east_tracker")
        self.start = time.monotonic()
        self.msg_count = 0
        self.max_x = -1e9
        self.min_x = +1e9
        self.last_x = None
        self.first_reach_t = None
        self.reach_count = 0
        self.cycles = 0
        self._was_east = False

        self.create_subscription(TFMessage, "/gt/character_1/tf", self._cb, 50)
        self.create_timer(10.0, self._heartbeat)
        print(f"Watching /gt/character_1/tf. Expected East endpoint = {EAST_TARGET}", flush=True)
        print(f"Reach threshold = {REACH_THRESHOLD_M:.2f}m radius. Heartbeat every 10s.", flush=True)
        print("Press ▶ play in Isaac Sim now.", flush=True)
        print("-" * 70, flush=True)

    def _cb(self, msg: TFMessage):
        for tr in msg.transforms:
            x = tr.transform.translation.x
            y = tr.transform.translation.y
            self.msg_count += 1
            t = time.monotonic() - self.start

            # Track new max x
            if x > self.max_x + 0.05:
                self.max_x = x
                print(f"  [t+{t:6.1f}s n={self.msg_count:5d}]  new max_x = {x:+6.3f}, y = {y:+6.3f}", flush=True)
            if x < self.min_x - 0.05:
                self.min_x = x

            # Distance to East target
            dx = x - EAST_TARGET[0]
            dy = y - EAST_TARGET[1]
            d = math.sqrt(dx * dx + dy * dy)

            if d <= REACH_THRESHOLD_M:
                if not self._was_east:
                    self.reach_count += 1
                    if self.first_reach_t is None:
                        self.first_reach_t = t
                        print(f"  *** REACHED EAST at t+{t:.1f}s  (dist={d:.3f}m, pos=({x:+.3f},{y:+.3f})) ***", flush=True)
                    else:
                        print(f"  *** REACHED EAST again #{self.reach_count} at t+{t:.1f}s ***", flush=True)
                self._was_east = True
            elif d > 1.0:
                if self._was_east:
                    self.cycles += 1
                self._was_east = False

            self.last_x = x

    def _heartbeat(self):
        t = time.monotonic() - self.start
        elapsed_msgs = self.msg_count
        hz = elapsed_msgs / t if t > 0 else 0
        d_to_east = abs((self.last_x or 0) - EAST_TARGET[0])
        print(f"[t+{t:6.1f}s n={self.msg_count:5d} hz={hz:5.1f}]  "
              f"x ∈ [{self.min_x:+.2f}, {self.max_x:+.2f}]  "
              f"last_x={self.last_x:+.2f}  Δeast_x={d_to_east:.2f}m  "
              f"reaches={self.reach_count}", flush=True)

    def summary(self):
        t = time.monotonic() - self.start
        print()
        print("=" * 70)
        print(f"Char_01 East-reach summary  ({t:.1f}s, {self.msg_count} msgs)")
        print("=" * 70)
        print(f"  x range observed : [{self.min_x:+.3f}, {self.max_x:+.3f}]")
        print(f"  East target      : {EAST_TARGET}")
        if self.first_reach_t is not None:
            print(f"  First reach at   : t+{self.first_reach_t:.1f}s")
            print(f"  Total reaches    : {self.reach_count}")
            print(f"  Verdict          : ✓ Char_01 DOES reach East endpoint")
        else:
            shortfall = EAST_TARGET[0] - self.max_x
            print(f"  First reach at   : NEVER (max x={self.max_x:+.3f}, shortfall={shortfall:+.2f}m)")
            print(f"  Verdict          : ✗ Char_01 stuck — investigate animation/path errors")
        print("=" * 70)


def main():
    os.environ.setdefault("ROS_DOMAIN_ID", "74")
    rclpy.init()
    node = Tracker()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    node.summary()
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == "__main__":
    main()
