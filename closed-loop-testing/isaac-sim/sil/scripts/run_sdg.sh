#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Actor SDG Runner Shell Script
#
# Wrapper script to run Actor SDG from Isaac Sim
#
# Usage:
#   ./run_sdg.sh -c /path/to/config.yaml [options]
#
# Options:
#   -c, --config     Path to IRA config file (required)
#   --start          Auto start data generation
#   --setup-only     Only setup, don't start generation
#   --headless       Run in headless mode
#   --help           Show help

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_ACTOR_SDG="$SCRIPT_DIR/run_actor_sdg.py"

# Default Isaac Sim path (can be overridden by ISAAC_SIM_PATH env var)
ISAAC_SIM_PATH="${ISAAC_SIM_PATH:-/isaac-sim}"

# Check if running inside container
if [ -d "/isaac-sim" ]; then
    ISAAC_SIM_PATH="/isaac-sim"
elif [ -d "$HOME/isaacsim-5.1.0-release" ]; then
    ISAAC_SIM_PATH="$HOME/isaacsim-5.1.0-release"
fi

PYTHON_SH="$ISAAC_SIM_PATH/python.sh"

if [ ! -f "$PYTHON_SH" ]; then
    echo "ERROR: Isaac Sim python.sh not found at: $PYTHON_SH"
    echo "Set ISAAC_SIM_PATH environment variable to Isaac Sim installation directory"
    exit 1
fi

echo "Using Isaac Sim at: $ISAAC_SIM_PATH"
echo "ROS2 Distro: $ROS_DISTRO | RMW: $RMW_IMPLEMENTATION"
echo "Running Actor SDG script: $RUN_ACTOR_SDG"
echo ""

# Run the script
exec "$PYTHON_SH" "$RUN_ACTOR_SDG" "$@"

