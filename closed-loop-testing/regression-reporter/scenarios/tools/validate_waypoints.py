# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Validate that every MoveTo waypoint in the IRA 1.6 behavior trees falls inside
the baked NavMesh. Run on host (stdlib only) AFTER export_navmesh.py has been
run inside Isaac Sim.

  # a single tree
  python3 validate_waypoints.py \
      --navmesh ../scenes/navmesh.json \
      --bt ../behavior-trees/srr_in-roi_char0.bt.json

  # every tree in the canonical dir
  python3 validate_waypoints.py \
      --navmesh ../scenes/navmesh.json \
      --bt ../behavior-trees

Report-only: prints off-NavMesh MoveTo targets per file. Note that at runtime
each MoveTo carries a ForceStatus->success modifier (see randomize_paths.py), so
an off-navmesh waypoint is skipped rather than freezing the character — this
check is a pre-flight quality gate, not a hard requirement.
"""
import argparse, glob, json, os
from collections import defaultdict


def point_in_tri(x, y, t):
    (x1,y1),(x2,y2),(x3,y3) = t
    d = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
    if d == 0: return False
    a = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / d
    b = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / d
    c = 1 - a - b
    eps = -1e-6  # tolerate edge
    return a >= eps and b >= eps and c >= eps


def build_index(triangles, cell=1.0):
    """Bbox-grid index: cell -> list of triangle indices that touch it."""
    grid = defaultdict(list)
    for ti, tri in enumerate(triangles):
        xs = [p[0] for p in tri]; ys = [p[1] for p in tri]
        ix0, ix1 = int(min(xs)//cell), int(max(xs)//cell)
        iy0, iy1 = int(min(ys)//cell), int(max(ys)//cell)
        for ix in range(ix0, ix1+1):
            for iy in range(iy0, iy1+1):
                grid[(ix, iy)].append(ti)
    return grid


def is_walkable(x, y, triangles, grid, cell=1.0):
    key = (int(x//cell), int(y//cell))
    for ti in grid.get(key, ()):
        if point_in_tri(x, y, triangles[ti]):
            return True
    return False


def is_walkable_with_radius(x, y, r, triangles, grid, cell=1.0, n=16):
    """walkable AND a circle of radius r around (x,y) is fully walkable."""
    if r <= 0:
        return is_walkable(x, y, triangles, grid, cell)
    if not is_walkable(x, y, triangles, grid, cell):
        return False
    import math
    for i in range(n):
        t = 2 * math.pi * i / n
        if not is_walkable(x + r*math.cos(t), y + r*math.sin(t), triangles, grid, cell):
            return False
    return True


def iter_move_to_targets(node):
    """Yield (x, y) for every MoveTo node in a behavior-tree subtree."""
    if not isinstance(node, dict):
        return
    if node.get("type") == "MoveTo":
        val = (node.get("portOverrides", {}).get("target", {}) or {}).get("value")
        if isinstance(val, (list, tuple)) and len(val) >= 2:
            yield float(val[0]), float(val[1])
    for child in node.get("children", []) or []:
        yield from iter_move_to_targets(child)


def bt_files(path):
    if os.path.isdir(path):
        return sorted(glob.glob(os.path.join(path, "*.bt.json")))
    return [path]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--navmesh", required=True)
    ap.add_argument("--bt", required=True,
                    help="a .bt.json file OR a dir of them (globs *.bt.json)")
    ap.add_argument("--agent-radius", type=float, default=0.6,
                    help="character body radius (m); WPs whose body would "
                         "overlap unwalkable area are reported (default 0.6)")
    args = ap.parse_args()

    nav = json.load(open(args.navmesh))
    triangles = nav["triangles_xy"]
    bbox = nav["bbox"]
    print(f"Loaded {len(triangles)} triangles, bbox xy [{bbox[0]:.2f},{bbox[1]:.2f}]..[{bbox[2]:.2f},{bbox[3]:.2f}]")

    grid = build_index(triangles, cell=1.0)
    print(f"Built grid index ({len(grid)} cells)")

    files = bt_files(args.bt)
    if not files:
        raise SystemExit(f"No .bt.json found at {args.bt}")

    total_moveto = 0
    total_bad = 0
    for f in files:
        doc = json.load(open(f))
        root = doc.get("root", {})
        bad = []
        n = 0
        for x, y in iter_move_to_targets(root):
            n += 1
            if not is_walkable_with_radius(x, y, args.agent_radius, triangles, grid):
                bad.append((x, y))
        total_moveto += n
        total_bad += len(bad)
        flag = "OK" if not bad else f"{len(bad)} bad"
        print(f"\n{os.path.basename(f)}: {n} MoveTo, {flag}")
        for x, y in bad[:8]:
            print(f"    ({x:+.3f}, {y:+.3f})")
        if len(bad) > 8:
            print(f"    ... +{len(bad)-8} more")

    print()
    print(f"Total MoveTo waypoints: {total_moveto}")
    print(f"Bad (off NavMesh):      {total_bad}")


if __name__ == "__main__":
    main()
