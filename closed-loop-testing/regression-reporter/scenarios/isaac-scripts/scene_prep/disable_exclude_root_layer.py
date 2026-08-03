# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Same as disable_exclude_volumes.py but writes nav:volume:type changes to
the ROOT LAYER explicitly. Previous attempt may have written to the
session layer which the bake's input hash didn't see → cached result was
re-used.

Also dumps WHICH layer holds the strongest opinion for nav:volume:type
on each exclude prim, so if some prim's authored value is in a referenced
sublayer we'll see that.
"""
import json, time
from pxr import Usd, UsdGeom, Sdf, Gf
import omni.usd

stage = omni.usd.get_context().get_stage()

EXCLUDE_PATHS = [
    "/World/Navmesh/NavMeshVolume_exclude",
    "/World/Navmesh/NavMeshVolume_exclude_01",
    "/World/Navmesh/NavMeshVolume_exclude_02",
    "/World/Navmesh/NavMeshVolume_exclude_03",
    "/World/Navmesh/NavMeshVolume_exclude_04",
]

print("=" * 78)
print("Diagnostic: which layer authors nav:volume:type for each exclude prim?")
print("=" * 78)
print(f"  current edit target layer: {stage.GetEditTarget().GetLayer().identifier}")
print(f"  root layer identifier    : {stage.GetRootLayer().identifier}")
print(f"  layer stack (strong→weak):")
for layer in stage.GetLayerStack():
    print(f"    - {layer.identifier}")

for p in EXCLUDE_PATHS:
    prim = stage.GetPrimAtPath(p)
    if not prim: continue
    attr = prim.GetAttribute("nav:volume:type")
    if not attr: continue
    spec_layers = []
    for layer in stage.GetLayerStack():
        spec = layer.GetAttributeAtPath(attr.GetPath())
        if spec is not None and spec.HasInfo("default"):
            spec_layers.append((layer.identifier, spec.default))
    print(f"  {p}")
    print(f"    composed value      : {attr.Get()!r}")
    print(f"    layer specs (strong first):")
    for ident, val in spec_layers:
        print(f"      {val!r}  on  {ident}")

# Force edit target = root layer
print()
print("=" * 78)
print("Switching edit target to root layer; setting nav:volume:type=Include")
print("=" * 78)
root = stage.GetRootLayer()
old_target = stage.GetEditTarget()
stage.SetEditTarget(root)
try:
    for p in EXCLUDE_PATHS:
        prim = stage.GetPrimAtPath(p)
        if not prim: continue
        if not prim.IsActive():
            prim.SetActive(True)
        attr = prim.GetAttribute("nav:volume:type")
        attr.Set("Include")
        print(f"  {p}: set Include on root layer  (composed now: {attr.Get()!r})")
finally:
    stage.SetEditTarget(old_target)

# Bake
print()
print("=" * 78)
print("Clear cache + sync bake")
print("=" * 78)
import omni.anim.navigation.core as nav
iface = nav.acquire_interface()
iface.clear_cache_dir()
try: iface.cancel_navmesh_baking()
except Exception: pass
t0 = time.time()
iface.start_navmesh_baking_and_wait()
print(f"  bake done in {time.time()-t0:.1f}s")

# Re-export
print()
print("=" * 78)
print("Re-export + spot-check")
print("=" * 78)
viz = None
for prim in stage.Traverse():
    pp = str(prim.GetPath())
    if pp.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
        viz = prim; break

if viz is None:
    print("  no viz mesh!")
else:
    mesh = UsdGeom.Mesh(viz)
    points = mesh.GetPointsAttr().Get()
    counts = mesh.GetFaceVertexCountsAttr().Get()
    indices = mesh.GetFaceVertexIndicesAttr().Get()
    world = UsdGeom.Xformable(viz).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
    triangles = []; xs=[]; ys=[]
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
    print(f"  wrote {len(triangles)} triangles  bbox=[{bbox[0]:.2f},{bbox[1]:.2f}]..[{bbox[2]:.2f},{bbox[3]:.2f}]")

    def pit(x,y,t):
        (x1,y1),(x2,y2),(x3,y3)=t
        d=(y2-y3)*(x1-x3)+(x3-x2)*(y1-y3)
        if d==0: return False
        a=((y2-y3)*(x-x3)+(x3-x2)*(y-y3))/d
        b=((y3-y1)*(x-x3)+(x1-x3)*(y-y3))/d
        return a>=-1e-6 and b>=-1e-6 and (1-a-b)>=-1e-6

    test_pts = [
        ("inside exclude_03",  5.72, -15.17),
        ("inside exclude_04",  7.97, -17.27),
        ("inside exclude_01",  6.07,  -9.30),
        ("inside exclude_E", 12.00, -15.68),
    ]
    print()
    for label,x,y in test_pts:
        ok = any(pit(x,y,t) for t in triangles)
        flag = "✓" if ok else "✗"
        print(f"  {flag} {label:25s} ({x:+.2f},{y:+.2f}) walkable={ok}")
