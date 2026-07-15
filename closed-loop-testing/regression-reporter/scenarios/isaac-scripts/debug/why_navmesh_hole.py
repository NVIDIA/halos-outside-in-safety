# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Diagnose why NavMesh leaves a hole at the currently selected prim.

Workflow:
  1. In Viewport, click the white/uncovered area so the prim is selected
     in the Stage outliner.
  2. Run this script (Script Editor → Run).
  3. Read Console output.

Reports for the selected prim and every active prim whose bbox
overlaps it:
  - active / visible / instanceable
  - has RigidBody / Collision API (rigidbody + auto-exclude → no NavMesh)
  - any omni:anim:navigation:* custom attrs (explicit exclude)
  - z extent (slope/elevation check)
Also dumps NavMesh extension settings (walkable slope/climb/agent radius).
"""
from pxr import Usd, UsdGeom, UsdPhysics, Gf
import omni.usd
import carb.settings

stage = omni.usd.get_context().get_stage()
ctx = omni.usd.get_context()
sel_paths = ctx.get_selection().get_selected_prim_paths()

if not sel_paths:
    print("No prim selected. Click the uncovered area in the viewport first.")
    raise SystemExit

target = stage.GetPrimAtPath(sel_paths[0])
print("=" * 78)
print(f"Selected: {target.GetPath()}")
print("=" * 78)


def aabb(prim):
    cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                              ["default", "render"], useExtentsHint=True)
    try:
        bb = cache.ComputeWorldBound(prim).ComputeAlignedRange()
        return bb if not bb.IsEmpty() else None
    except Exception:
        return None


def navmesh_attrs(prim):
    out = {}
    for a in prim.GetAttributes():
        n = a.GetName()
        if "navigation" in n.lower() or "navmesh" in n.lower() or n.startswith("omni:anim:navigation"):
            out[n] = a.Get()
    return out


def report(prim, indent=""):
    p = prim.GetPath()
    active = prim.IsActive()
    img = UsdGeom.Imageable(prim)
    vis = (img.ComputeVisibility() != UsdGeom.Tokens.invisible) if img else None
    rb = prim.HasAPI(UsdPhysics.RigidBodyAPI)
    col = prim.HasAPI(UsdPhysics.CollisionAPI)
    nav = navmesh_attrs(prim)
    bb = aabb(prim) if active and vis else None

    flags = []
    if not active: flags.append("INACTIVE")
    if vis is False: flags.append("invisible")
    if rb: flags.append("rb")
    if col: flags.append("collider")
    if nav: flags.append("hasNavAttrs")
    print(f"{indent}{p}  type={prim.GetTypeName()}  [{','.join(flags) or '-'}]")
    if bb:
        mn, mx = bb.GetMin(), bb.GetMax()
        z_extent = mx[2] - mn[2]
        print(f"{indent}  bbox=({mn[0]:+.3f},{mn[1]:+.3f},{mn[2]:+.3f})..({mx[0]:+.3f},{mx[1]:+.3f},{mx[2]:+.3f})  z_extent={z_extent:.3f}m")
    if nav:
        for k, v in nav.items():
            print(f"{indent}  {k} = {v!r}")


# 1. The selected prim itself
print()
print("--- selected prim ---")
report(target)

# 2. Walk up its ancestors — maybe a parent is what NavMesh sees
print()
print("--- ancestors (until /World) ---")
parent = target.GetParent()
while parent and parent.IsValid() and str(parent.GetPath()) != "/":
    report(parent, indent="  ")
    if str(parent.GetPath()) == "/World":
        break
    parent = parent.GetParent()

# 3. Descendants of the selected prim with collider/rigidbody/navAttrs
print()
print("--- descendants of selected with collider/rb/navAttrs ---")
desc_hits = []
for child in Usd.PrimRange(target):
    if child == target:
        continue
    if not child.IsActive():
        continue
    if (child.HasAPI(UsdPhysics.CollisionAPI) or
        child.HasAPI(UsdPhysics.RigidBodyAPI) or
        navmesh_attrs(child)):
        desc_hits.append(child)
print(f"  found {len(desc_hits)} descendant(s)")
for p in desc_hits[:30]:
    report(p, indent="  ")
if len(desc_hits) > 30:
    print(f"  ... and {len(desc_hits)-30} more")

# 4. Other active prims whose bbox overlaps target bbox
target_bb = aabb(target)
if target_bb is None:
    print("\n(target bbox empty; cannot do overlap scan)")
else:
    pad = 0.5
    mn = target_bb.GetMin(); mx = target_bb.GetMax()
    tmin = (mn[0] - pad, mn[1] - pad, mn[2] - pad)
    tmax = (mx[0] + pad, mx[1] + pad, mx[2] + pad)
    print()
    print(f"--- other active prims overlapping target bbox (±{pad}m) ---")
    found = []
    target_path = target.GetPath()
    for prim in stage.Traverse():
        pp = prim.GetPath()
        if pp == target_path or pp.HasPrefix(target_path):
            continue  # skip self + descendants (already shown above)
        if not prim.IsActive():
            continue
        if not (prim.HasAPI(UsdPhysics.CollisionAPI) or
                prim.HasAPI(UsdPhysics.RigidBodyAPI) or
                navmesh_attrs(prim)):
            continue
        bb = aabb(prim)
        if bb is None or bb.IsEmpty(): continue
        bmn, bmx = bb.GetMin(), bb.GetMax()
        if bmn[0] > tmax[0] or bmx[0] < tmin[0]: continue
        if bmn[1] > tmax[1] or bmx[1] < tmin[1]: continue
        if bmn[2] > tmax[2] or bmx[2] < tmin[2]: continue
        found.append(prim)
    print(f"  found {len(found)} overlapping prim(s)")
    for p in sorted(found, key=lambda x: str(x.GetPath()))[:40]:
        report(p, indent="  ")
    if len(found) > 40:
        print(f"  ... and {len(found)-40} more")

# 4. NavMesh global settings — Walkable Slope / Walkable Climb / Agent Radius
print()
print("--- NavMesh extension settings ---")
s = carb.settings.get_settings()
keys = [
    "/exts/omni.anim.navigation.core/navMesh/config/walkableSlopeAngle",
    "/exts/omni.anim.navigation.core/navMesh/config/walkableClimb",
    "/exts/omni.anim.navigation.core/navMesh/config/walkableHeight",
    "/exts/omni.anim.navigation.core/navMesh/config/walkableRadius",
    "/exts/omni.anim.navigation.core/navMesh/config/cellSize",
    "/exts/omni.anim.navigation.core/navMesh/config/cellHeight",
    "/exts/omni.anim.navigation.core/navMesh/config/maxClimbHeight",
    "/exts/omni.anim.navigation.core/navMesh/config/agentHeight",
    "/exts/omni.anim.navigation.core/navMesh/config/agentRadius",
    "/exts/omni.anim.navigation.core/navMesh/excludePaths",
    "/exts/omni.anim.navigation.core/navMesh/excludeRigidBodies",
    "/exts/omni.anim.navigation.core/navMesh/autoBakeOnSceneOpen",
]
for k in keys:
    try:
        v = s.get(k)
        print(f"  {k} = {v!r}")
    except Exception as e:
        print(f"  {k} = (error: {e})")

print()
print("=" * 78)
print("Likely causes (read top→bottom):")
print(" - if target has 'rb' flag and excludeRigidBodies=True → that's it")
print(" - if z_extent > walkableClimb → too tall a step")
print(" - if any ancestor has 'hasNavAttrs' or omni:anim:navigation:exclude=True")
print(" - if other overlapping prim has rb+collider → it sits on top, blocking")
print(" - if walkableSlopeAngle too small for the surface tilt")
print("=" * 78)
