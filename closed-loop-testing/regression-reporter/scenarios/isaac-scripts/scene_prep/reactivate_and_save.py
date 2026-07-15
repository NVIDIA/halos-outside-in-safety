# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Re-activate the 4 physical obstacle prims (kept exclude_volumes as Include),
re-bake NavMesh, spot-check, then Save As to a new USD file.

Result: warehouse with full visual obstacles back, but NavMesh has no
artificial exclusion volumes — only physical colliders carve holes.

Pre-conditions: scene is the *_nav_clear.usd variant (exclude volumes
already flipped to Include + 4 obstacles deactivated).
"""
import json, time, asyncio
from pxr import Usd, UsdGeom, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()

OBSTACLE_PATHS = [
    "/World/SM_HeavyDutyPalletTruck_A01_01",
    "/World/SM_Forklift_A01_Blue_01",
    "/World/Loading_Zone_Objects_01",
    "/World/Loading_Zone_Objects",
]
SAVE_AS = ("/isaac-sim/sil/scenes/"
           "indicator_warehouse_20x20_odom_srr_nav_clear_with_objects.usd")

print("=" * 78)
print("Step 1: re-activate physical obstacle prims")
print("=" * 78)
# Edit on root layer
old_target = stage.GetEditTarget()
stage.SetEditTarget(stage.GetRootLayer())
try:
    for p in OBSTACLE_PATHS:
        prim = stage.GetPrimAtPath(p)
        if not prim or not prim.IsValid():
            print(f"  SKIP not found: {p}")
            continue
        if prim.IsActive():
            print(f"  already active: {p}")
            continue
        prim.SetActive(True)
        print(f"  re-activated: {p}  IsActive={prim.IsActive()}")
finally:
    stage.SetEditTarget(old_target)

# Step 2: clear cache + sync bake
print()
print("=" * 78)
print("Step 2: clear cache + sync rebake")
print("=" * 78)
import omni.anim.navigation.core as nav
iface = nav.acquire_interface()
iface.clear_cache_dir()
try: iface.cancel_navmesh_baking()
except Exception: pass
t0 = time.time()
iface.start_navmesh_baking_and_wait()
print(f"  bake done in {time.time()-t0:.1f}s")
# A small extra wait — viz mesh may still be settling immediately after
time.sleep(2.0)

# Step 3: re-export navmesh.json + spot-check
print()
print("=" * 78)
print("Step 3: re-export + spot-check")
print("=" * 78)
viz = None
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if "/__omni_nav_mesh_viz_" in p and prim.GetTypeName() == "Mesh":
        viz = prim; break
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
            tri = []
            for vi in (verts[0], verts[k], verts[k+1]):
                pp = points[vi]
                wp = world.Transform(Gf.Vec3d(pp[0], pp[1], pp[2]))
                tri.append([float(wp[0]), float(wp[1])])
                xs.append(wp[0]); ys.append(wp[1])
            triangles.append(tri)
    bbox = [min(xs), min(ys), max(xs), max(ys)] if xs else [0, 0, 0, 0]
    out = "/isaac-sim/sil/configs/navmesh.json"
    with open(out, "w") as f:
        json.dump({"triangles_xy": triangles, "bbox": bbox,
                   "tri_count": len(triangles)}, f)
    print(f"  wrote {len(triangles)} triangles → {out}")
    print(f"  bbox xy: [{bbox[0]:.2f},{bbox[1]:.2f}] .. [{bbox[2]:.2f},{bbox[3]:.2f}]")

    def pit(x, y, t):
        (x1,y1),(x2,y2),(x3,y3) = t
        d = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
        if d == 0: return False
        a = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / d
        b = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / d
        return a >= -1e-6 and b >= -1e-6 and (1-a-b) >= -1e-6

    test_pts = [
        # char zones — should still be walkable (no physical obstacle there)
        ("char_0 spot",                    5.20, -11.24, True),
        ("char_1 spot",                    5.31, -17.82, True),
        ("char_2 spot",                    4.72, -16.89, True),
        # cardboard / pallet / loading_zone — now should be NOT walkable (obstacles back)
        ("Loading_Zone_Objects (cardboard)", 5.72, -15.17, False),
        ("Loading_Zone_Objects pallet",      5.50, -15.20, False),
        # exclude_04 area — no physical obstacle here, was just navmesh exclude
        ("formerly exclude_04",            7.97, -17.27, True),
        # NE area
        ("formerly exclude_01",            6.07,  -9.30, True),
    ]
    print()
    print("Spot-check (✓ = matches expected):")
    for label, x, y, expected in test_pts:
        ok = any(pit(x, y, t) for t in triangles)
        match = "✓" if ok == expected else "✗"
        print(f"  {match} {label:35s} ({x:+.2f},{y:+.2f}) walkable={ok}  expected={expected}")

# Step 4: save
print()
print("=" * 78)
print(f"Step 4: Save As → {SAVE_AS}")
print("=" * 78)
async def _save():
    ok, err = await omni.usd.get_context().save_as_stage_async(SAVE_AS)
    if ok:
        print(f"  saved successfully")
    else:
        print(f"  save failed: {err}")

asyncio.ensure_future(_save())
print("  save scheduled (will complete in a moment; check Console for result)")
print()
print("After save completes, set this in default_config_ros.yaml:")
print(f"  scene.asset_path: {SAVE_AS}")
