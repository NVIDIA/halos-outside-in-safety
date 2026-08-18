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
    python3-yaml \
    python3-dev \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy controller source. Everything the container runs must be here: /app is
# no longer bind-mounted from the host, so a module missing from this list is a
# container that dies at import instead of one that quietly runs host code.
COPY robot_controller.py /app/
COPY state_machine.py /app/
COPY command_handler.py /app/
COPY fleet_config.py /app/

# Copy test utilities
COPY test/ /app/test/

# Copy local waypoints
COPY waypoints/ /app/waypoints/

# Copy entrypoint
COPY entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Bind-mount targets, created here so the mount does not depend on the daemon
# creating them. The fleet file and its loader are shared with Isaac rather than
# copied, so both sides read one definition of the `models:` merge.
RUN mkdir -p /app/robots /app/isaac

# Environment defaults. The drive knobs (speed, heading, tolerances, …) are
# deliberately NOT set here: an image-level value is indistinguishable from one
# the operator asked for, and would outrank the robot's own drive: block in
# robots.yaml every time. Their defaults live in fleet_config.DRIVE_DEFAULTS.
ENV ROS_DOMAIN_ID=0
ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
ENV ROBOT_ID=forklift_b

# Disable Python output buffering for real-time logs
ENV PYTHONUNBUFFERED=1

ENTRYPOINT ["/app/entrypoint.sh"]
