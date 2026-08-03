# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Properly disable NavMeshVolume_exclude_* by setting their schema attribute
`nav:volume:type` from "Exclude" → "Include" (an Include over a region
already covered by another Include is a no-op = effectively disabled).

This is the *documented* attribute in the NavMesh schema:
    /isaac-sim/extscache/omni.anim.navigation.schema-*/plugins/NavSchema/
        resources/schema.usda  (lines ~22-30)

Setting prim.SetActive(False) was ignored by the bake. This is the
correct fix.

After running: clear cache + sync rebake + re-export navmesh.json.
"""
import json, time
from pxr import Usd, UsdGeom, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()

EXCLUDE_PATHS = [
    "/World/Navmesh/NavMeshVolume_exclude",
    "/World/Navmesh/NavMeshVolume_exclude_01",
    "/World/Navmesh/NavMeshVolume_exclude_02",
    "/World/Navmesh/NavMeshVolume_exclude_03",
    "/World/Navmesh/NavMeshVolume_exclude_04",
]

print("=" * 70)
print("Step 1: re-activate prims (IsActive=True) and flip volume type")
print("=" * 70)
for path in EXCLUDE_PATHS:
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsValid():
        print(f"  SKIP not found: {path}")
        continue
    if not prim.IsActive():
        prim.SetActive(True)  # undo earlier deactivation so attr can be read
    attr = prim.GetAttribute("nav:volume:type")
    if not attr:
        print(f"  WARN: {path} has no nav:volume:type attribute")
        continue
    old = attr.Get()
    attr.Set("Include")
    new = attr.Get()
    print(f"  {path}: nav:volume:type {old!r} → {new!r}")

# Step 2: clear cache + sync bake
print()
print("=" * 70)
print("Step 2: clear cache + start_navmesh_baking_and_wait()")
print("=" * 70)
import omni.anim.navigation.core as nav
iface = nav.acquire_interface()
try:
    iface.clear_cache_dir()
    print("  cache cleared")
except Exception as e:
    print(f"  clear_cache_dir failed: {e}")
try:
    iface.cancel_navmesh_baking()
except Exception:
    pass
t0 = time.time()
iface.start_navmesh_baking_and_wait()
print(f"  bake done in {time.time()-t0:.1f}s")

# Step 3: re-export
print()
print("=" * 70)
print("Step 3: Re-export navmesh.json")
print("=" * 70)
viz = None
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if p.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
        viz = prim
        break

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
    bbox = [min(xs), min(ys), max(xs), max(ys)] if xs else [0,0,0,0]
    out = "/isaac-sim/sil/configs/navmesh.json"
    with open(out, "w") as f:
        json.dump({"triangles_xy": triangles, "bbox": bbox,
                   "tri_count": len(triangles)}, f)
    print(f"  wrote {len(triangles)} triangles → {out}")
    print(f"  bbox xy: [{bbox[0]:.2f}, {bbox[1]:.2f}] .. [{bbox[2]:.2f}, {bbox[3]:.2f}]")

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
    print("Spot-check (formerly-excluded points should now be walkable=True):")
    for label, x, y in test_pts:
        ok = any(pit(x, y, t) for t in triangles)
        flag = "✓" if ok else "✗"
        print(f"  {flag} {label:35s} ({x:+.2f},{y:+.2f}) walkable={ok}")

print()
print("=" * 70)
print("Done. File → Save (Ctrl+S) to persist scene .usd.")
print("=" * 70)
