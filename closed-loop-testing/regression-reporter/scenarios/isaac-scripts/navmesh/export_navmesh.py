# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Export the live baked NavMesh viz mesh to JSON.

Run in Script Editor after Bake. The NavMesh extension creates a Mesh prim
under /__omni_nav_mesh_viz_*/_a0/mesh — that mesh IS the walkable surface
(every triangle = a walkable cell).

Output: /isaac-sim/sil/configs/navmesh.json with schema
{
  "triangles_xy": [ [[x0,y0],[x1,y1],[x2,y2]], ... ],
  "bbox": [xmin, ymin, xmax, ymax],
  "tri_count": N
}

Use it from host with a tiny point-in-polygon checker to validate waypoints.
"""
import json
from pxr import Usd, UsdGeom, Gf
import omni.usd

OUT_PATH = "/isaac-sim/sil/configs/navmesh.json"

stage = omni.usd.get_context().get_stage()

# Find the NavMesh viz mesh
viz_mesh = None
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if p.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
        viz_mesh = prim
        break

if viz_mesh is None:
    print("NavMesh viz mesh not found. Make sure NavMesh has been baked and is visible.")
    raise SystemExit

print(f"Found viz mesh: {viz_mesh.GetPath()}")

mesh = UsdGeom.Mesh(viz_mesh)
points = mesh.GetPointsAttr().Get()         # Vec3f[] in WORLD? Usually local
counts = mesh.GetFaceVertexCountsAttr().Get()
indices = mesh.GetFaceVertexIndicesAttr().Get()

# World transform
xformable = UsdGeom.Xformable(viz_mesh)
world = xformable.ComputeLocalToWorldTransform(Usd.TimeCode.Default())

# Triangulate & project to XY
triangles = []
xs, ys = [], []
i = 0
for c in counts:
    # NavMesh viz mesh is typically all triangles, but handle quads/n-gons by fan
    verts = indices[i:i+c]
    i += c
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

doc = {"triangles_xy": triangles,
       "bbox": bbox,
       "tri_count": len(triangles)}
with open(OUT_PATH, "w") as f:
    json.dump(doc, f)

print(f"Wrote {len(triangles)} triangles to {OUT_PATH}")
print(f"  bbox xy: [{bbox[0]:.2f}, {bbox[1]:.2f}] .. [{bbox[2]:.2f}, {bbox[3]:.2f}]")
print(f"  file size: ~{len(json.dumps(doc))//1024} KB")
