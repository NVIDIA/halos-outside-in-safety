#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Compute safety-critical unmute% per scenario.

For each summary.md in <multi-test-dir>/<label>/reports/, filter to clips
where chars > 70% in ROI AND fk > 50% in trailer (= dangerous condition),
then compute avg `100 - over-mute%` across those clips (over-mute% =
suppressed the safety response while a person was present).

Usage:
  python3 safety_critical_unmute.py --multi-test-dir <dir>
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path


def parse_summary(path: Path) -> list[dict]:
    """Parse the per-clip Markdown table by HEADER (robust to column reordering
    and Markdown-linked clip cells). Returns a list of clip dicts."""
    lines = path.read_text().splitlines()
    header_idx = None
    cols: list[str] = []
    for i, line in enumerate(lines):
        if line.lstrip().startswith("|") and "%char_in_roi" in line and "%fk_trailer" in line:
            cols = [c.strip() for c in line.strip().strip("|").split("|")]
            header_idx = i
            break
    if header_idx is None:
        return []

    def col(name: str) -> int:
        return cols.index(name) if name in cols else -1

    i_clip = col("Clip")
    i_roi = col("%char_in_roi")
    i_fk = col("%fk_trailer")
    i_over = col("over-mute%")
    i_match = col("match%")
    if min(i_clip, i_roi, i_fk, i_over) < 0:
        return []

    def num(cell: str) -> float:
        # strip markdown/bold/% and any link wrapper, keep the number
        s = re.sub(r"[*`%]", "", cell).strip()
        return float(s)

    rows = []
    for line in lines[header_idx + 2:]:          # skip header + its |---| separator
        if not line.lstrip().startswith("|"):
            break                                # table ended
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) <= max(i_clip, i_roi, i_fk, i_over):
            continue
        m = re.search(r"scn_\d+", cells[i_clip])  # handles "[scn_0000](scn_0000.md)"
        if not m:
            continue
        try:
            rows.append({
                "clip": m.group(0),
                "char_in_roi": num(cells[i_roi]),
                "fk_trailer": num(cells[i_fk]),
                "over_mute": num(cells[i_over]),
                "match": num(cells[i_match]) if i_match >= 0 else float("nan"),
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

    summaries_found = 0
    rows_parsed = 0
    scenarios_with_critical = 0

    for label_dir in sorted(base.iterdir()):
        summary = label_dir / "reports" / "summary.md"
        if not summary.exists():
            continue
        summaries_found += 1
        rows = parse_summary(summary)
        rows_parsed += len(rows)
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
        scenarios_with_critical += 1
        unmute = [100 - r["over_mute"] for r in critical]
        avg = sum(unmute) / len(unmute)
        passes = sum(1 for u in unmute if u >= 95)
        print(f"  {label_dir.name:<22} {f'{ncrit}/{total}':>8} "
              f"{avg:>7.1f}% {f'{passes}/{ncrit}':>10}")

    print()

    # INVALID guards: a run that produced summaries but yields an
    # empty result table must NOT look like a pass.
    if summaries_found == 0:
        print("  INVALID: no <scenario>/reports/summary.md found under the run dir")
        return 2
    if rows_parsed == 0:
        print("  INVALID: summaries present but 0 clip rows parsed "
              "(summary.md format changed? see safety_critical_unmute.parse_summary)")
        return 2
    if scenarios_with_critical == 0:
        print("  INVALID: parsed clips but NO scenario had a safety-critical "
              "(char_in_roi>70 & fk_trailer>50) clip — nothing to score")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
