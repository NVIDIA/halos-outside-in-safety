# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Per-clip evidence packager — converts parquet + run-level BA pool + pss.log
into a folder of human-readable files (CSV + JSON + raw text + MP4 copy)
that QA engineers can browse without opening parquet.

Output layout — clip_logs/ nests INSIDE each scenario, so everything for one
scenario stays together:
    <runs-dir>/<scenario>/clip_logs/
        index.json          per-scenario index (n_clips, verdict per clip, paths)
        <scn_id>/
            manifest.json   clip metadata (verdict, time bounds, GT summary, file index)
            gt_positions.csv per-sample positions (wall_time, sim_time, char/fk x/y, in_roi, in_trailer)
            psf_timeline.csv per-sample PSF state (wall_time, sim_time, is_muted, command, source)
            ba_events.csv   BA ROI/TW events from run-level pool (±5s pad), one row per event
            pss_log.txt     raw pss.log slice (UNFILTERED — all syslog lines in window)
            pss_log.jsonl   structured (ts, module, endpoint, msg) for safety modules only
            video.mp4       copy of clip MP4 (so zip is self-contained)

With --zip, the entire <runs-dir>/ (parquets + videos + reports + clip_logs all
together) is archived as <runs-dir>.zip placed in the parent runs/ folder.

Designed so that a future debug web UI (video + scrubbable timeline + multi-stream
log panels) can read manifest.json per clip and render directly.

Usage:
    docker exec srr python3 -m srr.clip_logs --runs-dir /app/runs/multi-test-XXX
    docker exec srr python3 -m srr.clip_logs --runs-dir /app/runs/multi-test-XXX --fails-only --zip
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import pandas as pd

from srr.aggregator import (
    BA_POOL_PAD_S,
    CHAR_COLS,
    DEFAULT_ROI_VERTICES,
    _GT_ACTOR_CLASS,
    compute_phase2_metrics,
    in_roi as _in_roi,
    load_roi,
    load_run_ba_pool,
    filter_ba_pool,
)
from shapely.geometry import Polygon


# pss.log line: 2026-05-04T05:23:43.519070+00:00 4u2g-0578 NVPSB_PSS_DAEMON[33]:  Timestamp: ... Data: ...
# Some modules log without the [PID] bracket (e.g. `nv_atl_client:`).
PSS_LINE_RE = re.compile(
    r"^(?P<iso>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+\+\d{2}:\d{2})\s+"
    r"(?P<host>\S+)\s+"
    r"(?P<module>[A-Za-z_][\w]*)(?:\[\d+\])?:\s*"
    r"(?P<rest>.*)$"
)

# Real syslog modules emitting safety-relevant lines (verified by scanning pss.log).
# NVPSB_PSS_SOURCE etc. are Endpoint: values inside Data, not syslog modules.
# comm-layer and psf-monitor read pss.log but don't write to it.
SAFETY_MODULES = {
    "NVPSB_PSS_DAEMON",
    "NVPSB_PSD_CLIENT",
    "nv_mdx_client",
    "nv_atl_client",
    "nvpss_daemon",
}


@dataclass
class ClipBounds:
    scn_id: str
    scenario: str
    wall_start: float
    wall_end: float
    sim_start: float
    sim_end: float
    duration_s: float


def _clip_bounds(parquet_path: Path) -> Optional[ClipBounds]:
    df = pd.read_parquet(parquet_path, columns=["arrival_wall_time", "sim_time"])
    if df.empty:
        return None
    return ClipBounds(
        scn_id=parquet_path.stem,
        scenario=parquet_path.parent.parent.name,
        wall_start=float(df["arrival_wall_time"].iloc[0]),
        wall_end=float(df["arrival_wall_time"].iloc[-1]),
        sim_start=float(df["sim_time"].iloc[0]),
        sim_end=float(df["sim_time"].iloc[-1]),
        duration_s=float(df["arrival_wall_time"].iloc[-1] - df["arrival_wall_time"].iloc[0]),
    )


def _write_gt_csv(df: pd.DataFrame, roi: Polygon, tw, out_path: Path) -> None:
    rows = []
    for _, r in df.iterrows():
        char_in = []
        for c in CHAR_COLS:
            x = r.get(f"{c}_x")
            y = r.get(f"{c}_y")
            char_in.append(_in_roi(roi, x, y))
        fx, fy = r.get("forklift_x"), r.get("forklift_y")
        in_trailer = bool(pd.notna(fx) and pd.notna(fy) and tw.is_inside(fx, fy))
        rows.append({
            "wall_time": r["arrival_wall_time"],
            "sim_time": r["sim_time"],
            "char_0_x": r.get("char_0_x"),
            "char_0_y": r.get("char_0_y"),
            "char_1_x": r.get("char_1_x"),
            "char_1_y": r.get("char_1_y"),
            "char_2_x": r.get("char_2_x"),
            "char_2_y": r.get("char_2_y"),
            "forklift_x": fx,
            "forklift_y": fy,
            "any_char_in_roi": any(char_in),
            "forklift_in_trailer": in_trailer,
            "expected_mute": in_trailer and not any(char_in),
        })
    pd.DataFrame(rows).to_csv(out_path, index=False)


# ---------- Phase 2 — mdx-bev detections + tracker timeline ----------

def _unique_bev_frames(df: pd.DataFrame) -> tuple[dict, list]:
    """Dedup the 30 Hz parquet to one entry per detector frame (bev_frame_id).

    Returns ``(frames, order)`` where ``frames[fid] = (wall_time, dets, gts)``
    keyed on the first sample that carried that detector frame, and ``order`` is
    the wall-time order of first appearance.
    """
    frames: dict = {}
    order: list = []
    for _, r in df.iterrows():
        fid = r.get("bev_frame_id")
        dj = r.get("detections_json")
        if fid is None or pd.isna(fid) or not isinstance(dj, str) or fid in frames:
            continue
        try:
            dets = json.loads(dj)
        except Exception:
            continue
        gts: dict = {}
        for a in _GT_ACTOR_CLASS:
            x, y = r.get(f"{a}_x"), r.get(f"{a}_y")
            if pd.notna(x) and pd.notna(y):
                gts[a] = (float(x), float(y))
        frames[fid] = (float(r["arrival_wall_time"]), dets, gts)
        order.append(fid)
    return frames, order


def _coverage_bbox(frames: dict, order: list, pad: float) -> Optional[tuple]:
    xs = [d["x"] for fid in order for d in frames[fid][1]]
    ys = [d["y"] for fid in order for d in frames[fid][1]]
    if not xs:
        return None
    return (min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad)


def _in_cov(cov: Optional[tuple], x: float, y: float) -> bool:
    return cov is None or (cov[0] <= x <= cov[1] and cov[2] <= y <= cov[3])


def _greedy_match(dets: list, gts: dict, gate: float) -> dict:
    """Greedy nearest per class. Returns ``{det_index: (gt_actor, dist)}``."""
    det_match: dict = {}
    for a, (gx, gy) in gts.items():
        cls = _GT_ACTOR_CLASS[a]
        best, best_d = None, gate
        for i, d in enumerate(dets):
            if i in det_match or d["class"] != cls:
                continue
            dist = ((d["x"] - gx) ** 2 + (d["y"] - gy) ** 2) ** 0.5
            if dist < best_d:
                best_d, best = dist, i
        if best is not None:
            det_match[best] = (a, round(best_d, 3))
    return det_match


def _write_detections_csv(df: pd.DataFrame, out_path: Path,
                          gate_m: float = 1.5, coverage_pad_m: float = 2.0) -> int:
    """One row per detection per detector frame, with GT match + coverage flag."""
    frames, order = _unique_bev_frames(df)
    cov = _coverage_bbox(frames, order, coverage_pad_m)
    rows = []
    for fid in order:
        wt, dets, gts = frames[fid]
        det_match = _greedy_match(dets, gts, gate_m)
        for i, d in enumerate(dets):
            m = det_match.get(i)
            rows.append({
                "wall_time": wt,
                "bev_frame_id": fid,
                "track_id": d["track_id"],
                "class": d["class"],
                "x": round(d["x"], 3),
                "y": round(d["y"], 3),
                "z": round(d.get("z") or 0.0, 3),
                "conf": d.get("conf"),
                "matched_gt": m[0] if m else "",
                "dist_m": m[1] if m else "",
                "in_coverage": _in_cov(cov, d["x"], d["y"]),
            })
    cols = ["wall_time", "bev_frame_id", "track_id", "class", "x", "y", "z",
            "conf", "matched_gt", "dist_m", "in_coverage"]
    pd.DataFrame(rows, columns=cols).to_csv(out_path, index=False)
    return len(order)


def _write_tracker_state_csv(df: pd.DataFrame, out_path: Path,
                             gate_m: float = 1.5) -> int:
    """One row per detector frame: track_id assigned to each GT actor over time."""
    frames, order = _unique_bev_frames(df)
    rows = []
    for fid in order:
        wt, dets, gts = frames[fid]
        det_match = _greedy_match(dets, gts, gate_m)
        actor_tid = {a: dets[i]["track_id"] for i, (a, _d) in det_match.items()}
        rows.append({
            "wall_time": wt,
            "bev_frame_id": fid,
            "n_dets": len(dets),
            "char_0_tid": actor_tid.get("char_0", ""),
            "char_1_tid": actor_tid.get("char_1", ""),
            "char_2_tid": actor_tid.get("char_2", ""),
            "forklift_tid": actor_tid.get("forklift", ""),
        })
    cols = ["wall_time", "bev_frame_id", "n_dets",
            "char_0_tid", "char_1_tid", "char_2_tid", "forklift_tid"]
    pd.DataFrame(rows, columns=cols).to_csv(out_path, index=False)
    return len(order)


def _write_ba_positions_csv(df: pd.DataFrame, out_path: Path,
                            gate_m: float = 1.5) -> int:
    """One row per BA-reported position (mdx-behavior) with nearest-GT match.

    De-duplicates consecutive identical snapshots (30 Hz oversampling). For each
    BA point we record the nearest GT actor within ``gate_m`` and the error, so
    the viewer can show BA localisation accuracy alongside the raw detector.
    """
    if "ba_positions_json" not in df.columns:
        return 0
    rows = []
    n_frames = 0
    last_snap = None
    for _, r in df.iterrows():
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
        if not bps:
            continue
        n_frames += 1
        wt = float(r["arrival_wall_time"])
        for p in bps:
            px, py = p.get("x"), p.get("y")
            best_a, best_d = "", ""
            if px is not None and py is not None and gts:
                bd = gate_m
                for a, (gx, gy) in gts.items():
                    d = ((px - gx) ** 2 + (py - gy) ** 2) ** 0.5
                    if d < bd:
                        bd, best_a = d, a
                if best_a:
                    best_d = round(bd, 3)
            rows.append({
                "wall_time": wt,
                "track_id": p.get("track_id"),
                "x": round(px, 3) if px is not None else "",
                "y": round(py, 3) if py is not None else "",
                "speed": p.get("speed"),
                "direction": p.get("direction") or "",
                "matched_gt": best_a,
                "dist_m": best_d,
            })
    cols = ["wall_time", "track_id", "x", "y", "speed", "direction",
            "matched_gt", "dist_m"]
    pd.DataFrame(rows, columns=cols).to_csv(out_path, index=False)
    return n_frames


def _write_psf_timeline(df: pd.DataFrame, out_path: Path) -> None:
    rows = []
    for _, r in df.iterrows():
        cmd_raw = r.get("psf_command")
        cmd_obj = {}
        if isinstance(cmd_raw, str) and cmd_raw.strip().startswith("{"):
            try:
                cmd_obj = json.loads(cmd_raw)
            except json.JSONDecodeError:
                pass
        rows.append({
            "wall_time": r["arrival_wall_time"],
            "sim_time": r["sim_time"],
            "is_muted": bool(r.get("is_muted")) if pd.notna(r.get("is_muted")) else None,
            "command_name": cmd_obj.get("command_name"),
            "status_name": cmd_obj.get("status_name"),
            "source": cmd_obj.get("source"),
            "is_alarm": cmd_obj.get("is_alarm"),
            "sequence": cmd_obj.get("sequence"),
            "ros_time": cmd_obj.get("ros_time"),
        })
    pd.DataFrame(rows).to_csv(out_path, index=False)


def _write_ba_events(pool: list[dict], bounds: ClipBounds, pad: float,
                     out_path: Path) -> int:
    events = filter_ba_pool(pool, bounds.wall_start, bounds.wall_end, pad=pad)
    rows = []
    for e in events:
        ts = e.get("_kafka_ts", 0.0)
        in_clip = bounds.wall_start <= ts <= bounds.wall_end
        types = e.get("event_types") or ([e["event_type"]] if e.get("event_type") else [None])
        classes = e.get("classes") or []
        ids = e.get("ids") or []
        cls = ",".join(c for c in classes if c)
        idstr = ",".join(i for i in ids if i)
        for t in types:
            rows.append({
                "wall_time": ts,
                "sim_time_offset": round(ts - bounds.wall_start, 4),
                "in_clip": in_clip,
                "event_type": t,
                "classes": cls,
                "direction": e.get("direction") or "",
                "ids": idstr,
                "create_time": e.get("create_time"),
                "raw_hex": e.get("_raw_first_60b_hex", ""),
            })
    rows.sort(key=lambda r: r["wall_time"])
    pd.DataFrame(rows).to_csv(out_path, index=False)
    return len(rows)


def _slice_pss_log(pss_lines: list[tuple[float, str]], wall_start: float,
                   wall_end: float, txt_path: Path, jsonl_path: Path) -> tuple[int, int]:
    lo = wall_start
    hi = wall_end
    raw_count = 0
    json_count = 0
    txt_path.parent.mkdir(parents=True, exist_ok=True)
    with txt_path.open("w") as txtf, jsonl_path.open("w") as jsonf:
        for ts, raw in pss_lines:
            if ts < lo:
                continue
            if ts > hi:
                break
            txtf.write(raw)
            if not raw.endswith("\n"):
                txtf.write("\n")
            raw_count += 1
            m = PSS_LINE_RE.match(raw)
            if not m:
                continue
            module = m.group("module")
            if module not in SAFETY_MODULES:
                continue
            rest = m.group("rest")
            entry = {
                "wall_time": ts,
                "iso": m.group("iso"),
                "module": module,
            }
            # PSF body format: "Timestamp: <inner> Endpoint: <ep> Data: <msg>"
            ep_idx = rest.find("Endpoint:")
            data_idx = rest.find("Data:")
            if ep_idx >= 0 and data_idx > ep_idx:
                entry["endpoint"] = rest[ep_idx + 9:data_idx].strip()
            if data_idx >= 0:
                entry["msg"] = rest[data_idx + 5:].strip()
            else:
                entry["msg"] = rest.strip()
            jsonf.write(json.dumps(entry) + "\n")
            json_count += 1
    return raw_count, json_count


def _load_pss_log(pss_path: Path, wall_lo: float, wall_hi: float) -> list[tuple[float, str]]:
    """Return [(epoch_ts, raw_line), ...] for lines whose ISO ts ∈ [wall_lo, wall_hi].

    Streams the file once; lines outside the window are skipped early to keep
    memory bounded (pss.log can be 100+ MB)."""
    if not pss_path.exists():
        return []
    out: list[tuple[float, str]] = []
    with pss_path.open() as f:
        for raw in f:
            if len(raw) < 30 or raw[4] != "-" or raw[10] != "T":
                continue
            try:
                ts = pd.Timestamp(raw[:32]).timestamp()
            except (ValueError, TypeError):
                continue
            if ts < wall_lo:
                continue
            if ts > wall_hi:
                break
            out.append((ts, raw))
    return out


_VERDICT_RE = re.compile(
    r"\*\*Verdict\*\*:\s*[^A-Z]*(PASS|NEAR|FAIL)\s*·\s*match\s*\*\*([\d.]+)%\*\*"
)


def _load_verdict(parquet_path: Path) -> dict:
    """Look up the per-clip report markdown (rendered by aggregator) for verdict + match%.

    Aggregator format (render_clip_report):
        **Verdict**: ❌ FAIL · match **0.12%**
    """
    report_path = parquet_path.parent.parent / "reports" / f"{parquet_path.stem}.md"
    if not report_path.exists():
        return {}
    m = _VERDICT_RE.search(report_path.read_text())
    if not m:
        return {}
    return {"verdict": m.group(1), "match_pct": float(m.group(2))}


def _gt_summary(gt_csv: Path) -> dict:
    df = pd.read_csv(gt_csv)
    if df.empty:
        return {}
    return {
        "char_in_roi_pct": round(100.0 * df["any_char_in_roi"].mean(), 2),
        "forklift_in_trailer_pct": round(100.0 * df["forklift_in_trailer"].mean(), 2),
        "expected_mute_pct": round(100.0 * df["expected_mute"].mean(), 2),
        "n_samples": len(df),
    }


def _emit_clip(parquet_path: Path, out_dir: Path, roi: Polygon, tw, pool: list[dict],
               pss_path: Optional[Path], copy_mp4: bool, pad: float) -> Optional[dict]:
    bounds = _clip_bounds(parquet_path)
    if bounds is None:
        print(f"[clip_logs] skip empty {parquet_path.name}")
        return None

    df = pd.read_parquet(parquet_path)
    out_dir.mkdir(parents=True, exist_ok=True)

    gt_csv = out_dir / "gt_positions.csv"
    psf_csv = out_dir / "psf_timeline.csv"
    ba_csv = out_dir / "ba_events.csv"

    _write_gt_csv(df, roi, tw, gt_csv)
    _write_psf_timeline(df, psf_csv)
    n_ba = _write_ba_events(pool, bounds, pad, ba_csv)

    # Phase 2 — mdx-bev detections + tracker timeline (only for 3D runs).
    has_phase2 = "detections_json" in df.columns and df["detections_json"].notna().any()
    n_det_frames = 0
    has_ba_pos = False
    if has_phase2:
        n_det_frames = _write_detections_csv(df, out_dir / "detections.csv")
        _write_tracker_state_csv(df, out_dir / "tracker_state.csv")
        has_ba_pos = ("ba_positions_json" in df.columns
                      and df["ba_positions_json"].notna().any())
        if has_ba_pos:
            _write_ba_positions_csv(df, out_dir / "ba_positions.csv")

    pss_txt = out_dir / "pss_log.txt"
    pss_jsonl = out_dir / "pss_log.jsonl"
    n_raw = n_json = 0
    if pss_path is not None:
        slice_lines = _load_pss_log(pss_path, bounds.wall_start - pad, bounds.wall_end + pad)
        n_raw, n_json = _slice_pss_log(slice_lines, bounds.wall_start - pad,
                                       bounds.wall_end + pad, pss_txt, pss_jsonl)

    # Video path: vst_video.py emits to <run>/videos/scenes/<scn>.mp4 (current layout).
    # Older runs may have put it directly under videos/<scn>.mp4 — accept both.
    run_dir = parquet_path.parent.parent
    video_src = run_dir / "videos" / "scenes" / f"{bounds.scn_id}.mp4"
    if not video_src.exists():
        video_src = run_dir / "videos" / f"{bounds.scn_id}.mp4"
    video_dst_name = ""
    if video_src.exists() and copy_mp4:
        shutil.copy2(video_src, out_dir / "video.mp4")
        video_dst_name = "video.mp4"
    elif (out_dir / "video.mp4").exists():
        # Re-export over an existing bundle (e.g. --no-mp4): the clip already
        # has its video — keep the manifest pointing at it instead of blanking
        # the reference and disconnecting the viewer.
        video_dst_name = "video.mp4"

    verdict = _load_verdict(parquet_path)
    gt_sum = _gt_summary(gt_csv)

    # Compact per-clip perception metrics for the viewer's summary panel.
    phase2_brief = None
    if has_phase2:
        try:
            p2 = compute_phase2_metrics(df, tw=tw)
        except Exception:
            p2 = None
        if p2 and p2.get("enabled") and p2.get("gt_available"):
            pc = p2.get("per_class") or {}
            phase2_brief = {
                "recall_blended": p2.get("recall"),
                "recall_at_gate": p2.get("recall_at_gate"),
                "per_class": {c: {"recall": d.get("recall"),
                                  "track_loss_pct": d.get("track_loss_pct"),
                                  "tracking_ok_pct": d.get("tracking_ok_pct"),
                                  "pos_offset_m": (d.get("pos") or {}).get("median"),
                                  "pos_jitter_p95_m": (d.get("pos") or {}).get("p95")}
                              for c, d in pc.items()},
                "forklift_boundary": p2.get("forklift_boundary"),
                "split": p2.get("split"),
                "total_id_switches": p2.get("total_id_switches"),
                "tracking_ok_pct": p2.get("tracking_ok_pct"),
                "coverage_mode": p2.get("coverage_mode"),
                "coverage_pad_m": p2.get("coverage_pad_m"),
                "forklift_origin_offset_m": p2.get("forklift_origin_offset_m"),
            }

    manifest = {
        "clip_id": bounds.scn_id,
        "scenario": bounds.scenario,
        "wall_time_start": bounds.wall_start,
        "wall_time_end": bounds.wall_end,
        "sim_time_start": bounds.sim_start,
        "sim_time_end": bounds.sim_end,
        "duration_s": bounds.duration_s,
        "padding_s": pad,
        "pad_wall_start": bounds.wall_start - pad,
        "pad_wall_end": bounds.wall_end + pad,
        "verdict": verdict.get("verdict"),
        "match_pct": verdict.get("match_pct"),
        "video": video_dst_name,
        "streams": {
            "gt_positions": "gt_positions.csv",
            "psf_timeline": "psf_timeline.csv",
            "ba_events": "ba_events.csv",
            "detections": "detections.csv" if has_phase2 else None,
            "tracker_state": "tracker_state.csv" if has_phase2 else None,
            "ba_positions": "ba_positions.csv" if has_ba_pos else None,
            "pss_log_text": "pss_log.txt" if pss_path else None,
            "pss_log_structured": "pss_log.jsonl" if pss_path else None,
        },
        "counts": {
            "samples": len(df),
            "ba_events": n_ba,
            "detector_frames": n_det_frames,
            "pss_lines_raw": n_raw,
            "pss_lines_safety": n_json,
        },
        "gt_summary": gt_sum,
        "phase2_metrics": phase2_brief,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"[clip_logs] {bounds.scenario}/{bounds.scn_id}: "
          f"samples={len(df)}, ba={n_ba}, pss_raw={n_raw}, video={'y' if video_dst_name else 'n'}")
    return {
        "clip_id": bounds.scn_id,
        "scenario": bounds.scenario,
        "verdict": verdict.get("verdict"),
        "match_pct": verdict.get("match_pct"),
        "path": f"{bounds.scenario}/{bounds.scn_id}/",
    }


def _zip_dir(src_dir: Path, zip_path: Path) -> None:
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in sorted(src_dir.rglob("*")):
            if p.is_file():
                zf.write(p, p.relative_to(src_dir.parent))
    print(f"[clip_logs] wrote {zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--runs-dir", required=True,
                   help="Multi-test root, e.g. /app/runs/multi-test-XXX")
    p.add_argument("--out-name", default="clip_logs",
                   help="Output subdir under --runs-dir (default: clip_logs)")
    p.add_argument("--scenarios", default="",
                   help="Comma-separated scenario filter (e.g. 'in-roi-5min,balanced-10min'); empty = all")
    p.add_argument("--fails-only", action="store_true",
                   help="Only emit clips marked FAIL in failures.json")
    p.add_argument("--no-mp4", action="store_true", help="Skip MP4 copy (slimmer output)")
    p.add_argument("--pad", type=float, default=BA_POOL_PAD_S,
                   help=f"Padding seconds for BA pool + pss.log slice (default {BA_POOL_PAD_S})")
    p.add_argument("--pss-log", default="",
                   help="pss.log path. If empty, prefer <runs-dir>/pss.log (snapshot from run_multi.sh) "
                        "and fall back to $SIL_DATA_DIR/psf-log/pss.log (host bind-mount, if set) or "
                        "/var/log/pss.log inside container.")
    p.add_argument("--zip", action="store_true",
                   help="Also write <runs-dir>/<out-name>.zip self-contained archive")
    p.add_argument("--calib", default="/app/calibration.json")
    args = p.parse_args()

    runs_dir = Path(args.runs_dir)
    if not runs_dir.is_dir():
        raise SystemExit(f"runs-dir not found: {runs_dir}")

    roi, tw = load_roi(Path(args.calib) if args.calib else None)

    scen_filter = {s.strip() for s in args.scenarios.split(",") if s.strip()}
    fail_set: set[tuple[str, str]] = set()
    if args.fails_only:
        fjson = runs_dir / "failures.json"
        if not fjson.exists():
            raise SystemExit(f"--fails-only requires {fjson} (run aggregator --top-level first)")
        d = json.loads(fjson.read_text())
        fail_set = {(f.get("run", ""), f.get("scn_id", "")) for f in d.get("failures", [])}
        print(f"[clip_logs] fails-only: {len(fail_set)} failed clip(s)")

    parquets = sorted(runs_dir.glob("*/scenes/scn_*.parquet"))
    if not parquets:
        raise SystemExit(f"No scn_*.parquet found under {runs_dir}/<run>/scenes/")

    if args.pss_log:
        candidates = [Path(args.pss_log)]
    else:
        candidates = [
            runs_dir / "pss.log",                                       # snapshot taken at end of multi-test
        ]
        sil_data_dir = os.environ.get("SIL_DATA_DIR", "")
        if sil_data_dir:                                                # host bind-mount of safety-core /var/log/pss.log
            candidates.append(Path(sil_data_dir) / "psf-log" / "pss.log")
        candidates.append(Path("/var/log/pss.log"))                     # SRR container internal
    pss_path: Optional[Path] = next((c for c in candidates if c.exists()), None)
    if pss_path is None:
        print(f"[clip_logs] WARN no pss.log found in {[str(c) for c in candidates]}; skipping log slice")
    else:
        print(f"[clip_logs] using pss.log at {pss_path}")

    pool_cache: dict[str, list[dict]] = {}
    indexes_by_scen: dict[str, list[dict]] = {}

    for pq in parquets:
        scen = pq.parent.parent.name
        if scen_filter and scen not in scen_filter:
            continue
        if args.fails_only and (scen, pq.stem) not in fail_set:
            continue
        run_dir = pq.parent.parent
        if scen not in pool_cache:
            pool_cache[scen] = load_run_ba_pool(run_dir)
            print(f"[clip_logs] BA pool {scen}: {len(pool_cache[scen])} events")
        clip_dir = run_dir / args.out_name / pq.stem
        rec = _emit_clip(pq, clip_dir, roi, tw,
                         pool_cache[scen], pss_path, copy_mp4=not args.no_mp4, pad=args.pad)
        if rec is not None:
            indexes_by_scen.setdefault(scen, []).append(rec)

    total = 0
    for scen, clips in indexes_by_scen.items():
        idx_path = runs_dir / scen / args.out_name / "index.json"
        idx_path.write_text(json.dumps({
            "runs_dir": runs_dir.name,
            "scenario": scen,
            "n_clips": len(clips),
            "pad_s": args.pad,
            "filter": {
                "scenarios": sorted(scen_filter) if scen_filter else None,
                "fails_only": args.fails_only,
            },
            "clips": clips,
        }, indent=2))
        total += len(clips)
        print(f"[clip_logs] wrote {idx_path} ({len(clips)} clips)")
    print(f"[clip_logs] total {total} clip(s) across {len(indexes_by_scen)} scenario(s)")

    if args.zip:
        # Zip the WHOLE multi-test dir (parquets + videos + reports + clip_logs all together)
        # so one archive is a complete handoff to QA.
        zip_path = runs_dir.parent / f"{runs_dir.name}.zip"
        _zip_dir(runs_dir, zip_path)


if __name__ == "__main__":
    main()
