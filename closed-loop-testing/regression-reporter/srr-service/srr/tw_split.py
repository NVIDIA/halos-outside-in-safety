# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Cut a continuous-run parquet into per-scene parquets at forklift TW crossings.

Each time forklift_x crosses the trailer tripwire (x = TW_X) is a scene boundary.
Consecutive boundaries delimit one scene.

Run inside SRR container:
    docker exec srr python3 -m srr.tw_split /app/runs/run-XYZ.parquet \\
        /app/runs/scenes/ --calib /app/calibration.json

Outputs:
  scenes/scn_0000.parquet, scenes/scn_0001.parquet, ...
  scenes/scene_index.csv         — t_start_wall/t_end_wall per scene
  scenes/scenes_manifest.csv     — adds ISO times + VST query templates

Configuration (precedence: --calib JSON > env var > default):
- TW_X            : x-coordinate of the tripwire (loaded from calibration.json
                    `sensors[0].tripwires[0].wire.p1.x` if --calib provided)
- VST_BASE_URL    : env var, e.g. http://<HOST_IP>:30888/vst/api (defined in
                    Halos compose .env; SRR inherits it via env_file)
- SENSORS         : env var (comma-separated), e.g. "Camera,Camera_01,Camera_02".
                    These names must match VSS topology — they're not in calibration.json.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import datetime as dt
from pathlib import Path
from typing import Optional

import pandas as pd


# Defaults — overridden by env vars at module load, optionally by --calib at runtime.
DEFAULT_TW_X = 9.574
DEFAULT_VST_BASE_URL = "http://127.0.0.1:30888/vst/api"
DEFAULT_SENSORS = "Camera,Camera_01,Camera_02"

PADDING_SEC = 1.0


def get_vst_base() -> str:
    return os.environ.get("VST_BASE_URL", DEFAULT_VST_BASE_URL).rstrip("/")


def get_sensors() -> tuple[str, ...]:
    raw = os.environ.get("SENSORS", DEFAULT_SENSORS)
    return tuple(s.strip() for s in raw.split(",") if s.strip())


def load_tw_x(calib_path: Optional[Path]) -> float:
    """Load the tripwire x-coordinate from calibration.json, fall back to default."""
    if calib_path and calib_path.exists():
        try:
            d = json.loads(calib_path.read_text())
            return float(d["sensors"][0]["tripwires"][0]["wire"]["p1"]["x"])
        except (KeyError, IndexError, json.JSONDecodeError, ValueError) as e:
            print(f"[tw_split] WARN: --calib failed to parse ({e}); using default TW_X={DEFAULT_TW_X}")
    return DEFAULT_TW_X


# Module-level (import-time) values — kept for backward compat with callers that
# import these names directly. Mutated by main() if --calib provided.
TW_X = DEFAULT_TW_X
VST_BASE = get_vst_base()
SENSORS = get_sensors()


def crossings(df: pd.DataFrame) -> list[tuple[int, float]]:
    """Return [(row_index, wall_time)] of each forklift_x TW crossing."""
    s = df["forklift_x"].dropna()
    side = (s > TW_X).astype(int).diff().fillna(0)
    cross = side[side != 0]
    return [(idx, float(df.loc[idx, "arrival_wall_time"])) for idx in cross.index]


def crossings_from_ba(df: pd.DataFrame) -> list[tuple[int, float]]:
    """Fallback boundary detector: each BA TW Forklift event = 1 boundary.

    Used when GT /gt/forklift/tf is frozen (known SRRGraph PubForklift
    bug — reads cached parent Xform translate while articulation moves
    only the body link). VSS perception still emits TripEvent events
    when forklift crosses the trailer tripwire on-camera.
    """
    import json as _json
    bounds: list[tuple[int, float]] = []
    for idx, evts_str in df["ba_events_json"].items():
        if not evts_str:
            continue
        for evt in _json.loads(evts_str):
            if "TW" in evt.get("event_types", []) and "Forklift" in evt.get("classes", []):
                bounds.append((idx, float(df.loc[idx, "arrival_wall_time"])))
                break
    # de-dupe near-duplicates within 0.5s
    deduped: list[tuple[int, float]] = []
    for b in bounds:
        if not deduped or b[1] - deduped[-1][1] > 0.5:
            deduped.append(b)
    return deduped


def split(parquet_in: Path, out_dir: Path, source: str = "gt") -> list[dict]:
    out_dir.mkdir(parents=True, exist_ok=True)
    df = pd.read_parquet(parquet_in)
    df = df.sort_values("arrival_wall_time").reset_index(drop=True)

    if source == "gt":
        bounds = crossings(df)
        src = "GT_forklift_tf"
    elif source == "ba":
        bounds = crossings_from_ba(df)
        src = "BA_TW_forklift"
    elif source == "auto":
        bounds = crossings(df)
        src = "GT_forklift_tf"
        if len(bounds) < 2:
            print("[tw_split] GT crossings <2 → auto-fallback BA TW Forklift events")
            bounds = crossings_from_ba(df)
            src = "BA_TW_forklift"
    else:
        raise ValueError(f"unknown source: {source}")
    print(f"[tw_split] {src} boundaries: {len(bounds)}")

    if len(bounds) < 2:
        print(f"[tw_split] WARN: <2 boundaries from {src} — 1 scene = whole run")
        bounds = [
            (0, float(df["arrival_wall_time"].iloc[0])),
            (len(df) - 1, float(df["arrival_wall_time"].iloc[-1])),
        ]

    scenes = []
    for i in range(len(bounds) - 1):
        t_start = bounds[i][1] - PADDING_SEC
        t_end = bounds[i + 1][1] + PADDING_SEC
        seg = df[
            (df["arrival_wall_time"] >= t_start)
            & (df["arrival_wall_time"] <= t_end)
        ].reset_index(drop=True)
        if len(seg) < 30:
            continue
        scn_id = f"scn_{i:04d}"
        path = out_dir / f"{scn_id}.parquet"
        seg.to_parquet(path)

        # Determine "side" of trailer — forklift entering or exiting?
        # Side at scene midpoint. forklift_x may be None/NaN when the forklift
        # GT is absent (e.g. out of sensor coverage) — treat as outside_trailer.
        mid = seg.iloc[len(seg) // 2]
        fk_x = mid["forklift_x"]
        forklift_side = "in_trailer" if (pd.notna(fk_x) and fk_x > TW_X) else "outside_trailer"

        scenes.append({
            "scenario_id":  scn_id,
            "t_start_wall": round(t_start, 6),
            "t_end_wall":   round(t_end, 6),
            "duration_sec": round(t_end - t_start, 2),
            "rows":         len(seg),
            "forklift_side": forklift_side,
            "parquet":      str(path.relative_to(out_dir.parent)),
        })

    if not scenes:
        print("[tw_split] no scenes survived min-row filter (≥30)")
        return []

    idx = pd.DataFrame(scenes)
    idx.to_csv(out_dir / "scene_index.csv", index=False)
    print(f"[tw_split] wrote {len(scenes)} scenes → {out_dir}")
    return scenes


def to_iso(ts_epoch: float) -> str:
    return dt.datetime.fromtimestamp(ts_epoch, tz=dt.timezone.utc).isoformat()


def write_manifest(scenes: list[dict], out_dir: Path) -> None:
    """Add ISO timestamps + VST playback URL templates per sensor."""
    rows = []
    for s in scenes:
        iso_start = to_iso(s["t_start_wall"])
        iso_end = to_iso(s["t_end_wall"])
        rec = {
            "scenario_id": s["scenario_id"],
            "iso_start": iso_start,
            "iso_end": iso_end,
            "epoch_start": s["t_start_wall"],
            "epoch_end": s["t_end_wall"],
            "duration_sec": s["duration_sec"],
            "rows": s["rows"],
            "forklift_side": s["forklift_side"],
            "parquet": s["parquet"],
        }
        for cam in SENSORS:
            # VST replaystream pattern (verify against deployed endpoint)
            rec[f"vst_query_{cam}"] = (
                f"{VST_BASE}/replaystream?sensor={cam}"
                f"&start={iso_start}&end={iso_end}"
            )
        rows.append(rec)

    mf = out_dir / "scenes_manifest.csv"
    pd.DataFrame(rows).to_csv(mf, index=False)
    print(f"[tw_split] wrote {mf}")


def main() -> None:
    global TW_X, VST_BASE, SENSORS
    ap = argparse.ArgumentParser()
    ap.add_argument("parquet_in", type=Path)
    ap.add_argument("out_dir", type=Path, nargs="?", default=None)
    ap.add_argument("--source", choices=["gt", "ba", "auto"], default="gt",
                    help="boundary source: gt (default, /gt/forklift/tf) | ba (Kafka mdx-events TW Forklift) | auto (gt → ba fallback)")
    ap.add_argument("--calib", type=Path, default=Path("/app/calibration.json"),
                    help="VSS calibration.json — loads tripwire X coord. Falls back to default %(default)s.")
    ap.add_argument("--vst-base-url",
                    help="Override VST base URL (default: $VST_BASE_URL or %(default)s).",
                    default=get_vst_base())
    ap.add_argument("--sensors",
                    help="Comma-separated sensor names for VST URL templates (default: $SENSORS or '%(default)s').",
                    default=os.environ.get("SENSORS", DEFAULT_SENSORS))
    args = ap.parse_args()

    # Apply runtime overrides to module-level constants used by split()/write_manifest()
    TW_X = load_tw_x(args.calib if args.calib and args.calib.exists() else None)
    VST_BASE = args.vst_base_url.rstrip("/")
    SENSORS = tuple(s.strip() for s in args.sensors.split(",") if s.strip())
    print(f"[tw_split] TW_X={TW_X} · VST={VST_BASE} · sensors={SENSORS}")

    out_dir = args.out_dir or (args.parquet_in.parent / "scenes")
    scenes = split(args.parquet_in, out_dir, source=args.source)
    if scenes:
        write_manifest(scenes, out_dir)


if __name__ == "__main__":
    main()
