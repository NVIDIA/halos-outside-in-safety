#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Usage: ./docker_run.sh [waypoints_file]
# Example: ./docker_run.sh waypoints/warehouse_20x20/forklift_b.json
# Waypoints are per scene (waypoints/<map id>/), because their coordinates are
# metres in one warehouse's world frame.

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="forklift-controller:latest"
NAME="forklift-controller"

# Build if image not exists or --build flag
if [ "$1" == "--build" ] || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "Building $IMAGE ..."
    docker build -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
    [ "$1" == "--build" ] && shift
fi

# Stop old container
docker rm -f "$NAME" 2>/dev/null || true

# Docker run args
DOCKER_ARGS=(
    docker run --rm --name "$NAME" --network host
    -e ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-0}"
    -e RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
    # No fleet file is mounted here, so the drive knobs come from
    # fleet_config.DRIVE_DEFAULTS — which hold the same values robots.yaml
    # states for forklift_b. Naming a truck other than the default therefore
    # gets the default's speed and heading; use compose for a real fleet.
    -e ROBOT_ID="${ROBOT_ID:-forklift_b}"
)

# Mount waypoints if provided
if [ -n "$1" ]; then
    WP="$(realpath "$1")"
    DOCKER_ARGS+=(-v "${WP}:/app/ext_waypoints/$(basename "$WP"):ro")
    DOCKER_ARGS+=(-e "WAYPOINT_FILE=/app/ext_waypoints/$(basename "$WP")")
fi

echo "Starting $NAME (ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-0}) ..."
echo "Note: topic names come from the robot's block in the robots config; without one, they default to /\${ROBOT_ID}/cmd_vel and /\${ROBOT_ID}/odom."
exec "${DOCKER_ARGS[@]}" "$IMAGE"
