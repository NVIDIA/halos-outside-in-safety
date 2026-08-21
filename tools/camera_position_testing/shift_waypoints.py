#!/usr/bin/env python3
"""Move the pedestrian walk targets with the work zone, one behaviour-tree set per distance.

Only needed when the tripwire and ROI are pushed away from the wall (see README §5). The
`MoveTo` targets in the scenario trees are absolute world coordinates, so a zone that moves
and waypoints that do not stops testing what the scenario is named after: with the zone 3 m
out, only 58 % of the "person in work zone" targets are still inside it and 3 % of the
"clear" ones. Rows recorded that way cannot be compared with D = 0.

Every shifted target is checked against the SIL navmesh before a set is written. An
off-navmesh target is not a visible failure — the character simply never arrives, the
scenario records a person who never enters the zone, and the row reads as a detection
problem instead of a bad waypoint.

The shift is along -x by default, which is "away from the trailer" in the shipped warehouse
scene, matching the outward normal of the tripwire's own `direction` arrow. Pass --axis y for
a scene laid out the other way round.
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIGS = REPO / "closed-loop-testing/isaac-sim/sil/configs"


def navmesh_test(path: Path):
    """Return a point-in-navmesh predicate, or one that accepts everything if absent."""
    if not path.exists():
        print(f"  no navmesh at {path} — skipping the walkability check")
        return lambda x, y: True

    tris = json.loads(path.read_text())["triangles_xy"]

    def inside(px: float, py: float) -> bool:
        for (ax, ay), (bx, by), (cx, cy) in tris:
            d1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
            d2 = (cx - bx) * (py - by) - (cy - by) * (px - bx)
            d3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx)
            if (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0):
                return True
        return False

    return inside


def shift(node, delta: float, axis: int, targets: list) -> None:
    if isinstance(node, dict):
        if node.get("type") == "MoveTo":
            value = node.get("portOverrides", {}).get("target", {}).get("value")
            if value:
                value[axis] = round(float(value[axis]) + delta, 4)
                targets.append((float(value[0]), float(value[1])))
        for child in node.values():
            shift(child, delta, axis, targets)
    elif isinstance(node, list):
        for item in node:
            shift(item, delta, axis, targets)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--distance", type=float, required=True, metavar="D",
                    help="metres to move the waypoints away from the wall")
    ap.add_argument("--configs-dir", type=Path, default=DEFAULT_CONFIGS,
                    help=f"Isaac SIL configs holding the trees (default {DEFAULT_CONFIGS})")
    ap.add_argument("--source-dir", type=Path,
                    help="unshifted trees to read (default: --configs-dir; point this at "
                         "your pristine copies once a shifted set has been installed)")
    ap.add_argument("--output-dir", type=Path, required=True,
                    help="where to write the shifted set")
    ap.add_argument("--navmesh", type=Path,
                    help="navmesh.json (default <configs-dir>/navmesh.json)")
    ap.add_argument("--axis", choices=("x", "y"), default="x",
                    help="axis the zone moves along (default x)")
    ap.add_argument("--pattern", default="srr_*_char?.bt.json",
                    help="tree filenames to shift (default srr_*_char?.bt.json)")
    args = ap.parse_args()

    source = args.source_dir or args.configs_dir
    trees = sorted(p for p in source.glob(args.pattern) if not p.name.startswith("srr_char"))
    if not trees:
        print(f"no trees matching {args.pattern} in {source}")
        return 1

    inside = navmesh_test(args.navmesh or args.configs_dir / "navmesh.json")
    axis = 0 if args.axis == "x" else 1
    args.output_dir.mkdir(parents=True, exist_ok=True)

    moved = off = 0
    lowest = None
    for tree in trees:
        data = json.loads(tree.read_text())
        targets: list = []
        shift(data, -args.distance, axis, targets)
        (args.output_dir / tree.name).write_text(json.dumps(data, indent=1) + "\n")
        moved += len(targets)
        off += sum(not inside(x, y) for x, y in targets)
        for point in targets:
            lowest = point[axis] if lowest is None else min(lowest, point[axis])

    print(f"{moved} waypoints moved {args.distance} m along -{args.axis} "
          f"→ {args.output_dir}")
    print(f"  lowest target {args.axis} = {lowest:.2f} "
          f"(assert this before recording; it is the cheapest proof the right set is live)")
    if off:
        # Written but reported as a failure: seeing which targets fell off is more useful
        # than an empty output directory, and the run must not start either way.
        print(f"  {off} target(s) fall outside the navmesh — do NOT record this set")
        return 1
    print("  every target stays on the navmesh")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
