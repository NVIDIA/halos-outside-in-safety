#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Probe: is the ~0.5s detector staleness driven by low FPS or by pipeline buffering?

Measures, run-wide:
  - detector frame rate  = unique bev_frame_id per second
  - detector frame interval = diff between consecutive unique bev_create_time
  - sim realtime factor  = sim_time span / wall span (1.0 = realtime, <1 = sim slower)
  - staleness            = arrival_wall_time - bev_create_time
  - staleness / frame_interval = how many frames deep the pipeline buffer is
"""
import sys, warnings
from pathlib import Path
import numpy as np, pandas as pd
warnings.filterwarnings("ignore")

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260615-043909")
pqs = sorted(RUN.glob("*/scenes/scn_*.parquet"))

stale, intervals, rt, fps_clip = [], [], [], []
for pq in pqs:
    df = pd.read_parquet(pq, columns=["arrival_wall_time", "sim_time",
                                      "bev_create_time", "bev_frame_id"])
    a = df["arrival_wall_time"].to_numpy()
    b = df["bev_create_time"].to_numpy()
    s = df["sim_time"].to_numpy()
    d = a - b
    stale += [x for x in d if np.isfinite(x)]

    uniq = df.dropna(subset=["bev_create_time"]).drop_duplicates(subset=["bev_frame_id"])
    bt = np.sort(uniq["bev_create_time"].to_numpy())
    diffs = np.diff(bt)
    intervals += [x for x in diffs if 0 < x < 5]

    dur = a[-1] - a[0]
    if dur > 0:
        fps_clip.append(len(uniq) / dur)
        sv = s[np.isfinite(s)]
        if len(sv) > 1:
            rt.append((sv[-1] - sv[0]) / dur)


def st(v):
    v = np.asarray(v, float)
    return {"median": round(float(np.median(v)), 3), "mean": round(float(v.mean()), 3),
            "p95": round(float(np.percentile(v, 95)), 3), "n": len(v)}


print("run:", RUN.name, "clips:", len(pqs))
print("staleness  arrival-bev_create (s):", st(stale))
print("detector frame interval (s):      ", st(intervals),
      "-> FPS ~", round(1 / np.median(intervals), 1))
print("detector FPS per clip:            ", st(fps_clip))
print("sim realtime factor (simT/wall):  ", st(rt))
print("staleness / frame_interval (med): ",
      round(np.median(stale) / np.median(intervals), 1), "frames deep")
