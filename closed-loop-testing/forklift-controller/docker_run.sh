#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Usage: ./docker_run.sh [waypoints_file]
# Example: ./docker_run.sh waypoints/waypoints.json

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
    -e ROBOT_ID="${ROBOT_ID:-forklift_1}"
    -e BASE_SPEED="${BASE_SPEED:-1.5}"
    -e ANGULAR_SPEED="${ANGULAR_SPEED:-0.4}"
    -e HEADING_OFFSET="${HEADING_OFFSET:-0}"
    -e USE_NAMESPACE="${USE_NAMESPACE:-false}"
    -e LOOP_PATH="${LOOP_PATH:-false}"
    -e NO_INVERT="${NO_INVERT:-true}"
    -e END_TOLERANCE="${END_TOLERANCE:-1.0}"
    -e END_POSE_COUNT="${END_POSE_COUNT:-5}"
    -e SPIRAL_TIMEOUT="${SPIRAL_TIMEOUT:-10.0}"
)

# Mount waypoints if provided
if [ -n "$1" ]; then
    WP="$(realpath "$1")"
    DOCKER_ARGS+=(-v "${WP}:/app/ext_waypoints/$(basename "$WP"):ro")
    DOCKER_ARGS+=(-e "WAYPOINT_FILE=/app/ext_waypoints/$(basename "$WP")")
fi

echo "Starting $NAME (ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-0}) ..."
exec "${DOCKER_ARGS[@]}" "$IMAGE"
