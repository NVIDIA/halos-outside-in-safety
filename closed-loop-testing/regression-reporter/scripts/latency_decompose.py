#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Decompose GT->BA latency into pipeline components, on the common wall clock.

For each crossing event we measure (all timestamps share one wall clock —
verified: arrival_wall_time, bev_create_time, _kafka_ts are the same epoch):

  t_gt        = arrival_wall_time when GT reference point crosses (ROI/tripwire)
  t_det_seen  = arrival_wall_time when the matched DETECTION centre crosses
  t_det_stamp = bev_create_time at that detection-crossing row (detector clock)
  t_ba        = nearest BA event _kafka_ts

Components:
  detector_staleness   = t_det_seen - t_det_stamp   (detector emit -> SRR saw it)
  geom_detloc          = t_det_seen - t_gt          (box-centre vs GT-origin + det jitter)
  ba_after_det         = t_ba - t_det_seen          (tracker-confirm + edge-rule + Kafka)
  total                = t_ba - t_gt                 (sanity vs earlier ~1 s)
"""
import sys, json, warnings
from pathlib import Path
import numpy as np, pandas as pd
sys.path.insert(0, "/app")
warnings.filterwarnings("ignore")
from srr import aggregator as A

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260615-043909")
roi, tw = A.load_roi(Path("/app/calibration.json"))
parquets = sorted(RUN.glob("*/scenes/scn_*.parquet"))
HZ = 30
WIN = 3 * HZ  # +-3s search window


def st(v):
    if not v:
        return None
    a = np.asarray(v, float)
    return {"median": round(float(np.median(a)), 1), "mean": round(float(a.mean()), 1),
            "p95": round(float(np.percentile(a, 95)), 1), "n": len(a)}


def parse(s):
    if isinstance(s, str) and s not in ("", "[]"):
        try:
            return json.loads(s)
        except Exception:
            return []
    return []


def fk_center_x(dets, gx, gy, gate=2.0):
    best, bd = None, gate
    for d in dets:
        if d.get("class") != "Forklift":
            continue
        dist = ((d["x"] - gx) ** 2 + (d["y"] - gy) ** 2) ** 0.5
        if dist < bd:
            bd, best = dist, d
    return best  # dict or None


def any_person_in_roi(dets):
    for d in dets:
        if d.get("class") == "Person" and A.in_roi(roi, d.get("x"), d.get("y")):
            return True
    return False


# accumulators
stale_all = []
comp = {k: {"geom": [], "ba_after_det": [], "total": [], "stale_at_cross": [],
            "offset_m": [], "n_no_det_cross": 0}
        for k in ("twin", "twout", "roi")}
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
    v, df = A.analyze_clip(pq, roi, tw, ba_events_override=ba_override)
    if len(df) < 5:
        continue
    t = df["arrival_wall_time"].to_numpy()
    bev = (pd.to_numeric(df["bev_create_time"], errors="coerce").to_numpy(dtype=float)
           if "bev_create_time" in df.columns else None)
    dets_row = [parse(s) for s in df["detections_json"]] if "detections_json" in df.columns else None
    if bev is not None:
        d = t - bev
        stale_all += [x for x in d if np.isfinite(x)]
    if dets_row is None:
        continue

    fk_x = df["forklift_x"].to_numpy()
    fk_y = df["forklift_y"].to_numpy()

    # GT crossing series
    fk_in = pd.Series(df["forklift_in_trailer"].astype(bool).tolist())
    fk_prev = fk_in.shift(1).astype(object).where(lambda z: z.notna(), fk_in.iloc[0]).astype(bool)
    gt_in_idx = [i for i, x in enumerate((fk_in & ~fk_prev).tolist()) if x and i > 0]
    gt_out_idx = [i for i, x in enumerate((~fk_in & fk_prev).tolist()) if x and i > 0]
    any_s = pd.Series(df["any_char_in_roi"].astype(bool).tolist())
    any_prev = any_s.shift(1).astype(object).where(lambda z: z.notna(), any_s.iloc[0]).astype(bool)
    gt_roi_idx = [i for i, x in enumerate((any_s & ~any_prev).tolist()) if x and i > 0]

    # detection-side per-row signals
    det_in_trailer = np.full(len(df), False)
    det_cx = np.full(len(df), np.nan)
    det_roi = np.full(len(df), False)
    for i in range(len(df)):
        dets = dets_row[i]
        if not dets:
            continue
        det_roi[i] = any_person_in_roi(dets)
        if np.isfinite(fk_x[i]) and np.isfinite(fk_y[i]):
            best = fk_center_x(dets, fk_x[i], fk_y[i])
            if best is not None:
                det_cx[i] = best["x"]
                det_in_trailer[i] = tw.is_inside(best["x"], best["y"])

    # BA event times
    evts = ba_override if ba_override is not None else []
    roi_ts = sorted(e["_kafka_ts"] for e in evts
                    if "ROI" in e.get("event_types", []) and "Person" in e.get("classes", [])
                    and e.get("_kafka_ts"))
    twr_ts = sorted(e["_kafka_ts"] for e in evts if "TW" in e.get("event_types", [])
                    and "Forklift" in e.get("classes", []) and e.get("direction") == "Right"
                    and e.get("_kafka_ts"))
    twl_ts = sorted(e["_kafka_ts"] for e in evts if "TW" in e.get("event_types", [])
                    and "Forklift" in e.get("classes", []) and e.get("direction") == "Left"
                    and e.get("_kafka_ts"))

    def nearest(ts_list, t0):
        c = [x for x in ts_list if abs(x - t0) <= 3.0]
        return min(c, key=lambda x: abs(x - t0)) if c else None

    def det_cross_idx(gi, want_enter, sig):
        """find det-signal transition near GT crossing row gi within +-WIN."""
        lo, hi = max(1, gi - WIN), min(len(df), gi + WIN)
        for j in range(lo, hi):
            if want_enter and sig[j] and not sig[j - 1]:
                return j
            if not want_enter and not sig[j] and sig[j - 1]:
                return j
        return None

    def handle(gi, sig, ba_list, key, enter, fk=True):
        t_gt = t[gi]
        j = det_cross_idx(gi, enter, sig)
        t_ba = nearest(ba_list, t_gt)
        if t_ba is None:
            return
        comp[key]["total"].append((t_ba - t_gt) * 1000)
        if j is None:
            comp[key]["n_no_det_cross"] += 1
            return
        t_det_seen = t[j]
        comp[key]["geom"].append((t_det_seen - t_gt) * 1000)
        comp[key]["ba_after_det"].append((t_ba - t_det_seen) * 1000)
        if bev is not None and np.isfinite(bev[j]):
            comp[key]["stale_at_cross"].append((t_det_seen - bev[j]) * 1000)
        if fk and np.isfinite(det_cx[j]) and np.isfinite(fk_x[gi]):
            comp[key]["offset_m"].append(det_cx[j] - fk_x[gi])

    for gi in gt_in_idx:
        handle(gi, det_in_trailer, twr_ts, "twin", enter=True)
    for gi in gt_out_idx:
        handle(gi, det_in_trailer, twl_ts, "twout", enter=False)
    for gi in gt_roi_idx:
        handle(gi, det_roi, roi_ts, "roi", enter=True, fk=False)

out = {
    "run": RUN.name, "clips": len(parquets),
    "detector_staleness_ms (arrival - bev_create)": st([x * 1000 for x in stale_all]),
    "components_ms": {},
}
for k in ("twin", "twout", "roi"):
    c = comp[k]
    out["components_ms"][k] = {
        "total (t_ba - t_gt)": st(c["total"]),
        "geom_detloc (t_det_seen - t_gt)": st(c["geom"]),
        "detector_staleness_at_cross (t_det_seen - t_det_stamp)": st(c["stale_at_cross"]),
        "ba_after_det (t_ba - t_det_seen)": st(c["ba_after_det"]),
        "det_center_minus_gt_x_m": st(c["offset_m"]) if c["offset_m"] else None,
        "n_no_det_crossing_found": c["n_no_det_cross"],
    }
print(json.dumps(out, indent=2))
