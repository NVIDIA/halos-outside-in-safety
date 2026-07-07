# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# ROS2 Forklift Controller Container
# Connects to Isaac Sim Action Graph via ROS2 bridge (CycloneDDS)
#
# Build:
#   docker build -t forklift-controller:latest -f Dockerfile .
#
# Run:
#   ./docker_run.sh

FROM ros:jazzy-ros-base

LABEL description="ROS2 Forklift Controller - Waypoint Navigation for Isaac Sim"
LABEL version="1.0.0"

# Install system dependencies and CycloneDDS for Isaac Sim compatibility
RUN apt-get update && apt-get install -y \
    python3-pip \
    python3-dev \
    python3-yaml \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy controller source
COPY robot_controller.py /app/
COPY state_machine.py /app/
COPY command_handler.py /app/

# Copy test utilities
COPY test/ /app/test/

# Copy local waypoints
COPY waypoints/ /app/waypoints/

# Copy entrypoint
COPY entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Environment defaults
ENV ROS_DOMAIN_ID=0
ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
ENV ROBOT_ID=forklift_1
ENV BASE_SPEED=1.5
ENV ANGULAR_SPEED=0.4
ENV HEADING_OFFSET=180
ENV USE_NAMESPACE=false
ENV LOOP_PATH=true
ENV NO_INVERT=false
ENV END_TOLERANCE=1.0
ENV END_POSE_COUNT=5
ENV SPIRAL_TIMEOUT=10.0
ENV WAYPOINT_FILE=""

# Disable Python output buffering for real-time logs
ENV PYTHONUNBUFFERED=1

ENTRYPOINT ["/app/entrypoint.sh"]
