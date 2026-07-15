# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Scan whole stage for prims that mark NavMesh exclusion zones:
1) any prim with custom attr containing 'navigation', 'navmesh', 'exclude'
2) any prim with name suggesting an exclusion zone
3) any active-but-invisible prim with collision (commonly used for invisible
   barriers / loading-dock cutouts).
Prints path, bbox, attrs. The 'no-NavMesh' rectangle in the viewport almost
always shows up here.
"""
from pxr import Usd, UsdGeom, UsdPhysics
import omni.usd

stage = omni.usd.get_context().get_stage()

NAME_HINTS = [
    "exclude", "exclusion", "navmesh", "no_walk", "nowalk", "no_nav",
    "loading_dock", "loadingdock", "dock_zone", "danger", "hazard",
    "safety", "off_limits", "obstacle", "barrier",
]

print("=" * 80)
print("1. Prims with navigation-related attrs")
print("=" * 80)
hits1 = []
for prim in stage.Traverse():
    if not prim.IsActive(): continue
    for a in prim.GetAttributes():
        n = a.GetName().lower()
        if "navigation" in n or "navmesh" in n or n.endswith(":exclude"):
            hits1.append((prim, a))
            break
print(f"  found {len(hits1)} prim(s)")
for prim, _ in hits1[:80]:
    print(f"  {prim.GetPath()}")
    for a in prim.GetAttributes():
        n = a.GetName().lower()
        if "navigation" in n or "navmesh" in n or n.endswith(":exclude"):
            print(f"    {a.GetName()} = {a.Get()!r}")

print()
print("=" * 80)
print("2. Prims with names hinting at exclusion zones")
print("=" * 80)
hits2 = []
for prim in stage.Traverse():
    if not prim.IsActive(): continue
    name = prim.GetName().lower()
    if any(h in name for h in NAME_HINTS):
        hits2.append(prim)
print(f"  found {len(hits2)} prim(s)")
for prim in hits2[:60]:
    bb = None
    try:
        cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                                  ["default", "render"], useExtentsHint=True)
        bb = cache.ComputeWorldBound(prim).ComputeAlignedRange()
    except Exception:
        pass
    img = UsdGeom.Imageable(prim)
    vis = (img.ComputeVisibility() != UsdGeom.Tokens.invisible) if img else None
    rb = prim.HasAPI(UsdPhysics.RigidBodyAPI)
    col = prim.HasAPI(UsdPhysics.CollisionAPI)
    flags = []
    if vis is False: flags.append("invisible")
    if rb: flags.append("rb")
    if col: flags.append("collider")
    bb_s = ""
    if bb and not bb.IsEmpty():
        mn, mx = bb.GetMin(), bb.GetMax()
        bb_s = f"  bbox=({mn[0]:+.2f},{mn[1]:+.2f},{mn[2]:+.2f})..({mx[0]:+.2f},{mx[1]:+.2f},{mx[2]:+.2f})"
    print(f"  {prim.GetPath()}  type={prim.GetTypeName()}  [{','.join(flags) or '-'}]{bb_s}")

print()
print("=" * 80)
print("3. Active-but-invisible prims with collider (potential blockers)")
print("=" * 80)
hits3 = []
for prim in stage.Traverse():
    if not prim.IsActive(): continue
    if not prim.HasAPI(UsdPhysics.CollisionAPI): continue
    img = UsdGeom.Imageable(prim)
    if not img: continue
    if img.ComputeVisibility() == UsdGeom.Tokens.invisible:
        hits3.append(prim)
print(f"  found {len(hits3)} invisible+collider prim(s)")
seen_parents = set()
for prim in hits3[:80]:
    # collapse repeated forklift links etc.
    parent = str(prim.GetPath().GetParentPath().GetParentPath())
    if parent in seen_parents and "forklift" in parent.lower():
        continue
    seen_parents.add(parent)
    try:
        cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                                  ["default", "render"], useExtentsHint=True)
        bb = cache.ComputeWorldBound(prim).ComputeAlignedRange()
    except Exception:
        bb = None
    bb_s = ""
    if bb and not bb.IsEmpty():
        mn, mx = bb.GetMin(), bb.GetMax()
        bb_s = f"  bbox=({mn[0]:+.2f},{mn[1]:+.2f},{mn[2]:+.2f})..({mx[0]:+.2f},{mx[1]:+.2f},{mx[2]:+.2f})"
    print(f"  {prim.GetPath()}{bb_s}")

print()
print("=" * 80)
print("Tip: NavMesh viz mesh path is /__omni_nav_mesh_viz_*.")
print("Anything in section 1 with omni:anim:navigation:exclude=True or similar")
print("is the explicit cutout. Section 2/3 are heuristic and may have FPs.")
print("=" * 80)
