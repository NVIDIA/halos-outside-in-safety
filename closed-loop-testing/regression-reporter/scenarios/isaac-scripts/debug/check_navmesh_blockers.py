# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
NavMesh blocker scan — paste into Isaac Sim Script Editor (Window → Script Editor)
with the warehouse scene loaded. Prints diagnostic for:

1. /World/Loading_Zone_Objects_01 itself — IsActive, visibility, instanceable
2. Each descendant — active/visible/has collider/has rigid body, world AABB
3. ALL prims with Physics Collider/RigidBody whose world AABB overlaps ROI south
   (x ∈ [4, 10], y ∈ [-19.5, -14])  — those would block NavMesh even if they
   look "deactivated" in the outliner.

Run, then read the console (Window → Console) for the report.
"""
from pxr import Usd, UsdGeom, UsdPhysics, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()
ROI = Gf.Range3f(Gf.Vec3f( 4.0, -19.5, -1.0),
                 Gf.Vec3f(10.0, -14.0,  4.0))

def has_api(prim, api_cls):
    return prim.HasAPI(api_cls)

def aabb_world(prim):
    cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                              ["default", "render"], useExtentsHint=True)
    try:
        bbox = cache.ComputeWorldBound(prim).ComputeAlignedRange()
        return bbox if not bbox.IsEmpty() else None
    except Exception:
        return None

def visible(prim):
    img = UsdGeom.Imageable(prim)
    if not img: return None
    return img.ComputeVisibility() != UsdGeom.Tokens.invisible

def report_prim(prim, indent=""):
    p = prim.GetPath()
    active = prim.IsActive()
    vis = visible(prim)
    rb = has_api(prim, UsdPhysics.RigidBodyAPI)
    col = has_api(prim, UsdPhysics.CollisionAPI)
    flags = []
    if not active: flags.append("INACTIVE")
    if vis is False: flags.append("invisible")
    if rb: flags.append("rb")
    if col: flags.append("collider")
    bb = aabb_world(prim) if active and vis else None
    bb_s = ""
    if bb:
        mn, mx = bb.GetMin(), bb.GetMax()
        bb_s = f"  bbox=({mn[0]:.2f},{mn[1]:.2f},{mn[2]:.2f})..({mx[0]:.2f},{mx[1]:.2f},{mx[2]:.2f})"
    print(f"{indent}{p}  [{','.join(flags) or '-'}]{bb_s}")

# 1. Loading_Zone_Objects_01 itself
target = stage.GetPrimAtPath("/World/Loading_Zone_Objects_01")
print("=" * 80)
print("1. /World/Loading_Zone_Objects_01")
print("=" * 80)
if not target or not target.IsValid():
    print("  PRIM NOT FOUND")
else:
    report_prim(target)
    print(f"  IsActive (full hierarchy considered):    {target.IsActive()}")
    print(f"  IsLoaded:                                 {target.IsLoaded()}")
    print(f"  IsInstanceable:                           {target.IsInstanceable()}")
    print(f"  HasPayload:                               {target.HasPayload()}")
    print()
    print("  Active descendants (would render/collide):")
    n_active = n_total = 0
    for child in Usd.PrimRange(target):
        n_total += 1
        if child.IsActive():
            n_active += 1
    print(f"    total={n_total}  active={n_active}")

# 2. Anything in ROI south with collider or rigidbody (regardless of which group)
print()
print("=" * 80)
print("2. Active prims with collider/rigidbody whose AABB overlaps ROI south")
print(f"   ROI x∈[{ROI.GetMin()[0]},{ROI.GetMax()[0]}] y∈[{ROI.GetMin()[1]},{ROI.GetMax()[1]}]")
print("=" * 80)
hits = []
for prim in stage.Traverse():
    if not prim.IsActive():
        continue
    if not (has_api(prim, UsdPhysics.CollisionAPI) or has_api(prim, UsdPhysics.RigidBodyAPI)):
        continue
    bb = aabb_world(prim)
    if bb is None or bb.IsEmpty():
        continue
    if bb.GetMin()[0] > ROI.GetMax()[0] or bb.GetMax()[0] < ROI.GetMin()[0]: continue
    if bb.GetMin()[1] > ROI.GetMax()[1] or bb.GetMax()[1] < ROI.GetMin()[1]: continue
    hits.append((str(prim.GetPath()), bb))
print(f"  found {len(hits)} prim(s) with collider/rigidbody overlapping ROI south:")
for p, bb in sorted(hits)[:60]:
    mn, mx = bb.GetMin(), bb.GetMax()
    print(f"    {p}")
    print(f"      bbox=({mn[0]:+.2f},{mn[1]:+.2f},{mn[2]:+.2f})..({mx[0]:+.2f},{mx[1]:+.2f},{mx[2]:+.2f})")
if len(hits) > 60:
    print(f"    ... and {len(hits)-60} more (truncated)")

# 3. NavMesh exclusion list (per the NavMesh tab)
print()
print("=" * 80)
print("3. Things explicitly excluded by NavMesh extension (omni.anim.navigation)")
print("=" * 80)
import carb.settings
s = carb.settings.get_settings()
excl_key = "/exts/omni.anim.navigation.core/navMesh/excludePaths"
try:
    excl = s.get(excl_key) or []
    print(f"  carb setting {excl_key}:")
    for e in excl:
        print(f"    {e}")
except Exception as e:
    print(f"  (could not read setting: {e})")

print()
print("=" * 80)
print("Done. If section 2 lists prims you thought were deactivated → those are")
print("still blocking NavMesh. Inspect them in the Stage outliner: a parent may")
print("be deactivated, but a child reference / sublayer override can keep the")
print("descendant active. Check `IsActive()` per-prim, and look at composition")
print("(Window → USD Layers) for any override that re-activates them.")
print("=" * 80)
