# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Diagnose NavMesh state after scene load. If viz mesh is missing/empty,
trigger a fresh bake.
"""
import time
from pxr import UsdGeom
import omni.usd

stage = omni.usd.get_context().get_stage()

print("=" * 70)
print("Stage diagnostic")
print("=" * 70)
print(f"  root layer: {stage.GetRootLayer().identifier}")

# 1. Find all viz mesh prims
viz_prims = []
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if "/__omni_nav_mesh_viz_" in p and prim.GetTypeName() == "Mesh":
        mesh = UsdGeom.Mesh(prim)
        counts = mesh.GetFaceVertexCountsAttr().Get() or []
        tri = sum(c-2 for c in counts)
        viz_prims.append((p, tri, prim.IsActive()))
print(f"\n  viz mesh prims: {len(viz_prims)}")
for p, tri, act in viz_prims:
    print(f"    {p}  tri={tri}  IsActive={act}")

# 2. List NavMeshVolume prims + their type
print("\n  NavMeshVolume prims:")
for prim in stage.Traverse():
    if prim.GetTypeName() == "NavMeshVolume":
        attr = prim.GetAttribute("nav:volume:type")
        v = attr.Get() if attr else "(no attr)"
        print(f"    {prim.GetPath()}  IsActive={prim.IsActive()}  type={v!r}")

# 3. NavMesh extension state
import omni.anim.navigation.core as nav
iface = nav.acquire_interface()
print(f"\n  is_navmesh_baking: {iface.is_navmesh_baking()}")
print(f"  cache_dir: {iface.get_cache_dir()}")

# 4. Force a fresh bake
print()
print("=" * 70)
print("Forcing fresh bake (sync)...")
print("=" * 70)
iface.clear_cache_dir()
try: iface.cancel_navmesh_baking()
except Exception: pass
t0 = time.time()
iface.start_navmesh_baking_and_wait()
print(f"  bake done in {time.time()-t0:.1f}s")
time.sleep(1.5)

# 5. Re-check viz mesh
print()
print("Post-bake viz mesh state:")
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if "/__omni_nav_mesh_viz_" in p and prim.GetTypeName() == "Mesh":
        mesh = UsdGeom.Mesh(prim)
        counts = mesh.GetFaceVertexCountsAttr().Get() or []
        tri = sum(c-2 for c in counts)
        print(f"  {p}  tri={tri}")

print()
print("If tri>0 → NavMesh exists. Toggle visualization:")
print("  Window → Navigation → NavMesh → check 'Show NavMesh' or similar")
print("If tri==0 → bake produced empty mesh; check NavMeshVolume scope")
print("(the parent 'NavMeshVolume' prim must be active and type=Include)")
