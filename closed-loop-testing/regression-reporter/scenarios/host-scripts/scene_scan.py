#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Scan a USD scene → emit JSON of every VISIBLE Xform/geometry prim with its
local xformOp:translate (payload-independent). Skips invisible prims.

Doesn't compute world bounding boxes — that requires loaded payloads.
Instead exports prim positions from xformOp:translate (Xform tree composes
naturally so we accumulate parent translations to get effective world pos).

Run:
    pip install usd-core
    python3 scene_scan.py --scene path/to/scene.usd --out scene.json [--tag /Characters]
"""
import argparse
import json
import sys
from pathlib import Path

from pxr import Usd, UsdGeom, Gf, Sdf


def is_effectively_visible(prim) -> bool:
    cur = prim
    while cur and cur.GetPath() != Sdf.Path.absoluteRootPath:
        if cur.IsA(UsdGeom.Imageable):
            v = UsdGeom.Imageable(cur).GetVisibilityAttr().Get()
            if v == "invisible":
                return False
        cur = cur.GetParent()
    return True


def get_translate(prim) -> tuple[float, float, float] | None:
    """Read the prim's xformOp:translate value if any."""
    if not prim.IsA(UsdGeom.Xformable):
        return None
    xf = UsdGeom.Xformable(prim)
    for op in xf.GetOrderedXformOps():
        if op.GetOpType() == UsdGeom.XformOp.TypeTranslate:
            v = op.Get()
            if v is None:
                continue
            return (float(v[0]), float(v[1]), float(v[2]))
    return None


def get_world_translate(prim) -> tuple[float, float, float]:
    """Sum xformOp:translate up the prim chain (cheap world-pos approximation
    for the common case where ancestors only translate)."""
    x = y = z = 0.0
    cur = prim
    while cur and cur.GetPath() != Sdf.Path.absoluteRootPath:
        t = get_translate(cur)
        if t:
            x += t[0]; y += t[1]; z += t[2]
        cur = cur.GetParent()
    return (round(x, 4), round(y, 4), round(z, 4))


def scan(scene_path: str, tag: str | None) -> dict:
    stage = Usd.Stage.Open(scene_path)
    if stage is None:
        raise RuntimeError(f"Could not open USD: {scene_path}")

    type_summary: dict[str, int] = {}
    prims_out: list[dict] = []
    name_groups: dict[str, list[dict]] = {}

    for prim in stage.Traverse():
        path = str(prim.GetPath())
        if not path.startswith("/World"):
            continue
        if tag and tag not in path:
            continue
        if not prim.IsA(UsdGeom.Xformable):
            continue
        if not is_effectively_visible(prim):
            continue

        type_name = str(prim.GetTypeName())
        depth = path.count("/")
        local_t = get_translate(prim)
        # only emit prims that actually carry a translate or are leaves of interest
        if local_t is None and type_name not in ("Mesh",):
            continue

        world_t = get_world_translate(prim)
        rec = {
            "path": path,
            "name": prim.GetName(),
            "type": type_name,
            "depth": depth,
            "local_translate": list(local_t) if local_t else None,
            "world_translate": list(world_t),
        }
        prims_out.append(rec)
        type_summary[type_name] = type_summary.get(type_name, 0) + 1

        # group by lowercase name fragment
        nm = prim.GetName().lower()
        for tag_name in (
            "shelf", "rack", "pallet", "palette", "box", "cardbox", "crate",
            "wall", "floor", "ceiling", "trailer", "truck",
            "forklift", "character", "person", "humanoid",
            "camera", "light", "fixture", "door"
        ):
            if tag_name in nm:
                name_groups.setdefault(tag_name, []).append({
                    "path": path, "name": prim.GetName(),
                    "world_translate": list(world_t),
                })
                break

    # bounding boxes per group
    group_extents = {}
    for grp, items in name_groups.items():
        xs = [it["world_translate"][0] for it in items]
        ys = [it["world_translate"][1] for it in items]
        zs = [it["world_translate"][2] for it in items]
        group_extents[grp] = {
            "n": len(items),
            "x_range": [round(min(xs), 3), round(max(xs), 3)],
            "y_range": [round(min(ys), 3), round(max(ys), 3)],
            "z_range": [round(min(zs), 3), round(max(zs), 3)],
        }

    return {
        "scene": scene_path,
        "type_summary": type_summary,
        "n_prims": len(prims_out),
        "groups": group_extents,
        "prims": prims_out,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default=None)
    args = ap.parse_args()

    result = scan(args.scene, args.tag)
    Path(args.out).write_text(json.dumps(result, indent=2))
    print(f"[scan] wrote {args.out}")
    print(f"[scan] n_prims emitted: {result['n_prims']}")
    print(f"[scan] type summary: {result['type_summary']}")
    print(f"[scan] groups (n, xy bounds):")
    for grp, ext in result["groups"].items():
        print(f"  {grp:12s} n={ext['n']:4d} "
              f"x=[{ext['x_range'][0]:+7.2f},{ext['x_range'][1]:+7.2f}] "
              f"y=[{ext['y_range'][0]:+7.2f},{ext['y_range'][1]:+7.2f}]")


if __name__ == "__main__":
    main()
