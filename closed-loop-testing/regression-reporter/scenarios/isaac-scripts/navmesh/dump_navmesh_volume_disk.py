# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Dump nav:volume:type from a USD on disk (no live stage). Run via
isaac python: docker exec isaac-sim /isaac-sim/python.sh path/to/this.py
"""
from pxr import Usd
import sys

DEFAULT = "/isaac-sim/sil/scenes/indicator_warehouse_20x20_odom_srr_nav_clear.usd"
path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
print(f"Opening {path}")
stage = Usd.Stage.Open(path)
if not stage:
    print("  FAILED to open"); sys.exit(1)

for prim in stage.Traverse():
    if prim.GetTypeName() == "NavMeshVolume":
        attr = prim.GetAttribute("nav:volume:type")
        v = attr.Get() if attr else "(no attr)"
        print(f"  {prim.GetPath()}  IsActive={prim.IsActive()}  type={v!r}")
