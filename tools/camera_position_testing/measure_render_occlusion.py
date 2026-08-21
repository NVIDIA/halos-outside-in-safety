#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Measure from a rendered frame how much of the ROI a camera actually delivers.

screen_fov.py answers "does the frustum reach the ROI", from geometry alone. That is
the question that scored y = -9.5 best of everything tried while the render showed
pipework and cage structures covering 5% of the ROI at foot level: the frustum was
clear, the pixels were not. This reads the pixels instead, so a candidate cannot pass
on geometry it does not actually see.

A sampled ROI point fails in one of two ways, counted separately because they have
different fixes:

  off-frame  the point projects outside the image. The frustum does not reach it;
             moving or re-aiming the camera is the only remedy.
  on black   the point projects onto a black pixel. Geometry inside the near plane
             renders as flat black, so something the camera cannot see past is
             between it and the ROI. Backing away turns the black into visible
             surface detail, which is why the same object reads as black at one
             mount and as panelling at another.

Both are measured at foot level (z = 0) and at head level, because a camera over the
ROI loses the near corner at foot level long before it loses a standing person.

Usage:
    measure_render_occlusion.py --calibration cal-gate-M.json \
      --shot Camera=shots/M-north.jpg --shot Camera_01=shots/M-south.jpg

Needs numpy and PIL. A bare SIL host has neither; the isaac-sim container ships both.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

from screen_fov import Sensor, point_in_polygon, project


def roi_polygon(entries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The ROI every sensor is measured against, in world metres."""
    geometry = next((e for e in entries if e.get("rois")), None)
    if geometry is None:
        raise SystemExit("no sensor in this calibration carries an ROI")
    return geometry["rois"][0]["roiCoordinates"]


def sample_roi(polygon: list[dict[str, Any]], per_axis: int) -> list[tuple[float, float]]:
    """A per_axis x per_axis grid over the ROI's bounding box, kept where it is inside."""
    xs = [p["x"] for p in polygon]
    ys = [p["y"] for p in polygon]
    grid_x = np.linspace(min(xs), max(xs), per_axis)
    grid_y = np.linspace(min(ys), max(ys), per_axis)
    return [(float(x), float(y))
            for x in grid_x for y in grid_y
            if point_in_polygon(float(x), float(y), polygon)]


def luma(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("L"))


def classify(sensor: Sensor, gray: np.ndarray, samples: list[tuple[float, float]],
             z: float, black: int) -> tuple[int, int]:
    """(off-frame, on-black) counts for `samples` lifted to height `z`."""
    off = dark = 0
    height, width = gray.shape
    for x, y in samples:
        u, v, depth = project(sensor.k, sensor.r, sensor.t, [x, y, z])
        if depth <= 0 or not (0.0 <= u < sensor.width and 0.0 <= v < sensor.height):
            off += 1
            continue
        # The render may be delivered at a different resolution than the
        # calibration declares; scale rather than silently sampling the wrong pixel.
        px = int(v * height / sensor.height), int(u * width / sensor.width)
        if gray[px] < black:
            dark += 1
    return off, dark


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--calibration", type=Path, required=True)
    parser.add_argument("--shot", action="append", default=[], metavar="NAME=path",
                        required=True, help="rendered frame for sensor NAME (repeatable)")
    parser.add_argument("--samples", type=int, default=99, metavar="N",
                        help="grid points per axis over the ROI bbox (default 99 -> 9801)")
    parser.add_argument("--height", type=float, default=1.8, metavar="M",
                        help="head level in metres (default 1.8)")
    parser.add_argument("--black", type=int, default=16, metavar="L",
                        help="luma at or below which a pixel counts as black (default 16)")
    args = parser.parse_args(argv)

    calibration = json.loads(args.calibration.read_text())
    entries = calibration["sensors"]
    polygon = roi_polygon(entries)
    samples = sample_roi(polygon, args.samples)
    print(f"\nROI sampled at {len(samples)} points "
          f"({args.samples}x{args.samples} over its bounding box)")
    print(f"black at luma <= {args.black}, head level {args.height:.2f} m\n")

    shots = {}
    for pair in args.shot:
        name, _, path = pair.partition("=")
        if not path:
            raise SystemExit(f"--shot wants NAME=path, got {pair!r}")
        shots[name] = Path(path)

    print(f"  {'sensor':<12} {'frame black':>11}  "
          f"{'foot: off / black':>19}  {'head: off / black':>19}")
    for entry in entries:
        sensor = Sensor(entry, (1920, 1080))
        shot = shots.get(sensor.id)
        if shot is None:
            print(f"  {sensor.id:<12} {'no shot given':>11}")
            continue
        gray = luma(shot)
        frame_black = float((gray < args.black).mean()) * 100.0
        foot_off, foot_dark = classify(sensor, gray, samples, 0.0, args.black)
        head_off, head_dark = classify(sensor, gray, samples, args.height, args.black)
        n = len(samples)
        print(f"  {sensor.id:<12} {frame_black:10.1f}%  "
              f"{foot_off / n * 100:8.1f}% /{foot_dark / n * 100:7.1f}%  "
              f"{head_off / n * 100:8.1f}% /{head_dark / n * 100:7.1f}%")

    print("\noff-frame: the frustum does not reach it. on black: something inside the "
          "near plane is\nin the way. Neither is visible to screen_fov.py, which "
          "sees only the frustum.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
