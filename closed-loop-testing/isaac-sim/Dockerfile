# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Isaac Sim 6.0 with FFmpeg for RTSP streaming
# Extends official Isaac Sim image with ffmpeg dependency.
#
# Isaac Sim 6.0 notes:
#   - The 3 legacy patches (patch_rtsp.py, patch_default_config.py,
#     patch_wait_for_render.py) are dropped: their 5.1 targets were removed
#     in IRA 1.5.1+ (RTSPWriter gone, config_file/ replaced by configuration/,
#     data_generation.py restructured). RTSP / config-path / render behavior is
#     now handled by the action_graphs/ + runtime_patches/ Python modules.
#   - ffmpeg is kept for RTSP streaming (the streaming.rtsp extension may bundle
#     it; left installed to stay safe).

ARG ISAAC_SIM_IMAGE=nvcr.io/nvidia/isaac-sim:6.0.0
FROM ${ISAAC_SIM_IMAGE}

# Switch to root to install packages
USER root

# Install ffmpeg (required for RTSP streaming in Isaac Sim)
RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# Set umask 0002 so cache files are group/world readable (fixes permission issues when copying sil-data to another machine)
RUN echo 'umask 0002' >> /isaac-sim/.bashrc

# Switch back to isaac-sim user
USER 1234:1234

# Verify installation
RUN ffmpeg -version
