# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
FROM ros:jazzy-ros-base

ENV DEBIAN_FRONTEND=noninteractive PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3-pip \
        ros-jazzy-tf2-ros \
        ros-jazzy-tf2-msgs \
        ros-jazzy-rosgraph-msgs \
        ros-jazzy-std-srvs \
        ros-jazzy-rmw-cyclonedds-cpp \
 && rm -rf /var/lib/apt/lists/*

# Isaac Sim 6.0's ROS 2 bridge + the whole Halos ROS 2 stack (comm-layer,
# forklift-controller, safety-core) run ROS 2 JAZZY on CycloneDDS. SRR must
# match BOTH:
#   - Distro: Jazzy uses XCDR2 + newer message type hashes. A Humble container
#     can't reliably receive Jazzy-published /gt/*/tf (FastRTPS decoded garbage
#     -> "sequence size exceeds remaining buffer"; CycloneDDS received nothing).
#   - RMW: Humble/other defaults differ; the stack standardizes on CycloneDDS.
ENV RMW_IMPLEMENTATION=rmw_cyclonedds_cpp

# Build context is the regression-reporter dir (see docker-compose.yml) so that
# the post-processing scripts/ dir (heatmap/density renderers) can be baked in.
WORKDIR /app
COPY srr-service/pyproject.toml ./
# Noble (Jazzy base) marks the system Python as externally-managed (PEP 668);
# this is a dedicated container, so install straight into it.
RUN pip install --no-cache-dir --break-system-packages \
        kafka-python pyarrow pandas 'pydantic>=2.0' shapely numpy matplotlib 'protobuf>=5.27.3'

COPY srr-service/srr /app/srr
# Heatmap / density renderers (render_perception_heatmap.py, …). matplotlib above
# is required by these; without it the scripts fail at import time.
COPY scripts /app/scripts
ENV PYTHONPATH=/app

# Source ROS env on every shell; rclpy needs RMW_IMPLEMENTATION etc.
SHELL ["/bin/bash", "-c"]
CMD ["bash", "-c", "source /opt/ros/jazzy/setup.bash && exec python3 -m srr.service"]
