#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Launch robot controller with common options
# Usage: ./launch_controller.sh [robot_id] [path_file] [--namespace]
#
# By default uses global topics (/odom, /cmd_vel) for single robot
# Add --namespace to use /{robot_id}/odom, /{robot_id}/cmd_vel for multi-robot

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOT_ID="${1:-forklift_1}"
PATH_FILE="${2:-$SCRIPT_DIR/waypoints/waypoints.json}"
USE_NAMESPACE="${3:-}"

# Check if --namespace flag is passed
NAMESPACE_FLAG="--no-namespace"
if [ "$USE_NAMESPACE" == "--namespace" ]; then
    NAMESPACE_FLAG=""
    echo "Using namespaced topics: /${ROBOT_ID}/odom, /${ROBOT_ID}/cmd_vel"
else
    echo "Using global topics: /odom, /cmd_vel"
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