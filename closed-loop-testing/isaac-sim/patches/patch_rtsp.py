#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Patch RTSPWriter for FFmpeg 6.x compatibility and better video quality.
- Change preset from deprecated "ll" to "p4" + tune "ll"
- Add GOP size 30 (keyframe every 1 second at 30fps)
"""
import glob
import sys

# Find rtsp.py
files = glob.glob("/isaac-sim/**/writers/rtsp.py", recursive=True)
if not files:
    print("Warning: rtsp.py not found, skipping patch")
    sys.exit(0)

rtsp_file = files[0]
print(f"Patching: {rtsp_file}")

with open(rtsp_file, "r") as f:
    content = f.read()

# 1. Fix preset for FFmpeg 6.x
content = content.replace(
    '            "-preset",\n            "ll",',
    '            "-preset",\n            "p4",\n            "-tune",\n            "ll",'
)

# 2. Add GOP size
content = content.replace(
    '            "-maxrate:v",',
    '            "-g",\n            "30",\n            "-maxrate:v",'
)

with open(rtsp_file, "w") as f:
    f.write(content)

print("RTSPWriter patched: preset=p4, tune=ll, gop=30")

