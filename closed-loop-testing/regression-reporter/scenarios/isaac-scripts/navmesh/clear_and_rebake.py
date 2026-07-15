# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Deactivate physical obstacle prims AND NavMeshVolume_exclude_* prims, then
re-bake NavMesh + re-export the navmesh.json. Verifies each step.

Paste into Script Editor with the warehouse scene loaded. Run.

Steps:
  1. Deactivate physical obstacle Xforms (SM_*Truck, SM_*Forklift_*, Loading_Zone_*)
  2. Deactivate /World/Navmesh/NavMeshVolume_exclude_* (the things actually
     carving holes in NavMesh — set toggle below to skip if you want to keep)
  3. Trigger bake
  4. Re-export new NavMesh viz mesh as JSON for client-side validation
  5. Print before/after triangle counts

After save: File → Save (Ctrl+S) to persist deactivations to scene .usd.
"""
from pxr import Usd, UsdGeom, Gf
import omni.usd, time, json

stage = omni.usd.get_context().get_stage()

# Toggle: clear NavMeshVolume_exclude_* too (the actual NavMesh holes).
# Without this, NavMesh holes remain even if obstacles are removed.
CLEAR_NAVMESH_EXCLUDE_VOLUMES = True

OBSTACLE_PATHS = [
    "/World/SM_HeavyDutyPalletTruck_A01_01",
    "/World/SM_Forklift_A01_Blue_01",
    "/World/Loading_Zone_Objects_01",
    "/World/Loading_Zone_Objects",
]
EXCLUDE_VOLUME_PATHS = [
    "/World/Navmesh/NavMeshVolume_exclude",
    "/World/Navmesh/NavMeshVolume_exclude_01",
    "/World/Navmesh/NavMeshVolume_exclude_02",
    "/World/Navmesh/NavMeshVolume_exclude_03",
    "/World/Navmesh/NavMeshVolume_exclude_04",
]
TARGETS = OBSTACLE_PATHS + (EXCLUDE_VOLUME_PATHS if CLEAR_NAVMESH_EXCLUDE_VOLUMES else [])

print("=" * 78)
print("Step 1: Deactivate target prims")
print("=" * 78)
deactivated = []
for path in TARGETS:
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsValid():
        print(f"  SKIP not found: {path}")
        continue
    if not prim.IsActive():
        print(f"  already inactive: {path}")
        continue
    prim.SetActive(False)
    deactivated.append(path)
    # verify
    again = stage.GetPrimAtPath(path)
    print(f"  deactivated: {path}  IsActive={again.IsActive()}")
print(f"\nDeactivated {len(deactivated)} prim(s) this run.")

# Quick sanity: count active descendants per target
print()
print("Sanity check — active descendants under each target:")
for path in TARGETS:
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsValid(): continue
    n_active = sum(1 for c in Usd.PrimRange(prim) if c.IsActive())
    print(f"  {path}: {n_active} active prim(s) in subtree")

# Step 2: Re-bake NavMesh
print()
print("=" * 78)
print("Step 2: Trigger NavMesh re-bake")
print("=" * 78)
baked = False
try:
    import omni.anim.navigation.core as nav
    iface = nav.acquire_interface()
    for method in ("start_navmesh_baking", "bake_navmesh", "bake"):
        if hasattr(iface, method):
            getattr(iface, method)()
            baked = True
            print(f"  called nav.{method}()")
            break
    if not baked:
        print("  available iface methods:")
        for a in dir(iface):
            if not a.startswith("_"):
                print(f"    {a}")
except Exception as e:
    print(f"  nav core API failed: {e}")

if not baked:
    try:
        import omni.kit.commands
        omni.kit.commands.execute("BakeNavMesh")
        baked = True
        print("  called kit command BakeNavMesh")
    except Exception as e:
        print(f"  BakeNavMesh command failed: {e}")

if not baked:
    print()
    print("  AUTO-BAKE FAILED. Bake manually:")
    print("    Window → Navigation → NavMesh → Bake")
    print("  Then re-run this script (it will skip the deactivation if")
    print("  prims are already inactive and re-export the navmesh.json).")

# Wait briefly for bake to settle
time.sleep(2.0)

# Step 3: Re-export NavMesh to JSON
print()
print("=" * 78)
print("Step 3: Re-export NavMesh viz mesh → /isaac-sim/sil/configs/navmesh.json")
print("=" * 78)
viz = None
for prim in stage.Traverse():
    p = str(prim.GetPath())
    if p.startswith("/__omni_nav_mesh_viz_") and prim.GetTypeName() == "Mesh":
        viz = prim
        break

if viz is None:
    print("  NavMesh viz mesh not found (bake may have failed).")
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
    out_path = "/isaac-sim/sil/configs/navmesh.json"
    with open(out_path, "w") as f:
        json.dump({"triangles_xy": triangles, "bbox": bbox,
                   "tri_count": len(triangles)}, f)
    print(f"  wrote {len(triangles)} triangles to {out_path}")
    print(f"  bbox xy: [{bbox[0]:.2f}, {bbox[1]:.2f}] .. [{bbox[2]:.2f}, {bbox[3]:.2f}]")

# Step 4: Verify previously-blocked points are now walkable
print()
print("=" * 78)
print("Step 4: Spot-check that previously-blocked points are now walkable")
print("=" * 78)
# These were the 3 stuck-spots from the 10-min monitor
test_pts = [
    ("char_0 stuck spot", 5.20, -11.24),
    ("char_1 stuck spot", 5.31, -17.82),
    ("char_2 stuck spot", 4.72, -16.89),
    ("inside exclude_03 (cardboard)", 5.72, -15.17),
    ("inside exclude_04 (south)", 7.97, -17.27),
    ("inside exclude_01 (NE)", 6.07, -9.30),
]
if viz is not None:
    # quick point-in-tri test against re-exported mesh
    def pit(x, y, t):
        (x1,y1),(x2,y2),(x3,y3)=t
        d = (y2-y3)*(x1-x3) + (x3-x2)*(y1-y3)
        if d == 0: return False
        a = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / d
        b = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / d
        return a >= -1e-6 and b >= -1e-6 and (1-a-b) >= -1e-6
    for label, x, y in test_pts:
        ok = any(pit(x, y, t) for t in triangles)
        print(f"  {label:35s} ({x:+.2f},{y:+.2f}) walkable={ok}")

print()
print("=" * 78)
print("Done.")
print("If walkable=True for all stuck spots → restart scenario, chars should")
print("no longer get cornered there. File → Save (Ctrl+S) to persist scene.")
print("=" * 78)
