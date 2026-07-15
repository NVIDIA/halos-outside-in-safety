#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Compute safety-critical unmute% per scenario.

For each summary.md in <multi-test-dir>/<label>/reports/, filter to clips
where chars > 70% in ROI AND fk > 50% in trailer (= dangerous condition),
then compute avg `100 - actMute%` across those clips.

Usage:
  python3 safety_critical_unmute.py --multi-test-dir <dir>
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path


def parse_summary(path: Path) -> list[dict]:
    """Read summary.md table → list of clip dicts."""
    rows = []
    for line in path.read_text().splitlines():
        if not line.startswith("| scn_"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        # Columns: clip rows dur char_in_roi fk_trailer expMute actMute match%
        # unmute_lag mute_lag gt_ROI n_ROI_dedup n_ROI_raw gt_TW_in n_TW_R
        # gt_TW_out n_TW_L
        try:
            rows.append({
                "clip": cells[0],
                "char_in_roi": float(cells[3]),
                "fk_trailer": float(cells[4]),
                "expMute": float(cells[5]),
                "actMute": float(cells[6]),
                "match": float(cells[7]),
            })
        except (IndexError, ValueError):
            continue
    return rows


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--multi-test-dir", required=True,
                   help="Path to runs/multi-test-YYYYMMDD-HHMMSS/")
    args = p.parse_args()

    base = Path(args.multi_test_dir)
    if not base.is_dir():
        print(f"  ERROR: {base} not a directory")
        return 1

    print()
    print(f"  {'Scenario':<22} {'Crit/All':>8} {'Unmute%':>8} {'Pass(>=95)':>10}")
    print(f"  {'-'*22} {'-'*8} {'-'*8} {'-'*10}")

    for label_dir in sorted(base.iterdir()):
        summary = label_dir / "reports" / "summary.md"
        if not summary.exists():
            continue
        rows = parse_summary(summary)
        if not rows:
            continue
        critical = [r for r in rows
                    if r["char_in_roi"] > 70 and r["fk_trailer"] > 50]
        total = len(rows)
        ncrit = len(critical)
        if ncrit == 0:
            print(f"  {label_dir.name:<22} {f'0/{total}':>8} {'-':>8} "
                  f"{'(no overlap)':>10}")
            continue
        unmute = [100 - r["actMute"] for r in critical]
        avg = sum(unmute) / len(unmute)
        passes = sum(1 for u in unmute if u >= 95)
        print(f"  {label_dir.name:<22} {f'{ncrit}/{total}':>8} "
              f"{avg:>7.1f}% {f'{passes}/{ncrit}':>10}")

    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
