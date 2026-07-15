# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
SRR — scan visible Imageable prims whose 2D footprint intersects either of the
character zones defined 2026-04-30.

Live-state scan: catches ad-hoc object removals/additions that haven't been
File→Saved yet.

Paste into Isaac Sim's Script Editor with the warehouse scene loaded.
Copy the final JSON block for offline analysis.
"""
import json

import omni.usd
from pxr import Usd, UsdGeom


# Polygon vertices (clockwise or CCW, doesn't matter — using ray-cast PIP)
ZONE_CHAR0 = [
    (-2.604, -12.339), (-2.604, -8.619), (4.603, -8.619),
    (4.603, -10.657), (8.313, -11.032), (-1.278, -12.355),
]
ZONE_CHAR12 = [
    (-1.88, -14.97), (3.74, -14.97), (3.74, -15.98), (9.13, -15.74),
    (9.13, -19.00), (4.20, -19.29), (-4.16, -19.29), (-4.16, -14.85),
]

ZONES = {"char_0": ZONE_CHAR0, "char_12": ZONE_CHAR12}

# Don't flag these as obstacles (they're the actors / infra)
SKIP_PATTERNS = (
    "/World/Characters/",
    "/World/forklift",
    "/World/Cameras/",
    "/World/Navmesh",
    "/World/ActionGraph",
    "/World/SRRGraph",
    "/World/RectLight",
    "/World/DomeLight",
    "/World/warehouse/RectLight",
    "/World/Loading_Zone",  # the floor-paint mesh, not an obstacle
    "/World/Loading_Zone_Objects/",  # parent xform — its children carry the geom
)


def aabb_polygon(verts):
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    return min(xs), min(ys), max(xs), max(ys)


def point_in_polygon(x, y, verts):
    inside = False
    n = len(verts)
    j = n - 1
    for i in range(n):
        xi, yi = verts[i]
        xj, yj = verts[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-9) + xi):
            inside = not inside
        j = i
    return inside


def aabb_intersects_polygon(bx0, by0, bx1, by1, verts) -> bool:
    """Cheap test: bbox intersects polygon if (a) any bbox corner is in poly,
    (b) any poly vertex is in bbox, OR (c) bbox center in poly. Misses some
    edge-only intersections — fine for our use (small obstacles, large zones)."""
    # bbox corners in poly?
    for cx, cy in [(bx0, by0), (bx1, by0), (bx1, by1), (bx0, by1)]:
        if point_in_polygon(cx, cy, verts):
            return True
    # poly vertices in bbox?
    for vx, vy in verts:
        if bx0 <= vx <= bx1 and by0 <= vy <= by1:
            return True
    return False


def is_visible(prim) -> bool:
    cur = prim
    while cur and cur.GetPath().pathString != "/":
        if cur.IsA(UsdGeom.Imageable):
            v = UsdGeom.Imageable(cur).GetVisibilityAttr().Get()
            if v == "invisible":
                return False
        cur = cur.GetParent()
    return True


def main() -> None:
    stage = omni.usd.get_context().get_stage()
    if stage is None:
        print("FAIL: no stage loaded")
        return
    bcache = UsdGeom.BBoxCache(
        Usd.TimeCode.Default(),
        [UsdGeom.Tokens.default_, UsdGeom.Tokens.render],
        useExtentsHint=True,
    )

    obstacles = {z: [] for z in ZONES}
    n_scanned = 0

    for prim in stage.Traverse():
        n_scanned += 1
        if not prim.IsA(UsdGeom.Imageable):
            continue
        path = str(prim.GetPath())
        if any(p in path for p in SKIP_PATTERNS):
            continue
        if not is_visible(prim):
            continue

        # Only emit at "interesting" depth — usually pallets/boxes sit at
        # /World/Loading_Zone_Objects_01/SM_PaletteA_xx — depth 4. Cap at 5
        # to skip ultra-fine mesh shards.
        if path.count("/") > 5:
            continue

        try:
            b = bcache.ComputeWorldBound(prim)
        except Exception:
            continue
        if b.GetRange().IsEmpty():
            continue
        r = b.ComputeAlignedRange()
        mn, mx = r.GetMin(), r.GetMax()
        x0, y0, x1, y1 = float(mn[0]), float(mn[1]), float(mx[0]), float(mx[1])
        sx, sy = x1 - x0, y1 - y0

        # Skip world-spanning (walls, floor) and degenerate
        if sx > 6 or sy > 6 or sx < 0.05 or sy < 0.05:
            continue

        for zname, verts in ZONES.items():
            if aabb_intersects_polygon(x0, y0, x1, y1, verts):
                obstacles[zname].append({
                    "path": path,
                    "name": prim.GetName(),
                    "type": str(prim.GetTypeName()),
                    "center": [round((x0 + x1) / 2, 3), round((y0 + y1) / 2, 3)],
                    "size_xy": [round(sx, 3), round(sy, 3)],
                })
                break

    # Pretty print
    print(f"\nscanned {n_scanned} prims total\n")
    for zname, items in obstacles.items():
        print(f"--- {zname}: {len(items)} obstacles in zone ---")
        for it in items:
            print(f"  {it['name']:30s}  type={it['type']:8s} center={it['center']} size={it['size_xy']}")
        print()

    print("=" * 60)
    print("=== JSON DUMP — copy this entire block for offline analysis ===")
    print("=" * 60)
    print(json.dumps(obstacles, indent=2))


main()
