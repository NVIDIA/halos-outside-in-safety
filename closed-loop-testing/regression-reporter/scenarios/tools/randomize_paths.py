#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Generate randomized SRR pedestrian paths as IRA 1.6 behavior trees (Isaac Sim
6.0). This is the sole scenario generator: it emits srr_<name>_char{0,1,2}.bt.json
directly into scenarios/behavior-trees/ (the canonical source of truth that
sync_to_halos.sh copies into the Isaac SIL configs/). The legacy omni.anim.people
command-file layer (and its command_to_bt.py converter) has been removed —
Isaac 6.0 / IRA 1.6 dropped external command files entirely.

Sampling: uniformly inside per-character polygon zones, with optional rejection
based on:
  - NavMesh walkability (if --navmesh navmesh.json is supplied)
  - ROI-boundary clearance (if --roi-clearance N is supplied)

  # behavior trees for a new scenario (Isaac 6.0)
  python3 randomize_paths.py --navmesh ../scenes/navmesh.json --roi-clearance 0.3 \\
      --cycles 150 --name my-scenario
"""
import argparse
import json
import random
from collections import defaultdict
from pathlib import Path

# --- IRA 1.6 behavior-tree emission (Isaac Sim 6.0) ---------------------------
# Isaac 6.0 dropped external command files; each pedestrian is a behavior tree.
# We build an in-memory waypoint schedule (the `<Name> GoTo/Idle ...` line list
# below is just an internal representation, not a file) and emit the trees
# directly. Node mapping: GoTo -> MoveTo, Idle -> Wait.
BT_NODE_LIBRARIES = [
    {"name": "omni.behavior.tree.core", "version": "1.0"},
    {"name": "omni.anim.behavior.tree", "version": "0.1"},
]
# SRR pedestrian name -> character index (drives srr_<name>_char<idx>.bt.json,
# which default_config_ros.yaml binds via the fixed srr_char<idx>.bt.json names).
ACTOR_INDEX = {"Character": 0, "Character_01": 1, "Character_02": 2}


def _bt_move_to(idx, x, y, z):
    return {
        "type": "MoveTo",
        "source": "omni.anim.behavior.tree",
        "name": f"n{idx}_MoveTo",
        "portOverrides": {"target": {"type": "carb::Float3", "value": [x, y, z]}},
        # ForceStatus->success (default status) makes a MoveTo nav failure
        # (status=2 on an unreachable waypoint) non-fatal: RUNNING passes through
        # so the character keeps walking, and on finish the node reports success
        # so the root Sequence advances instead of aborting (which would freeze
        # the character for the rest of the run). Restores the legacy
        # command-file player's advance-on-failed-GoTo semantics.
        "modifiers": [
            {
                "type": "ForceStatus",
                "source": "omni.behavior.tree.core",
                "name": f"n{idx}_MoveTo_ForceSuccess",
            }
        ],
    }


def _bt_wait(idx, duration):
    return {
        "type": "Wait",
        "source": "omni.behavior.tree.core",
        "name": f"n{idx}_Wait",
        "portOverrides": {"duration": duration},
    }


def block_to_tree(lines):
    """Convert one character's `<Name> GoTo/Idle ...` lines into a behavior tree.

    One-shot (root Sequence, no Repeat) to match the finite command-file
    semantics: after the scripted path the character idles until
    simulation_duration elapses.
    """
    children = []
    for ln in lines:
        tok = ln.split()
        if len(tok) < 2:
            continue
        verb = tok[1]
        idx = len(children)
        if verb == "GoTo":
            children.append(_bt_move_to(idx, float(tok[2]), float(tok[3]), float(tok[4])))
        elif verb == "Idle":
            children.append(_bt_wait(idx, float(tok[2])))
    return {
        "schemaVersion": "2.0.0",
        "nodeLibraries": BT_NODE_LIBRARIES,
        "root": {
            "type": "Sequence",
            "source": "omni.behavior.tree.core",
            "name": "Root",
            "children": children,
        },
    }


def write_behavior_trees(sections, name, out_dir):
    """Write srr_<name>_char{0,1,2}.bt.json for the 3 character blocks."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for block in sections:
        if not block:
            continue
        actor = block[0].split()[0]
        idx = ACTOR_INDEX[actor]
        tree = block_to_tree(block)
        out = out_dir / f"srr_{name}_char{idx}.bt.json"
        out.write_text(json.dumps(tree, indent=2) + "\n")
        moves = sum(1 for n in tree["root"]["children"] if n["type"] == "MoveTo")
        print(f"  [{name}] {actor:14s} -> {out.name}  ({moves} MoveTo)")
        written.append(out)
    return written


# --- polygon zones (redrawn 2026-04-30 for nav_clear scene) ---
# Simple rectangles that extend INTO ROI substantially. Forklift corridor
# sits in the y∈[-12.0, -14.5] gap between the two zones.
ZONE_CHAR0: list[tuple[float, float]] = [
    (-2.6,   -12.0),
    (-2.6,    -8.6),
    ( 9.13,   -8.6),
    ( 9.13,  -12.0),
]
ZONE_CHAR12: list[tuple[float, float]] = [
    (-4.16, -19.29),
    ( 9.13, -19.29),
    ( 9.13, -14.5),
    (-4.16, -14.5),
]

CHAR_POLYGON: dict[str, list[tuple[float, float]]] = {
    "Character":    ZONE_CHAR0,
    "Character_01": ZONE_CHAR12,
    "Character_02": ZONE_CHAR12,
}

CHAR_SPAWN: dict[str, tuple[float, float]] = {
    "Character":    (2.26, -10.30),
    "Character_01": (0.29, -18.00),
    "Character_02": (-2.86, -15.28),
}

# ROI rectangle (calibration sensor[*].rois[0].roiCoordinates)
ROI: tuple[float, float, float, float] = (4.877, -18.976, 9.574, -11.239)
#                                         xmin   ymin     xmax   ymax


# ---- geometry utils ----
def polygon_bbox(verts):
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]
    return min(xs), min(ys), max(xs), max(ys)


def point_in_polygon(x, y, verts):
    inside = False
    n = len(verts); j = n - 1
    for i in range(n):
        xi, yi = verts[i]; xj, yj = verts[j]
        denom = (yj - yi) or 1e-12
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / denom + xi):
            inside = not inside
        j = i
    return inside


def dist_to_roi_boundary(x, y, roi):
    """Signed distance to ROI rectangle boundary line.
    Positive both inside and outside; 0 exactly on the edge."""
    xmin, ymin, xmax, ymax = roi
    if xmin <= x <= xmax and ymin <= y <= ymax:
        return min(x - xmin, xmax - x, y - ymin, ymax - y)
    cx = max(xmin, min(xmax, x))
    cy = max(ymin, min(ymax, y))
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5


def in_roi(x, y, roi=None):
    r = roi if roi is not None else ROI
    return r[0] <= x <= r[2] and r[1] <= y <= r[3]


# ---- navmesh validator ----
def point_in_tri(x, y, t):
    (x1, y1), (x2, y2), (x3, y3) = t
    d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    if d == 0: return False
    a = ((y2 - y3) * (x - x3) + (x3 - x2) * (y - y3)) / d
    b = ((y3 - y1) * (x - x3) + (x1 - x3) * (y - y3)) / d
    c = 1 - a - b
    eps = -1e-6
    return a >= eps and b >= eps and c >= eps


class NavMesh:
    def __init__(self, path):
        doc = json.load(open(path))
        self.tris = doc["triangles_xy"]
        self.cell = 1.0
        self.grid = defaultdict(list)
        for ti, tri in enumerate(self.tris):
            xs = [p[0] for p in tri]; ys = [p[1] for p in tri]
            ix0, ix1 = int(min(xs)//self.cell), int(max(xs)//self.cell)
            iy0, iy1 = int(min(ys)//self.cell), int(max(ys)//self.cell)
            for ix in range(ix0, ix1 + 1):
                for iy in range(iy0, iy1 + 1):
                    self.grid[(ix, iy)].append(ti)

    def walkable(self, x, y) -> bool:
        for ti in self.grid.get((int(x // self.cell), int(y // self.cell)), ()):
            if point_in_tri(x, y, self.tris[ti]):
                return True
        return False

    def walkable_with_radius(self, x, y, r, n=16) -> bool:
        """Walkable AND a circle of radius r around (x,y) is fully walkable.
        Approximates 'agent body fits here' (n-point sampling on the circle)."""
        if r <= 0:
            return self.walkable(x, y)
        if not self.walkable(x, y):
            return False
        from math import cos, sin, pi
        for i in range(n):
            t = 2 * pi * i / n
            if not self.walkable(x + r * cos(t), y + r * sin(t)):
                return False
        return True


# ---- sampling ----
def sample_in_polygon(verts, rng, navmesh=None, roi_clearance=0.0,
                      agent_radius=0.0, target_in_roi=None, max_tries=600):
    """target_in_roi: None=any, True=must be inside ROI, False=must be outside.
    If target can't be satisfied within max_tries, falls back to first sample
    that satisfies all OTHER constraints."""
    pxmin, pymin, pxmax, pymax = polygon_bbox(verts)
    # If target_in_roi=True, restrict sampling bbox to polygon∩ROI
    # so we don't waste tries outside ROI. Only the bbox is restricted —
    # point_in_polygon still enforces the polygon shape.
    if target_in_roi is True:
        xmin = max(pxmin, ROI[0]); xmax = min(pxmax, ROI[2])
        ymin = max(pymin, ROI[1]); ymax = min(pymax, ROI[3])
        if xmin >= xmax or ymin >= ymax:
            xmin, ymin, xmax, ymax = pxmin, pymin, pxmax, pymax  # no overlap → unrestricted
    else:
        xmin, ymin, xmax, ymax = pxmin, pymin, pxmax, pymax
    fallback = None
    for _ in range(max_tries):
        # round FIRST so all checks operate on the value we'll store
        x = round(rng.uniform(xmin, xmax), 2)
        y = round(rng.uniform(ymin, ymax), 2)
        if not point_in_polygon(x, y, verts):
            continue
        if navmesh is not None and not navmesh.walkable_with_radius(x, y, agent_radius):
            continue
        if roi_clearance > 0 and dist_to_roi_boundary(x, y, ROI) < roi_clearance:
            continue
        if target_in_roi is not None and in_roi(x, y) != target_in_roi:
            if fallback is None:
                fallback = (x, y)
            continue
        return (x, y)
    if fallback is not None:
        return (fallback[0], fallback[1])
    cx = sum(v[0] for v in verts) / len(verts)
    cy = sum(v[1] for v in verts) / len(verts)
    return (round(cx, 2), round(cy, 2))


# ---- idle ranges ----
def parse_range(s, name):
    try:
        a, b = s.split(",")
        a = int(a); b = int(b)
        if a < 0 or b < a:
            raise ValueError
        return a, b
    except Exception:
        raise SystemExit(f"--{name} must be 'min,max' with 0 <= min <= max, got {s!r}")


def gen_for_char(name, n_cycles, base_seed, navmesh, roi_clearance,
                 agent_radius, roi_bias, spawn_every, idle_short, idle_anim,
                 idle_long, initial_idle):
    rng = random.Random(base_seed + sum(ord(c) for c in name))
    polygon = CHAR_POLYGON[name]
    spawn = CHAR_SPAWN[name]

    lines = [f"{name} Idle {initial_idle[name]}"]
    for i in range(n_cycles):
        target_in_roi = None
        if roi_bias is not None:
            target_in_roi = rng.random() < roi_bias
        x, y = sample_in_polygon(polygon, rng, navmesh, roi_clearance,
                                 agent_radius, target_in_roi=target_in_roi)
        lines.append(f"{name} GoTo {x} {y} 0.0 _")
        # single Idle per cycle — two consecutive Idles confuse character_behavior
        lines.append(f"{name} Idle {rng.randint(*idle_short)}")

        if spawn_every > 0 and (i + 1) % spawn_every == 0:
            sx, sy = spawn
            for _ in range(40):
                cand_x = round(spawn[0] + rng.uniform(-0.4, 0.4), 2)
                cand_y = round(spawn[1] + rng.uniform(-0.4, 0.4), 2)
                if navmesh is None or \
                   navmesh.walkable_with_radius(cand_x, cand_y, agent_radius):
                    sx, sy = cand_x, cand_y
                    break
            lines.append(f"{name} GoTo {sx} {sy} 0.0 _")
            lines.append(f"{name} Idle {rng.randint(*idle_long)}")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--cycles", type=int, default=150)
    ap.add_argument("--name", default="custom",
                    help="scenario name for the .bt.json filenames "
                         "(srr_<name>_char{0,1,2}.bt.json; default: custom)")
    ap.add_argument("--bt-out-dir", default=None,
                    help="output dir for *.bt.json (default: ../behavior-trees relative to this script)")

    ap.add_argument("--navmesh",
                    help="path to navmesh.json from export_navmesh.py "
                         "(reject WPs outside NavMesh)")
    ap.add_argument("--roi-clearance", type=float, default=0.0,
                    help="reject WPs within N meters of ROI rectangle boundary "
                         "(0 = waypoints can lie ON the boundary; "
                         "larger = clear band of width N around the edge)")
    ap.add_argument("--agent-radius", type=float, default=0.6,
                    help="character body radius in meters; reject WPs whose "
                         "agent body would overlap any unwalkable area "
                         "(default 0.6m; set 0 to disable circle check)")
    ap.add_argument("--roi-bias", type=float, default=None,
                    help="0..1 — target fraction of WPs that should fall "
                         "INSIDE ROI rectangle. e.g. 0.4 = ~40%% inside ROI, "
                         "60%% outside. omit/None = uniform polygon sampling. "
                         "Higher exposes PSF to more in-ROI events.")
    ap.add_argument("--spawn-return-every", type=int, default=3,
                    help="every N cycles, GoTo near spawn + long idle "
                         "(default 3; set 0 to disable, e.g. for "
                         "stay-inside-ROI tests since spawns are outside ROI)")

    ap.add_argument("--idle-short", default="1,3",
                    help="min,max seconds for short idle between GoTos")
    ap.add_argument("--idle-anim", default="4,5",
                    help="min,max seconds for arrival 'animation' idle")
    ap.add_argument("--idle-long", default="10,30",
                    help="min,max seconds for long idle after spawn return")

    ap.add_argument("--initial-idle", default="10,25,35",
                    help="comma-separated initial idle seconds per "
                         "Character,Character_01,Character_02")

    args = ap.parse_args()

    idle_short = parse_range(args.idle_short, "idle-short")
    idle_anim  = parse_range(args.idle_anim,  "idle-anim")
    idle_long  = parse_range(args.idle_long,  "idle-long")

    init_parts = args.initial_idle.split(",")
    if len(init_parts) != 3:
        raise SystemExit("--initial-idle must be 3 comma-separated ints")
    initial_idle = {
        "Character":    int(init_parts[0]),
        "Character_01": int(init_parts[1]),
        "Character_02": int(init_parts[2]),
    }

    navmesh = NavMesh(args.navmesh) if args.navmesh else None
    if navmesh:
        print(f"[gen] loaded navmesh ({len(navmesh.tris)} triangles)")
    if args.roi_clearance > 0:
        print(f"[gen] ROI-boundary clearance: {args.roi_clearance:.2f}m")
    if navmesh and args.agent_radius > 0:
        print(f"[gen] agent radius: {args.agent_radius:.2f}m (circle check)")
    if args.roi_bias is not None:
        if not 0.0 <= args.roi_bias <= 1.0:
            raise SystemExit("--roi-bias must be in [0, 1]")
        print(f"[gen] ROI sampling bias: {args.roi_bias*100:.0f}% target inside ROI")

    sections = []
    for name in ("Character", "Character_01", "Character_02"):
        block = gen_for_char(
            name, args.cycles, args.seed,
            navmesh, args.roi_clearance, args.agent_radius, args.roi_bias,
            args.spawn_return_every,
            idle_short, idle_anim, idle_long, initial_idle,
        )
        sections.append(block)

    # post-validate
    bad_polygon = bad_navmesh = bad_clear = bad_radius = 0
    for block in sections:
        name = block[0].split()[0]
        poly = CHAR_POLYGON[name]
        sx, sy = CHAR_SPAWN[name]
        for ln in block:
            if " GoTo " not in ln: continue
            p = ln.split(); x, y = float(p[2]), float(p[3])
            near_spawn = abs(x - sx) <= 1.0 and abs(y - sy) <= 1.0
            if not point_in_polygon(x, y, poly) and not near_spawn:
                bad_polygon += 1
            if navmesh is not None and not navmesh.walkable(x, y):
                bad_navmesh += 1
            if navmesh is not None and args.agent_radius > 0 and \
               not navmesh.walkable_with_radius(x, y, args.agent_radius) and \
               not near_spawn:
                bad_radius += 1
            if args.roi_clearance > 0 and \
               dist_to_roi_boundary(x, y, ROI) < args.roi_clearance and \
               not near_spawn:
                bad_clear += 1

    for block in sections:
        n = block[0].split()[0]
        gotos = sum(1 for ln in block if " GoTo " in ln)
        print(f"  {n:14s} {len(block)} waypoints scheduled, {gotos} GoTo")
    print(f"[gen] zone-polygon violations:    {bad_polygon}")
    print(f"[gen] navmesh-walkable violations:{bad_navmesh}")
    print(f"[gen] agent-radius violations:    {bad_radius}")
    print(f"[gen] roi-clearance violations:   {bad_clear}")

    bt_dir = args.bt_out_dir or (Path(__file__).resolve().parent.parent / "behavior-trees")
    print(f"[gen] emitting IRA 1.6 behavior trees for scenario '{args.name}' -> {bt_dir}")
    written = write_behavior_trees(sections, args.name, bt_dir)
    print(f"[gen] wrote {len(written)} behavior-tree files")


if __name__ == "__main__":
    main()
