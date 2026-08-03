# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Wait for an in-progress NavMesh bake to finish, then re-export navmesh.json.
Run AFTER clear_and_rebake.py if the spot-check still showed walkable=False
for points inside formerly-active exclusion volumes.

Or, click Bake manually in the NavMesh tab, wait until the cyan viz mesh
visibly updates, then run this script to refresh the JSON.
"""
import json, time
from pxr import Usd, UsdGeom, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()


def get_viz():
    for prim in stage.Traverse():
        p = str(prim.GetPath())
        if p.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
            return prim
    return None


def viz_tri_count(prim):
    if prim is None: return None
    counts = UsdGeom.Mesh(prim).GetFaceVertexCountsAttr().Get()
    return sum(c-2 for c in counts)


def export(prim, path="/isaac-sim/sil/configs/navmesh.json"):
    mesh = UsdGeom.Mesh(prim)
    points = mesh.GetPointsAttr().Get()
    counts = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    world = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
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
    with open(path, "w") as f:
        json.dump({"triangles_xy": triangles, "bbox": bbox,
                   "tri_count": len(triangles)}, f)
    return triangles, bbox


# 1. Trigger bake again to be sure
print("Triggering bake (idempotent)...")
try:
    import omni.anim.navigation.core as nav
    nav.acquire_interface().start_navmesh_baking()
    print("  bake started")
except Exception as e:
    print(f"  could not start bake: {e}")

# 2. Poll viz mesh tri count up to 30s for it to "stabilize"
print("\nPolling NavMesh viz mesh tri count (up to 30s)...")
last_count = None
stable_for = 0
for s in range(30):
    viz = get_viz()
    n = viz_tri_count(viz) if viz else None
    print(f"  t={s:2d}s  tri_count={n}")
    if n is not None and n == last_count:
        stable_for += 1
        if stable_for >= 3:
            print(f"  → stable at {n} triangles")
            break
    else:
        stable_for = 0
        last_count = n
    time.sleep(1.0)

# 3. Re-export
viz = get_viz()
if viz is None:
    print("\nNo viz mesh — cannot export.")
else:
    triangles, bbox = export(viz)
    print(f"\nExported {len(triangles)} triangles → /isaac-sim/sil/configs/navmesh.json")
    print(f"  bbox xy: [{bbox[0]:.2f}, {bbox[1]:.2f}] .. [{bbox[2]:.2f}, {bbox[3]:.2f}]")

    # spot-check
    def pit(x, y, t):
        (x1,y1),(x2,y2),(x3,y3)=t
        d = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
        if d == 0: return False
        a = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / d
        b = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / d
        return a >= -1e-6 and b >= -1e-6 and (1-a-b) >= -1e-6

    test_pts = [
        ("inside exclude_03 (cardboard)", 5.72, -15.17),
        ("inside exclude_04 (south)",     7.97, -17.27),
        ("inside exclude_01 (NE)",        6.07,  -9.30),
        ("inside exclude (E strip)",     12.00, -15.68),
        ("char_0 stuck",                  5.20, -11.24),
        ("char_1 stuck",                  5.31, -17.82),
        ("char_2 stuck",                  4.72, -16.89),
    ]
    print("\nSpot check after fresh bake:")
    for label, x, y in test_pts:
        ok = any(pit(x, y, t) for t in triangles)
        print(f"  {label:35s} ({x:+.2f},{y:+.2f}) walkable={ok}")
