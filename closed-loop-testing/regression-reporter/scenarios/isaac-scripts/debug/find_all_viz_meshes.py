# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
List ALL /__omni_nav_mesh_viz_*/_a0/mesh prims in stage. After multiple
bakes/cache-clears, multiple orphan viz prims can exist and our exporter
might be picking the stale one. Compare tri counts + bboxes to find the
fresh one.
"""
from pxr import Usd, UsdGeom, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()

found = []
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if "/__omni_nav_mesh_viz_" in p and prim.GetTypeName() == "Mesh":
        mesh = UsdGeom.Mesh(prim)
        counts = mesh.GetFaceVertexCountsAttr().Get() or []
        tri = sum(c-2 for c in counts)
        pts = mesh.GetPointsAttr().Get() or []
        if pts:
            world = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
            wp = [world.Transform(Gf.Vec3d(p[0],p[1],p[2])) for p in pts]
            xs = [w[0] for w in wp]; ys = [w[1] for w in wp]
            bbox = (min(xs), min(ys), max(xs), max(ys))
        else:
            bbox = None
        found.append((p, tri, bbox, prim.IsActive()))

print(f"Found {len(found)} viz mesh prim(s):")
for p, tri, bb, active in found:
    print(f"\n  {p}")
    print(f"    tri_count : {tri}")
    print(f"    IsActive  : {active}")
    if bb:
        print(f"    bbox xy   : [{bb[0]:.2f},{bb[1]:.2f}] .. [{bb[2]:.2f},{bb[3]:.2f}]")

# Also sanity: do any of them cover the formerly-excluded points?
test_pts = [(5.72, -15.17, "exclude_03"), (7.97, -17.27, "exclude_04"),
            (6.07, -9.30, "exclude_01"), (12.00, -15.68, "exclude_E")]

def pit(x, y, t):
    (x1,y1),(x2,y2),(x3,y3) = t
    d = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
    if d == 0: return False
    a = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / d
    b = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / d
    return a >= -1e-6 and b >= -1e-6 and (1-a-b) >= -1e-6

print()
print("Walkability of formerly-excluded points per each viz mesh:")
for p, _, _, _ in found:
    prim = stage.GetPrimAtPath(p)
    mesh = UsdGeom.Mesh(prim)
    points = mesh.GetPointsAttr().Get()
    counts = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    world = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
    triangles = []
    i = 0
    for c in counts:
        verts = indices[i:i+c]; i += c
        for k in range(1, c-1):
            tri = []
            for vi in (verts[0], verts[k], verts[k+1]):
                pp = points[vi]
                wp = world.Transform(Gf.Vec3d(pp[0], pp[1], pp[2]))
                tri.append([float(wp[0]), float(wp[1])])
            triangles.append(tri)
    print(f"\n  {p}:")
    for x, y, label in test_pts:
        ok = any(pit(x, y, t) for t in triangles)
        print(f"    {label:12s} ({x:+.2f},{y:+.2f})  walkable={ok}")
