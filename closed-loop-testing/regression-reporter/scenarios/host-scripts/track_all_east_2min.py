#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
2-minute east-endpoint reach tracker for all 4 SRR ground-truth actors.
Subscribes BEFORE the sim plays, then waits and records.

Run: python3 track_all_east_2min.py
"""
import math
import os
import time

import rclpy
from rclpy.node import Node
from tf2_msgs.msg import TFMessage

DURATION_S = 130                 # ~2 min + 10s setup buffer
EAST_TARGETS = {
    "character_0": (9.20, -11.65),
    "character_1": (6.28, -16.37),
    "character_2": (5.73, -18.25),
    "forklift":    (15.86, -13.38),
}
REACH_M = 0.40
FORKLIFT_FRAME = "body"


class Tracker(Node):
    def __init__(self):
        super().__init__("srr_east_tracker_2min")
        self.t0 = time.monotonic()
        self.stats = {k: {"max_x": -1e9, "min_x": 1e9, "max_y": -1e9, "min_y": 1e9,
                          "best_dist": 1e9, "first_reach_t": None,
                          "n": 0, "last": None}
                      for k in EAST_TARGETS}

        for k in EAST_TARGETS:
            topic = f"/gt/{k}/tf"
            self.create_subscription(TFMessage, topic, self._make_cb(k), 50)

        self.create_timer(15.0, self._heartbeat)
        self.create_timer(DURATION_S, self._stop)

    def _make_cb(self, key):
        is_forklift = (key == "forklift")
        def cb(msg: TFMessage):
            for tr in msg.transforms:
                if is_forklift and tr.child_frame_id != FORKLIFT_FRAME:
                    continue
                x = tr.transform.translation.x
                y = tr.transform.translation.y
                s = self.stats[key]
                s["n"] += 1
                s["last"] = (x, y)
                if x > s["max_x"]: s["max_x"] = x
                if x < s["min_x"]: s["min_x"] = x
                if y > s["max_y"]: s["max_y"] = y
                if y < s["min_y"]: s["min_y"] = y

                tx, ty = EAST_TARGETS[key]
                d = math.sqrt((x - tx) ** 2 + (y - ty) ** 2)
                if d < s["best_dist"]:
                    s["best_dist"] = d
                if d <= REACH_M and s["first_reach_t"] is None:
                    s["first_reach_t"] = time.monotonic() - self.t0
                    print(f"  *** {key} REACHED EAST at t+{s['first_reach_t']:.1f}s "
                          f"(pos=({x:+.2f},{y:+.2f}), d={d:.2f}m) ***", flush=True)
        return cb

    def _heartbeat(self):
        t = time.monotonic() - self.t0
        line = [f"[t+{t:5.1f}s]"]
        for k in EAST_TARGETS:
            s = self.stats[k]
            if s["n"] == 0:
                line.append(f"{k[:9]:>9}=NO_MSG")
            else:
                lx, ly = s["last"]
                tag = "✓" if s["first_reach_t"] is not None else f"d={s['best_dist']:.1f}"
                line.append(f"{k[:9]:>9}: n={s['n']:5d} pos=({lx:+5.1f},{ly:+5.1f}) {tag}")
        print("  ".join(line), flush=True)

    def _stop(self):
        self._summary()
        rclpy.shutdown()

    def _summary(self):
        t = time.monotonic() - self.t0
        print()
        print("=" * 70)
        print(f"2-min summary  ({t:.1f}s)")
        print("=" * 70)
        all_pass = True
        for k, target in EAST_TARGETS.items():
            s = self.stats[k]
            if s["n"] == 0:
                print(f"  {k:12s}  NO MESSAGES")
                all_pass = False
                continue
            print(f"  {k:12s}")
            print(f"    msgs        : {s['n']}")
            print(f"    x range     : [{s['min_x']:+6.2f}, {s['max_x']:+6.2f}]   span {s['max_x']-s['min_x']:.2f}m")
            print(f"    y range     : [{s['min_y']:+6.2f}, {s['max_y']:+6.2f}]")
            print(f"    east target : {target}")
            print(f"    best dist   : {s['best_dist']:.3f}m")
            if s['first_reach_t'] is not None:
                print(f"    reached     : ✓ at t+{s['first_reach_t']:.1f}s  (within {REACH_M}m)")
            else:
                print(f"    reached     : ✗ (never within {REACH_M}m of target)")
                all_pass = False
        print("=" * 70)
        print(f"  Verdict: {'ALL PASS' if all_pass else 'SOME ACTORS DID NOT REACH'}")


def main():
    os.environ.setdefault("ROS_DOMAIN_ID", "74")
    rclpy.init()
    node = Tracker()
    print("=" * 70, flush=True)
    print(f"All-4-actor east-reach tracker, duration={DURATION_S}s", flush=True)
    print(f"Targets: {EAST_TARGETS}", flush=True)
    print("Reach threshold = 0.40m radius. Heartbeat every 15s.", flush=True)
    print("=" * 70, flush=True)
    print("READY → press ▶ play now", flush=True)
    print("=" * 70, flush=True)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        node._summary()
    if rclpy.ok():
        rclpy.shutdown()


if __name__ == "__main__":
    main()
