#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Aggregate end-to-end reaction latencies for one multi-test run.

Reuses srr.aggregator's exact per-clip logic (same calibration ROI/TW, same
run-level BA pool + ±pad scene matching used by `--top-level`), then pools the
raw per-event/per-transition latencies across all clips and reports
median/mean/p95/max/n per reaction.

Reactions:
  1. Danger appears -> raise alarm (unmute)   : reaction_lags(expected=False)
  2. Scene safe -> all-clear (mute)           : reaction_lags(expected=True)
  3. Person enters work-zone -> BA event      : ba_roi_entry_match delays
  4. Forklift enters trailer -> BA event      : ba_tw_in_match  (Right) delays
  5. Forklift exits trailer  -> BA event      : ba_tw_out_match (Left)  delays
"""
import sys
sys.path.insert(0, "/app")

import json
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")  # silence pandas fillna downcast FutureWarnings

import numpy as np
import pandas as pd

from srr import aggregator as A

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260612-044956")
CALIB = Path("/app/calibration.json")

roi, tw_x, tw_y_min, tw_y_max = A.load_roi(CALIB)
parquets = sorted(RUN.glob("*/scenes/scn_*.parquet"))


def reaction_lags(df: pd.DataFrame, expected_value: bool) -> list[float]:
    """Verbatim copy of the nested reaction_lags() inside analyze_clip()."""
    em = df["expected_mute"].astype(bool)
    prev = em.shift(1).fillna(not expected_value).astype(bool)
    edges = (em == expected_value) & (prev != expected_value)
    edge_positions = [i for i, v in enumerate(edges.tolist()) if v]
    am = df["actual_mute"].astype(bool).tolist()
    wt = df["arrival_wall_time"].tolist()
    lags = []
    for pos in edge_positions:
        t_edge = wt[pos]
        try:
            next_match = next(i for i in range(pos, len(am)) if am[i] == expected_value)
        except StopIteration:
            continue
        t1 = wt[next_match]
        if t1 >= t_edge:
            lags.append((t1 - t_edge) * 1000.0)
    return lags


def stats(vals):
    if not vals:
        return None
    a = np.asarray(vals, dtype=float)
    return {
        "median": round(float(np.median(a)), 1),
        "mean": round(float(a.mean()), 1),
        "p95": round(float(np.percentile(a, 95)), 1),
        "max": round(float(a.max()), 1),
        "min": round(float(a.min()), 1),
        "n": int(a.size),
    }


pools: dict[str, list] = {}
agg = {k: [] for k in ("unmute", "mute", "roi", "twin", "twout")}
per_run: dict[str, dict] = {}
unmute_never = mute_never = 0
roi_gt = roi_m = twin_gt = twin_m = twout_gt = twout_m = 0

for pq in parquets:
    run_dir = pq.parent.parent
    label = run_dir.name
    if label not in pools:
        pools[label] = A.load_run_ba_pool(run_dir)
    pool = pools[label]
    ba_override = None
    if pool:
        sdf = pd.read_parquet(pq, columns=["arrival_wall_time"])
        s0 = float(sdf["arrival_wall_time"].iloc[0])
        s1 = float(sdf["arrival_wall_time"].iloc[-1])
        ba_override = A.filter_ba_pool(pool, s0, s1)
    v, df = A.analyze_clip(pq, roi, tw_x, tw_y_min, tw_y_max,
                           ba_events_override=ba_override)

    u = reaction_lags(df, False)
    m = reaction_lags(df, True)
    rd = [x.delay_ms for x in v.ba_roi_entry_match.matches if x.matched]
    ti = [x.delay_ms for x in v.ba_tw_in_match.matches if x.matched]
    to = [x.delay_ms for x in v.ba_tw_out_match.matches if x.matched]

    agg["unmute"] += u
    agg["mute"] += m
    agg["roi"] += rd
    agg["twin"] += ti
    agg["twout"] += to

    unmute_never += int(v.unmute_never_engaged)
    mute_never += int(v.mute_never_engaged)
    roi_gt += v.ba_roi_entry_match.gt_count; roi_m += v.ba_roi_entry_match.matched
    twin_gt += v.ba_tw_in_match.gt_count;   twin_m += v.ba_tw_in_match.matched
    twout_gt += v.ba_tw_out_match.gt_count; twout_m += v.ba_tw_out_match.matched

    pr = per_run.setdefault(label, {k: [] for k in agg})
    pr["unmute"] += u; pr["mute"] += m
    pr["roi"] += rd; pr["twin"] += ti; pr["twout"] += to

# BA delays are SIGNED (ba - gt). Report both signed and absolute magnitude.
def both(vals):
    return {"signed": stats(vals), "abs": stats([abs(x) for x in vals])}

out = {
    "run": RUN.name,
    "clips": len(parquets),
    "tw_x": tw_x,
    "reactions": {
        "unmute": stats(agg["unmute"]),
        "mute": stats(agg["mute"]),
        "roi": both(agg["roi"]),
        "twin": both(agg["twin"]),
        "twout": both(agg["twout"]),
    },
    "ba_detect": {
        "roi": f"{roi_m}/{roi_gt}",
        "twin": f"{twin_m}/{twin_gt}",
        "twout": f"{twout_m}/{twout_gt}",
    },
    "unmute_never_engaged_clips": unmute_never,
    "mute_never_engaged_clips": mute_never,
    "per_run": {
        lbl: {
            "unmute": stats(d["unmute"]),
            "mute": stats(d["mute"]),
            "roi_abs": stats([abs(x) for x in d["roi"]]),
            "twin_abs": stats([abs(x) for x in d["twin"]]),
            "twout_abs": stats([abs(x) for x in d["twout"]]),
        }
        for lbl, d in sorted(per_run.items())
    },
}
print(json.dumps(out, indent=2))
