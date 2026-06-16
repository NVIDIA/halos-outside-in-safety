#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
WAIT_FOR_RENDER PATCH - Performance Fix for Docker

Change: wait_for_render=True → False in step_async()
Result: Timeline is not blocked by rendering  
Expected: Smooth simulation, people walk fast
"""

import glob
import sys

# Find data_generation.py
files = glob.glob("/isaac-sim/**/data_generation/data_generation.py", recursive=True)
if not files:
    print("Warning: data_generation.py not found, skipping patch")
    sys.exit(0)

DATA_GEN = files[0]
print(f"Patching: {DATA_GEN}")

with open(DATA_GEN, 'r') as f:
    content = f.read()

# Check if already patched
if '# WAIT_FOR_RENDER_PATCH' in content:
    print("Already patched, skipping")
    sys.exit(0)

# Apply simple patch - don't wait for render
old = 'await rep.orchestrator.step_async(pause_timeline=False)'
new = 'await rep.orchestrator.step_async(pause_timeline=False, wait_for_render=False)  # WAIT_FOR_RENDER_PATCH'

if old not in content:
    print("Warning: Target code not found, may be different version")
    sys.exit(1)

content = content.replace(old, new)

with open(DATA_GEN, 'w') as f:
    f.write(content)

print("wait_for_render=False patch applied")
