#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Probe a 3-stage split: GT -> detection(mdx-bev) -> BA-track(ba_positions) -> BA-event.

All on the common wall clock (arrival_wall_time / _kafka_ts verified same epoch).
For each crossing measure the row where each stream first reflects the crossing:
  t_gt        GT reference crosses
  t_det       mdx-bev detection centre crosses (arrival_wall_time)
  t_bapos     mdx-behavior ba_positions crosses (arrival_wall_time)
  t_event     ba_events _kafka_ts
"""
import sys, json, warnings
from pathlib import Path
import numpy as np, pandas as pd
sys.path.insert(0, "/app")
warnings.filterwarnings("ignore")
from srr import aggregator as A

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260615-043909")
roi, tw_x, tw_y_min, tw_y_max = A.load_roi(Path("/app/calibration.json"))
parquets = sorted(RUN.glob("*/scenes/scn_*.parquet"))
HZ, WIN = 30, 90


def st(v):
    if not v:
        return None
    a = np.asarray(v, float)
    return {"median": round(float(np.median(a)), 1), "mean": round(float(a.mean()), 1),
            "p95": round(float(np.percentile(a, 95)), 1), "max": round(float(a.max()), 1),
            "min": round(float(a.min()), 1), "n": len(a)}


def parse(s):
    if isinstance(s, str) and s not in ("", "[]"):
        try:
            return json.loads(s)
        except Exception:
            return []
    return []


def nearest_in(items, gx, gy, gate):
    best, bd = None, gate
    for d in items:
        x, y = d.get("x"), d.get("y")
        if x is None or y is None:
            continue
        dist = ((x - gx) ** 2 + (y - gy) ** 2) ** 0.5
        if dist < bd:
            bd, best = dist, d
    return best


comp = {k: {"det": [], "track": [], "event": [], "total": []} for k in ("twin", "twout", "roi")}
pools = {}

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
    v, df = A.analyze_clip(pq, roi, tw_x, tw_y_min, tw_y_max, ba_events_override=ba_override)
    if len(df) < 5 or "detections_json" not in df.columns:
        continue
    t = df["arrival_wall_time"].to_numpy()
    fk_x = df["forklift_x"].to_numpy(); fk_y = df["forklift_y"].to_numpy()
    dets_row = [parse(s) for s in df["detections_json"]]
    bapos_row = [parse(s) for s in df["ba_positions_json"]] if "ba_positions_json" in df.columns else [[]]*len(df)

    # per-row crossing signals for detection & ba_positions
    det_tr = np.full(len(df), False); det_roi = np.full(len(df), False)
    bap_tr = np.full(len(df), False); bap_roi = np.full(len(df), False)
    for i in range(len(df)):
        dts, bps = dets_row[i], bapos_row[i]
        det_roi[i] = any(d.get("class") == "Person" and A.in_roi(roi, d.get("x"), d.get("y")) for d in dts)
        bap_roi[i] = any(A.in_roi(roi, p.get("x"), p.get("y")) for p in bps if p.get("x") is not None)
        if np.isfinite(fk_x[i]) and np.isfinite(fk_y[i]):
            bd = nearest_in([d for d in dts if d.get("class") == "Forklift"], fk_x[i], fk_y[i], 2.0)
            if bd is not None:
                det_tr[i] = bd["x"] > tw_x and tw_y_min <= bd["y"] <= tw_y_max
            bp = nearest_in(bps, fk_x[i], fk_y[i], 2.0)
            if bp is not None:
                bap_tr[i] = bp["x"] > tw_x and tw_y_min <= bp["y"] <= tw_y_max

    # GT crossing rows
    fk_in = pd.Series(df["forklift_in_trailer"].astype(bool).tolist())
    fk_pv = fk_in.shift(1).astype(object).where(lambda z: z.notna(), fk_in.iloc[0]).astype(bool)
    gt_in = [i for i, x in enumerate((fk_in & ~fk_pv).tolist()) if x and i > 0]
    gt_out = [i for i, x in enumerate((~fk_in & fk_pv).tolist()) if x and i > 0]
    an = pd.Series(df["any_char_in_roi"].astype(bool).tolist())
    an_pv = an.shift(1).astype(object).where(lambda z: z.notna(), an.iloc[0]).astype(bool)
    gt_roi = [i for i, x in enumerate((an & ~an_pv).tolist()) if x and i > 0]

    evts = ba_override or []
    roi_ts = sorted(e["_kafka_ts"] for e in evts if "ROI" in e.get("event_types", []) and "Person" in e.get("classes", []) and e.get("_kafka_ts"))
    twr = sorted(e["_kafka_ts"] for e in evts if "TW" in e.get("event_types", []) and e.get("direction") == "Right" and e.get("_kafka_ts"))
    twl = sorted(e["_kafka_ts"] for e in evts if "TW" in e.get("event_types", []) and e.get("direction") == "Left" and e.get("_kafka_ts"))

    def cross(gi, sig, enter):
        lo, hi = max(1, gi - WIN), min(len(df), gi + WIN)
        for j in range(lo, hi):
            if enter and sig[j] and not sig[j - 1]:
                return j
            if not enter and not sig[j] and sig[j - 1]:
                return j
        return None

    def near(ts, t0):
        c = [x for x in ts if abs(x - t0) <= 3.0]
        return min(c, key=lambda x: abs(x - t0)) if c else None

    def handle(gi, dsig, bsig, ba_ts, key, enter):
        t_gt = t[gi]
        ev = near(ba_ts, t_gt)
        if ev is None:
            return
        comp[key]["total"].append((ev - t_gt) * 1000)
        jd = cross(gi, dsig, enter)
        jb = cross(gi, bsig, enter)
        if jd is not None:
            comp[key]["det"].append((t[jd] - t_gt) * 1000)
            # monotonic chain only (det <= ba_positions <= ba_event, 1-frame tol):
            # drops cases where the windowed ba_positions crossing was mis-picked.
            if jb is not None and t[jb] >= t[jd] - 0.034 and ev >= t[jb] - 0.034:
                comp[key]["track"].append((t[jb] - t[jd]) * 1000)
                comp[key]["event"].append((ev - t[jb]) * 1000)

    for gi in gt_in:
        handle(gi, det_tr, bap_tr, twr, "twin", True)
    for gi in gt_out:
        handle(gi, det_tr, bap_tr, twl, "twout", False)
    for gi in gt_roi:
        handle(gi, det_roi, bap_roi, roi_ts, "roi", True)

out = {"run": RUN.name}
for k in ("twin", "twout", "roi"):
    c = comp[k]
    out[k] = {
        "1_detection GT->mdx-bev (incl transport)": st(c["det"]),
        "2_tracking mdx-bev->ba_positions": st(c["track"]),
        "3_BA-event ba_positions->ba_event": st(c["event"]),
        "total GT->ba_event": st(c["total"]),
    }
print(json.dumps(out, indent=2))
