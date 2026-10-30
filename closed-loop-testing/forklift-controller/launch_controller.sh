#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Launch robot controller with common options
# Usage: ./launch_controller.sh [robot_id] [path_file]
#
# CONTRACT: robot_id MUST be a block name in the fleet file, which ROBOTS_CONFIG
# picks and which defaults to robots.yaml — forklift_b alone, so a second truck
# names its file too. An undeclared robot_id is refused rather than defaulted.
#
# Example (second forklift, 40x20):
#   ROBOTS_CONFIG=robots-40x20.yaml WAYPOINTS_MAP=warehouse_40x20 \
#     ./launch_controller.sh forklift_b2
#
# Waypoints live under waypoints/<map id>/ because their coordinates are metres in
# one warehouse's world frame; WAYPOINTS_MAP picks the warehouse, the same
# variable the compose services use, and defaults to the 20x20 scenes to match the
# default fleet file. CONFIGS_DIR overrides where both are looked up.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_ID="${1:-forklift_b}"
# Relative override resolves against this script, not the caller's cwd, so the
# default and an override behave the same wherever it is run from.
WAYPOINTS_DIR="./waypoints/${WAYPOINTS_MAP:-warehouse_20x20}"
case "$WAYPOINTS_DIR" in /*) ;; *) WAYPOINTS_DIR="$SCRIPT_DIR/${WAYPOINTS_DIR#./}" ;; esac
PATH_FILE="${2:-$WAYPOINTS_DIR/${ROBOT_ID}.json}"


echo "═══════════════════════════════════════════════════"
echo "Starting Robot Controller"
echo "  Robot ID: $ROBOT_ID"
echo "  Path: $PATH_FILE"
echo "═══════════════════════════════════════════════════"
echo ""
echo "Commands:"
echo "  python test/send_command.py proceed   # Start moving"
echo "  python test/send_command.py stop      # Stop"
echo "  python test/send_command.py slow      # Slow down (30%)"
echo ""
echo "Or use ros2 CLI:"
echo "  ros2 topic pub /safety/command std_msgs/msg/String 'data: \"{\\\"command\\\": \\\"proceed\\\"}\"' --once"
echo "═══════════════════════════════════════════════════"
echo ""

cd "$SCRIPT_DIR"

# No drive knob is passed as a flag: a flag outranks the fleet file, so one
# hardcoded here would drive the truck unlike the deployment this mimics.
CONFIGS_DIR="${CONFIGS_DIR:-$SCRIPT_DIR/../isaac-sim/sil/configs}"
ROBOTS_CONFIG="${ROBOTS_CONFIG:-robots.yaml}"
case "$ROBOTS_CONFIG" in /*) ;; *) ROBOTS_CONFIG="$CONFIGS_DIR/$ROBOTS_CONFIG" ;; esac
# fleet_config imports the shared loader from here (a mount in the container).
export ISAAC_SCRIPTS_DIR="${ISAAC_SCRIPTS_DIR:-$SCRIPT_DIR/../isaac-sim/sil/scripts}"

exec python3 robot_controller.py \
    --robot-id "$ROBOT_ID" \
    --path "$PATH_FILE" \
    --configs-dir "$CONFIGS_DIR" \
    --robots-config "$ROBOTS_CONFIG"
