#!/usr/bin/env python3
"""Put several test runs side by side, one column per placement.

Each run leaves its own `summary.md`; reading four of them and transcribing numbers by hand
is exactly where a row ends up in the wrong column. This reads them and writes one table as
text and as CSV.

It also reports, per run, how much of each scenario Safety Core spent not deciding. That
number belongs next to the metrics rather than in a footnote: a scenario recorded while the
decision path was stalled produces mute/unmute figures that look like a bad placement, and
nothing else in the report says so. Anything above a few percent means re-record before
comparing.

Usage:
    ./compare_runs.py <run-dir> [<run-dir> ...] --output-prefix comparison
    ./compare_runs.py --runs-root .../srr-service/runs --label pushoutI
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import re
from pathlib import Path

SCENARIOS = ("in-roi", "psf-edge", "psf-clear", "balanced", "fast")

FIELDS = [
    ("clips", r"total clips: \*\*(\d+)\*\*", 1),
    ("PASS/NEAR/FAIL", r"\*\*(\d+) PASS\*\* \((\d+)%\).*?(\d+) NEAR \((\d+)%\).*?"
                       r"\*\*(\d+) FAIL\*\* \((\d+)%\)", 6),
    ("mute ok", r"\*\*Mute correct%\*\*[^*]*\*\*([\d.]+%)", 1),
    ("unmute ok", r"\*\*Unmute correct%\*\*[^*]*\*\*([\d.]+%)", 1),
    ("ROI entry", r"ROI entries[^*]*\*\*([\d/]+)\*\* \(([\d.]+)%\)", 2),
    ("TW IN", r"TW IN[^*]*\*\*([\d/]+)\*\* \(([\d.]+)%\)", 2),
    ("TW OUT", r"TW OUT[^*]*\*\*([\d/]+)\*\* \(([\d.]+)%\)", 2),
    ("no-direction TW events", r"missing `direction` field: \*\*(\d+)\*\*", 1),
    ("worst unmute lag", r"worst UNMUTE lag[^*]*\*\*([\d.]+ ms)", 1),
    ("worst mute lag", r"worst MUTE lag[^*]*\*\*([\d.]+ ms)", 1),
]


def read_summary(run: Path) -> dict[str, str]:
    text = (run / "summary.md").read_text(encoding="utf-8", errors="replace")
    out: dict[str, str] = {}
    for name, pattern, groups in FIELDS:
        m = re.search(pattern, text, re.S)
        if not m:
            out[name] = "—"
        elif name == "PASS/NEAR/FAIL":
            p, n, f = m.groups()[1::2]
            out[name] = f"{p}% / {n}% / {f}%"
        elif groups == 2:
            out[name] = f"{m.group(1)} ({m.group(2)}%)"
        else:
            out[name] = m.group(1)
    return out


PSF_SNIPPET = """
import glob, json, sys
import pandas as pd
worst = []
for scenario in {scenarios!r}:
    hits = sorted(glob.glob(f'{{sys.argv[1]}}/{{scenario}}/run-*.parquet'))
    if not hits:
        continue
    column = pd.read_parquet(hits[-1], columns=['psf_command'])['psf_command'].dropna()
    if column.empty:
        continue
    beats = sum(json.loads(s).get('command_name') == 'HEARTBEAT' for s in column)
    share = round(100.0 * beats / len(column))
    if share > 5:
        worst.append(f'{{scenario}} {{share}}%')
print(', '.join(worst) if worst else 'none')
"""


def undecided(run: Path) -> str:
    """Percent of frames per scenario where Safety Core only heartbeated, worst first.

    Falls back to running inside the `srr` container, which is where pandas lives on a
    deployed host; the runs directory is the same tree, mounted at /app/runs.
    """
    code = PSF_SNIPPET.format(scenarios=list(SCENARIOS))
    try:
        import pandas  # noqa: F401
        import subprocess
        return subprocess.run(["python3", "-c", code, str(run)],
                              capture_output=True, text=True).stdout.strip() or "—"
    except ImportError:
        import subprocess
        done = subprocess.run(
            ["docker", "exec", "srr", "python3", "-c", code, f"/app/runs/{run.name}"],
            capture_output=True, text=True)
        return done.stdout.strip() or "no pandas on host, and srr container did not answer"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("runs", nargs="*", type=Path, help="run directories to compare")
    ap.add_argument("--runs-root", type=Path,
                    help="srr-service/runs, to collect runs by label instead")
    ap.add_argument("--label", help="only runs whose directory name contains this")
    ap.add_argument("--output-prefix", type=Path, default=Path("comparison"),
                    help="writes <prefix>.txt and <prefix>.csv (default comparison)")
    ap.add_argument("--no-psf-check", action="store_true",
                    help="skip the Safety Core liveness column (it reads every parquet)")
    args = ap.parse_args()

    runs = list(args.runs)
    if args.runs_root:
        pattern = f"*{args.label}*" if args.label else "multi-test-*"
        runs += [Path(p) for p in sorted(glob.glob(str(args.runs_root / pattern)))]
    runs = [r for r in runs if (r / "summary.md").exists()]
    if not runs:
        print("no run directories with a summary.md")
        return 1

    columns = [(r.name, read_summary(r)) for r in runs]
    rows = [name for name, _, _ in FIELDS]
    if not args.no_psf_check:
        for (name, data), run in zip(columns, runs):
            data["Safety Core idle"] = undecided(run)
        rows.append("Safety Core idle")

    width = max(len(r) for r in rows) + 2
    cells = [max(len(str(data.get(r, "—"))) for _, data in columns) for r in rows]
    head = max(max(len(name) for name, _ in columns), max(cells))
    lines = ["".ljust(width) + "".join(name.ljust(head + 2) for name, _ in columns)]
    for row in rows:
        lines.append(row.ljust(width)
                     + "".join(str(data.get(row, "—")).ljust(head + 2) for _, data in columns))
    text = "\n".join(lines) + "\n"

    args.output_prefix.with_suffix(".txt").write_text(text, encoding="utf-8")
    with args.output_prefix.with_suffix(".csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["metric"] + [name for name, _ in columns])
        for row in rows:
            writer.writerow([row] + [data.get(row, "—") for _, data in columns])

    print(text)
    print(f"wrote {args.output_prefix.with_suffix('.txt')} and "
          f"{args.output_prefix.with_suffix('.csv')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
