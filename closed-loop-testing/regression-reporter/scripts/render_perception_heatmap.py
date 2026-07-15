#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Render detect/track perception heatmaps in world coordinates.

For a multi-test run directory produced by SRR (clip_logs/ subtree per
scenario), this script aggregates per-actor, per-frame matched / missed /
tracker-id behaviour into a 2D world-coord histogram, then renders 4 heatmaps
per scenario (+ a cross-scenario combined set):

    - Person   detect-fail %     (was the GT seen at all?)
    - Person   tracking-loss %   (of seen frames, % on the dominant track-id)
    - Forklift detect-fail %
    - Forklift tracking-loss %

**Default render is in world coordinates (meters)** — axes are world XY, FOV
polygons from `calibration.json` are drawn rigorously, ROI rectangle and
tripwire X line shown. No floor-plan background by default because the
world→`Top.png`-pixel transform is not authoritatively documented in
`calibration.json` (the `imageCoordinates` ↔ `globalCoordinates` anchors are
3D points projected to one of the camera images, not Top.png floor-plan
pixels). Pass `--floorplan <Top.png>` plus a hand-calibrated transform via
`--floorplan-corners` to optionally overlay it.

Coverage-mask comparison:
SRR's current `compute_phase2_metrics` uses the convex hull of detections as
the coverage region, NOT the true camera FOV from calibration. Pass
`--fov-mask` to mask bins outside the union of `fieldOfViewPolygon`s from
calibration — that lets you see what the heatmap would look like with a
proper FOV-derived coverage gate. **This is a comparison aid**, not a
verdict on whether the FOV polygons themselves are the correct ground
truth (their occlusion handling — multipolygon-with-holes — still needs
ground-truth verification against scene geometry).

Data inputs (per clip, under <runs>/<scenario>/clip_logs/<scn_id>/):
    gt_positions.csv     per-GT-frame world XY for each actor (30 Hz)
    tracker_state.csv    per-detector-frame: tracker id per actor (NaN = missed)

Calibration input:
    calibration.json     per-camera fieldOfViewPolygon (WKT MULTIPOLYGON in
                         world coords, with interior holes accounting for
                         occlusion from scene geometry per VST convention).

Usage (inside srr container — pip install pillow matplotlib first):
    docker exec srr python3 /app/scripts/render_perception_heatmap.py \\
        --runs-dir /app/runs/multi-test-20260612-044956 \\
        --calib    /app/calibration.json \\
        --out      /app/runs/multi-test-20260612-044956/perception_heatmaps

Add `--fov-mask` to mask bins outside any FOV polygon. Add `--scenario X` to
restrict to one scenario.
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.colors import LinearSegmentedColormap
from shapely import wkt
from shapely.geometry import Point, Polygon
from shapely.ops import unary_union


log = logging.getLogger("perception-heatmap")


# Actors recorded in gt_positions.csv ↔ class in detections.csv / tracker col.
ACTORS = {
    "char_0":   "Person",
    "char_1":   "Person",
    "char_2":   "Person",
    "forklift": "Forklift",
}
CLASS_ACTORS = {
    "Person":   ["char_0", "char_1", "char_2"],
    "Forklift": ["forklift"],
}


# ───────────────────────────────────────────────────────────────────────────
# Calibration parsing — extract per-camera FOV polygons + their union
# ───────────────────────────────────────────────────────────────────────────


class Calibration:
    def __init__(self, path: Path):
        d = json.loads(path.read_text())
        sensors = d.get("sensors", [])
        if not sensors:
            raise ValueError(f"no sensors in {path}")
        self.cameras: list[dict] = []
        for s in sensors:
            attrs = {a["name"]: a["value"] for a in s.get("attributes", [])}
            fov_geom = None
            fov_wkt = attrs.get("fieldOfViewPolygon", "")
            if fov_wkt:
                try:
                    fov_geom = wkt.loads(fov_wkt)
                except Exception as exc:  # pragma: no cover
                    log.warning("FOV WKT parse failed for %s: %s", s.get("id"), exc)
            coords = s.get("coordinates") or {}
            self.cameras.append({
                "id":  s.get("id"),
                "x":   float(coords.get("x", 0)),
                "y":   float(coords.get("y", 0)),
                "fov": fov_geom,
            })
        polys = [c["fov"] for c in self.cameras if c["fov"] is not None]
        self.fov_union = unary_union(polys) if polys else None


# ───────────────────────────────────────────────────────────────────────────
# Per-clip aggregation
# ───────────────────────────────────────────────────────────────────────────


def _dominant_tid_per_actor(tracker_df: pd.DataFrame) -> dict[str, int | None]:
    out: dict[str, int | None] = {}
    for actor in ACTORS:
        col = f"{actor}_tid"
        if col not in tracker_df.columns:
            out[actor] = None
            continue
        ids = tracker_df[col].dropna().astype(int).tolist()
        out[actor] = Counter(ids).most_common(1)[0][0] if ids else None
    return out


def _aggregate_clip(clip_dir: Path, accum: dict, world_bins: tuple[np.ndarray, np.ndarray]) -> None:
    gt_p = clip_dir / "gt_positions.csv"
    tr_p = clip_dir / "tracker_state.csv"
    if not gt_p.exists() or not tr_p.exists():
        log.debug("skip clip %s (missing csv)", clip_dir.name)
        return
    gt = pd.read_csv(gt_p)
    tr = pd.read_csv(tr_p)
    if gt.empty or tr.empty:
        return

    gt_sorted = gt.sort_values("wall_time").reset_index(drop=True)
    gt_wall = gt_sorted["wall_time"].to_numpy()
    tr_sorted = tr.sort_values("wall_time").reset_index(drop=True)

    dom = _dominant_tid_per_actor(tr_sorted)
    x_edges, y_edges = world_bins

    for _, row in tr_sorted.iterrows():
        wt = row["wall_time"]
        idx = int(np.clip(np.searchsorted(gt_wall, wt), 0, len(gt_wall) - 1))
        if idx > 0 and abs(gt_wall[idx - 1] - wt) < abs(gt_wall[idx] - wt):
            idx -= 1
        gt_row = gt_sorted.iloc[idx]
        if abs(gt_row["wall_time"] - wt) > 0.2:
            continue  # GT gap > 200 ms — skip rather than trust stale pose.

        for actor, cls in ACTORS.items():
            gx = gt_row.get(f"{actor}_x")
            gy = gt_row.get(f"{actor}_y")
            if pd.isna(gx) or pd.isna(gy):
                continue
            tid = row.get(f"{actor}_tid")
            matched = pd.notna(tid)
            track_ok = matched and dom[actor] is not None and int(tid) == dom[actor]

            ix = int(np.searchsorted(x_edges, gx) - 1)
            iy = int(np.searchsorted(y_edges, gy) - 1)
            if (ix < 0 or iy < 0
                    or ix >= accum[cls]["visits"].shape[0]
                    or iy >= accum[cls]["visits"].shape[1]):
                continue
            accum[cls]["visits"][ix, iy] += 1
            if matched:
                accum[cls]["matched"][ix, iy] += 1
                if track_ok:
                    accum[cls]["track_ok"][ix, iy] += 1


# ───────────────────────────────────────────────────────────────────────────
# Rendering — world coords, optional Top.png overlay
# ───────────────────────────────────────────────────────────────────────────


_FAIL_CMAP = LinearSegmentedColormap.from_list(
    "srr_fail",
    [(0.0, "#1e9c4a"),
     (0.4, "#f7e93e"),
     (1.0, "#d83b3b")],
)

# Sequential map for occupancy density. Low end is a visible blue (not black)
# so 1-visit bins stand out from the dark masked/empty background.
_DENSITY_CMAP = LinearSegmentedColormap.from_list(
    "srr_density",
    [(0.0, "#2c3e8c"),
     (0.35, "#1f9e89"),
     (0.7, "#8fd744"),
     (1.0, "#fde725")],
)


def _polygon_to_xy(poly) -> list[np.ndarray]:
    """Flatten shapely Polygon / MultiPolygon to list of (N,2) ring arrays."""
    rings: list[np.ndarray] = []
    if poly is None or poly.is_empty:
        return rings
    if poly.geom_type == "MultiPolygon":
        for sub in poly.geoms:
            rings.extend(_polygon_to_xy(sub))
        return rings
    if poly.exterior is not None:
        rings.append(np.array(poly.exterior.coords))
    for hole in poly.interiors:
        rings.append(np.array(hole.coords))
    return rings


def _fov_mask_grid(fov_union, x_edges: np.ndarray, y_edges: np.ndarray) -> np.ndarray:
    """Boolean grid (Nx, Ny): True if bin centre is inside fov_union."""
    Nx = len(x_edges) - 1
    Ny = len(y_edges) - 1
    out = np.zeros((Nx, Ny), dtype=bool)
    if fov_union is None:
        return out
    xc = 0.5 * (x_edges[:-1] + x_edges[1:])
    yc = 0.5 * (y_edges[:-1] + y_edges[1:])
    for i, x in enumerate(xc):
        for j, y in enumerate(yc):
            if fov_union.covers(Point(x, y)):
                out[i, j] = True
    return out


def _render_heatmap(*, title: str, fail_pct: np.ndarray, visits: np.ndarray,
                    x_edges: np.ndarray, y_edges: np.ndarray,
                    calib: Calibration, fov_mask: np.ndarray | None,
                    min_visits: int, out_path: Path, subtitle: str = "",
                    cmap=_FAIL_CMAP, vmin: float = 0.0, vmax: float = 100.0,
                    cbar_label: str = "%") -> None:
    fig, ax = plt.subplots(figsize=(11, 9), dpi=140)
    fig.patch.set_facecolor("#f5f5f5")
    ax.set_facecolor("#1c1c1c")

    # Mask: low-visit bins, plus optional fov-mask exclusion.
    mask = visits < min_visits
    if fov_mask is not None:
        mask = mask | (~fov_mask)
    masked = np.ma.masked_where(mask, fail_pct)
    cmesh = ax.pcolormesh(x_edges, y_edges, masked.T,
                          cmap=cmap, vmin=vmin, vmax=vmax,
                          shading="auto", alpha=0.92)

    # FOV outlines, one color per camera.
    cam_colors = ["#3aa1f0", "#f0823a", "#a13af0"]
    for c, color in zip(calib.cameras, cam_colors):
        for ring in _polygon_to_xy(c["fov"]):
            ax.plot(ring[:, 0], ring[:, 1], color=color, lw=1.6,
                    alpha=0.9, solid_capstyle="round", solid_joinstyle="round")
        ax.plot(c["x"], c["y"], marker="^", color=color, markersize=11,
                markeredgecolor="white", markeredgewidth=0.9, zorder=6)

    # ROI rectangle + tripwire X line.
    roi_x0, roi_x1 = 4.877, 9.574
    roi_y0, roi_y1 = -18.976, -11.239
    ax.add_patch(mpatches.Rectangle((roi_x0, roi_y0),
                                    roi_x1 - roi_x0, roi_y1 - roi_y0,
                                    fill=False, ec="white", lw=1.3, ls=":", zorder=5))
    ax.axvline(9.574, color="white", lw=1.0, ls="--", alpha=0.85, zorder=5)

    ax.set_xlabel("world x (m)", fontsize=10)
    ax.set_ylabel("world y (m)", fontsize=10)
    ax.set_xlim(x_edges[0], x_edges[-1])
    ax.set_ylim(y_edges[0], y_edges[-1])
    ax.set_aspect("equal")
    ax.grid(True, color="#444", lw=0.4, alpha=0.5)

    full_title = title + (f"\n{subtitle}" if subtitle else "")
    ax.set_title(full_title, fontsize=11, loc="left", color="#222")

    cb = plt.colorbar(cmesh, ax=ax, fraction=0.038, pad=0.02)
    cb.set_label(cbar_label, rotation=90, labelpad=12)

    legend = [
        mpatches.Patch(color=cam_colors[i], label=calib.cameras[i]["id"] + " FOV")
        for i in range(min(3, len(calib.cameras)))
    ]
    legend.append(mpatches.Patch(color="white", label="ROI / tripwire"))
    if fov_mask is not None:
        legend.append(mpatches.Patch(color="#1c1c1c",
                                     label="(masked: outside FOV union)"))
    ax.legend(handles=legend, loc="upper right", fontsize=8,
              framealpha=0.85, facecolor="#2a2a2a", edgecolor="none",
              labelcolor="white")

    plt.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    log.info("wrote %s", out_path)


# ───────────────────────────────────────────────────────────────────────────
# Driver
# ───────────────────────────────────────────────────────────────────────────


def _collect_clip_dirs(runs_dir: Path, scenario_filter: str | None) -> list[Path]:
    out: list[Path] = []
    for scn_dir in sorted(runs_dir.iterdir()):
        if not scn_dir.is_dir():
            continue
        if scenario_filter and scn_dir.name != scenario_filter:
            continue
        cl = scn_dir / "clip_logs"
        if not cl.is_dir():
            continue
        for clip_dir in sorted(cl.iterdir()):
            if clip_dir.is_dir():
                out.append(clip_dir)
    return out


def _new_accum(grid_shape: tuple[int, int]) -> dict:
    return {
        cls: {
            "visits":   np.zeros(grid_shape, dtype=np.int64),
            "matched":  np.zeros(grid_shape, dtype=np.int64),
            "track_ok": np.zeros(grid_shape, dtype=np.int64),
        }
        for cls in CLASS_ACTORS
    }


def _render_set(*, accum: dict, label: str, out_dir: Path, calib: Calibration,
                fov_mask: np.ndarray | None,
                x_edges: np.ndarray, y_edges: np.ndarray,
                min_visits: int, density: bool = False) -> None:
    for cls in CLASS_ACTORS:
        visits = accum[cls]["visits"]
        matched = accum[cls]["matched"]
        track_ok = accum[cls]["track_ok"]

        # Occupancy density: how often each bin was occupied by this class.
        # This is the "where do people actually concentrate" view — the fail/
        # loss maps colour ANY visited bin, so they read as "crowded" even when
        # a cell was only crossed a few times. Density answers that directly.
        if density:
            visited = visits[visits > 0]
            # cap the scale at the 98th percentile so a couple of hot cells
            # (e.g. a spawn/idle point) don't wash out the rest.
            vmax = float(np.percentile(visited, 98)) if visited.size else 1.0
            vmax = max(vmax, 1.0)
            dens_sub = (f"{label}   ·   {cls} GT frames {int(visits.sum())}   ·   "
                        f"occupancy = frames/bin (cap p98={vmax:.0f})"
                        f"{'' if fov_mask is None else '  ·  FOV-mask ON'}")
            _render_heatmap(title=f"{cls} — occupancy density (frames per bin; brighter = more time spent)",
                            fail_pct=visits.astype(float), visits=visits,
                            x_edges=x_edges, y_edges=y_edges, calib=calib,
                            fov_mask=fov_mask, min_visits=1,
                            out_path=out_dir / f"{cls.lower()}_density.png",
                            subtitle=dens_sub, cmap=_DENSITY_CMAP,
                            vmin=0.0, vmax=vmax, cbar_label="frames / bin")

        with np.errstate(divide="ignore", invalid="ignore"):
            detect_fail_pct = 100.0 * (1.0 - matched / np.maximum(visits, 1))
            tracking_loss_pct = 100.0 * (1.0 - track_ok / np.maximum(matched, 1))

        # Headline aggregates: when --fov-mask is on, restrict the sum to bins
        # inside the FOV union so the % matches what the rendered map shows
        # (i.e. bins outside the sensor FOV are excluded from the headline %).
        # Without the mask, all in-grid bins count (closest to aggregator's recall_raw).
        if fov_mask is not None:
            n_visits   = int(visits[fov_mask].sum())
            n_matched  = int(matched[fov_mask].sum())
            n_track_ok = int(track_ok[fov_mask].sum())
        else:
            n_visits   = int(visits.sum())
            n_matched  = int(matched.sum())
            n_track_ok = int(track_ok.sum())
        overall_det = 100.0 * (1.0 - n_matched / max(n_visits, 1))
        overall_trk = 100.0 * (1.0 - n_track_ok / max(n_matched, 1))

        mask_label = "" if fov_mask is None else "  ·  FOV-mask ON"
        det_sub = (f"{label}   ·   GT frames {n_visits}   ·   "
                   f"overall detect-fail {overall_det:.1f} %   ·   "
                   f"min visits/bin {min_visits}{mask_label}")
        trk_sub = (f"{label}   ·   matched frames {n_matched}   ·   "
                   f"overall tracking-loss {overall_trk:.1f} %   ·   "
                   f"min visits/bin {min_visits}{mask_label}")

        _render_heatmap(title=f"{cls} — detect-fail % (red = high fail)",
                        fail_pct=detect_fail_pct, visits=visits,
                        x_edges=x_edges, y_edges=y_edges, calib=calib,
                        fov_mask=fov_mask, min_visits=min_visits,
                        out_path=out_dir / f"{cls.lower()}_detect_fail.png",
                        subtitle=det_sub)
        # tracking-loss masks on matched frames (not all visits).
        _render_heatmap(title=f"{cls} — tracking-loss % (red = high id-switch / fragmentation)",
                        fail_pct=tracking_loss_pct, visits=matched,
                        x_edges=x_edges, y_edges=y_edges, calib=calib,
                        fov_mask=fov_mask, min_visits=min_visits,
                        out_path=out_dir / f"{cls.lower()}_tracking_loss.png",
                        subtitle=trk_sub)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runs-dir", required=True, type=Path,
                   help="multi-test dir (contains <scenario>/clip_logs/)")
    p.add_argument("--calib", required=True, type=Path,
                   help="3D VSS calibration.json")
    p.add_argument("--out", required=True, type=Path,
                   help="output dir for heatmap PNGs")
    p.add_argument("--scenario", default=None,
                   help="restrict to one scenario name (e.g. in-roi-5min)")
    p.add_argument("--bin-m", type=float, default=0.3,
                   help="bin size in metres (default: 0.3)")
    p.add_argument("--min-visits", type=int, default=5,
                   help="mask bins with fewer visits (default: 5)")
    p.add_argument("--world-bounds", nargs=4, type=float,
                   default=[-4.0, 14.0, -22.0, -4.0],
                   metavar=("X_MIN", "X_MAX", "Y_MIN", "Y_MAX"),
                   help="world XY bin range; default covers the warehouse + FOV margin")
    p.add_argument("--density", action="store_true",
                   help="also render occupancy-density maps (frames per bin) — "
                        "shows WHERE actors actually concentrate, not just fail %")
    p.add_argument("--fov-mask", action="store_true",
                   help="mask bins outside union of camera FOV polygons "
                        "(comparison aid for the convex-hull-vs-FOV coverage question)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    calib = Calibration(args.calib)

    x_min, x_max, y_min, y_max = args.world_bounds
    x_edges = np.arange(x_min, x_max + args.bin_m, args.bin_m)
    y_edges = np.arange(y_min, y_max + args.bin_m, args.bin_m)
    grid_shape = (len(x_edges) - 1, len(y_edges) - 1)

    fov_mask = None
    if args.fov_mask:
        log.info("computing FOV mask grid (%d × %d bins)…", *grid_shape)
        fov_mask = _fov_mask_grid(calib.fov_union, x_edges, y_edges)
        in_fov = int(fov_mask.sum())
        log.info("  %d/%d bins inside FOV union (%.1f %%)",
                 in_fov, fov_mask.size, 100.0 * in_fov / fov_mask.size)

    per_scn_accum: dict[str, dict] = defaultdict(lambda: _new_accum(grid_shape))
    combined = _new_accum(grid_shape)

    clip_dirs = _collect_clip_dirs(args.runs_dir, args.scenario)
    log.info("found %d clips to process under %s", len(clip_dirs), args.runs_dir)
    for cd in clip_dirs:
        scn = cd.parent.parent.name
        log.debug("  %s :: %s", scn, cd.name)
        _aggregate_clip(cd, per_scn_accum[scn], (x_edges, y_edges))
        _aggregate_clip(cd, combined, (x_edges, y_edges))

    args.out.mkdir(parents=True, exist_ok=True)
    _render_set(accum=combined, label=f"all scenarios ({len(clip_dirs)} clips)",
                out_dir=args.out / "_combined", calib=calib, fov_mask=fov_mask,
                x_edges=x_edges, y_edges=y_edges, min_visits=args.min_visits,
                density=args.density)
    for scn, accum in per_scn_accum.items():
        _render_set(accum=accum, label=scn, out_dir=args.out / scn,
                    calib=calib, fov_mask=fov_mask,
                    x_edges=x_edges, y_edges=y_edges, min_visits=args.min_visits,
                    density=args.density)

    log.info("done. output: %s", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
