#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Patch default config path to point to /isaac-sim/sil/configs/default_config_ros.yaml
Instead of extscache/config/default_config.yaml
"""

import glob
import sys

# Find default.py in agent.core extension
files = glob.glob("/isaac-sim/**/config_file/default.py", recursive=True)
if not files:
    print("Warning: default.py not found, skipping patch")
    sys.exit(0)

default_py = files[0]
print(f"Patching: {default_py}")

with open(default_py, 'r') as f:
    content = f.read()

# Check if already patched
if '# CUSTOM_CONFIG_PATH_PATCH' in content:
    print("Already patched, skipping")
    sys.exit(0)

# Replace get_default_config_file_path() method
old_method = '''    @classmethod
    def get_default_config_file_path(cls):
        ext_path = Infos.ext_path
        return f"{ext_path}/{cls.DEFAULT_CONFIG_FILE_RELATIVE_PATH}"'''

new_method = '''    @classmethod
    def get_default_config_file_path(cls):
        # CUSTOM_CONFIG_PATH_PATCH: Use custom config in /isaac-sim/sil/configs
        import os
        custom_path = "/isaac-sim/sil/configs/default_config_ros.yaml"
        if os.path.exists(custom_path):
            return custom_path
        # Fallback to original if custom not found
        ext_path = Infos.ext_path
        return f"{ext_path}/{cls.DEFAULT_CONFIG_FILE_RELATIVE_PATH}"'''

if old_method not in content:
    print("Warning: Target method not found, may be different version")
    sys.exit(1)

content = content.replace(old_method, new_method)

with open(default_py, 'w') as f:
    f.write(content)

print("✓ Default config path patched")
print("  Default: /isaac-sim/sil/configs/default_config_ros.yaml")
print("  Fallback: extscache/config/default_config.yaml")

