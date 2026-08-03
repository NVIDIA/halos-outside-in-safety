# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Diagnose NavMesh extension API + try multiple bake methods to find one
that ACTUALLY re-bakes (the viz mesh's tri count must change, since the
exclude volumes are deactivated).

Run after deactivating exclude volumes. Tries each method, checks tri count
after each.
"""
import time
from pxr import UsdGeom
import omni.usd

stage = omni.usd.get_context().get_stage()


def viz_tri_count():
    for prim in stage.Traverse():
        p = str(prim.GetPath())
        if p.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
            counts = UsdGeom.Mesh(prim).GetFaceVertexCountsAttr().Get()
            return sum(c-2 for c in counts) if counts else 0
    return None


print("=" * 70)
print("Initial state")
print("=" * 70)
print(f"  viz tri_count: {viz_tri_count()}")

# 1. Inspect nav core interface
print()
print("=" * 70)
print("Available methods on omni.anim.navigation.core interface")
print("=" * 70)
try:
    import omni.anim.navigation.core as nav
    iface = nav.acquire_interface()
    print(f"  type: {type(iface).__name__}")
    methods = [a for a in dir(iface) if not a.startswith("_")]
    for m in methods:
        print(f"    {m}")
except Exception as e:
    print(f"  ERROR: {e}")

# 2. Try kit commands related to navmesh
print()
print("=" * 70)
print("Kit commands matching navmesh/nav")
print("=" * 70)
try:
    import omni.kit.commands
    all_cmds = omni.kit.commands.get_commands()
    matching = [c for c in all_cmds if "nav" in c.lower() or "bake" in c.lower()]
    for c in sorted(matching):
        print(f"    {c}")
except Exception as e:
    print(f"  ERROR: {e}")

# 3. Try each bake method with longer wait, log tri count change
print()
print("=" * 70)
print("Trying bake methods (10s wait each, watching tri count)")
print("=" * 70)


def try_method(label, fn):
    t0 = viz_tri_count()
    print(f"\n  {label} — initial tri={t0}")
    try:
        fn()
        for s in range(10):
            time.sleep(1.0)
            t = viz_tri_count()
            if t != t0:
                print(f"    t={s+1}s  tri_count CHANGED: {t0} → {t}  ← THIS METHOD WORKS")
                return True
        print(f"    after 10s, tri still {t0} (no change)")
    except Exception as e:
        print(f"    EXCEPTION: {e}")
    return False


try:
    import omni.anim.navigation.core as nav
    iface = nav.acquire_interface()
    if hasattr(iface, "start_navmesh_baking"):
        try_method("nav.start_navmesh_baking()", iface.start_navmesh_baking)
    if hasattr(iface, "bake_navmesh"):
        try_method("nav.bake_navmesh()", iface.bake_navmesh)
    if hasattr(iface, "rebuild_navmesh"):
        try_method("nav.rebuild_navmesh()", iface.rebuild_navmesh)
    if hasattr(iface, "request_bake"):
        try_method("nav.request_bake()", iface.request_bake)
except Exception as e:
    print(f"  nav iface error: {e}")

try:
    import omni.kit.commands
    for cmd in ("BakeNavMesh", "RebakeNavMesh", "NavMeshBake"):
        try_method(f'commands.execute("{cmd}")',
                   lambda c=cmd: omni.kit.commands.execute(c))
except Exception as e:
    print(f"  kit commands error: {e}")

print()
print("=" * 70)
print("Final tri count:", viz_tri_count())
print("If no method worked → click Bake button in NavMesh tab manually,")
print("watch the cyan mesh visibly redraw, then run wait_and_reexport.py")
print("=" * 70)
