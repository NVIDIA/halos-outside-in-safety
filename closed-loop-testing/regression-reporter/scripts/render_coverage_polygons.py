#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Render convex-hull coverage vs FOV-from-calibration side-by-side.

Visualizes the two coverage-gate approaches:

1. **3 camera FOV polygons + their union** — read straight from
   `calibration.json`'s `fieldOfViewPolygon`.
2. **Convex hull of detections** — replicates `aggregator.py`'s
   `compute_phase2_metrics` coverage region (convex hull of all detection
   points, padded by `--pad-m`, default 2.0 m to match).

Outputs one figure per scenario + a combined figure across the run. Each
figure shows world coords (meters), the FOV polygons + union, the
convex hull, raw detection points (faded), and the ROI rectangle + tripwire.

Usage (inside srr container — pip install pillow matplotlib first):
    docker exec srr python3 /app/scripts/render_coverage_polygons.py \\
        --runs-dir /app/runs/multi-test-20260612-044956 \\
        --calib    /app/calibration.json \\
        --out      /app/runs/multi-test-20260612-044956/coverage_polygons

Options:
    --scenario in-roi-5min      restrict to one scenario
    --pad-m 2.0                 convex-hull pad (m) — match aggregator default
    --max-pts 50000             cap scatter point count per figure
    --world-bounds X_MIN X_MAX Y_MIN Y_MAX
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.lines import Line2D
from shapely import wkt
from shapely.geometry import MultiPoint, Point
from shapely.ops import unary_union


log = logging.getLogger("coverage-polygons")


# ───────────────────────────────────────────────────────────────────────────
# Calibration parsing (same shape as render_perception_heatmap.py)
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
                except Exception as exc:
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
# Detection collection + convex hull
# ───────────────────────────────────────────────────────────────────────────


def _collect_clip_dirs(runs_dir: Path, scenario_filter: str | None) -> dict[str, list[Path]]:
    """Return {scenario_name: [clip_dir, ...]}, scenario-filtered."""
    out: dict[str, list[Path]] = defaultdict(list)
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
                out[scn_dir.name].append(clip_dir)
    return out


def _load_detections(clip_dirs: list[Path]) -> np.ndarray:
    """Load (x, y) of all detections across given clips. Returns (N, 2) array."""
    xs: list[float] = []
    ys: list[float] = []
    for cd in clip_dirs:
        det_p = cd / "detections.csv"
        if not det_p.exists():
            continue
        df = pd.read_csv(det_p)
        if df.empty:
            continue
        if "x" in df.columns and "y" in df.columns:
            xs.extend(df["x"].dropna().tolist())
            ys.extend(df["y"].dropna().tolist())
    return np.column_stack([xs, ys]) if xs else np.zeros((0, 2))


def _convex_hull_padded(points: np.ndarray, pad_m: float):
    """Convex hull + buffer pad (replicates aggregator.py compute_phase2_metrics)."""
    if len(points) < 3:
        return None
    try:
        hull = MultiPoint([(p[0], p[1]) for p in points]).convex_hull
        return hull.buffer(pad_m)
    except Exception as exc:
        log.warning("hull build failed (n=%d): %s", len(points), exc)
        return None


# ───────────────────────────────────────────────────────────────────────────
# Rendering helpers
# ───────────────────────────────────────────────────────────────────────────


def _polygon_to_xy(poly) -> list[np.ndarray]:
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


def _fill_polygon(ax, poly, *, facecolor, edgecolor, lw, alpha, label=None,
                  hatch=None, ls="-"):
    """Plot shapely polygon (with holes) as filled patch + outline."""
    if poly is None or poly.is_empty:
        return
    geoms = poly.geoms if poly.geom_type == "MultiPolygon" else [poly]
    first = True
    for g in geoms:
        if g.is_empty:
            continue
        ext = np.array(g.exterior.coords)
        holes = [np.array(h.coords) for h in g.interiors]
        path_verts = list(ext) + [(np.nan, np.nan)] + sum(
            ([list(h) + [(np.nan, np.nan)] for h in holes]), [])
        path_arr = np.array([v for v in path_verts])
        # Use Path + PathPatch for holes; simple alternative — draw ext fill
        # then "punch out" hole outlines. We just draw exterior fill + outlines
        # (good enough visually).
        ax.fill(ext[:, 0], ext[:, 1], facecolor=facecolor, edgecolor="none",
                alpha=alpha, zorder=1,
                label=label if first else None, hatch=hatch)
        ax.plot(ext[:, 0], ext[:, 1], color=edgecolor, lw=lw, ls=ls, zorder=3)
        for h in holes:
            # Draw hole as background-coloured fill on top to "punch out".
            ax.fill(h[:, 0], h[:, 1], facecolor=ax.get_facecolor(),
                    edgecolor="none", zorder=2)
            ax.plot(h[:, 0], h[:, 1], color=edgecolor, lw=lw * 0.8, ls=":", zorder=3)
        first = False


def _render_figure(*, ax_title: str, subtitle: str, calib: Calibration,
                   hull_poly, detection_pts: np.ndarray,
                   world_bounds: tuple[float, float, float, float],
                   max_pts: int, out_path: Path) -> None:
    fig, ax = plt.subplots(figsize=(13, 10), dpi=140)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#1e1e1e")

    cam_colors = ["#3aa1f0", "#f0823a", "#a13af0"]

    # 1. Individual camera FOV polygons — filled, low alpha.
    for c, color in zip(calib.cameras, cam_colors):
        _fill_polygon(ax, c["fov"], facecolor=color, edgecolor=color,
                      lw=1.4, alpha=0.18,
                      label=f"{c['id']} FOV")
        # Camera position triangle.
        ax.plot(c["x"], c["y"], marker="^", color=color, markersize=12,
                markeredgecolor="white", markeredgewidth=0.9, zorder=8)

    # 2. FOV-union outline — heavy.
    if calib.fov_union is not None:
        for ring in _polygon_to_xy(calib.fov_union):
            ax.plot(ring[:, 0], ring[:, 1], color="#80ff80", lw=2.4,
                    ls="-", alpha=0.95, zorder=4)
        # Hidden line for legend.
        ax.plot([], [], color="#80ff80", lw=2.4, label="FOV-union (calibration)")

    # 3. Convex hull of detections — different colour, hatched.
    if hull_poly is not None and not hull_poly.is_empty:
        for ring in _polygon_to_xy(hull_poly):
            ax.plot(ring[:, 0], ring[:, 1], color="#ff5050", lw=2.4,
                    ls="--", alpha=0.95, zorder=5)
        # Hidden line for legend.
        ax.plot([], [], color="#ff5050", lw=2.4, ls="--",
                label=f"Convex hull + 2 m pad (n={len(detection_pts)} dets)")

    # 4. Detection points scatter — small, faded.
    if len(detection_pts):
        pts = detection_pts
        if len(pts) > max_pts:
            idx = np.random.default_rng(42).choice(len(pts), max_pts, replace=False)
            pts = pts[idx]
        ax.scatter(pts[:, 0], pts[:, 1], s=2, c="#ffd24a", alpha=0.35,
                   edgecolors="none", zorder=6,
                   label=f"detection points (subsampled to {len(pts)})"
                   if len(detection_pts) > max_pts else "detection points")

    # 5. ROI rectangle + tripwire.
    roi_x0, roi_x1 = 4.877, 9.574
    roi_y0, roi_y1 = -18.976, -11.239
    ax.add_patch(mpatches.Rectangle((roi_x0, roi_y0),
                                    roi_x1 - roi_x0, roi_y1 - roi_y0,
                                    fill=False, ec="white", lw=1.4, ls=":", zorder=7))
    ax.axvline(9.574, color="white", lw=1.0, ls="--", alpha=0.85, zorder=7)
    ax.plot([], [], color="white", lw=1.4, ls=":", label="ROI / tripwire")

    x_min, x_max, y_min, y_max = world_bounds
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)
    ax.set_aspect("equal")
    ax.grid(True, color="#444", lw=0.4, alpha=0.5)
    ax.set_xlabel("world x (m)", fontsize=10)
    ax.set_ylabel("world y (m)", fontsize=10)
    ax.set_title(ax_title + (f"\n{subtitle}" if subtitle else ""),
                 fontsize=11, loc="left", color="#222")

    fov_area = calib.fov_union.area if calib.fov_union is not None else 0.0
    hull_area = hull_poly.area if hull_poly is not None and not hull_poly.is_empty else 0.0
    info_text = (
        f"FOV-union area : {fov_area:6.1f} m²\n"
        f"Convex hull area: {hull_area:6.1f} m²\n"
        f"hull / fov ratio: {(hull_area / fov_area) if fov_area else 0:.2f}×"
    )
    ax.text(0.015, 0.985, info_text, transform=ax.transAxes,
            ha="left", va="top", fontsize=9, color="white", family="monospace",
            bbox=dict(facecolor="#2a2a2a", edgecolor="none", alpha=0.85, pad=6))

    ax.legend(loc="lower right", fontsize=8.5, framealpha=0.85,
              facecolor="#2a2a2a", edgecolor="none", labelcolor="white",
              ncol=1)

    plt.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out_path, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    log.info("wrote %s", out_path)


# ───────────────────────────────────────────────────────────────────────────
# Driver
# ───────────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runs-dir", required=True, type=Path,
                   help="multi-test dir (contains <scenario>/clip_logs/)")
    p.add_argument("--calib", required=True, type=Path,
                   help="3D VSS calibration.json")
    p.add_argument("--out", required=True, type=Path,
                   help="output dir for coverage-polygon PNGs")
    p.add_argument("--scenario", default=None,
                   help="restrict to one scenario name (e.g. in-roi-5min)")
    p.add_argument("--pad-m", type=float, default=2.0,
                   help="convex-hull pad metres (default: 2.0, matches aggregator)")
    p.add_argument("--max-pts", type=int, default=50000,
                   help="cap on detection scatter points per figure (default: 50k)")
    p.add_argument("--per-clip", action="store_true",
                   help="also render one figure per clip (lots of files; off by default)")
    p.add_argument("--world-bounds", nargs=4, type=float,
                   default=[-4.0, 14.0, -22.0, -4.0],
                   metavar=("X_MIN", "X_MAX", "Y_MIN", "Y_MAX"))
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    calib = Calibration(args.calib)
    log.info("calibration: %d camera(s), FOV-union area=%.1f m²",
             len(calib.cameras),
             calib.fov_union.area if calib.fov_union is not None else 0.0)

    by_scn = _collect_clip_dirs(args.runs_dir, args.scenario)
    if not by_scn:
        log.warning("no clips found under %s (scenario filter: %s)",
                    args.runs_dir, args.scenario)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)

    # Per-scenario figures + collect all detections for the combined figure.
    all_pts_list: list[np.ndarray] = []
    for scn, clips in by_scn.items():
        log.info("scenario %s — %d clips", scn, len(clips))
        pts = _load_detections(clips)
        all_pts_list.append(pts)
        hull = _convex_hull_padded(pts, args.pad_m)
        _render_figure(
            ax_title=f"Coverage polygons — {scn}",
            subtitle=f"{len(clips)} clips · {len(pts)} detections · "
                     f"hull pad {args.pad_m:.1f} m",
            calib=calib, hull_poly=hull, detection_pts=pts,
            world_bounds=tuple(args.world_bounds),
            max_pts=args.max_pts,
            out_path=args.out / f"{scn}.png",
        )
        if args.per_clip:
            for cd in clips:
                cpts = _load_detections([cd])
                if len(cpts) < 3:
                    continue
                chull = _convex_hull_padded(cpts, args.pad_m)
                _render_figure(
                    ax_title=f"Coverage polygons — {scn} / {cd.name}",
                    subtitle=f"{len(cpts)} detections · hull pad {args.pad_m:.1f} m",
                    calib=calib, hull_poly=chull, detection_pts=cpts,
                    world_bounds=tuple(args.world_bounds),
                    max_pts=args.max_pts,
                    out_path=args.out / scn / f"{cd.name}.png",
                )

    # Combined cross-scenario figure.
    if all_pts_list:
        all_pts = np.vstack(all_pts_list)
        hull_all = _convex_hull_padded(all_pts, args.pad_m)
        _render_figure(
            ax_title="Coverage polygons — all scenarios",
            subtitle=f"{sum(len(c) for c in by_scn.values())} clips · "
                     f"{len(all_pts)} detections · hull pad {args.pad_m:.1f} m",
            calib=calib, hull_poly=hull_all, detection_pts=all_pts,
            world_bounds=tuple(args.world_bounds),
            max_pts=args.max_pts,
            out_path=args.out / "_combined.png",
        )

    log.info("done. output: %s", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
