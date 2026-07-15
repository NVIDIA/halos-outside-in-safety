#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Live clip monitor — print one log line per forklift TW crossing during recording.

Subscribes to /gt/forklift/tf (BEST_EFFORT QoS to match Isaac publisher),
tracks forklift_x, and prints on each high-edge / low-edge crossing of
TW_X (default 9.574 m).

Output format:
  [MM:SS] Clip K: forklift <entered|exited> trailer (x=<x.xx>, t=<MM:SS>)

Run as background process during /srr/record true → false window.
"""
from __future__ import annotations

import argparse
import signal
import sys
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
from tf2_msgs.msg import TFMessage


class ClipMonitor(Node):
    def __init__(self, tw_x: float, label: str):
        super().__init__("srr_live_clip_monitor")
        self.tw_x = tw_x
        self.label = label
        self.start_wall = time.time()
        self.prev_side: int | None = None
        self.clip_idx = 0

        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=50,
        )
        self.sub = self.create_subscription(
            TFMessage, "/gt/forklift/tf", self._on_tf, qos
        )
        self._log("Live clip monitor started "
                  f"(label={label}, TW_X={tw_x})")

    def _on_tf(self, msg: TFMessage) -> None:
        for tr in msg.transforms:
            if "forklift" not in tr.child_frame_id:
                continue
            x = tr.transform.translation.x
            side = 1 if x > self.tw_x else 0
            if self.prev_side is None:
                self.prev_side = side
                return
            if side != self.prev_side:
                self.clip_idx += 1
                direction = "entered" if side == 1 else "exited"
                wall_elapsed = time.time() - self.start_wall
                wall_str = self._fmt_time(wall_elapsed)
                self._log(
                    f"Clip {self.clip_idx}: forklift {direction} trailer "
                    f"(x={x:.2f}, t={wall_str})"
                )
                self.prev_side = side
            return

    def _log(self, msg: str) -> None:
        wall_str = self._fmt_time(time.time() - self.start_wall)
        print(f"  [{wall_str}] {msg}", flush=True)

    @staticmethod
    def _fmt_time(seconds: float) -> str:
        m, s = divmod(int(seconds), 60)
        return f"{m:02d}:{s:02d}"

    def shutdown_summary(self) -> None:
        self._log(f"Live monitor stopped. {self.clip_idx} total clip "
                  f"boundaries detected.")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--tw-x", type=float, default=9.574,
                   help="Trailer tripwire X coordinate")
    p.add_argument("--label", default="run",
                   help="Scenario label (printed at startup)")
    args = p.parse_args()

    rclpy.init()
    node = ClipMonitor(args.tw_x, args.label)

    def _shutdown(_sig, _frm):
        node.shutdown_summary()
        rclpy.shutdown()
        sys.exit(0)

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.shutdown_summary()
        try:
            rclpy.shutdown()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
