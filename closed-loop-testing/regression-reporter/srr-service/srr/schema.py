# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
from pydantic import BaseModel
from typing import Optional


class Frame(BaseModel):
    """One record per SRR sample tick (default 30 Hz)."""

    arrival_wall_time: float
    sim_time: Optional[float] = None

    char_0_xy: Optional[tuple[float, float]] = None
    char_1_xy: Optional[tuple[float, float]] = None
    char_2_xy: Optional[tuple[float, float]] = None
    forklift_xy: Optional[tuple[float, float]] = None

    psf_command: Optional[str] = None
    is_muted: Optional[bool] = None

    ba_events: list[dict] = []

    # --- Phase 2 (3D pipeline) — nullable for Phase 1 backward compat ---
    # Latest mdx-bev snapshot at sample time: one dict per detected object,
    # {track_id, class, conf, x, y, z, w, l, h, yaw}. None = no 3D pipeline.
    detections: Optional[list[dict]] = None
    # Track ids present in that snapshot (drives id-switch analysis).
    tracker_state: Optional[list[str]] = None
    # Source mdx-bev frame id + detector wall_time, for GT time-alignment.
    bev_frame_id: Optional[str] = None
    bev_create_time: Optional[float] = None

    # Latest BA-reported positions (mdx-behavior) per active track at sample
    # time: {track_id, x, y, z, speed, direction}. Drives BA position MAE.
    ba_positions: Optional[list[dict]] = None
