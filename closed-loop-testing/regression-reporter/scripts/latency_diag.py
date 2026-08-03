#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Diagnostic: reconcile two latency tables for one run.

Computes the disputed rows (mute reaction, Person->BA ROI) under several
counting policies so we can see which policy yields which table.
"""
import sys
sys.path.insert(0, "/app")
import json
import warnings
from pathlib import Path

import numpy as np
import pandas as pd

warnings.filterwarnings("ignore")
from srr import aggregator as A

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260615-043909")
roi, tw = A.load_roi(Path("/app/calibration.json"))
parquets = sorted(RUN.glob("*/scenes/scn_*.parquet"))


def st(vals):
    if not vals:
        return None
    a = np.asarray(vals, float)
    return {"median": round(float(np.median(a)), 1),
            "mean": round(float(a.mean()), 1),
            "p95_linear": round(float(np.percentile(a, 95)), 1),
            "p95_nearest": round(float(np.percentile(a, 95, method="nearest")), 1),
            "p95_lower": round(float(np.percentile(a, 95, method="lower")), 1),
            "max": round(float(a.max()), 1), "n": len(a)}


def mute_edges(df):
    """Return (settled_lags, n_never_settle_edges) for expected_mute True edges."""
    em = df["expected_mute"].astype(bool)
    prev = em.shift(1).fillna(False).astype(bool)            # expected_value=True -> fill not True = False
    edges = (em == True) & (prev != True)
    pos = [i for i, v in enumerate(edges.tolist()) if v]
    am = df["actual_mute"].astype(bool).tolist()
    wt = df["arrival_wall_time"].tolist()
    settled, never = [], 0
    for p in pos:
        t0 = wt[p]
        try:
            j = next(i for i in range(p, len(am)) if am[i] is True or am[i] == True)
        except StopIteration:
            never += 1
            continue
        if wt[j] >= t0:
            settled.append((wt[j] - t0) * 1000.0)
    return settled, never


pools = {}
anychar_delays_2s, anychar_delays_3s = [], []
anychar_gt = 0
mute_settled, mute_never_edges = [], 0
mute_capped = []   # settled + never(capped at clip end)
roi_raw_delays = []           # canonical: every matched per-char entry
roi_raw_gt = roi_raw_matched = 0
roi_dedup_entries_all = []    # dedup GT entries (merge within 1s across chars), per clip
roi_dedup_delays_2s = []
roi_dedup_delays_3s = []
roi_dedup_gt = 0

for pq in parquets:
    rd = pq.parent.parent
    if rd.name not in pools:
        pools[rd.name] = A.load_run_ba_pool(rd)
    pool = pools[rd.name]
    ba_override = None
    if pool:
        sdf = pd.read_parquet(pq, columns=["arrival_wall_time"])
        ba_override = A.filter_ba_pool(pool, float(sdf["arrival_wall_time"].iloc[0]),
                                       float(sdf["arrival_wall_time"].iloc[-1]))
    v, df = A.analyze_clip(pq, roi, tw, ba_events_override=ba_override)
    times = df["arrival_wall_time"].tolist()
    t0 = times[0] if times else 0.0
    clip_end = times[-1] if times else 0.0

    s, nev = mute_edges(df)
    mute_settled += s
    mute_never_edges += nev
    mute_capped += s
    # cap never-settle edges at clip end: find their edge times
    em = df["expected_mute"].astype(bool)
    prev = em.shift(1).fillna(False).astype(bool)
    edges = (em == True) & (prev != True)
    am = df["actual_mute"].astype(bool).tolist()
    for p in [i for i, x in enumerate(edges.tolist()) if x]:
        if not any(am[i] == True for i in range(p, len(am))):
            mute_capped.append((clip_end - times[p]) * 1000.0)

    # ROI canonical (per-char entries) -> reuse verdict's match set
    roi_raw_gt += v.ba_roi_entry_match.gt_count
    roi_raw_matched += v.ba_roi_entry_match.matched
    roi_raw_delays += [m.delay_ms for m in v.ba_roi_entry_match.matches if m.matched]

    # ROI dedup: collect per-char entry times, merge within 1.0s
    ent = []
    for c in A.CHAR_COLS:
        s_ = pd.Series([A.in_roi(roi, x, y) for x, y in zip(df[f"{c}_x"], df[f"{c}_y"])])
        pv = s_.shift(1).astype(object).where(lambda z: z.notna(), s_.iloc[0]).astype(bool)
        for i, hit in enumerate((s_ & ~pv).tolist()):
            if hit and i > 0:
                ent.append(times[i])
    ent.sort()
    dedup = []
    for t in ent:
        if not dedup or t - dedup[-1] > 1.0:
            dedup.append(t)
    roi_dedup_gt += len(dedup)
    roi_dedup_entries_all += dedup
    # match dedup entries to BA roi events
    all_evts = ba_override if ba_override is not None else []
    roi_ts = sorted(e.get("_kafka_ts") for e in all_evts
                    if "ROI" in e.get("event_types", []) and "Person" in e.get("classes", [])
                    and e.get("_kafka_ts") is not None)
    for win, sink in ((2.0, roi_dedup_delays_2s), (3.0, roi_dedup_delays_3s)):
        ms = A.match_first_ba(dedup, roi_ts, t0, max_delay=win)
        sink += [abs(m.delay_ms) for m in ms.matches if m.matched]

    # ROI scene-level: any_char False->True transitions (zone becomes occupied)
    any_s = pd.Series(df["any_char_in_roi"].astype(bool).tolist())
    any_prev = any_s.shift(1).astype(object).where(lambda z: z.notna(), any_s.iloc[0]).astype(bool)
    any_ent = [times[i] for i, hit in enumerate((any_s & ~any_prev).tolist()) if hit and i > 0]
    anychar_gt += len(any_ent)
    for win, sink in ((2.0, anychar_delays_2s), (3.0, anychar_delays_3s)):
        ms = A.match_first_ba(any_ent, roi_ts, t0, max_delay=win)
        sink += [abs(m.delay_ms) for m in ms.matches if m.matched]

print(json.dumps({
    "run": RUN.name, "clips": len(parquets),
    "MUTE": {
        "canonical_settled(drop never)": st(mute_settled),
        "never_settle_edges": mute_never_edges,
        "incl_never_capped_at_clip_end": st(mute_capped),
    },
    "ROI_person_BA": {
        "canonical_per_char (|dt|, 3s win)": {**st([abs(x) for x in roi_raw_delays]),
                                              "gt": roi_raw_gt, "matched": roi_raw_matched},
        "dedup_entries_total": roi_dedup_gt,
        "dedup_matched_2s_win(|dt|)": st(roi_dedup_delays_2s),
        "dedup_matched_3s_win(|dt|)": st(roi_dedup_delays_3s),
        "scene_anychar_gt": anychar_gt,
        "scene_anychar_matched_2s(|dt|)": st(anychar_delays_2s),
        "scene_anychar_matched_3s(|dt|)": st(anychar_delays_3s),
    },
}, indent=2))
