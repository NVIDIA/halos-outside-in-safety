# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Deactivate the leftover Loading_Zone_Objects group blocking ROI south
NavMesh, then trigger NavMesh re-bake.

Paste into Script Editor in the running scene, then File → Save the scene
to persist the deactivation.
"""
from pxr import Usd, UsdPhysics, Gf, UsdGeom
import omni.usd

stage = omni.usd.get_context().get_stage()

# Targets: prims that the blocker scan found inside ROI south.
# Deactivating the parent group is simpler than removing them one by one.
TARGETS = [
    "/World/Loading_Zone_Objects",  # parent group (sibling of the already-
                                    # deactivated Loading_Zone_Objects_01)
]

# If you want to keep some children of /World/Loading_Zone_Objects active
# and only kill the 3 blocking ones, comment the line above and use:
# TARGETS = [
#     "/World/Loading_Zone_Objects/SM_CardBoxA_01_614",
#     "/World/Loading_Zone_Objects/SM_CardBoxA_01_846",
#     "/World/Loading_Zone_Objects/SM_PaletteA_02",
# ]

print("=" * 70)
print("Deactivating blocker prims")
print("=" * 70)
deactivated = []
for path in TARGETS:
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsValid():
        print(f"  SKIP (not found): {path}")
        continue
    if not prim.IsActive():
        print(f"  already inactive: {path}")
        continue
    prim.SetActive(False)
    deactivated.append(path)
    print(f"  deactivated: {path}")

print()
print(f"Deactivated {len(deactivated)} prim(s).")

# Trigger NavMesh re-bake. The extension's API name varies; try a few.
print()
print("=" * 70)
print("Triggering NavMesh re-bake")
print("=" * 70)
baked = False
try:
    import omni.anim.navigation.core as nav
    nav_iface = nav.acquire_interface()
    if hasattr(nav_iface, "start_navmesh_baking"):
        nav_iface.start_navmesh_baking()
        baked = True
        print("  called nav.start_navmesh_baking()")
    elif hasattr(nav_iface, "bake_navmesh"):
        nav_iface.bake_navmesh()
        baked = True
        print("  called nav.bake_navmesh()")
    else:
        print("  nav interface has no obvious bake method; available attrs:")
        for a in dir(nav_iface):
            if not a.startswith("_"):
                print(f"    {a}")
except Exception as e:
    print(f"  could not auto-bake via omni.anim.navigation.core: {e}")

if not baked:
    try:
        import omni.kit.commands
        omni.kit.commands.execute("BakeNavMesh")
        print("  called kit command 'BakeNavMesh'")
        baked = True
    except Exception as e:
        print(f"  kit command BakeNavMesh failed: {e}")

if not baked:
    print()
    print("  Auto-bake did not work. Bake manually:")
    print("    Window → Navigation → NavMesh → Bake")

print()
print("=" * 70)
print("Done. Don't forget: File → Save (Ctrl+S) to persist deactivation.")
print("=" * 70)
