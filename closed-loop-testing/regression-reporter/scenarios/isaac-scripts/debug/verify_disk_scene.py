# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Run in Script Editor — opens the saved scene file from disk
(without loading it as live stage) and reports nav:volume:type per
NavMeshVolume. Lets us confirm if Save-As actually persisted the
Include changes."""
from pxr import Sdf

PATHS = [
    "/isaac-sim/sil/scenes/indicator_warehouse_20x20_odom_srr_nav_clear.usd",
    "/isaac-sim/sil/scenes/indicator_warehouse_20x20_layout_overflow_test_odom_srr.usd",
]

VOLUMES = [
    "/World/Navmesh/NavMeshVolume",
    "/World/Navmesh/NavMeshVolume_exclude",
    "/World/Navmesh/NavMeshVolume_exclude_01",
    "/World/Navmesh/NavMeshVolume_exclude_02",
    "/World/Navmesh/NavMeshVolume_exclude_03",
    "/World/Navmesh/NavMeshVolume_exclude_04",
]

for path in PATHS:
    layer = Sdf.Layer.FindOrOpen(path)
    if not layer:
        print(f"\n!!! cannot open {path}")
        continue
    print(f"\n=== {path} ===")
    for v in VOLUMES:
        prim_spec = layer.GetPrimAtPath(v)
        if not prim_spec:
            print(f"  {v}  (no spec — inherited from sublayer)")
            continue
        active = prim_spec.GetInfo("active") if prim_spec.HasInfo("active") else "(default True)"
        attr = prim_spec.attributes.get("nav:volume:type")
        attr_val = attr.default if attr else "(no spec)"
        print(f"  {v}")
        print(f"    active spec    : {active}")
        print(f"    type spec      : {attr_val!r}")
