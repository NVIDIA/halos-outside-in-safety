# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Communication Layer Container
# Bridges: UDP Receiver + OPC UA Server + ROS2 Bridge
#
# Build:
#   docker build -t comm-layer:latest -f Dockerfile .

FROM ros:jazzy-ros-base

LABEL maintainer="NVIDIA"
LABEL description="Communication Layer - UDP/OPC UA/ROS2 Bridge"
LABEL version="1.0.0"

# Install system dependencies and CycloneDDS for Isaac Sim compatibility
RUN apt-get update && apt-get install -y \
    python3-pip \
    python3-dev \
    curl \
    netcat-openbsd \
    ros-jazzy-rmw-cyclonedds-cpp \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies (--break-system-packages for Ubuntu 24.04/Python 3.12)
# Note: Using asyncua>=0.9.96 instead of opcua to fix CVE-2022-25304
# --ignore-installed to avoid conflict with system cryptography package
RUN pip3 install --no-cache-dir --break-system-packages --ignore-installed \
    asyncua>=0.9.96 \
    python-dateutil

# Create app directory
WORKDIR /app

# Copy source
COPY src/comm_layer /app/comm_layer
COPY src/ros_bridge /app/ros_bridge

# Copy entrypoint script
COPY scripts/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Create logs directory
RUN mkdir -p /app/logs

# Expose ports
# UDP port for receiving commands from PSF
EXPOSE 12346/udp
# OPC UA server port
EXPOSE 4840

# Environment defaults
ENV UDP_PORT=12346
ENV OPCUA_ENDPOINT=opc.tcp://0.0.0.0:4840/safety/
ENV ROS_TOPIC_PREFIX=/safety
ENV ROS_DOMAIN_ID=0

# Python path for local modules
ENV PYTHONPATH=/app/comm_layer:/app/ros_bridge

# Disable Python output buffering for real-time logs
ENV PYTHONUNBUFFERED=1

# Health check (using asyncua.sync for synchronous API)
HEALTHCHECK --interval=15s --timeout=10s --start-period=10s --retries=3 \
    CMD python3 -c "from asyncua.sync import Client; c=Client('opc.tcp://localhost:4840/safety/'); c.connect(); c.disconnect()" || exit 1

ENTRYPOINT ["/app/entrypoint.sh"]
