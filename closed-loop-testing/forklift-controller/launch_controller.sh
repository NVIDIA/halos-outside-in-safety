#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Launch robot controller with common options
# Usage: ./launch_controller.sh [robot_id] [path_file]
#
# CONTRACT: robot_id MUST be a block name in the fleet file this reads — the
# Isaac graphs only subscribe /<robot_id>/cmd_vel and publish /<robot_id>/odom,
# and speed, heading and loop all come from that block. ROBOTS_CONFIG picks the
# file and defaults to configs/robots.yaml, which declares forklift_b alone, so
# a second truck names its file too. A robot_id the file does not declare is
# refused by name rather than driven with defaults.
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

# No drive knob is passed as a flag. A flag outranks the fleet file, so a speed
# hardcoded here would quietly override robots.yaml and this script would drive
# the truck differently from the deployment it exists to mimic — which is what it
# used to do: --speed 1 against a fleet file that says 1.5. It reads the same
# fleet file Isaac reads instead.
CONFIGS_DIR="${CONFIGS_DIR:-$SCRIPT_DIR/../isaac-sim/sil/configs}"
ROBOTS_CONFIG="${ROBOTS_CONFIG:-robots.yaml}"
case "$ROBOTS_CONFIG" in /*) ;; *) ROBOTS_CONFIG="$CONFIGS_DIR/$ROBOTS_CONFIG" ;; esac
# Where fleet_config imports the shared loader from: a mount in the container,
# the checkout here.
export ISAAC_SCRIPTS_DIR="${ISAAC_SCRIPTS_DIR:-$SCRIPT_DIR/../isaac-sim/sil/scripts}"

exec python3 robot_controller.py \
    --robot-id "$ROBOT_ID" \
    --path "$PATH_FILE" \
    --configs-dir "$CONFIGS_DIR" \
    --robots-config "$ROBOTS_CONFIG"
