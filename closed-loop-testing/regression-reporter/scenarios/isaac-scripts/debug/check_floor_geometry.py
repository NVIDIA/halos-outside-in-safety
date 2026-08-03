# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Find why bake produces empty mesh: dump every Mesh prim with collider near
the warehouse floor area, plus active-state of all top-level prims.
"""
from pxr import Usd, UsdGeom, UsdPhysics, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()

print("=" * 78)
print("Top-level prims under /World — active/inactive")
print("=" * 78)
world = stage.GetPrimAtPath("/World")
if world:
    for child in world.GetChildren():
        path = str(child.GetPath())
        active = child.IsActive()
        loaded = child.IsLoaded()
        flag = "" if active else " ←INACTIVE"
        print(f"  {path:60s}  active={active} loaded={loaded}{flag}")

print()
print("=" * 78)
print("Mesh prims with CollisionAPI within warehouse area "
      "(x∈[-20,20], y∈[-25,5], z∈[-1,1])")
print("=" * 78)
cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                          ["default", "render"], useExtentsHint=True)
hit_floor = []
for prim in stage.Traverse():
    if not prim.IsActive():
        continue
    if prim.GetTypeName() != "Mesh":
        continue
    if not prim.HasAPI(UsdPhysics.CollisionAPI):
        continue
    img = UsdGeom.Imageable(prim)
    visible = img.ComputeVisibility() != UsdGeom.Tokens.invisible if img else False
    try:
        bb = cache.ComputeWorldBound(prim).ComputeAlignedRange()
    except Exception:
        continue
    if bb.IsEmpty(): continue
    mn, mx = bb.GetMin(), bb.GetMax()
    if mn[0] > 20 or mx[0] < -20: continue
    if mn[1] > 5  or mx[1] < -25: continue
    if mn[2] > 1  or mx[2] < -1:  continue
    flag = "" if visible else " (invisible)"
    hit_floor.append((str(prim.GetPath()), mn, mx, visible))

print(f"  {len(hit_floor)} mesh+collider prims in floor band")
# Sort by path; show floor-like names first
hit_floor.sort(key=lambda h: ("floor" not in h[0].lower(), h[0]))
for path, mn, mx, vis in hit_floor[:25]:
    print(f"  {'V' if vis else 'I'}  {path}")
    print(f"       bbox=({mn[0]:+.2f},{mn[1]:+.2f},{mn[2]:+.2f})..({mx[0]:+.2f},{mx[1]:+.2f},{mx[2]:+.2f})")
if len(hit_floor) > 25:
    print(f"  ... +{len(hit_floor)-25} more")

print()
print("=" * 78)
print("NavMeshVolume parent bbox (where bake will look for floor)")
print("=" * 78)
nv = stage.GetPrimAtPath("/World/Navmesh/NavMeshVolume")
if nv and nv.IsActive():
    bb = cache.ComputeWorldBound(nv).ComputeAlignedRange()
    if not bb.IsEmpty():
        mn, mx = bb.GetMin(), bb.GetMax()
        print(f"  bbox=({mn[0]:+.2f},{mn[1]:+.2f},{mn[2]:+.2f})..({mx[0]:+.2f},{mx[1]:+.2f},{mx[2]:+.2f})")
    else:
        print("  bbox empty (parent NavMeshVolume has no extents → bake covers nothing)")
else:
    print("  parent NavMeshVolume not active or not found!")
