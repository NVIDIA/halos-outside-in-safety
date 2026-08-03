#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Per-class split-frames% (frame-weighted) + id-switches (sum) for one run."""
import sys, warnings
from pathlib import Path
import pandas as pd
sys.path.insert(0, "/app")
warnings.filterwarnings("ignore")
from srr import aggregator as A

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260615-043909")
roi, tw = A.load_roi(Path("/app/calibration.json"))
pqs = sorted(RUN.glob("*/scenes/scn_*.parquet"))

idsw = {"Person": 0, "Forklift": 0}
split_frames = {"Person": 0.0, "Forklift": 0.0}
tot_frames = 0
clip_split = {"Person": [], "Forklift": []}

for pq in pqs:
    v, _ = A.analyze_clip(pq, roi, tw)
    p2 = v.phase2
    if not p2 or not p2.get("gt_available"):
        continue
    for a, info in (p2.get("id_switches") or {}).items():
        cls = "Person" if a.startswith("char") else "Forklift"
        idsw[cls] += info.get("switches", 0)
    sp = p2.get("split") or {}
    nfr = p2.get("unique_detector_frames") or 0
    tot_frames += nfr
    for c in ("Person", "Forklift"):
        pct = (sp.get("by_class_split_pct") or {}).get(c)
        if pct is not None:
            split_frames[c] += pct / 100.0 * nfr
            clip_split[c].append(pct)

print("run:", RUN.name, "clips:", len(pqs), "detector_frames:", tot_frames)
for c in ("Person", "Forklift"):
    fw = round(100 * split_frames[c] / tot_frames, 2) if tot_frames else None
    cm = round(sum(clip_split[c]) / len(clip_split[c]), 2) if clip_split[c] else None
    print(f"{c:9s}  split% frame-weighted={fw}  split% clip-mean={cm}  "
          f"id_switches(sum)={idsw[c]}")
print("total id_switches:", idsw["Person"] + idsw["Forklift"])
