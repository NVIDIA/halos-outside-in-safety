# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Where a truck already on its path picks the path up again.

A controller that starts while the truck is mid-path, or a sim that resumes,
finds the truck wherever it stopped. The pose nearest to it, heading counted,
is where it stands; the pose after that one is where it drives next.
Targeting the nearest pose itself drives the truck on towards a point it has
already reached: at a segment end, where the next segment starts at the same
point and heading, that is past the end of the segment — on the 20x20 path,
into the end wall of the trailer.

Pose `theta` is the heading of the truck at that pose (reverse segments
included), in the controller's frame, so it compares directly with the
truck's heading.
"""

import math

# One radian of heading difference weighs as much as this many metres.
HEADING_WEIGHT_M_PER_RAD = 3.0


def _wrap(angle: float) -> float:
    return math.atan2(math.sin(angle), math.cos(angle))


def nearest_pose(poses, x: float, y: float, theta: float):
    """(index, score) of the pose closest to (x, y) with heading theta.

    On a tie the earlier pose wins, so at a segment end it is the last pose of
    the segment just driven.
    """
    best_idx, best_score = 0, float('inf')
    for i, pose in enumerate(poses):
        dist = math.hypot(float(pose['x']) - x, float(pose['y']) - y)
        heading = abs(_wrap(float(pose['theta']) - theta))
        score = dist + heading * HEADING_WEIGHT_M_PER_RAD
        if score < best_score:
            best_idx, best_score = i, score
    return best_idx, best_score


def resume_index(poses, x: float, y: float, theta: float) -> int:
    """Index of the pose to drive to next. len(poses) when the truck is at the last pose."""
    nearest, _ = nearest_pose(poses, x, y, theta)
    return nearest + 1
