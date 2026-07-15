#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Monitor 4 SRR ground-truth TF topics for 5 minutes.

Subscribes to /gt/{character_0,character_1,character_2,forklift}/tf, records
every transform, prints heartbeat every 30s, and on exit summarizes:
  - total msgs received
  - average rate (Hz)
  - x/y range observed
  - distance traveled (sum of consecutive Euclidean steps)
  - Number of "round trips" inferred (forklift only, by counting x-axis sign changes)

Run from host shell with ROS Humble sourced:
  source /opt/ros/humble/setup.bash
  python3 scenarios/host-scripts/monitor_gt_5min.py
"""
import math
import os
import sys
import time
from collections import defaultdict

import rclpy
from rclpy.node import Node
from tf2_msgs.msg import TFMessage

DURATION_S = 300       # 5 min
HEARTBEAT_S = 30       # status line every 30s
TOPICS = [
    "/gt/character_0/tf",
    "/gt/character_1/tf",
    "/gt/character_2/tf",
    "/gt/forklift/tf",
]
# For forklift, only track 'body' frame (skip 'lift' sub-frame)
FORKLIFT_FRAME = "body"


class Stats:
    __slots__ = ("count", "first_ts", "last_ts", "xs", "ys", "dist", "last_xy")

    def __init__(self):
        self.count = 0
        self.first_ts = None
        self.last_ts = None
        self.xs = []
        self.ys = []
        self.dist = 0.0
        self.last_xy = None

    def add(self, t_wall, x, y):
        if self.first_ts is None:
            self.first_ts = t_wall
        self.last_ts = t_wall
        self.count += 1
        self.xs.append(x)
        self.ys.append(y)
        if self.last_xy is not None:
            dx = x - self.last_xy[0]
            dy = y - self.last_xy[1]
            self.dist += math.sqrt(dx * dx + dy * dy)
        self.last_xy = (x, y)


class Monitor(Node):
    def __init__(self):
        super().__init__("srr_gt_monitor")
        self.stats = defaultdict(Stats)
        self.start = time.monotonic()
        for t in TOPICS:
            self.create_subscription(TFMessage, t,
                                     self._make_cb(t), 50)
        # heartbeat timer
        self.create_timer(HEARTBEAT_S, self._heartbeat)
        # stop timer
        self.create_timer(DURATION_S, self._stop)

    def _make_cb(self, topic):
        is_forklift = "forklift" in topic

        def cb(msg: TFMessage):
            for tr in msg.transforms:
                if is_forklift and tr.child_frame_id != FORKLIFT_FRAME:
                    continue
                t_wall = time.monotonic()
                x = tr.transform.translation.x
                y = tr.transform.translation.y
                self.stats[topic].add(t_wall, x, y)
        return cb

    def _heartbeat(self):
        elapsed = time.monotonic() - self.start
        line = [f"[t+{elapsed:5.1f}s]"]
        for t in TOPICS:
            s = self.stats[t]
            if s.count == 0:
                line.append(f"{t}=NO_MSG")
                continue
            x, y = s.last_xy
            line.append(f"{t.split('/')[-2][:9]:>9}: n={s.count:5d} pos=({x:+6.2f},{y:+6.2f})")
        print("  ".join(line), flush=True)

    def _stop(self):
        self._summary()
        rclpy.shutdown()

    def _summary(self):
        print()
        print("=" * 72)
        print(f"5-minute monitor summary  (target {DURATION_S}s)")
        print("=" * 72)
        for t in TOPICS:
            s = self.stats[t]
            if s.count == 0:
                print(f"  {t:30s} : NO MESSAGES RECEIVED")
                continue
            duration = (s.last_ts - s.first_ts) if s.first_ts else 0
            hz = s.count / duration if duration > 0 else 0
            x_min, x_max = min(s.xs), max(s.xs)
            y_min, y_max = min(s.ys), max(s.ys)
            print(f"  {t}")
            print(f"    msgs       : {s.count}")
            print(f"    avg rate   : {hz:5.2f} Hz   (over {duration:.1f}s)")
            print(f"    x range    : [{x_min:+.3f}, {x_max:+.3f}]   span {x_max-x_min:.2f}")
            print(f"    y range    : [{y_min:+.3f}, {y_max:+.3f}]   span {y_max-y_min:.2f}")
            print(f"    path dist  : {s.dist:.2f} m")

            # Forklift-only: count round trips by detecting x dir reversals
            if "forklift" in t:
                trips = self._count_trips(s.xs)
                print(f"    direction reversals (round-trip count) : {trips}")
        print("=" * 72)

    @staticmethod
    def _count_trips(xs, eps=0.01):
        # Count number of times x dx changes sign (above eps to ignore noise)
        if len(xs) < 3:
            return 0
        sign = 0
        reversals = 0
        for i in range(1, len(xs)):
            d = xs[i] - xs[i - 1]
            if abs(d) < eps:
                continue
            new_sign = 1 if d > 0 else -1
            if sign != 0 and new_sign != sign:
                reversals += 1
            sign = new_sign
        return reversals


def main():
    os.environ.setdefault("ROS_DOMAIN_ID", "74")
    rclpy.init()
    node = Monitor()
    print(f"Monitoring {len(TOPICS)} topics for {DURATION_S}s "
          f"(heartbeat every {HEARTBEAT_S}s). ROS_DOMAIN_ID={os.environ['ROS_DOMAIN_ID']}",
          flush=True)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == "__main__":
    main()
