# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Where the ROI and the tripwire sit in a VSS calibration.json.

A 2D calibration lists them per sensor. A 3D (BEV) one lists them at the top
level, and VSS 3.3's warehouse-3d-app datasets leave the sensors' own lists
empty. The SRR scene has one of each, shared by every camera.
"""


def first_entry(calib: dict, key: str) -> dict:
    """The first ``key`` ("rois" or "tripwires") entry: per sensor first, then top level."""
    for sensor in calib.get("sensors", []):
        if sensor.get(key):
            return sensor[key][0]
    if calib.get(key):
        return calib[key][0]
    raise KeyError(f"calibration has no {key}")
