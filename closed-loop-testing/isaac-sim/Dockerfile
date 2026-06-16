# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Isaac Sim with FFmpeg for RTSP streaming
# Extends official Isaac Sim image with ffmpeg dependency

ARG ISAAC_SIM_IMAGE=nvcr.io/nvidia/isaac-sim:5.1.0
FROM ${ISAAC_SIM_IMAGE}

# Switch to root to install packages
USER root

# Install ffmpeg (required for RTSP streaming in Isaac Sim)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# Apply patches for Isaac Sim
# 1. RTSP config: FFmpeg 6.x compatibility, GOP
# - Preset: "ll" → "p4" + tune "ll" (FFmpeg 6.x compatibility)
# - GOP: Add keyframe interval of 30 frames (1 second at 30fps)
# 2. Config path: Point to /isaac-sim/sil/configs/default_config_ros.yaml
# - Overrides `ConfigFileDefault.get_default_config_file_path()`
# - Points to `/isaac-sim/sil/configs/default_config_ros.yaml`
# 3. Performance: wait_for_render=False fix slow motion issue
# - Tradeoff: Multi-camera desync: 3-8ms between cameras but in RTSP case is acceptable
COPY patches/ /tmp/patches/
RUN /isaac-sim/python.sh /tmp/patches/patch_rtsp.py && \
    /isaac-sim/python.sh /tmp/patches/patch_default_config.py && \
    /isaac-sim/python.sh /tmp/patches/patch_wait_for_render.py && \
    rm -rf /tmp/patches

# Set umask 0002 so cache files are group/world readable (fixes permission issues when copying sil-data to another machine)
RUN echo 'umask 0002' >> /isaac-sim/.bashrc

# Switch back to isaac-sim user
USER 1234:1234

# Verify installation
RUN ffmpeg -version

