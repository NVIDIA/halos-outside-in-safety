# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Inspect schema + attributes of NavMeshVolume_exclude_* prims to find the
right way to disable them. IsActive=False is being ignored by the NavMesh
extension — there must be a custom attribute.
"""
from pxr import Usd, UsdGeom
import omni.usd

stage = omni.usd.get_context().get_stage()

paths = [
    "/World/Navmesh/NavMeshVolume",
    "/World/Navmesh/NavMeshVolume_exclude",
    "/World/Navmesh/NavMeshVolume_exclude_01",
    "/World/Navmesh/NavMeshVolume_exclude_02",
    "/World/Navmesh/NavMeshVolume_exclude_03",
    "/World/Navmesh/NavMeshVolume_exclude_04",
]

for path in paths:
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsValid():
        print(f"\n--- {path}: NOT FOUND ---")
        continue
    print(f"\n--- {path} ---")
    print(f"  type            : {prim.GetTypeName()}")
    print(f"  IsActive        : {prim.IsActive()}")
    print(f"  HasAuthoredAct  : {prim.HasAuthoredActive()}")
    print(f"  applied schemas : {list(prim.GetAppliedSchemas())}")
    print(f"  attributes:")
    for attr in prim.GetAttributes():
        n = attr.GetName()
        v = attr.Get()
        # filter to non-trivial / nav-related
        if any(k in n.lower() for k in ("volume","exclude","enable","include","active","navmesh","navigation","mode","type")):
            print(f"    {n} = {v!r}  (type {attr.GetTypeName()})")
    # also list relationships
    rels = list(prim.GetRelationships())
    if rels:
        print(f"  relationships:")
        for r in rels:
            print(f"    {r.GetName()} -> {r.GetTargets()}")
