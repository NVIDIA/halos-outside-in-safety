# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
SRR Phase 1 aggregator — reads runs/*.parquet, computes per-clip verdict,
emits reports/<basename>.md + reports/summary.md + reports/failures.json.

Run inside the SRR container:
    docker exec srr python3 -m srr.aggregator

ROI source: /app/calibration.json (mounted from VSS calibration sample-data)
or fall back to hardcoded bounds from 01-prerequisites.md.
"""
from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Optional

import pandas as pd
from shapely.geometry import MultiPoint, Point, Polygon

from srr.tripwire import Tripwire


# ---------- ROI / tripwire defaults (from VSS calibration sample-data) ----------

DEFAULT_ROI_VERTICES = [
    (4.87747667, -18.9756077),
    (4.87747667, -11.2385153),
    (9.57440535, -11.2385153),
    (9.57440535, -18.9756077),
]
DEFAULT_TW_X = 9.57440535
DEFAULT_TW_Y_MIN = -18.9756077
DEFAULT_TW_Y_MAX = -11.2385153


def load_roi(calib_path: Optional[Path]) -> tuple[Polygon, Tripwire]:
    """Returns (roi_polygon, tripwire).

    The tripwire's inside side comes from the calibration's ``direction``
    field (arrow head = inside/trailer side) — see srr.tripwire.
    """
    if calib_path and calib_path.exists():
        d = json.loads(calib_path.read_text())
        # all sensors share the same ROI/TW in this scene; take first
        s0 = d["sensors"][0]
        coords = [(c["x"], c["y"]) for c in s0["rois"][0]["roiCoordinates"]]
        return (Polygon(coords), Tripwire.from_calib_dict(s0["tripwires"][0]))
    return (
        Polygon(DEFAULT_ROI_VERTICES),
        Tripwire.legacy(DEFAULT_TW_X, DEFAULT_TW_Y_MIN, DEFAULT_TW_Y_MAX),
    )


# ---------- Verdict math ----------

CHAR_COLS = ["char_0", "char_1", "char_2"]
PASS_THRESHOLD = 95.0
NEAR_THRESHOLD = 80.0
MIN_WINDOW_S = 0.5  # ignore single-frame flickers
MAX_MATCH_DELAY_S = 3.0  # BA event must lie within ±this window of GT (BA may fire before/after)
MAX_TRAILING_S = 5.0     # for ROI exit, look for trailing BA events up to this far after GT exit
BA_POOL_PAD_S = 5.0      # when pooling BA events from run-level parquet, pad scene boundary by this
GHOST_RESULT_BA_THRESHOLD = 0.50  # min fraction of GT events BA must match; below = perception likely dead
# Coverage hull below this area (m²) is treated as degenerate: detections
# collapsed onto ~1 line/point (typically only one actor ever visible because
# the others stayed static/out-of-FOV), so no GT frame can be "in coverage" and
# recall comes out None. That reads like a dead pipeline even though detections
# are flowing — flag it explicitly instead. A meaningful multi-actor footprint
# is tens of m²; a single collinear track is ~0.
MIN_COVERAGE_AREA_M2 = 1.0


def classify_verdict(match_rate: float) -> str:
    if match_rate >= PASS_THRESHOLD:
        return "PASS"
    if match_rate >= NEAR_THRESHOLD:
        return "NEAR"
    return "FAIL"


@dataclass
class MismatchWindow:
    start_s: float       # seconds since clip start
    end_s: float
    duration_s: float
    direction: str       # "over-mute" | "under-mute"


@dataclass
class BAEventMatch:
    """One GT event ↔ first BA event within match window (or no match)."""
    gt_t_s: float                 # GT event time relative to clip start
    ba_t_s: Optional[float]       # BA event time relative to clip start (None if no match)
    delay_ms: Optional[float]     # ba_t - gt_t in ms (None if no match)
    matched: bool                 # True iff a BA event was found within MAX_MATCH_DELAY_S
    note: str = ""                # extra info, e.g. "no-direction"


@dataclass
class BAMatchSet:
    """Aggregate match stats for one event class (e.g. ROI entries)."""
    gt_count: int = 0
    matched: int = 0
    matches: list[BAEventMatch] = field(default_factory=list)
    median_delay_ms: Optional[float] = None
    p95_delay_ms: Optional[float] = None
    max_delay_ms: Optional[float] = None
    detect_pct: float = 0.0       # 100 * matched / gt_count
    extra_ba_count: int = 0       # BA events in window with no GT (informational)


@dataclass
class ClipVerdict:
    file: str
    run_label: str       # parent run dir, e.g. "in-roi-5min"
    scn_id: str          # e.g. "scn_0004"
    iso_start: str       # clip start UTC ISO
    iso_end: str
    forklift_side: str   # "in_trailer" | "outside_trailer" | "" if manifest missing
    duration_s: float
    rows: int
    sample_hz: float

    # ground truth
    pct_any_char_in_roi: float
    pct_forklift_in_trailer: float
    pct_expected_mute: float

    # observed
    pct_actual_mute: float
    pct_no_op: float
    pct_unmute_alarm: float

    # match (per-frame: expected_mute == actual_is_muted)
    match_rate: float
    n_mismatch_frames: int
    verdict: str  # PASS / NEAR / FAIL

    # mismatch direction breakdown
    pct_over_mute: float        # actual=True, expected=False (suppressed when shouldn't)
    pct_under_mute: float       # actual=False, expected=True (failed to suppress when should)
    n_over_mute_frames: int
    n_under_mute_frames: int
    n_expected_mute_frames: int     # total frames where GT says SHOULD mute
    n_expected_unmute_frames: int   # total frames where GT says SHOULD NOT mute

    # mismatch windows (≥0.5s contiguous), times relative to clip start (seconds)
    mismatch_windows: list[MismatchWindow] = field(default_factory=list)

    # reaction lags (ms). Each = first time gt transition X is reflected in PSF state.
    median_reaction_unmute_ms: Optional[float] = None
    p95_reaction_unmute_ms: Optional[float] = None
    max_reaction_unmute_ms: Optional[float] = None
    median_reaction_mute_ms: Optional[float] = None
    p95_reaction_mute_ms: Optional[float] = None
    max_reaction_mute_ms: Optional[float] = None
    n_unmute_transitions: int = 0
    n_mute_transitions: int = 0
    # True iff GT required mute (resp. unmute) at some point but the system NEVER
    # reached that state for ANY frame that needed it → reaction lag = ∞. These
    # clips have no transition to time, so a None lag would otherwise render as
    # "–" (looks benign) and the worst case (never reacted) gets hidden.
    mute_never_engaged: bool = False
    unmute_never_engaged: bool = False

    # BA events (raw counts)
    n_ba_events: int = 0
    n_ba_roi_person: int = 0       # raw count (over-fires 3-9× due to per-sensor + sliding-window emission)
    n_ba_tw_forklift: int = 0      # raw total = n_ba_tw_right + n_ba_tw_left

    # BA events (deduplicated / direction-split — for fair comparison with GT)
    n_ba_roi_person_dedup: int = 0 # cluster events within 1s sliding window across 3 sensors
    n_ba_tw_right: int = 0         # forklift entered trailer (W→E direction)
    n_ba_tw_left: int = 0          # forklift exited trailer  (E→W direction)

    # GT-derived event counts (for direct BA-vs-GT comparison)
    gt_roi_entries: int = 0
    gt_tw_entries: int = 0
    gt_tw_exits: int = 0

    # BA event matching (per-event timing analysis vs GT)
    # NOTE: BA emits ROI events while person INSIDE (no exit event). For ROI we only match entries
    #       and report a "trailing fire" delay (how long BA continues firing after GT exit).
    ba_roi_entry_match: BAMatchSet = field(default_factory=BAMatchSet)
    ba_tw_in_match: BAMatchSet = field(default_factory=BAMatchSet)
    ba_tw_out_match: BAMatchSet = field(default_factory=BAMatchSet)
    ba_tw_no_direction: int = 0   # TW Forklift events missing the `direction` field
    # ROI exit "trailing fire" — for each GT exit, (exit_t_s, trailing_ms).
    # trailing_ms = how long after the GT exit BA was still emitting (within MAX_TRAILING_S window).
    roi_trailing_pairs: list[tuple[float, float]] = field(default_factory=list)
    median_roi_trailing_ms: Optional[float] = None
    max_roi_trailing_ms: Optional[float] = None

    # video pointer (relative to clip's report file)
    video_path: str = ""

    # --- Phase 2 (3D perception metrics from mdx-bev). None for Phase 1 clips. ---
    phase2: Optional[dict] = None

    # diagnostic note (e.g. "trimmed N cold-start frames")
    note: str = ""

    # --- validity guards (additive; defaults keep legacy output unchanged) ---
    # % frames carrying a real /safety/is_muted observation. Frames recorded
    # before any message arrives hold None and are scored as UNMUTED — 100%
    # means every frame had real PSF data.
    pct_psf_data: float = 100.0
    # Actors whose GT position never changed across the whole clip while BA
    # simultaneously observed tripwire crossings (the known /gt TF freeze) —
    # expected_mute is untrustworthy for such clips.
    gt_frozen_actors: list[str] = field(default_factory=list)


def in_roi(roi: Polygon, x: Optional[float], y: Optional[float]) -> bool:
    if x is None or y is None or pd.isna(x) or pd.isna(y):
        return False
    return roi.covers(Point(float(x), float(y)))


def find_mismatch_windows(over: list[bool], under: list[bool],
                          times: list[float], t0: float,
                          min_dur_s: float = MIN_WINDOW_S) -> list[MismatchWindow]:
    """Find contiguous windows where any mismatch is active, classify by dominant direction."""
    n = len(over)
    windows: list[MismatchWindow] = []
    i = 0
    while i < n:
        if not (over[i] or under[i]):
            i += 1
            continue
        start = i
        while i < n and (over[i] or under[i]):
            i += 1
        end = i - 1  # inclusive
        dur = times[end] - times[start]
        if dur < min_dur_s:
            continue
        n_over = sum(1 for k in range(start, end + 1) if over[k])
        n_under = sum(1 for k in range(start, end + 1) if under[k])
        direction = "over-mute" if n_over >= n_under else "under-mute"
        windows.append(MismatchWindow(
            start_s=round(times[start] - t0, 2),
            end_s=round(times[end] - t0, 2),
            duration_s=round(dur, 2),
            direction=direction,
        ))
    return windows


def percentile(values: list[float], q: float) -> Optional[float]:
    if not values:
        return None
    s = pd.Series(values)
    return round(float(s.quantile(q / 100.0)), 1)


def match_first_ba(gt_times: list[float], ba_times: list[float],
                   t0: float, max_delay: float = MAX_MATCH_DELAY_S) -> BAMatchSet:
    """For each GT event, find the BA event closest in time within ±max_delay seconds.
    BA may fire BEFORE the GT moment (e.g. tripwire detected via bbox-edge crossing
    before the GT-defined centroid crosses). Greedy: each BA event matches at most one GT.
    Delay is SIGNED: positive = BA late, negative = BA early."""
    ba_sorted = sorted(ba_times)
    used = [False] * len(ba_sorted)
    matches: list[BAEventMatch] = []
    for gt in sorted(gt_times):
        best_j = None
        best_abs = None
        for j, ba in enumerate(ba_sorted):
            if used[j]:
                continue
            d = ba - gt
            if abs(d) > max_delay:
                continue
            if best_abs is None or abs(d) < best_abs:
                best_abs = abs(d)
                best_j = j
        if best_j is not None:
            ba = ba_sorted[best_j]
            d = ba - gt
            used[best_j] = True
            matches.append(BAEventMatch(
                gt_t_s=round(gt - t0, 2),
                ba_t_s=round(ba - t0, 2),
                delay_ms=round(d * 1000.0, 1),
                matched=True,
            ))
        else:
            matches.append(BAEventMatch(
                gt_t_s=round(gt - t0, 2),
                ba_t_s=None,
                delay_ms=None,
                matched=False,
            ))
    delays_signed = [m.delay_ms for m in matches if m.matched]
    delays_abs = [abs(d) for d in delays_signed]
    extra = len(ba_sorted) - sum(1 for u in used if u)
    n_matched = sum(1 for m in matches if m.matched)
    n_gt = len(matches)
    return BAMatchSet(
        gt_count=n_gt,
        matched=n_matched,
        matches=matches,
        median_delay_ms=percentile(delays_abs, 50),
        p95_delay_ms=percentile(delays_abs, 95),
        max_delay_ms=round(max(delays_abs), 1) if delays_abs else None,
        detect_pct=round(100 * n_matched / n_gt, 1) if n_gt else 0.0,
        extra_ba_count=extra,
    )


def compute_roi_trailing(gt_exit_times: list[float], ba_roi_times: list[float],
                         t0: float, max_trailing: float = MAX_TRAILING_S) -> list[tuple[float, float]]:
    """For each GT ROI exit (clip-relative), return (exit_t_s, trailing_ms): how long after
    the exit BA was still firing, within `max_trailing` seconds. 0 if BA stopped before exit."""
    ba_sorted = sorted(ba_roi_times)
    trailing = []
    for gt_exit in gt_exit_times:
        last_after = None
        for ba in ba_sorted:
            d = ba - gt_exit
            if d < 0:
                continue
            if d > max_trailing:
                break
            last_after = ba
        trail_ms = round((last_after - gt_exit) * 1000.0, 1) if last_after is not None else 0.0
        trailing.append((round(gt_exit - t0, 2), trail_ms))
    return trailing


def find_video_path(parquet_path: Path) -> str:
    """Look for a clip MP4 next to the parquet (../videos/<stem>.mp4 relative to reports/)."""
    scn_id = parquet_path.stem
    # canonical layout: <run>/videos/<scn>.mp4 — report lives in <run>/reports/
    run_dir = parquet_path.parent.parent  # parquet is in <run>/scenes/
    candidate = run_dir / "videos" / f"{scn_id}.mp4"
    if candidate.exists():
        return f"../videos/{scn_id}.mp4"
    return ""


def load_manifest(parquet_path: Path) -> dict:
    """Return manifest row for this scn_id, if scenes_manifest.csv exists nearby."""
    scn_id = parquet_path.stem
    manifest = parquet_path.parent / "scenes_manifest.csv"
    if not manifest.exists():
        return {}
    with manifest.open() as f:
        for row in csv.DictReader(f):
            if row.get("scenario_id") == scn_id:
                return row
    return {}


def load_run_ba_pool(run_dir: Path) -> list[dict]:
    """Read run-*.parquet (the unsplit full-run capture) and return all BA events sorted by ts.
    Used to avoid scene-boundary spillover: BA events that fire ±2s offset from GT crossings
    often land in the neighboring scene, causing false negatives at scene-level matching."""
    run_pq = sorted(run_dir.glob("run-*.parquet"))
    if not run_pq:
        return []
    df = pd.read_parquet(run_pq[0])
    pool: list[dict] = []
    for s in df["ba_events_json"]:
        pool.extend(json.loads(s))
    pool.sort(key=lambda e: e.get("_kafka_ts", 0.0))
    return pool


def filter_ba_pool(pool: list[dict], t_start: float, t_end: float,
                   pad: float = BA_POOL_PAD_S) -> list[dict]:
    """Return BA events whose _kafka_ts falls within [t_start - pad, t_end + pad]."""
    if not pool:
        return []
    lo = t_start - pad
    hi = t_end + pad
    return [e for e in pool
            if e.get("_kafka_ts") is not None and lo <= e["_kafka_ts"] <= hi]


# Phase 2: which GT actor maps to which detector class.
_GT_ACTOR_CLASS = {
    "char_0": "Person", "char_1": "Person", "char_2": "Person",
    "forklift": "Forklift",
}


def _estimate_forklift_origin_offset(rows: list, gate_m: float,
                                     actor: str = "forklift",
                                     min_speed: float = 0.03,
                                     min_samples: int = 40):
    """Estimate the forklift GT-origin→box-centre offset; return a per-frame
    corrected ``{bev_frame_id: (x, y)}`` map.

    The forklift GT is the Isaac ``body`` TF frame, which sits off the detected
    3D-box centre → a ~0.35 m bias that **rotates with the truck**, so a fixed
    world-frame shift is wrong. GT has no orientation, so we infer heading from
    the GT velocity (central difference), project each matched ``det − gt``
    residual into that heading frame, and take the median (longitudinal,
    lateral) offset — random localization noise averages out across the varied
    headings, leaving the systematic body-vs-centre bias. We then add that
    offset back (rotated by the per-frame heading) to move GT onto the box
    centre. Returns ``({fid: (x, y)}, (long, lat), n_samples)`` or
    ``({}, None, n)`` when there is not enough moving + matched data.
    """
    import math
    import statistics

    seen: set = set()
    series: list = []  # (fid, gx, gy, dets)
    for r, dets in rows:
        fid = r.get("bev_frame_id")
        if fid in seen:
            continue
        seen.add(fid)
        gx, gy = r.get(f"{actor}_x"), r.get(f"{actor}_y")
        if pd.isna(gx) or pd.isna(gy):
            continue
        series.append((fid, float(gx), float(gy), dets))
    if len(series) < 3:
        return {}, None, 0

    headings: list = [None] * len(series)
    for i in range(len(series)):
        j0, j1 = max(0, i - 1), min(len(series) - 1, i + 1)
        dx = series[j1][1] - series[j0][1]
        dy = series[j1][2] - series[j0][2]
        if (dx * dx + dy * dy) ** 0.5 >= min_speed:
            headings[i] = math.atan2(dy, dx)

    longs: list = []
    lats: list = []
    for i, (fid, gx, gy, dets) in enumerate(series):
        h = headings[i]
        if h is None:
            continue
        best, best_d = None, gate_m
        for d in dets:
            if d.get("class") != "Forklift":
                continue
            dist = ((d["x"] - gx) ** 2 + (d["y"] - gy) ** 2) ** 0.5
            if dist < best_d:
                best_d, best = dist, d
        if best is None:
            continue
        rx, ry = best["x"] - gx, best["y"] - gy
        ch, sh = math.cos(-h), math.sin(-h)
        longs.append(ch * rx - sh * ry)
        lats.append(sh * rx + ch * ry)

    if len(longs) < min_samples:
        return {}, None, len(longs)

    off_long = statistics.median(longs)
    off_lat = statistics.median(lats)
    corr: dict = {}
    last_h = None
    for i, (fid, gx, gy, dets) in enumerate(series):
        h = headings[i] if headings[i] is not None else last_h
        if headings[i] is not None:
            last_h = headings[i]
        if h is None:
            corr[fid] = (gx, gy)
            continue
        ch, sh = math.cos(h), math.sin(h)
        corr[fid] = (gx + ch * off_long - sh * off_lat,
                     gy + sh * off_long + ch * off_lat)
    return corr, (round(off_long, 3), round(off_lat, 3)), len(longs)


def compute_phase2_metrics(df: pd.DataFrame, gate_m: float = 1.5,
                           coverage_pad_m: float = 0.0,
                           tw: Optional[Tripwire] = None,
                           boundary_m: float = 2.0,
                           split_dist_m: float = 1.0) -> Optional[dict]:
    """Per-clip 3D perception metrics from the recorded mdx-bev detections.

    The 30 Hz sampler over-stamps each detector frame onto several rows, so we
    dedup by ``bev_frame_id`` before counting. When GT poses are present
    (scene has GT publishers) we also match detections → GT per frame (greedy
    nearest within ``gate_m``, per class) to get detection precision/recall,
    position MAE, and per-actor tracker id-switch counts. Without GT we still
    emit detection counts + unique track ids (GT-free).

    **Coverage gating**: detections only appear where the sensors can see, so a
    GT actor standing outside the camera/BEV footprint is *not* a perception
    miss — counting it as a false negative makes recall meaningless. We derive
    the coverage region empirically as the convex hull of every detection in the
    clip. ``coverage_pad_m`` defaults to **0** (2026-06-15): a positive pad
    inflated the hull outward into regions the cameras never actually saw, so
    out-of-FOV GT frames were wrongly counted as detection failures and
    detect-fail % was over-reported (verified with the FOV-masked heatmap —
    person detect-fail dropped 33.9%→4.6% inside the true camera FOV). The
    *correct* long-term region is the per-camera ``fieldOfViewPolygon`` union
    from ``calibration.json`` (a backlog item); pad=0 is the interim fix that keeps
    the empirical hull but stops it bleeding past the detection footprint. GT
    actors outside the region are excluded from recall and reported separately
    (``out_of_coverage``). Headline ``recall`` is in-coverage; ``recall_raw``
    keeps the unfiltered value for transparency.

    Returns None for Phase 1 parquets (no ``detections_json`` column).
    """
    import numpy as _np
    from collections import Counter

    if "detections_json" not in df.columns:
        return None

    rows: list[tuple] = []
    for _, r in df.iterrows():
        dj = r.get("detections_json")
        if not isinstance(dj, str):
            continue
        try:
            dets = json.loads(dj)
        except Exception:
            continue
        rows.append((r, dets))
    if not rows:
        return {"enabled": False, "reason": "no mdx-bev detections recorded"}

    # Dedup to unique detector frames for count stats.
    uniq: dict = {}
    for r, dets in rows:
        fid = r.get("bev_frame_id")
        uniq.setdefault(fid, dets)
    det_counts = [len(d) for d in uniq.values()]
    cls_counter: Counter = Counter()
    all_tids: set = set()
    for dets in uniq.values():
        for d in dets:
            cls_counter[d["class"]] += 1
            all_tids.add(d["track_id"])

    gt_cols = [f"{a}_x" for a in _GT_ACTOR_CLASS]
    has_gt = bool(df[[c for c in gt_cols if c in df.columns]].notna().any().any())

    out: dict = {
        "enabled": True,
        "frames_sampled_with_dets": len(rows),
        "unique_detector_frames": len(uniq),
        "mean_dets_per_frame": round(float(_np.mean(det_counts)), 2),
        "class_counts": dict(cls_counter),
        "unique_track_ids": len(all_tids),
        "gt_available": has_gt,
    }
    if not has_gt:
        out["note"] = ("GT poses absent (scene without GT publishers) — "
                       "precision/recall/MAE skipped; emitting counts + "
                       "unique track ids only")
        return out

    # Empirical coverage region = CONVEX HULL of every detection in the clip,
    # padded by coverage_pad_m (default 0 since 2026-06-15). A non-zero pad
    # bled the hull out past the camera footprint so out-of-FOV GT was wrongly
    # judged "in coverage" → false miss → inflated detect-fail %. pad=0 keeps
    # the region to where detections actually land.
    #
    # NOTE (limitation): this is still *detection-derived*, so a region the
    # camera sees but where nothing was ever detected (e.g. forklift fully LOST
    # deep in the trailer) can fall outside the hull and be excluded. A true
    # FOV-based region would need the camera ground footprint clipped by scene
    # occlusion geometry (walls/racks) — the calibration has camera matrices but
    # not the floor/occlusion mesh, so horizon pixels project to infinity and a
    # naive footprint is unusable. Tracked as a backlog item; cross-check
    # recall_raw + out-of-coverage until then.
    all_x = [d["x"] for dets in uniq.values() for d in dets]
    all_y = [d["y"] for dets in uniq.values() for d in dets]
    cov = None
    cov_poly = None
    if all_x:
        cov = (min(all_x) - coverage_pad_m, max(all_x) + coverage_pad_m,
               min(all_y) - coverage_pad_m, max(all_y) + coverage_pad_m)
        try:
            hull = MultiPoint(list(zip(all_x, all_y))).convex_hull
            cov_poly = hull.buffer(coverage_pad_m)
        except Exception:
            cov_poly = None

    # Degeneracy guard — see MIN_COVERAGE_AREA_M2. When every detection lands on
    # ~one line/point the hull area collapses toward 0; recall then has 0
    # in-coverage GT frames and returns None, which looks like a dead pipeline
    # even though detections were recorded. Surface it as a distinct, explained
    # state instead of a silent None.
    coverage_degenerate = False
    coverage_degenerate_reason = None
    if all_x and (cov_poly is None
                  or getattr(cov_poly, "geom_type", "") != "Polygon"
                  or cov_poly.area < MIN_COVERAGE_AREA_M2):
        coverage_degenerate = True
        _area = round(cov_poly.area, 3) if cov_poly is not None else 0.0
        coverage_degenerate_reason = (
            f"detection coverage hull is degenerate (area {_area} m² < "
            f"{MIN_COVERAGE_AREA_M2} m²): the {len(all_x)} recorded detections "
            f"collapsed onto ~1 line/point — typically only one actor was ever "
            f"visible (the others stayed static or out of camera FOV). recall is "
            f"unscoreable (0 in-coverage GT frames), which is a COVERAGE/scene "
            f"issue, NOT a perception failure — detections were flowing."
        )

    def _in_cov(x: float, y: float) -> bool:
        if cov_poly is not None:
            return cov_poly.covers(Point(x, y))
        return cov is None or (cov[0] <= x <= cov[1] and cov[2] <= y <= cov[3])

    # Forklift GT-origin → box-centre correction (the `body` TF anchor sits off
    # the detected box centre by ~0.35 m, rotating with the truck). Estimate it
    # data-driven (no recorded orientation) and shift forklift GT onto the box
    # centre before matching, so the residual is true jitter not a fixed anchor
    # bias. Leaves persons untouched.
    fk_corr, fk_off, fk_off_n = _estimate_forklift_origin_offset(rows, gate_m)

    # GT present → per-frame detection ↔ GT matching (greedy nearest per class).
    tp_in = tp_ooc = fp = fn_in = fn_ooc = 0
    pos_err: list[float] = []
    actor_tid_seq: dict = {a: [] for a in _GT_ACTOR_CLASS}
    actor_stats: dict = {a: {"present": 0, "in_cov": 0, "matched": 0,
                             "missed_in_cov": 0, "out_cov": 0} for a in _GT_ACTOR_CLASS}
    out_cov_xy: dict = {a: [] for a in _GT_ACTOR_CLASS}
    # Per-class breakdown — headline recall blends Person + Forklift and hides
    # that the safety-critical forklift is the worst detected object. Track each
    # class separately. cls_pos[cls] = matched-pair distances (localization
    # quality); cls_incov_dist[cls] = in-coverage matched distances (drives
    # multi-gate recall); cls_incov_present[cls] = in-coverage GT frames (recall
    # denominator). pos median ≈ systematic anchor offset (GT origin vs det box
    # centre), p95/std ≈ jitter — reported apart so a constant offset (forklift
    # ~0.35 m) is not mis-read as detection error.
    classes = sorted(set(_GT_ACTOR_CLASS.values()))
    cls_tp_in = {c: 0 for c in classes}
    cls_fp = {c: 0 for c in classes}
    cls_fn_in = {c: 0 for c in classes}
    cls_pos: dict = {c: [] for c in classes}
    cls_incov_dist: dict = {c: [] for c in classes}
    cls_incov_present = {c: 0 for c in classes}
    # Per-actor in-coverage matched distances → per-actor multi-gate recall.
    actor_incov_dist: dict = {a: [] for a in _GT_ACTOR_CLASS}
    if tw is None:
        tw = Tripwire.legacy(DEFAULT_TW_X, DEFAULT_TW_Y_MIN, DEFAULT_TW_Y_MAX)
    # Trailer-boundary slice for the forklift: frames where the forklift GT
    # sits within ±boundary_m of the tripwire (perpendicular distance to the
    # wire line — was abs(x - tw_x) for the vertical wire). This isolates the
    # previously-observed ~33% forklift detection loss around the trailer boundary.
    fk_boundary_present = 0
    fk_boundary_missed = 0
    seen: set = set()
    frames_scored = 0
    frames_skipped_no_gt = 0
    for r, dets in rows:
        fid = r.get("bev_frame_id")
        if fid in seen:
            continue
        seen.add(fid)
        gts: dict = {}
        for a in _GT_ACTOR_CLASS:
            x, y = r.get(f"{a}_x"), r.get(f"{a}_y")
            if pd.notna(x) and pd.notna(y):
                if a == "forklift" and fid in fk_corr:
                    gts[a] = fk_corr[fid]  # origin→centre corrected
                else:
                    gts[a] = (float(x), float(y))
        if not gts:
            # No GT for this detector frame (cold-start / TF gap). Skip rather
            # than counting detections as false positives — absence of GT is a
            # recording gap, not a perception error.
            frames_skipped_no_gt += 1
            continue
        frames_scored += 1
        used: set = set()
        for a, (gx, gy) in gts.items():
            actor_stats[a]["present"] += 1
            cls = _GT_ACTOR_CLASS[a]
            best, best_d = None, gate_m
            for i, d in enumerate(dets):
                if i in used or d["class"] != cls:
                    continue
                dist = ((d["x"] - gx) ** 2 + (d["y"] - gy) ** 2) ** 0.5
                if dist < best_d:
                    best_d, best = dist, i
            matched = best is not None
            if matched:
                used.add(best)
                pos_err.append(best_d)
                cls_pos[cls].append(best_d)
                actor_tid_seq[a].append(dets[best]["track_id"])
                actor_stats[a]["matched"] += 1
            in_cov = _in_cov(gx, gy)
            if in_cov:
                actor_stats[a]["in_cov"] += 1
                cls_incov_present[cls] += 1
                if matched:
                    tp_in += 1
                    cls_tp_in[cls] += 1
                    cls_incov_dist[cls].append(best_d)
                    actor_incov_dist[a].append(best_d)
                else:
                    fn_in += 1
                    cls_fn_in[cls] += 1
                    actor_stats[a]["missed_in_cov"] += 1
                if a == "forklift" and tw.line_distance(gx, gy) <= boundary_m:
                    fk_boundary_present += 1
                    if not matched:
                        fk_boundary_missed += 1
            else:
                # Out-of-coverage GT: a miss here is a coverage gap, not an FN.
                actor_stats[a]["out_cov"] += 1
                out_cov_xy[a].append((round(gx, 2), round(gy, 2)))
                if matched:
                    tp_ooc += 1
                else:
                    fn_ooc += 1
        for i, d in enumerate(dets):
            if i not in used:
                fp += 1
                if d["class"] in cls_fp:
                    cls_fp[d["class"]] += 1

    tp_all = tp_in + tp_ooc
    prec = tp_all / (tp_all + fp) if (tp_all + fp) else None      # all detections
    rec = tp_in / (tp_in + fn_in) if (tp_in + fn_in) else None    # in-coverage
    rec_raw = tp_all / (tp_all + fn_in + fn_ooc) if (tp_all + fn_in + fn_ooc) else None
    f1 = (2 * prec * rec / (prec + rec)) if (prec and rec) else None
    id_switches: dict = {}
    # tracking-correct (id-consistency): of an actor's matched frames, the
    # fraction that sit on its DOMINANT (majority) track-id. Any frame on a
    # different id = an id-switch/fragmentation event → counted as tracked-wrong.
    # This is the single tracking-correctness figure: detection is reported as
    # detect-fail%, tracking as tracking-loss% (= 100 - this). Aggregated per
    # class + overall below.
    cls_track_ok = {c: 0 for c in classes}
    cls_track_frames = {c: 0 for c in classes}
    for a, seq in actor_tid_seq.items():
        sw = sum(1 for i in range(1, len(seq)) if seq[i] != seq[i - 1])
        # id-switch rate = switches per matched frame (normalised so clips of
        # different length compare). Raw count alone hides that a short clip with
        # 3 switches is worse than a long one with 5.
        rate = round(sw / len(seq), 4) if seq else None
        dom = max(set(seq), key=seq.count) if seq else None
        on_dom = sum(1 for t in seq if t == dom) if seq else 0
        id_consistency = round(100 * on_dom / len(seq), 2) if seq else None
        id_switches[a] = {"track_ids": sorted(set(seq)), "switches": sw,
                          "frames": len(seq), "switch_rate": rate,
                          "dominant_id": dom, "id_consistency_pct": id_consistency}
        cls = _GT_ACTOR_CLASS.get(a)
        if cls in cls_track_ok:
            cls_track_ok[cls] += on_dom
            cls_track_frames[cls] += len(seq)
    tracking_ok_by_class = {
        c: round(100 * cls_track_ok[c] / cls_track_frames[c], 2) if cls_track_frames[c] else None
        for c in classes
    }
    _tot_ok = sum(cls_track_ok.values())
    _tot_fr = sum(cls_track_frames.values())
    tracking_ok_pct = round(100 * _tot_ok / _tot_fr, 2) if _tot_fr else None

    def _stat(xs):
        if not xs:
            return {"n": 0, "median": None, "mean": None, "p95": None, "std": None}
        arr = _np.array(xs)
        return {"n": len(xs),
                "median": round(float(_np.median(arr)), 4),
                "mean": round(float(arr.mean()), 4),
                "p95": round(float(_np.percentile(arr, 95)), 4),
                "std": round(float(arr.std()), 4)}

    GATES = (0.5, 1.0, gate_m)

    def _recall_at(dists, present):
        # recall@g = fraction of in-coverage GT frames matched within g metres.
        if not present:
            return {g: None for g in GATES}
        return {g: round(sum(1 for d in dists if d <= g) / present, 4) for g in GATES}

    # Per-class detection + localization (Person vs Forklift separately).
    per_class: dict = {}
    for c in classes:
        tp = cls_tp_in[c]; fpc = cls_fp[c]; fnc = cls_fn_in[c]
        present = cls_incov_present[c]
        per_class[c] = {
            "tp_in": tp, "fp": fpc, "fn_in": fnc,
            "in_cov_present": present,
            "precision": round(tp / (tp + fpc), 4) if (tp + fpc) else None,
            "recall": round(tp / (tp + fnc), 4) if (tp + fnc) else None,
            # track-loss % = in-coverage frames where this class had NO matched
            # detection. Complement of recall; this is the track-loss figure.
            "track_loss_pct": round(100 * fnc / present, 2) if present else None,
            "pos": _stat(cls_pos[c]),          # localization: median≈offset, p95≈jitter
            "recall_at_gate": _recall_at(cls_incov_dist[c], present),
            "tracking_ok_pct": tracking_ok_by_class.get(c),  # % matched frames on dominant id
            "track_ok_n": cls_track_ok[c],         # raw counts so multi-clip rollups
            "track_frames_n": cls_track_frames[c],  # aggregate cleanly
        }

    # Per-actor track-loss + recall (extends actor_stats in place).
    for a, s in actor_stats.items():
        icv = s["in_cov"]
        s["recall_in_cov"] = round((icv - s["missed_in_cov"]) / icv, 4) if icv else None
        s["track_loss_pct"] = round(100 * s["missed_in_cov"] / icv, 2) if icv else None
        s["out_cov_pct"] = round(100 * s["out_cov"] / s["present"], 2) if s["present"] else None

    # Overall multi-gate recall (all classes, in-coverage).
    all_incov_dist = [d for c in classes for d in cls_incov_dist[c]]
    all_incov_present = sum(cls_incov_present.values())
    recall_at_gate = _recall_at(all_incov_dist, all_incov_present)

    # Forklift boundary recall.
    fk_boundary = {
        "present": fk_boundary_present,
        "missed": fk_boundary_missed,
        "recall": round((fk_boundary_present - fk_boundary_missed) / fk_boundary_present, 4)
                  if fk_boundary_present else None,
        "track_loss_pct": round(100 * fk_boundary_missed / fk_boundary_present, 2)
                          if fk_boundary_present else None,
        "window_m": boundary_m, "tw_x": round(tw.x_ref, 3),
    }

    # Split / fragmentation: per detector frame, ≥2 tracks of the same class
    # within split_dist_m = one physical object re-tracked under multiple ids
    # (the "many ids for one object" symptom). Report frames
    # affected and the distinct track-id count per class over the clip.
    split_frames = {c: 0 for c in classes}
    tids_by_class = {c: set() for c in classes}
    frames_with_split = 0
    for dets in uniq.values():
        by_cls: dict = {}
        for d in dets:
            c = d["class"]
            if c in tids_by_class:
                tids_by_class[c].add(d["track_id"])
                by_cls.setdefault(c, []).append((d["x"], d["y"]))
        frame_split = False
        for c, pts in by_cls.items():
            has = any(((pts[i][0] - pts[j][0]) ** 2 + (pts[i][1] - pts[j][1]) ** 2) ** 0.5 < split_dist_m
                      for i in range(len(pts)) for j in range(i + 1, len(pts)))
            if has:
                split_frames[c] += 1
                frame_split = True
        if frame_split:
            frames_with_split += 1
    nfr = len(uniq)
    split = {
        "dist_m": split_dist_m,
        "frames_with_split_pct": round(100 * frames_with_split / nfr, 2) if nfr else None,
        "by_class_split_pct": {c: round(100 * split_frames[c] / nfr, 2) if nfr else None
                               for c in classes},
        "unique_tids_by_class": {c: len(tids_by_class[c]) for c in classes},
    }

    # --- BA position accuracy (mdx-behavior) --------------------------------
    # ba_positions is the latest BA-reported position per active track. We match
    # each BA point to its nearest GT actor within gate_m (BA has no class) and
    # report the localization error. Consecutive identical snapshots from the
    # 30 Hz sampler are de-duplicated so a stationary actor is not over-weighted.
    ba_err: list[float] = []
    ba_frames = 0
    if "ba_positions_json" in df.columns:
        last_snap = None
        for r in (rr for rr, _ in rows):
            bj = r.get("ba_positions_json")
            if not isinstance(bj, str) or bj == last_snap:
                continue
            last_snap = bj
            try:
                bps = json.loads(bj)
            except Exception:
                continue
            gts = {}
            for a in _GT_ACTOR_CLASS:
                gx, gy = r.get(f"{a}_x"), r.get(f"{a}_y")
                if pd.notna(gx) and pd.notna(gy):
                    gts[a] = (float(gx), float(gy))
            if not bps or not gts:
                continue
            ba_frames += 1
            for p in bps:
                px, py = p.get("x"), p.get("y")
                if px is None or py is None:
                    continue
                best = min((((px - gx) ** 2 + (py - gy) ** 2) ** 0.5
                            for gx, gy in gts.values()), default=None)
                if best is not None and best <= gate_m:
                    ba_err.append(best)
    out["ba_pos_mae_m"] = round(float(_np.mean(ba_err)), 4) if ba_err else None
    out["ba_pos_p95_m"] = percentile(ba_err, 95) if ba_err else None
    out["ba_pos_matched"] = len(ba_err)
    out["ba_frames"] = ba_frames

    out_of_coverage: dict = {}
    for a, s in actor_stats.items():
        if s["out_cov"] > 0:
            pts = out_cov_xy[a]
            out_of_coverage[a] = {
                "frames": s["out_cov"],
                "mean_xy": [round(float(_np.mean([p[0] for p in pts])), 2),
                            round(float(_np.mean([p[1] for p in pts])), 2)],
            }

    out.update({
        "frames_scored": frames_scored,
        "frames_skipped_no_gt": frames_skipped_no_gt,
        "coverage_bbox": [round(c, 2) for c in cov] if cov else None,
        "det_tp": tp_all, "det_fp": fp, "det_fn": fn_in, "det_fn_raw": fn_in + fn_ooc,
        "precision": round(prec, 4) if prec is not None else None,
        "recall": round(rec, 4) if rec is not None else None,
        "recall_raw": round(rec_raw, 4) if rec_raw is not None else None,
        "f1": round(f1, 4) if f1 is not None else None,
        "pos_mae_m": round(float(_np.mean(pos_err)), 4) if pos_err else None,
        "pos_median_m": round(float(_np.median(pos_err)), 4) if pos_err else None,
        "pos_p95_m": percentile(pos_err, 95) if pos_err else None,
        "id_switches": id_switches,
        "total_id_switches": sum(v["switches"] for v in id_switches.values()),
        "tracking_ok_pct": tracking_ok_pct,
        "tracking_ok_by_class": tracking_ok_by_class,
        "out_of_coverage": out_of_coverage,
        "actor_stats": actor_stats,
        "per_class": per_class,
        "recall_at_gate": recall_at_gate,
        "gates": list(GATES),
        "forklift_boundary": fk_boundary,
        "split": split,
        "coverage_mode": ("convex_hull" if coverage_pad_m == 0 else "convex_hull+pad") if cov_poly is not None else "bbox",
        "coverage_pad_m": coverage_pad_m,
        "coverage_area_m2": round(cov_poly.area, 1) if cov_poly is not None else None,
        "coverage_degenerate": coverage_degenerate,
        "coverage_degenerate_reason": coverage_degenerate_reason,
        "forklift_origin_offset_m": list(fk_off) if fk_off else None,
        "forklift_origin_samples": fk_off_n,
    })
    return out


def analyze_clip(parquet_path: Path, roi: Polygon, tw: Tripwire,
                 ba_events_override: Optional[list[dict]] = None) -> tuple[ClipVerdict, pd.DataFrame]:
    df = pd.read_parquet(parquet_path)
    # Phase 2 metrics computed on the full df (before the cold-start GT drop).
    phase2 = compute_phase2_metrics(df, tw=tw)

    # Drop the cold-start window: skip leading frames where any required GT TF is still NaN.
    # In Isaac Sim, /gt/<char>/tf can publish ~seconds after the sim starts, so a clip captured
    # at run start may have valid /gt/forklift/tf but no chars yet → expected_mute computes
    # falsely True (forklift in trailer AND any_char=False because no TF data) for that window.
    # Counting those frames as "PSF should have muted" misattributes a GT-data gap to PSF.
    char_xy_cols = [f"{c}_x" for c in CHAR_COLS] + [f"{c}_y" for c in CHAR_COLS]
    required = char_xy_cols + ["forklift_x", "forklift_y"]
    valid_arr = df[required].notna().all(axis=1).to_numpy()
    first_valid = next((i for i, v in enumerate(valid_arr) if v), None)
    n_dropped = 0
    if first_valid is not None and first_valid > 0:
        n_dropped = first_valid
        df = df.iloc[first_valid:].reset_index(drop=True)
    elif first_valid is None:
        # No row has the complete GT set → the clip cannot be graded. Raise a
        # clear error (main() records it in failures.json and the summary
        # counts it) instead of crashing later with an opaque IndexError.
        raise ValueError(
            "no frame has complete GT TF data (some /gt/*/tf never published "
            "in this window) — clip is ungradable"
        )

    # --- ground truth flags per frame ---
    char_in_roi = pd.DataFrame({
        c: [in_roi(roi, x, y) for x, y in zip(df[f"{c}_x"], df[f"{c}_y"])]
        for c in CHAR_COLS
    })
    import numpy as _np
    any_char = _np.array(char_in_roi.any(axis=1).fillna(False).tolist(), dtype=bool)
    fk_in_trailer = _np.array([
        bool(pd.notna(fx) and pd.notna(fy) and tw.is_inside(fx, fy))
        for fx, fy in zip(df["forklift_x"], df["forklift_y"])
    ], dtype=bool)
    df["any_char_in_roi"] = any_char
    df["forklift_in_trailer"] = fk_in_trailer
    # Expected MUTE iff forklift in trailer AND no person in ROI.
    df["expected_mute"] = fk_in_trailer & ~any_char

    # --- observed PSF state (NaN-safe bool) ---
    # Validity first: frames recorded before ANY /safety/is_muted message hold
    # None; the fill below scores them as UNMUTED. Track how much of the clip
    # had a real observation so a dead PSF feed cannot silently score as PASS.
    n_psf_valid = int(df["is_muted"].notna().sum())
    pct_psf_data = round(100.0 * n_psf_valid / len(df), 2) if len(df) else 0.0
    is_muted_raw = df["is_muted"]
    if is_muted_raw.dtype == "object":
        is_muted_raw = is_muted_raw.where(is_muted_raw.notna(), False)
    df["actual_mute"] = _np.array(is_muted_raw.fillna(False).tolist(), dtype=bool)

    psf_cmd_name = df["psf_command"].dropna().map(
        lambda s: json.loads(s).get("command_name", "")
    )

    times = df["arrival_wall_time"].tolist()
    duration_s = float(times[-1] - times[0]) if len(times) > 1 else 0.0
    sample_hz = len(df) / duration_s if duration_s > 0 else 0.0
    t0 = times[0] if times else 0.0

    # match (NaN-safe)
    matched = (df["expected_mute"] == df["actual_mute"]).fillna(False)
    n_mismatch = int((~matched).sum())

    # mismatch direction
    over_mute_mask = (df["actual_mute"] & ~df["expected_mute"]).tolist()
    under_mute_mask = (df["expected_mute"] & ~df["actual_mute"]).tolist()
    n_over = int(sum(over_mute_mask))
    n_under = int(sum(under_mute_mask))
    pct_over = round(100 * n_over / len(df), 2) if len(df) else 0.0
    pct_under = round(100 * n_under / len(df), 2) if len(df) else 0.0
    n_expected_mute = int(df["expected_mute"].sum())
    n_expected_unmute = int((~df["expected_mute"]).sum())

    mismatch_windows = find_mismatch_windows(over_mute_mask, under_mute_mask, times, t0)

    # --- reaction lag: when expected_mute toggles, when does actual toggle to match? ---
    def reaction_lags(expected_value: bool) -> list[float]:
        lags = []
        em = df["expected_mute"].astype(bool)
        prev = em.shift(1).fillna(not expected_value).astype(bool)
        edges = (em == expected_value) & (prev != expected_value)
        edge_positions = [i for i, v in enumerate(edges.tolist()) if v]
        am = df["actual_mute"].astype(bool).tolist()
        wt = df["arrival_wall_time"].tolist()
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

    unmute_lags = reaction_lags(False)
    mute_lags = reaction_lags(True)

    # --- GT-derived event TIMES (clip-relative, for BA matching) ---
    # NOTE: skip transitions at frame 0 — `shift(1, fill_value=False)` creates a spurious
    # "False→True" at i=0 whenever the entity starts the clip already inside, which would
    # double-count entries that actually happened in the *previous* scene.
    gt_roi_entry_times: list[float] = []
    gt_roi_exit_times: list[float] = []
    for c in CHAR_COLS:
        s = pd.Series([in_roi(roi, x, y) for x, y in zip(df[f"{c}_x"], df[f"{c}_y"])])
        prev = s.shift(1).astype(object).where(lambda x: x.notna(), s.iloc[0]).astype(bool)
        for i, v in enumerate((s & ~prev).tolist()):
            if v and i > 0:
                gt_roi_entry_times.append(times[i])
        for i, v in enumerate((~s & prev).tolist()):
            if v and i > 0:
                gt_roi_exit_times.append(times[i])
    fk_series = pd.Series(fk_in_trailer)
    fk_prev = fk_series.shift(1).astype(object).where(
        lambda x: x.notna(), fk_series.iloc[0]
    ).astype(bool)
    gt_tw_entry_times = [times[i] for i, v in enumerate((fk_series & ~fk_prev).tolist()) if v and i > 0]
    gt_tw_exit_times = [times[i] for i, v in enumerate((~fk_series & fk_prev).tolist()) if v and i > 0]
    gt_roi_entries = len(gt_roi_entry_times)
    gt_tw_entries = len(gt_tw_entry_times)
    gt_tw_exits = len(gt_tw_exit_times)

    # --- BA events count + classify ---
    # Prefer the run-level pool (passed in via ba_events_override) — this avoids the scene-boundary
    # spillover problem where BA events that fire ±2s offset from a GT tripwire crossing end up
    # in the neighboring scn_*.parquet rather than the one whose GT contains the crossing.
    if ba_events_override is not None:
        all_evts = list(ba_events_override)
    else:
        all_evts = []
        for s in df["ba_events_json"]:
            all_evts.extend(json.loads(s))
    roi_evts = [e for e in all_evts
                if "ROI" in e.get("event_types", []) and "Person" in e.get("classes", [])]
    tw_evts  = [e for e in all_evts
                if "TW"  in e.get("event_types", []) and "Forklift" in e.get("classes", [])]
    n_ba_roi_person = len(roi_evts)
    n_ba_tw_forklift = len(tw_evts)
    n_ba_tw_right = sum(1 for e in tw_evts if e.get("direction") == "Right")
    n_ba_tw_left  = sum(1 for e in tw_evts if e.get("direction") == "Left")
    n_ba_tw_no_dir = sum(1 for e in tw_evts if e.get("direction") not in ("Right", "Left"))
    DEDUP_WINDOW = 1.0
    roi_ts = sorted(e.get("_kafka_ts", 0.0) for e in roi_evts if e.get("_kafka_ts") is not None)
    tw_right_ts = sorted(e["_kafka_ts"] for e in tw_evts
                         if e.get("direction") == "Right" and e.get("_kafka_ts") is not None)
    tw_left_ts = sorted(e["_kafka_ts"] for e in tw_evts
                        if e.get("direction") == "Left" and e.get("_kafka_ts") is not None)
    n_ba_roi_person_dedup = 0
    last_ts = -1e18
    for t in roi_ts:
        if t - last_ts > DEDUP_WINDOW:
            n_ba_roi_person_dedup += 1
        last_ts = t

    # --- GT-freshness guard: frozen forklift TF (known /gt freeze bug) ---
    # The freeze leaves valid-looking CONSTANT coordinates (not NaN), so the
    # cold-start trim never catches it. Fire only on a conservative dual
    # condition: the forklift GT never moved for the WHOLE clip (>=3 s of
    # data) while BA simultaneously reported tripwire crossings — a truly
    # parked forklift produces no TW events, so this cannot false-positive.
    gt_frozen_actors: list[str] = []
    if len(df) >= 90 and n_ba_tw_forklift > 0:
        _fkx = df["forklift_x"].dropna()
        _fky = df["forklift_y"].dropna()
        if len(_fkx) and _fkx.nunique() <= 1 and _fky.nunique() <= 1:
            gt_frozen_actors.append("forklift")

    # --- per-event matching: GT → first BA event within MAX_MATCH_DELAY_S ---
    ba_roi_entry_match = match_first_ba(gt_roi_entry_times, roi_ts, t0)
    ba_tw_in_match = match_first_ba(gt_tw_entry_times, tw_right_ts, t0)
    ba_tw_out_match = match_first_ba(gt_tw_exit_times, tw_left_ts, t0)
    # ROI exit: BA does not emit exit events. Measure how long BA keeps firing after GT exit.
    roi_trailing_pairs = compute_roi_trailing(gt_roi_exit_times, roi_ts, t0)
    roi_trailing_ms = [trail for _, trail in roi_trailing_pairs]

    # --- clip context ---
    manifest = load_manifest(parquet_path)
    iso_start = manifest.get("iso_start", "")
    iso_end = manifest.get("iso_end", "")
    forklift_side = manifest.get("forklift_side", "")
    if not iso_start:
        iso_start = pd.Timestamp(t0, unit="s", tz="UTC").isoformat()
    if not iso_end:
        iso_end = pd.Timestamp(times[-1], unit="s", tz="UTC").isoformat()
    run_label = parquet_path.parent.parent.name  # <run>/scenes/scn_xxxx.parquet → <run>
    scn_id = parquet_path.stem
    video_path = find_video_path(parquet_path)
    match_rate = round(100 * matched.mean(), 2)

    verdict = ClipVerdict(
        file=parquet_path.name,
        run_label=run_label,
        scn_id=scn_id,
        iso_start=iso_start,
        iso_end=iso_end,
        forklift_side=forklift_side,
        duration_s=round(duration_s, 2),
        rows=len(df),
        sample_hz=round(sample_hz, 2),
        pct_any_char_in_roi=round(100 * df["any_char_in_roi"].mean(), 2),
        pct_forklift_in_trailer=round(100 * df["forklift_in_trailer"].mean(), 2),
        pct_expected_mute=round(100 * df["expected_mute"].mean(), 2),
        pct_actual_mute=round(100 * df["actual_mute"].mean(), 2),
        pct_no_op=round(100 * (psf_cmd_name == "No Operation").mean(), 2) if len(psf_cmd_name) else 0,
        pct_unmute_alarm=round(100 * psf_cmd_name.str.startswith("UNMUTE").mean(), 2) if len(psf_cmd_name) else 0,
        match_rate=match_rate,
        n_mismatch_frames=n_mismatch,
        verdict=classify_verdict(match_rate),
        pct_over_mute=pct_over,
        pct_under_mute=pct_under,
        n_over_mute_frames=n_over,
        n_under_mute_frames=n_under,
        n_expected_mute_frames=n_expected_mute,
        n_expected_unmute_frames=n_expected_unmute,
        mismatch_windows=mismatch_windows,
        median_reaction_unmute_ms=percentile(unmute_lags, 50),
        p95_reaction_unmute_ms=percentile(unmute_lags, 95),
        max_reaction_unmute_ms=round(max(unmute_lags), 1) if unmute_lags else None,
        median_reaction_mute_ms=percentile(mute_lags, 50),
        p95_reaction_mute_ms=percentile(mute_lags, 95),
        max_reaction_mute_ms=round(max(mute_lags), 1) if mute_lags else None,
        n_unmute_transitions=len(unmute_lags),
        n_mute_transitions=len(mute_lags),
        mute_never_engaged=(n_expected_mute > 0 and n_under == n_expected_mute),
        unmute_never_engaged=(n_expected_unmute > 0 and n_over == n_expected_unmute),
        pct_psf_data=pct_psf_data,
        gt_frozen_actors=gt_frozen_actors,
        n_ba_events=len(all_evts),
        n_ba_roi_person=n_ba_roi_person,
        n_ba_tw_forklift=n_ba_tw_forklift,
        n_ba_roi_person_dedup=n_ba_roi_person_dedup,
        n_ba_tw_right=n_ba_tw_right,
        n_ba_tw_left=n_ba_tw_left,
        gt_roi_entries=gt_roi_entries,
        gt_tw_entries=gt_tw_entries,
        gt_tw_exits=gt_tw_exits,
        ba_roi_entry_match=ba_roi_entry_match,
        ba_tw_in_match=ba_tw_in_match,
        ba_tw_out_match=ba_tw_out_match,
        ba_tw_no_direction=n_ba_tw_no_dir,
        roi_trailing_pairs=roi_trailing_pairs,
        median_roi_trailing_ms=percentile(roi_trailing_ms, 50),
        max_roi_trailing_ms=round(max(roi_trailing_ms), 1) if roi_trailing_ms else None,
        video_path=video_path,
    )
    if n_dropped > 0:
        # Annotate so reviewer sees the clip was trimmed.
        verdict.note = f"trimmed {n_dropped} leading frames (cold-start: GT TF not yet valid)"
    verdict.phase2 = phase2
    return verdict, df


# ---------- Report rendering ----------

def _fmt_lag(median: Optional[float], p95: Optional[float],
             mx: Optional[float], n: int) -> str:
    if not n:
        return "n/a (no transitions)"
    return f"median **{median} ms** · p95 **{p95} ms** · max **{mx} ms** (n={n})"


def render_clip_report(v: ClipVerdict) -> str:
    _fail = lambda ok: round(100 - ok, 1) if ok is not None else None  # tracking-loss% = 100 − tracking-ok%  # noqa: E731
    verdict_badge = {"PASS": "✅ PASS", "NEAR": "⚠ NEAR", "FAIL": "❌ FAIL"}[v.verdict]
    lines = [
        f"# Clip — {v.run_label} / {v.scn_id}",
        "",
        f"**Verdict**: {verdict_badge} · match **{v.match_rate}%**",
        "",
        f"- **Run**: `{v.run_label}` · **Scene**: `{v.scn_id}` · forklift_side: `{v.forklift_side or '—'}`",
        f"- **Window**: {v.iso_start} → {v.iso_end}",
        f"- **Duration**: {v.duration_s}s · rows: {v.rows} · sample rate: {v.sample_hz} Hz",
    ]
    if v.video_path:
        lines.append(f"- **Video**: [{v.video_path}]({v.video_path})")
    validity_lines = []
    if v.pct_psf_data < 100.0:
        validity_lines.append(
            f"- ⚠ **PSF feed coverage: {v.pct_psf_data}%** — frames without a real "
            f"/safety/is_muted observation score as UNMUTED"
        )
    if v.gt_frozen_actors:
        validity_lines.append(
            f"- 🚫 **GT_FROZEN**: {', '.join(v.gt_frozen_actors)} GT never moved this "
            f"clip while BA observed tripwire crossings — expected_mute and the "
            f"verdict are untrustworthy"
        )
    lines += [
        "",
        "## Ground truth (from /gt/*/tf)",
        "",
        f"- any character in ROI: **{v.pct_any_char_in_roi}%** of frames",
        f"- forklift in trailer (inside-side of the trailer tripwire): **{v.pct_forklift_in_trailer}%**",
        f"- expected MUTE: **{v.pct_expected_mute}%**",
        "",
        "## Observed PSF state (from /safety/is_muted + /safety/command)",
        "",
        *validity_lines,
        f"- actual MUTE: **{v.pct_actual_mute}%**",
        f"- UNMUTE+Alarm command: {v.pct_unmute_alarm}%",
        f"- No-Op command: {v.pct_no_op}%",
        "",
        "## Match",
        "",
        f"- per-frame match rate: **{v.match_rate}%** ({v.verdict})",
        f"- mismatched frames: {v.n_mismatch_frames}",
        f"- **over-mute** (actual=mute, expected=unmute): {v.n_over_mute_frames} frames ({v.pct_over_mute}%)",
        f"- **under-mute** (actual=unmute, expected=mute): {v.n_under_mute_frames} frames ({v.pct_under_mute}%)",
    ]

    # mismatch windows
    lines += [
        "",
        "### Mismatch windows (≥0.5s contiguous, t relative to clip start)",
        "",
    ]
    if not v.mismatch_windows:
        lines.append("- *(none)*")
    else:
        lines.append("| # | start | end | duration | direction |")
        lines.append("|---|---:|---:|---:|---|")
        for i, w in enumerate(v.mismatch_windows):
            lines.append(
                f"| {i+1} | {w.start_s:>5.2f}s | {w.end_s:>5.2f}s | "
                f"{w.duration_s:>5.2f}s | {w.direction} |"
            )

    lines += [
        "",
        "## Reaction lag",
        "",
        f"- UNMUTE lag (danger appears → alarm on): "
        + ("**∞ — system NEVER unmuted while danger present** (stuck muted)"
           if v.unmute_never_engaged else
           _fmt_lag(v.median_reaction_unmute_ms, v.p95_reaction_unmute_ms, v.max_reaction_unmute_ms, v.n_unmute_transitions)),
        f"- MUTE lag (scene safe → suppress): "
        + ("**∞ — system NEVER muted while it should** (indicator never went green)"
           if v.mute_never_engaged else
           _fmt_lag(v.median_reaction_mute_ms, v.p95_reaction_mute_ms, v.max_reaction_mute_ms, v.n_mute_transitions)),
        "",
        "## BA events (mdx-events Kafka)",
        "",
        f"- total events: {v.n_ba_events}",
        f"- ROI ⛔ Person: {v.n_ba_roi_person} raw / {v.n_ba_roi_person_dedup} deduped",
        f"- TW ➡ Forklift: {v.n_ba_tw_forklift} (Right={v.n_ba_tw_right}, Left={v.n_ba_tw_left}, no-direction={v.ba_tw_no_direction})",
    ]

    # --- per-event BA matching (the real "did BA detect every GT event in time?" check) ---
    lines += [
        "",
        f"## BA detection per GT event (window ≤ {MAX_MATCH_DELAY_S}s)",
        "",
        f"### ROI entries ⛔ Person — `{v.ba_roi_entry_match.matched}/{v.ba_roi_entry_match.gt_count}` detected ({v.ba_roi_entry_match.detect_pct}%)",
        "",
    ]
    if v.ba_roi_entry_match.gt_count == 0:
        lines.append("- *(no GT ROI entries in this clip)*")
    else:
        lines.append("| # | GT entry (t) | BA detected (t) | Delay |")
        lines.append("|---|---:|---:|---:|")
        for i, m in enumerate(v.ba_roi_entry_match.matches):
            ba_cell = f"{m.ba_t_s:>5.2f}s" if m.matched else "**MISSED**"
            delay_cell = f"{m.delay_ms} ms" if m.matched else "—"
            lines.append(f"| {i+1} | {m.gt_t_s:>5.2f}s | {ba_cell} | {delay_cell} |")
        if v.ba_roi_entry_match.median_delay_ms is not None:
            lines.append("")
            lines.append(
                f"- delay: median **{v.ba_roi_entry_match.median_delay_ms} ms** · "
                f"p95 **{v.ba_roi_entry_match.p95_delay_ms} ms** · "
                f"max **{v.ba_roi_entry_match.max_delay_ms} ms**"
            )
        if v.ba_roi_entry_match.extra_ba_count:
            lines.append(
                f"- extra BA ROI events with no matching GT entry: **{v.ba_roi_entry_match.extra_ba_count}** "
                f"(BA fires continuously while person is inside — this is expected over-fire, not false positives)"
            )

    # ROI exits — BA does not emit exit events
    lines += [
        "",
        "### ROI exits ⛔ Person — *BA emits NO exit event*",
        "",
        f"BA fires continuously while a person is inside the ROI; when the person leaves, BA simply "
        f"stops firing. The metric below is the **trailing fire delay** = how long after the GT exit "
        f"BA was still emitting (within {MAX_TRAILING_S}s window).",
        "",
    ]
    if not v.roi_trailing_pairs:
        lines.append("- *(no GT ROI exits in this clip)*")
    else:
        lines.append("| # | GT exit (t) | BA last fired | Trailing |")
        lines.append("|---|---:|---:|---:|")
        for i, (et, td) in enumerate(v.roi_trailing_pairs):
            if td > 0:
                last_t = et + td / 1000.0
                lines.append(f"| {i+1} | {et:>5.2f}s | {last_t:>5.2f}s | {td} ms |")
            else:
                lines.append(f"| {i+1} | {et:>5.2f}s | before exit | 0 ms |")
        lines.append("")
        lines.append(
            f"- trailing fire: median **{v.median_roi_trailing_ms} ms** · "
            f"max **{v.max_roi_trailing_ms} ms**"
        )

    # TW in / out
    for label, sym, ms in [
        ("TW IN ➡ Forklift (entered trailer)", "Right", v.ba_tw_in_match),
        ("TW OUT ⬅ Forklift (exited trailer)", "Left", v.ba_tw_out_match),
    ]:
        lines += [
            "",
            f"### {label} — `{ms.matched}/{ms.gt_count}` detected ({ms.detect_pct}%)",
            "",
        ]
        if ms.gt_count == 0:
            lines.append("- *(no GT events of this type)*")
            continue
        lines.append("| # | GT (t) | BA detected (t) | Delay |")
        lines.append("|---|---:|---:|---:|")
        for i, m in enumerate(ms.matches):
            ba_cell = f"{m.ba_t_s:>5.2f}s ({sym})" if m.matched else "**MISSED**"
            delay_cell = f"{m.delay_ms} ms" if m.matched else "—"
            lines.append(f"| {i+1} | {m.gt_t_s:>5.2f}s | {ba_cell} | {delay_cell} |")
        if ms.median_delay_ms is not None:
            lines.append("")
            lines.append(
                f"- delay: median **{ms.median_delay_ms} ms** · "
                f"p95 **{ms.p95_delay_ms} ms** · "
                f"max **{ms.max_delay_ms} ms**"
            )

    if v.ba_tw_no_direction:
        lines += [
            "",
            f"⚠ **{v.ba_tw_no_direction} TW Forklift event(s) had no `direction` field** — "
            f"these are counted in raw total but cannot be classified as IN or OUT.",
        ]

    # --- Phase 2 — 3D perception metrics (mdx-bev) ---
    p2 = v.phase2
    if p2 and p2.get("enabled"):
        lines += [
            "",
            "## Phase 2 — perception (mdx-bev 3D detector + tracker)",
            "",
            f"- detector frames: **{p2['unique_detector_frames']}** "
            f"(sampled rows with dets: {p2['frames_sampled_with_dets']})",
            f"- mean detections/frame: **{p2['mean_dets_per_frame']}** · "
            f"by class: {p2['class_counts']}",
            f"- unique tracker ids: **{p2['unique_track_ids']}**",
        ]
        if p2.get("gt_available"):
            cov = p2.get("coverage_bbox")
            cov_str = (f"X[{cov[0]}, {cov[1]}] Y[{cov[2]}, {cov[3]}]"
                       if cov else "n/a")
            lines += [
                "",
                "### Detection vs GT (greedy nearest, per class, gate 1.5 m)",
                "",
                f"- sensor coverage bbox (detection footprint + pad): {cov_str}",
                f"- TP/FP/FN (in-coverage): **{p2['det_tp']}/{p2['det_fp']}/{p2['det_fn']}**",
                f"- precision: **{p2['precision']}** · "
                f"recall (in-coverage): **{p2['recall']}** · F1: **{p2['f1']}**",
                f"- recall (raw, incl. out-of-coverage GT): {p2.get('recall_raw')}",
            ]
            if p2.get("coverage_degenerate"):
                lines.append(
                    f"- ⚠️ **coverage degenerate** — {p2.get('coverage_degenerate_reason')}"
                )
            # Multi-gate recall — a loose 1.5 m gate lets a near-miss/ghost count
            # as a hit; tighter gates expose LOST/split the headline gate absorbs.
            rg = p2.get("recall_at_gate") or {}
            if rg:
                gtxt = " · ".join(f"@{g}m **{rg.get(g)}**" for g in (p2.get("gates") or []))
                lines.append(f"- recall by match gate: {gtxt}")
            # Per-class: Person vs Forklift split out.
            pcl = p2.get("per_class") or {}
            if pcl:
                lines += [
                    "",
                    "| class | recall | detect-fail% | tracking-loss% | precision | pos median (offset) | pos p95 (jitter) |",
                    "|---|---:|---:|---:|---:|---:|---:|",
                ]
                for c, d in pcl.items():
                    pos = d.get("pos") or {}
                    lines.append(
                        f"| {c} | {d.get('recall')} | **{d.get('track_loss_pct')}** | "
                        f"{_fail(d.get('tracking_ok_pct'))} | "
                        f"{d.get('precision')} | {pos.get('median')} m | {pos.get('p95')} m |"
                    )
                lines.append("")
                lines.append("_pos median ≈ fixed anchor offset (GT origin vs detected "
                             "box centre); pos p95 ≈ real localization jitter. For the "
                             "forklift the ~0.35 m median is geometry, not detector error._")
            lines += [
                f"- blended position error vs GT: MAE **{p2['pos_mae_m']} m** · "
                f"median **{p2.get('pos_median_m')} m** · p95 **{p2['pos_p95_m']} m**",
            ]
            if p2.get("ba_pos_matched"):
                lines.append(
                    f"- BA position vs GT (mdx-behavior, {p2['ba_pos_matched']} pts "
                    f"over {p2['ba_frames']} frames): MAE **{p2['ba_pos_mae_m']} m** · "
                    f"p95 **{p2['ba_pos_p95_m']} m**"
                )
            ooc = p2.get("out_of_coverage") or {}
            if ooc:
                lines += [
                    "",
                    "#### ⚠ GT actors outside sensor coverage (excluded from recall)",
                    "",
                    "| actor | frames out | mean position |",
                    "|---|---:|---|",
                ]
                for a, info in ooc.items():
                    mx, my = info["mean_xy"]
                    lines.append(f"| {a} | {info['frames']} | ({mx}, {my}) |")
                lines.append("")
                lines.append("These are coverage gaps (actor never in any camera "
                             "FOV), not detector false negatives — file as a "
                             "scene/sensor-coverage issue, not a perception bug.")
            ast = p2.get("actor_stats") or {}
            lines += [
                "",
                f"### Per-actor tracking — id-switches total **{p2['total_id_switches']}**",
                "",
                "detect-fail% = in-coverage frames with no matched detection. "
                "out-cov% = frames the actor was outside sensor coverage (not "
                "counted as a miss).",
                "",
                "| actor | recall (in-cov) | detect-fail% | out-cov% | track ids | switches | switch-rate |",
                "|---|---:|---:|---:|---|---:|---:|",
            ]
            for a, st in p2["id_switches"].items():
                s = ast.get(a, {})
                lines.append(
                    f"| {a} | {s.get('recall_in_cov')} | **{s.get('track_loss_pct')}** | "
                    f"{s.get('out_cov_pct')} | {','.join(st['track_ids']) or '–'} | "
                    f"{st['switches']} | {st.get('switch_rate')} |"
                )
            fb = p2.get("forklift_boundary") or {}
            sp = p2.get("split") or {}
            lines += [
                "",
                "### Trailer-boundary & split",
                "",
                f"- 🚜 forklift recall within ±{fb.get('window_m')} m of tripwire "
                f"(x={fb.get('tw_x')}): **{fb.get('recall')}** · "
                f"detect-fail **{fb.get('track_loss_pct')}%** (n={fb.get('present')})",
                f"- split (≥2 same-class tracks <{sp.get('dist_m')} m): "
                f"**{sp.get('frames_with_split_pct')}%** of frames · "
                f"distinct ids — {sp.get('unique_tids_by_class')}",
                f"- coverage region: {p2.get('coverage_mode')} "
                f"(area {p2.get('coverage_area_m2')} m²)",
            ]
        else:
            lines += ["", f"- ⚠ {p2.get('note', 'GT absent')}"]
    elif p2 and not p2.get("enabled"):
        lines += ["", "## Phase 2 — perception (mdx-bev)", "",
                  f"- *(no detections: {p2.get('reason', 'n/a')})*"]

    return "\n".join(lines) + "\n"


def render_summary(verdicts: list[ClipVerdict], top_level: bool = False,
                   n_analyze_errors: int = 0) -> str:
    if not verdicts:
        return "# SRR Aggregator — no clips found\n"
    n = len(verdicts)

    # headline
    pass_n = sum(1 for v in verdicts if v.verdict == "PASS")
    near_n = sum(1 for v in verdicts if v.verdict == "NEAR")
    fail_n = sum(1 for v in verdicts if v.verdict == "FAIL")
    match_series = pd.Series([v.match_rate for v in verdicts])
    all_unmute_max = [v.max_reaction_unmute_ms for v in verdicts if v.max_reaction_unmute_ms is not None]
    all_mute_max = [v.max_reaction_mute_ms for v in verdicts if v.max_reaction_mute_ms is not None]
    worst_unmute = max(all_unmute_max) if all_unmute_max else None
    worst_mute = max(all_mute_max) if all_mute_max else None
    n_over = sum(v.n_over_mute_frames for v in verdicts)
    n_under = sum(v.n_under_mute_frames for v in verdicts)
    tot_exp_mute = sum(v.n_expected_mute_frames for v in verdicts)
    tot_exp_unmute = sum(v.n_expected_unmute_frames for v in verdicts)
    mute_correct_hd = (
        f"{100 * (1 - n_under / tot_exp_mute):.1f}% (n={tot_exp_mute})"
        if tot_exp_mute else "n/a"
    )
    unmute_correct_hd = (
        f"{100 * (1 - n_over / tot_exp_unmute):.1f}% (n={tot_exp_unmute})"
        if tot_exp_unmute else "n/a"
    )

    # BA per-event detection
    def headline_match(field_name: str) -> str:
        gt = sum(getattr(v, field_name).gt_count for v in verdicts)
        mt = sum(getattr(v, field_name).matched for v in verdicts)
        if not gt:
            return "n/a"
        delays = []
        for v in verdicts:
            delays += [m.delay_ms for m in getattr(v, field_name).matches if m.matched]
        med = percentile(delays, 50)
        p95 = percentile(delays, 95)
        return f"**{mt}/{gt}** ({100*mt/gt:.1f}%) · median {med} ms · p95 {p95} ms"
    ba_roi_hd = headline_match("ba_roi_entry_match")
    ba_twin_hd = headline_match("ba_tw_in_match")
    ba_twout_hd = headline_match("ba_tw_out_match")
    tot_no_dir = sum(v.ba_tw_no_direction for v in verdicts)

    # Degenerate-for-mute clips: person always in ROI → never an expected-mute
    # frame → the clip can only ever test UNMUTE and trivially PASSes. Counting
    # them in the headline PASS% over-states mute reliability.
    n_trivial = sum(1 for v in verdicts if v.n_expected_mute_frames == 0)
    n_trivial_pass = sum(1 for v in verdicts
                         if v.n_expected_mute_frames == 0 and v.verdict == "PASS")

    # ghost-result detector: if BA detection rate is far below threshold, perception
    # was likely dead (FPS=0, empty Kafka). match% becomes meaningless because both
    # PSF and GT default to UNMUTE without events → spurious agreement.
    total_gt_events = sum(
        getattr(v, fld).gt_count
        for v in verdicts
        for fld in ("ba_roi_entry_match", "ba_tw_in_match", "ba_tw_out_match")
    )
    total_ba_matched = sum(
        getattr(v, fld).matched
        for v in verdicts
        for fld in ("ba_roi_entry_match", "ba_tw_in_match", "ba_tw_out_match")
    )
    # Phase 2 gives DIRECT evidence of perception health (decoded mdx-bev
    # detections). If 3D detections are flowing, perception is demonstrably
    # alive even when BA ROI/TW events are 0 — common in 3D when actors start
    # already inside the ROI (no ENTER transition for BA to fire). In that case
    # the BA-event-based ghost detector is a false positive, so downgrade it to
    # a soft note instead of the red PERCEPTION_LIKELY_DEAD banner.
    phase2_alive = any(
        v.phase2 and v.phase2.get("enabled")
        and (v.phase2.get("unique_detector_frames") or 0) > 0
        and (v.phase2.get("det_tp") or 0) > 0
        for v in verdicts
    )
    ghost_banner: list[str] = []
    if total_gt_events > 0 and total_ba_matched / total_gt_events < GHOST_RESULT_BA_THRESHOLD:
        rate_pct = 100 * total_ba_matched / total_gt_events
        if phase2_alive:
            ghost_banner = [
                f"> ⚠️ **BA event mismatch** — BA ROI/TW match rate "
                f"{total_ba_matched}/{total_gt_events} ({rate_pct:.1f}%) is low, but 3D "
                f"perception is ALIVE (Phase 2 detections flowing), so this is NOT a dead "
                f"pipeline. Usually means actors started inside the ROI (no ENTER event to "
                f"match) or BA event timing differs in 3D — interpret BA ROI/TW columns with care.",
                "",
            ]
        else:
            ghost_banner = [
                f"> 🚫 **PERCEPTION_LIKELY_DEAD** — match% UNTRUSTWORTHY. BA detection rate "
                f"{total_ba_matched}/{total_gt_events} ({rate_pct:.1f}%) is below the "
                f"{GHOST_RESULT_BA_THRESHOLD * 100:.0f}% threshold. Inspect `docker logs vss-rtvi-cv` "
                f"and re-run the scenario after fixing VSS perception.",
                "",
            ]

    # Per-scenario ghost check: the pooled rate above can mask one dead
    # scenario diluted by healthy ones (4×90% + 1×0% pools to ~72%). Same
    # threshold + same phase2-alive downgrade, applied per run_label. Additive
    # — the pooled banner above is unchanged.
    scen_ghost_banner: list[str] = []
    _runs_g: dict[str, list[ClipVerdict]] = {}
    for v in verdicts:
        _runs_g.setdefault(v.run_label or "(unknown)", []).append(v)
    if len(_runs_g) > 1:
        for _run in sorted(_runs_g):
            _vs = _runs_g[_run]
            _g = sum(getattr(v, fld).gt_count for v in _vs
                     for fld in ("ba_roi_entry_match", "ba_tw_in_match", "ba_tw_out_match"))
            _m = sum(getattr(v, fld).matched for v in _vs
                     for fld in ("ba_roi_entry_match", "ba_tw_in_match", "ba_tw_out_match"))
            _alive = any(
                v.phase2 and v.phase2.get("enabled")
                and (v.phase2.get("unique_detector_frames") or 0) > 0
                and (v.phase2.get("det_tp") or 0) > 0
                for v in _vs
            )
            if _g > 0 and _m / _g < GHOST_RESULT_BA_THRESHOLD and not _alive:
                scen_ghost_banner.append(
                    f"> 🚫 **PERCEPTION_LIKELY_DEAD in `{_run}`** — BA match {_m}/{_g} "
                    f"({100 * _m / _g:.1f}%) for this scenario alone (the pooled rate "
                    f"above can mask a single dead scenario)."
                )
        if scen_ghost_banner:
            scen_ghost_banner.append("")

    # PSF-feed validity banner: a dead /safety/is_muted feed makes every frame
    # score as UNMUTED — unmute-test scenarios then PASS with zero real safety
    # output. Fires only when clips carry missing PSF observations.
    n_clips_all = len(verdicts)
    n_psf_dead = sum(1 for v in verdicts if v.pct_psf_data == 0)
    n_psf_partial = sum(1 for v in verdicts if 0 < v.pct_psf_data < 100.0)
    psf_banner: list[str] = []
    if n_clips_all and n_psf_dead == n_clips_all:
        psf_banner = [
            "> 🚫 **PSF_FEED_DEAD** — no /safety/is_muted message was received in ANY "
            "clip; every frame scored as UNMUTED by default. match%, verdicts and "
            "Unmute-correct% are fabricated. Check the safety-core container and the "
            "comm-layer bridge, then re-run.",
            "",
        ]
    elif n_psf_dead or n_psf_partial:
        psf_banner = [
            f"> ⚠️ **PSF feed gaps** — {n_psf_dead} clip(s) with NO /safety/is_muted "
            f"data and {n_psf_partial} with partial coverage; affected frames score "
            f"as UNMUTED (see per-clip reports).",
            "",
        ]

    # GT-frozen banner (known /gt TF freeze: constant coordinates while BA saw
    # tripwire crossings) — those clips grade against wrong ground truth.
    n_frozen = sum(1 for v in verdicts if v.gt_frozen_actors)
    frozen_banner: list[str] = []
    if n_frozen:
        frozen_banner = [
            f"> 🚫 **GT_FROZEN** — {n_frozen} clip(s) where forklift GT never moved "
            f"while BA observed tripwire crossings (known /gt TF freeze). "
            f"expected_mute is wrong there; treat those verdicts as untrustworthy.",
            "",
        ]

    analyze_error_lines: list[str] = []
    if n_analyze_errors:
        analyze_error_lines = [
            f"> ⚠️ **{n_analyze_errors} clip(s) failed analysis** and are EXCLUDED "
            f"from every number below (see failures.json).",
            "",
        ]

    lines = [
        f"# SRR Aggregator — {n} clip(s)",
        "",
        *ghost_banner,
        *scen_ghost_banner,
        *psf_banner,
        *frozen_banner,
        *analyze_error_lines,
        "## Headline",
        "",
        f"- total clips: **{n}**",
        f"- verdicts: ✅ **{pass_n} PASS** ({100*pass_n/n:.0f}%) · ⚠ {near_n} NEAR ({100*near_n/n:.0f}%) · ❌ **{fail_n} FAIL** ({100*fail_n/n:.0f}%)",
        (f"  - ⚠ **{n_trivial} clips have ~zero expected-mute frames** "
         f"(person always in ROI → mute function untestable); {n_trivial_pass} of them "
         f"are PASS and inflate this headline. Judge mute behaviour on the mute-test "
         f"scenarios + per-scenario rollup, not this blended number." if n_trivial else ""),
        f"- match%: mean {match_series.mean():.1f} · median {match_series.median():.1f} · min {match_series.min():.1f}",
        f"- **Mute correct%** (system muted when GT said mute): **{mute_correct_hd}**",
        f"- **Unmute correct%** (system unmuted when GT said no mute): **{unmute_correct_hd}**",
        "",
        "**BA perception — per-event match (window ≤ 2 s)**:",
        f"- ROI entries (Person enters work zone): {ba_roi_hd}",
        f"- TW IN (forklift enters trailer): {ba_twin_hd}",
        f"- TW OUT (forklift exits trailer): {ba_twout_hd}",
        f"- *Note*: BA emits NO ROI exit events; person leaving the zone is detected by BA simply ceasing to fire.",
        f"- TW Forklift events missing `direction` field: **{tot_no_dir}**" if tot_no_dir else "",
        "",
        f"- worst UNMUTE lag (danger response): **{worst_unmute} ms**" if worst_unmute is not None else "- worst UNMUTE lag: n/a",
        f"- worst MUTE lag (safe-state settle): **{worst_mute} ms**" if worst_mute is not None else "- worst MUTE lag: n/a",
        f"- mismatch frame totals — over-mute: {n_over} · under-mute: {n_under}",
        "",
    ]

    # --- Phase 2 — perception rollup (clips with 3D mdx-bev + GT) ---
    p2v = [v for v in verdicts
           if v.phase2 and v.phase2.get("enabled") and v.phase2.get("gt_available")]
    if p2v:
        s_tp = sum(v.phase2.get("det_tp") or 0 for v in p2v)
        s_fp = sum(v.phase2.get("det_fp") or 0 for v in p2v)
        s_fn = sum(v.phase2.get("det_fn") or 0 for v in p2v)
        prec = s_tp / (s_tp + s_fp) if (s_tp + s_fp) else None
        rec = s_tp / (s_tp + s_fn) if (s_tp + s_fn) else None
        f1 = (2 * prec * rec / (prec + rec)) if (prec and rec) else None
        maes = [v.phase2["pos_mae_m"] for v in p2v if v.phase2.get("pos_mae_m") is not None]
        ba_maes = [v.phase2["ba_pos_mae_m"] for v in p2v if v.phase2.get("ba_pos_mae_m") is not None]
        sw = sum(v.phase2.get("total_id_switches") or 0 for v in p2v)
        mean = lambda xs: round(sum(xs) / len(xs), 3) if xs else None  # noqa: E731
        _fail = lambda ok: round(100 - ok, 1) if ok is not None else None  # tracking-loss% = 100 − tracking-ok%  # noqa: E731

        def agg_class(vs: list, c: str) -> dict:
            """Sum a class's TP/FP/FN + in-cov frames across clips (counts merge
            cleanly); position median is averaged over per-clip medians."""
            tp = fp = fn = present = 0
            tok = tfr = 0
            meds = []
            for v in vs:
                pc = ((v.phase2 or {}).get("per_class") or {}).get(c)
                if not pc:
                    continue
                tp += pc["tp_in"]; fp += pc["fp"]; fn += pc["fn_in"]
                present += pc["in_cov_present"]
                tok += pc.get("track_ok_n") or 0
                tfr += pc.get("track_frames_n") or 0
                if pc["pos"]["median"] is not None:
                    meds.append(pc["pos"]["median"])
            return {
                "recall": round(tp / (tp + fn), 3) if (tp + fn) else None,
                "track_loss_pct": round(100 * fn / present, 1) if present else None,
                "tracking_ok_pct": round(100 * tok / tfr, 1) if tfr else None,
                "precision": round(tp / (tp + fp), 3) if (tp + fp) else None,
                "pos_offset_m": round(sum(meds) / len(meds), 3) if meds else None,
                "present": present,
            }
        pc_person = agg_class(p2v, "Person")
        pc_fk = agg_class(p2v, "Forklift")

        lines += [
            "## Phase 2 — perception (3D mdx-bev detector + tracker)",
            "",
            f"- clips with 3D + GT: **{len(p2v)}/{n}**",
            f"- detection blended (in-coverage, gate 1.5 m): precision **{prec:.3f}** · "
            f"recall **{rec:.3f}** · F1 **{f1:.3f}**"
            if (prec is not None and rec is not None and f1 is not None)
            else "- detection: n/a",
            f"  (TP/FP/FN = {s_tp}/{s_fp}/{s_fn} summed across clips)",
            "",
            "**Per-class detection (the blend above hides that the safety-critical "
            "forklift is the worst object):**",
            f"- 🧍 Person: recall **{pc_person['recall']}** · "
            f"**detect-fail {pc_person['track_loss_pct']}%** · "
            f"**tracking-loss {_fail(pc_person['tracking_ok_pct'])}%** · precision {pc_person['precision']} · "
            f"pos-offset {pc_person['pos_offset_m']} m",
            f"- 🚜 Forklift: recall **{pc_fk['recall']}** · "
            f"**detect-fail {pc_fk['track_loss_pct']}%** · "
            f"**tracking-loss {_fail(pc_fk['tracking_ok_pct'])}%** · precision {pc_fk['precision']} · "
            f"pos-offset {pc_fk['pos_offset_m']} m",
            "",
            f"- detector position error (blended): median **{mean([v.phase2.get('pos_median_m') for v in p2v if v.phase2.get('pos_median_m') is not None])} m** "
            f"(≈ systematic anchor offset, GT origin vs box centre) · mean MAE **{mean(maes)} m**",
            f"- BA position error (mdx-behavior): mean MAE **{mean(ba_maes)} m**",
            f"- tracker id-switches (total across clips): **{sw}**",
            "- _**detect-fail%** = of in-coverage frames, the % the object was NOT "
            "detected (a.k.a. the old 'track-loss'; it is a DETECTION miss, ignores id). "
            "**tracking-loss%** = of an object's detected frames, the % NOT on its dominant "
            "track-id (id-switch/fragmentation) — a TRACKING failure. "
            "Detection asks 'was it seen'; tracking-loss asks 'did it keep one id'._",
        ]
        # Forklift boundary recall + split, aggregated across clips.
        b_pres = sum(((v.phase2.get("forklift_boundary") or {}).get("present") or 0) for v in p2v)
        b_miss = sum(((v.phase2.get("forklift_boundary") or {}).get("missed") or 0) for v in p2v)
        b_rec = round((b_pres - b_miss) / b_pres, 3) if b_pres else None
        b_loss = round(100 * b_miss / b_pres, 1) if b_pres else None
        max_person_ids = max((((v.phase2.get("split") or {}).get("unique_tids_by_class") or {}).get("Person") or 0) for v in p2v) if p2v else 0
        max_fk_ids = max((((v.phase2.get("split") or {}).get("unique_tids_by_class") or {}).get("Forklift") or 0) for v in p2v) if p2v else 0
        split_pcts = [(v.phase2.get("split") or {}).get("frames_with_split_pct") for v in p2v
                      if (v.phase2.get("split") or {}).get("frames_with_split_pct") is not None]
        lines += [
            f"- 🚜 **forklift recall AROUND trailer boundary** (±{(p2v[0].phase2.get('forklift_boundary') or {}).get('window_m', 2.0)} m of tripwire): "
            f"**{b_rec}** · detect-fail **{b_loss}%** (n={b_pres}) — isolates the known "
            f"LOST-around-trailer failure mode",
            f"- **split / fragmentation**: frames with ≥2 same-class tracks <1 m: "
            f"mean **{mean(split_pcts)}%** · max distinct ids in a clip — Person **{max_person_ids}**, Forklift **{max_fk_ids}**",
            "",
            "_pos-offset = median distance GT↔detection. The forklift GT origin "
            "(Isaac `body` TF) sits ~0.39 m behind the detected 3D-box centre; we "
            "now estimate that bias data-driven (heading from GT velocity) and shift "
            "the forklift GT onto the box centre before matching, so pos-offset is "
            "true localization error (~0.08 m, on par with persons), not an anchor "
            "artefact. Jitter is the spread (p95) in each clip report._",
            "",
            "| Clip | Run | 🧍 rec | 🧍 det-fail% | 🧍 tracking-loss% | 🚜 rec | 🚜 det-fail% | 🚜 tracking-loss% | split% | precision | det MAE (m) | BA MAE (m) | id-sw | out-of-cov |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
        ]
        any_degenerate = False
        for v in p2v:
            p2 = v.phase2
            ooc = ",".join((p2.get("out_of_coverage") or {}).keys()) or "–"
            if p2.get("coverage_degenerate"):
                any_degenerate = True
                ooc = (ooc + " ⚠deg-cov").strip()
            pcl = p2.get("per_class") or {}
            prec_p = (pcl.get("Person") or {}).get("recall")
            loss_p = (pcl.get("Person") or {}).get("track_loss_pct")
            tfail_p = _fail((pcl.get("Person") or {}).get("tracking_ok_pct"))
            prec_f = (pcl.get("Forklift") or {}).get("recall")
            loss_f = (pcl.get("Forklift") or {}).get("track_loss_pct")
            tfail_f = _fail((pcl.get("Forklift") or {}).get("tracking_ok_pct"))
            spp = (p2.get("split") or {}).get("frames_with_split_pct")
            lines.append(
                f"| {v.scn_id or '?'} | {v.run_label or '–'} | "
                f"{prec_p} | {loss_p} | {tfail_p} | {prec_f} | {loss_f} | {tfail_f} | {spp} | "
                f"{p2.get('precision')} | "
                f"{p2.get('pos_mae_m')} | {p2.get('ba_pos_mae_m')} | "
                f"{p2.get('total_id_switches')} | {ooc} |"
            )
        if any_degenerate:
            lines.append("")
            lines.append(
                "> ⚠️ **`⚠deg-cov`** = degenerate coverage: detections collapsed onto "
                "~1 line/point (usually only one actor stayed visible while others were "
                "static/out-of-FOV), so the convex-hull coverage area ≈ 0 and recall is "
                "**unscoreable** (`None` = 0 in-coverage GT frames). This is a "
                "**scene/coverage** problem, **not** a dead perception pipeline — "
                "detections were still recorded. Re-run the scenario; if it recurs, the "
                "actors aren't spreading across the camera footprint."
            )
        lines.append("")

        # --- Per-scenario perception rollup (%track / %detect per scene) ---
        p2_by_run: dict[str, list] = {}
        for v in p2v:
            p2_by_run.setdefault(v.run_label or "(unknown)", []).append(v)
        if len(p2_by_run) > 1:
            lines += [
                "### Per-scenario perception rollup",
                "",
                "Detect-fail % + tracking-loss % per scene, split by class. "
                "**detect-fail%** = in-coverage frames where the object had no matched "
                "detection (a DETECTION miss; was the old 'track-loss'). "
                "**tracking-loss%** = of detected frames, the % not on the dominant track-id "
                "(a TRACKING failure).",
                "",
                "| Scenario | clips | 🧍 recall | 🧍 det-fail% | 🧍 tracking-loss% | 🚜 recall | 🚜 det-fail% | 🚜 tracking-loss% | 🚜 boundary det-fail% | split frames% | id-switches |",
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
            ]
            for run, vs in p2_by_run.items():
                pp = agg_class(vs, "Person")
                pf = agg_class(vs, "Forklift")
                rsw = sum(v.phase2.get("total_id_switches") or 0 for v in vs)
                bp = sum(((v.phase2.get("forklift_boundary") or {}).get("present") or 0) for v in vs)
                bm = sum(((v.phase2.get("forklift_boundary") or {}).get("missed") or 0) for v in vs)
                bl = round(100 * bm / bp, 1) if bp else None
                spv = [(v.phase2.get("split") or {}).get("frames_with_split_pct") for v in vs
                       if (v.phase2.get("split") or {}).get("frames_with_split_pct") is not None]
                spm = round(sum(spv) / len(spv), 1) if spv else None
                lines.append(
                    f"| {run} | {len(vs)} | {pp['recall']} | **{pp['track_loss_pct']}** | {_fail(pp['tracking_ok_pct'])} | "
                    f"{pf['recall']} | **{pf['track_loss_pct']}** | {_fail(pf['tracking_ok_pct'])} | **{bl}** | {spm} | {rsw} |"
                )
            lines.append("")

    # per-run rollup
    by_run: dict[str, list[ClipVerdict]] = {}
    for v in verdicts:
        by_run.setdefault(v.run_label or "(unknown)", []).append(v)
    if len(by_run) > 1:
        lines += [
            "## Per-run rollup",
            "",
            "**Mute correct%** = of frames GT says SHOULD mute, fraction system actually muted (1 − under-mute ratio).  ",
            "**Unmute correct%** = of frames GT says should NOT mute, fraction system actually unmuted (1 − over-mute ratio).  ",
            f"**BA ROI/TW**: per-event detection — for each GT event, did BA emit a matching event within {MAX_MATCH_DELAY_S}s? Format = `matched/total · median delay`.",
            "",
            "| Run | Clips | Avg match% | Mute correct% | Unmute correct% | PASS / NEAR / FAIL | BA ROI entry | BA TW in | BA TW out | ROI trailing | Max unmute_lag | Max mute_lag |",
            "|---|---:|---:|---:|---:|:---:|:---:|:---:|:---:|:---:|---:|---:|",
        ]
        for run, vs in by_run.items():
            ms = pd.Series([x.match_rate for x in vs])
            run_p = sum(1 for x in vs if x.verdict == "PASS")
            run_n = sum(1 for x in vs if x.verdict == "NEAR")
            run_f = sum(1 for x in vs if x.verdict == "FAIL")
            run_unmute_max = [x.max_reaction_unmute_ms for x in vs if x.max_reaction_unmute_ms is not None]
            run_mute_max = [x.max_reaction_mute_ms for x in vs if x.max_reaction_mute_ms is not None]

            tot_exp_mute = sum(x.n_expected_mute_frames for x in vs)
            tot_exp_unmute = sum(x.n_expected_unmute_frames for x in vs)
            tot_under = sum(x.n_under_mute_frames for x in vs)
            tot_over = sum(x.n_over_mute_frames for x in vs)

            # Detect run "intent" from data: scenarios designed with chars constantly in ROI
            # (e.g. `in-roi`) shouldn't have an expected-mute period at all — any mute frames
            # come from the walking-into-ROI cold-start at run start, not from a real mute test.
            avg_chars_in_roi = sum(x.pct_any_char_in_roi for x in vs) / len(vs) if vs else 0
            avg_chars_out = 100 - avg_chars_in_roi
            unmute_test_scenario = avg_chars_in_roi >= 95.0
            mute_test_scenario = avg_chars_out >= 95.0

            if unmute_test_scenario:
                mute_correct = f"n/a (unmute-test scenario)"
            elif tot_exp_mute:
                mute_correct = f"{100 * (1 - tot_under / tot_exp_mute):.1f}% (n={tot_exp_mute})"
            else:
                mute_correct = "n/a"

            if mute_test_scenario:
                unmute_correct = f"n/a (mute-test scenario)"
            elif tot_exp_unmute:
                unmute_correct = f"{100 * (1 - tot_over / tot_exp_unmute):.1f}% (n={tot_exp_unmute})"
            else:
                unmute_correct = "n/a"

            def aggregate_match(field_name: str) -> str:
                gt = sum(getattr(x, field_name).gt_count for x in vs)
                mt = sum(getattr(x, field_name).matched for x in vs)
                if not gt:
                    return "n/a"
                delays = []
                for x in vs:
                    delays += [m.delay_ms for m in getattr(x, field_name).matches if m.matched]
                med = percentile(delays, 50)
                return f"**{mt}/{gt}** · {med} ms" if med is not None else f"**{mt}/{gt}**"

            ba_roi_cell = aggregate_match("ba_roi_entry_match")
            ba_twin_cell = aggregate_match("ba_tw_in_match")
            ba_twout_cell = aggregate_match("ba_tw_out_match")

            trailing_all: list[float] = []
            for x in vs:
                trailing_all += [td for _, td in x.roi_trailing_pairs]
            trailing_med = percentile(trailing_all, 50)
            trailing_max = max(trailing_all) if trailing_all else None
            trail_cell = (
                f"med {trailing_med} / max {trailing_max:.0f} ms"
                if trailing_max is not None else "n/a"
            )

            lines.append(
                f"| {run} | {len(vs)} | {ms.mean():.1f} | {mute_correct} | {unmute_correct} | "
                f"{run_p} / {run_n} / {run_f} | "
                f"{ba_roi_cell} | {ba_twin_cell} | {ba_twout_cell} | {trail_cell} | "
                f"{max(run_unmute_max) if run_unmute_max else '–'} ms | "
                f"{max(run_mute_max) if run_mute_max else '–'} ms |"
            )
        lines.append("")

    # per-clip table
    lines += [
        "## Per-clip detail",
        "",
        "Columns: BA ROI / TW in / TW out = per-event match `matched/total` (BA events pulled from run-level pool, ±2s padding).",
        "",
        "| Clip | Run | Verdict | match% | over-mute% | under-mute% | unmute_lag (med/max) | mute_lag (med/max) | %char_in_roi | %fk_trailer | BA ROI | BA TW in | BA TW out | video |",
        "|---|---|:---:|---:|---:|---:|---:|---:|---:|---:|:---:|:---:|:---:|---|",
    ]
    for v in verdicts:
        verdict_cell = {"PASS": "✅", "NEAR": "⚠", "FAIL": "❌"}[v.verdict]
        unmute_cell = (
            "∞ (never)" if v.unmute_never_engaged else
            f"{v.median_reaction_unmute_ms}/{v.max_reaction_unmute_ms}"
            if v.median_reaction_unmute_ms is not None else "–"
        )
        mute_cell = (
            "∞ (never)" if v.mute_never_engaged else
            f"{v.median_reaction_mute_ms}/{v.max_reaction_mute_ms}"
            if v.median_reaction_mute_ms is not None else "–"
        )
        if top_level:
            report_link = f"{v.run_label}/reports/{v.scn_id}.md"
            video_link = f"{v.run_label}/videos/{v.scn_id}.mp4"
        else:
            report_link = f"{v.scn_id}.md"
            video_link = f"../videos/{v.scn_id}.mp4"
        video_cell = f"[mp4]({video_link})" if v.video_path else "–"

        def cell(ms: BAMatchSet) -> str:
            if not ms.gt_count:
                return "–"
            return f"{ms.matched}/{ms.gt_count}"
        roi_cell = cell(v.ba_roi_entry_match)
        twin_cell = cell(v.ba_tw_in_match)
        twout_cell = cell(v.ba_tw_out_match)

        lines.append(
            f"| [{v.scn_id}]({report_link}) | {v.run_label} | "
            f"{verdict_cell} | {v.match_rate} | {v.pct_over_mute} | {v.pct_under_mute} "
            f"| {unmute_cell} | {mute_cell} "
            f"| {v.pct_any_char_in_roi} | {v.pct_forklift_in_trailer} "
            f"| {roi_cell} | {twin_cell} | {twout_cell} "
            f"| {video_cell} |"
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--runs-dir", default="/app/runs")
    p.add_argument("--out-dir",  default="/app/runs/reports")
    p.add_argument("--calib",    default="/app/calibration.json")
    p.add_argument("--top-level", action="store_true",
                   help="Scan <runs-dir>/<run>/scenes/*.parquet recursively, write top-level summary at <runs-dir>/summary.md.")
    args = p.parse_args()

    runs_dir = Path(args.runs_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    calib_path = Path(args.calib) if args.calib else None
    roi, tw = load_roi(calib_path)
    print(f"[agg] ROI bounds: {list(roi.exterior.coords)}")
    print(f"[agg] TW {tw}")

    if args.top_level:
        parquets = sorted(runs_dir.glob("*/scenes/scn_*.parquet"))
    else:
        # Accept both pre-split run-*.parquet and post-split scn_*.parquet
        parquets = sorted(list(runs_dir.glob("run-*.parquet")) + list(runs_dir.glob("scn_*.parquet")))
    if not parquets:
        print(f"[agg] no parquet found in {runs_dir}")
        return

    # In top-level mode, pre-load each run's BA event pool from run-*.parquet so per-scene
    # matching uses the full timeline (avoids scene-boundary spillover for ±2s offset events).
    run_ba_pools: dict[str, list[dict]] = {}
    if args.top_level:
        for pq in parquets:
            run_dir = pq.parent.parent
            if run_dir.name not in run_ba_pools:
                run_ba_pools[run_dir.name] = load_run_ba_pool(run_dir)
                print(f"[agg] BA pool for {run_dir.name}: {len(run_ba_pools[run_dir.name])} events")

    verdicts: list[ClipVerdict] = []
    failures: list[dict] = []
    for pq in parquets:
        try:
            ba_override: Optional[list[dict]] = None
            if args.top_level:
                run_name = pq.parent.parent.name
                pool = run_ba_pools.get(run_name, [])
                if pool:
                    # filter pool to scene window ±BA_POOL_PAD_S
                    scene_df = pd.read_parquet(pq, columns=["arrival_wall_time"])
                    scene_start = float(scene_df["arrival_wall_time"].iloc[0])
                    scene_end = float(scene_df["arrival_wall_time"].iloc[-1])
                    ba_override = filter_ba_pool(pool, scene_start, scene_end)
            v, _df = analyze_clip(pq, roi, tw,
                                   ba_events_override=ba_override)
            verdicts.append(v)
            if args.top_level:
                report_dir = pq.parent.parent / "reports"
                report_dir.mkdir(parents=True, exist_ok=True)
                (report_dir / f"{pq.stem}.md").write_text(render_clip_report(v))
            else:
                (out_dir / f"{pq.stem}.md").write_text(render_clip_report(v))
            print(f"[agg] {pq.name}: {v.verdict} match {v.match_rate}%, unmute_lag {v.median_reaction_unmute_ms}ms, windows {len(v.mismatch_windows)}")
            if v.verdict == "FAIL":
                failures.append({
                    "file": v.file,
                    "run": v.run_label,
                    "scn_id": v.scn_id,
                    "match_rate": v.match_rate,
                    "n_mismatch_frames": v.n_mismatch_frames,
                    "n_over_mute_frames": v.n_over_mute_frames,
                    "n_under_mute_frames": v.n_under_mute_frames,
                    "max_unmute_lag_ms": v.max_reaction_unmute_ms,
                    "max_mute_lag_ms": v.max_reaction_mute_ms,
                    "category": "low_match_rate",
                    "video": v.video_path,
                })
        except Exception as e:
            print(f"[agg] FAIL {pq.name}: {e}")
            failures.append({"file": pq.name, "category": "analyze_error", "error": str(e)})
    n_analyze_errors = sum(1 for f in failures if f.get("category") == "analyze_error")

    if args.top_level:
        summary_path = runs_dir / "summary.md"
        failures_path = runs_dir / "failures.json"
    else:
        summary_path = out_dir / "summary.md"
        failures_path = out_dir / "failures.json"
    summary_path.write_text(render_summary(verdicts, top_level=args.top_level,
                                           n_analyze_errors=n_analyze_errors))
    failures_path.write_text(json.dumps({
        "total_clips": len(verdicts),
        "failed_clips": len(failures),
        "failures": failures,
    }, indent=2))
    print(f"[agg] wrote {summary_path} and {failures_path.name}")

    # In top-level mode, also emit per-run summary.md (links relative to <run>/reports/).
    if args.top_level:
        by_run: dict[str, list[ClipVerdict]] = {}
        for v in verdicts:
            by_run.setdefault(v.run_label, []).append(v)
        for run, vs in by_run.items():
            run_summary = runs_dir / run / "reports" / "summary.md"
            run_summary.write_text(render_summary(vs, top_level=False))
            print(f"[agg] wrote {run_summary}")


if __name__ == "__main__":
    main()
