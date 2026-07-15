# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Check internal NavMesh data via get_navmesh() — might exist even if no
viz prim got created.
"""
import time
import omni.anim.navigation.core as nav

iface = nav.acquire_interface()

print(f"is_baking: {iface.is_navmesh_baking()}")

# Force fresh bake to be sure
print("Forcing sync bake...")
iface.clear_cache_dir()
try: iface.cancel_navmesh_baking()
except Exception: pass
t0 = time.time()
result = iface.start_navmesh_baking_and_wait()
print(f"  bake done in {time.time()-t0:.1f}s, returned: {result!r}")
time.sleep(1.0)

# Try get_navmesh
print()
print("Calling get_navmesh()...")
try:
    nm = iface.get_navmesh()
    print(f"  type: {type(nm)}")
    print(f"  attrs: {[a for a in dir(nm) if not a.startswith('_')]}")
    # try common methods
    for attr in ('get_tri_count','get_triangle_count','get_vertex_count',
                 'is_valid','get_polygons','get_vertices','triangle_count',
                 'vertex_count'):
        if hasattr(nm, attr):
            v = getattr(nm, attr)
            try:
                v = v() if callable(v) else v
                print(f"    {attr} = {v}")
            except Exception as e:
                print(f"    {attr}: {e}")
except Exception as e:
    print(f"  get_navmesh() raised: {e}")

# Inspect viz mesh again after small delay
import omni.usd
from pxr import UsdGeom
stage = omni.usd.get_context().get_stage()
print()
print("Re-checking viz prims after extra 3s wait:")
time.sleep(3.0)
n = 0
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if "/__omni_nav_mesh_viz_" in p:
        n += 1
        if prim.GetTypeName() == "Mesh":
            counts = UsdGeom.Mesh(prim).GetFaceVertexCountsAttr().Get() or []
            tri = sum(c-2 for c in counts)
            print(f"  {p}  tri={tri}")
        else:
            print(f"  {p}  type={prim.GetTypeName()}")
print(f"  total: {n} viz prims")

# Also check NavMesh extension is loaded
print()
import omni.kit.app
app = omni.kit.app.get_app()
exts = app.get_extension_manager().get_extensions()
for ext in exts:
    name = ext.get('name','')
    if 'navigation' in name.lower():
        print(f"  ext: {name}  enabled={ext.get('enabled')}")
