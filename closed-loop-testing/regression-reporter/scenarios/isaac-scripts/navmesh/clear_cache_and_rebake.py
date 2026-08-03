# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Clear NavMesh cache + run SYNCHRONOUS bake (start_navmesh_baking_and_wait).
The async start_navmesh_baking() doesn't actually re-bake when there's a
cached result; this version forces a fresh bake.

Run AFTER the prims have been deactivated (clear_and_rebake.py step 1).
"""
import json, time
from pxr import Usd, UsdGeom, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()


def viz_prim():
    for prim in stage.Traverse():
        p = str(prim.GetPath())
        if p.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
            return prim
    return None


def tri_count():
    p = viz_prim()
    if p is None: return None
    counts = UsdGeom.Mesh(p).GetFaceVertexCountsAttr().Get()
    return sum(c-2 for c in counts) if counts else 0


import omni.anim.navigation.core as nav
iface = nav.acquire_interface()

print("=" * 70)
print("Initial tri_count:", tri_count())
print("Is currently baking:", iface.is_navmesh_baking() if hasattr(iface, "is_navmesh_baking") else "?")
print("Cache dir:", iface.get_cache_dir() if hasattr(iface, "get_cache_dir") else "?")
print("=" * 70)

# 1. Clear cache
print()
print("Step 1: clear_cache_dir()")
try:
    iface.clear_cache_dir()
    print("  cache cleared")
except Exception as e:
    print(f"  failed: {e}")

# 2. Cancel any in-flight async bake (just in case)
try:
    iface.cancel_navmesh_baking()
    print("  cancelled any in-flight bake")
except Exception:
    pass

# 3. Synchronous bake
print()
print("Step 2: start_navmesh_baking_and_wait() — SYNCHRONOUS, may take 5-30s...")
t0 = time.time()
try:
    iface.start_navmesh_baking_and_wait()
    print(f"  bake completed in {time.time()-t0:.1f}s")
except Exception as e:
    print(f"  failed: {e}")

print(f"\nNew tri_count: {tri_count()}")

# 4. Re-export
print()
print("Step 3: Re-export navmesh.json")
viz = viz_prim()
if viz is None:
    print("  no viz mesh!")
else:
    mesh = UsdGeom.Mesh(viz)
    points = mesh.GetPointsAttr().Get()
    counts = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    world = UsdGeom.Xformable(viz).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
    triangles = []; xs = []; ys = []
    i = 0
    for c in counts:
        verts = indices[i:i+c]; i += c
        for k in range(1, c-1):
            tri = [verts[0], verts[k], verts[k+1]]
            pts = []
            for vi in tri:
                p = points[vi]
                wp = world.Transform(Gf.Vec3d(p[0], p[1], p[2]))
                pts.append([float(wp[0]), float(wp[1])])
                xs.append(wp[0]); ys.append(wp[1])
            triangles.append(pts)
    bbox = [min(xs), min(ys), max(xs), max(ys)] if xs else [0, 0, 0, 0]
    out = "/isaac-sim/sil/configs/navmesh.json"
    with open(out, "w") as f:
        json.dump({"triangles_xy": triangles, "bbox": bbox, "tri_count": len(triangles)}, f)
    print(f"  wrote {len(triangles)} triangles → {out}")
    print(f"  bbox xy: [{bbox[0]:.2f}, {bbox[1]:.2f}] .. [{bbox[2]:.2f}, {bbox[3]:.2f}]")

    # 5. Spot-check points that USED to be inside exclude volumes
    def pit(x, y, t):
        (x1,y1),(x2,y2),(x3,y3)=t
        d = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
        if d == 0: return False
        a = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / d
        b = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / d
        return a >= -1e-6 and b >= -1e-6 and (1-a-b) >= -1e-6

    test_pts = [
        ("inside exclude_03 (cardboard)",  5.72, -15.17),
        ("inside exclude_04 (south)",      7.97, -17.27),
        ("inside exclude_01 (NE)",         6.07,  -9.30),
        ("inside exclude (E strip)",      12.00, -15.68),
        ("char_0 stuck",                   5.20, -11.24),
        ("char_1 stuck",                   5.31, -17.82),
        ("char_2 stuck",                   4.72, -16.89),
    ]
    print()
    print("Spot-check after fresh bake:")
    for label, x, y in test_pts:
        ok = any(pit(x, y, t) for t in triangles)
        flag = "✓" if ok else "✗"
        print(f"  {flag} {label:35s} ({x:+.2f},{y:+.2f}) walkable={ok}")

print()
print("=" * 70)
print("Done. If exclude interior points are now walkable=True, the bake")
print("worked. File → Save (Ctrl+S) to persist scene .usd.")
print("=" * 70)
