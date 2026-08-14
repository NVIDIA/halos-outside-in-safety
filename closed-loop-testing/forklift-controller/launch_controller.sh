#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Launch robot controller with common options
# Usage: ./launch_controller.sh [robot_id] [path_file] [--no-namespace]
#
# Namespaced topics by default: /{robot_id}/odom, /{robot_id}/cmd_vel.
# CONTRACT: robot_id MUST match a robots.yaml `name` (forklift_b | forklift_b2)
# — the Isaac graphs only subscribe /<robot_id>/cmd_vel and publish
# /<robot_id>/odom. Example (second forklift):
#   ./launch_controller.sh forklift_b2 waypoints/warehouse_40x20/forklift_b2.json
# Pass --no-namespace for the legacy global-topic mode (/odom, /cmd_vel).
#
# Waypoints live under waypoints/<map id>/ because their coordinates are metres in
# one warehouse's world frame; FORKLIFT_WAYPOINTS_DIR picks the set, the same
# variable the compose services use, and defaults to the 20x20 scenes to match the
# default configs/robots.yaml.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_ID="${1:-forklift_b}"
WAYPOINTS_DIR="${FORKLIFT_WAYPOINTS_DIR:-$SCRIPT_DIR/waypoints/warehouse_20x20}"
PATH_FILE="${2:-$WAYPOINTS_DIR/${ROBOT_ID}.json}"
NAMESPACE_ARG="${3:-}"

# Namespaced by default; --no-namespace switches to legacy global topics
NAMESPACE_FLAG=""
if [ "$NAMESPACE_ARG" == "--no-namespace" ]; then
    NAMESPACE_FLAG="--no-namespace"
    echo "Using global topics: /odom, /cmd_vel (legacy mode — current robots.yaml graphs won't listen)"
else
    echo "Using namespaced topics: /${ROBOT_ID}/odom, /${ROBOT_ID}/cmd_vel"
fi

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
# python3 robot_controller.py \
#     --robot-id "$ROBOT_ID" \
#     --path "$PATH_FILE" \
#     --speed 0.7 \
#     $NAMESPACE_FLAG

exec python3 robot_controller.py \
    --robot-id "$ROBOT_ID" \
    --path "$PATH_FILE" \
    --speed 1 \
    --end-tolerance 1.0 \
    --end-pose-count 5 \
    --spiral-timeout 10.0 \
    --loop \
    $NAMESPACE_FLAG