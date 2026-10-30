# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Picking the path up mid-way. No ROS: python3 -m pytest test/test_path_resume.py"""

import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from path_resume import nearest_pose, resume_index  # noqa: E402


def in_and_out(n=21, length=10.0, heading=math.pi):
    """Drive n poses along -x facing `heading`, then reverse back over the same poses.

    The shape of the 20x20 path: into the trailer forwards, out in reverse, the
    truck facing the same way throughout.
    """
    forward = [{'x': -length * i / (n - 1), 'y': 0.0, 'theta': heading} for i in range(n)]
    return forward + list(reversed(forward))


def test_at_the_segment_end_the_next_segment_is_the_target():
    poses = in_and_out()
    # Deep in the trailer, where the forward segment ends and the reverse one starts.
    assert resume_index(poses, -10.0, 0.0, math.pi) == 21


def test_mid_segment_targets_the_pose_ahead():
    poses = in_and_out()
    assert resume_index(poses, -5.05, 0.0, math.pi) == 11


def test_heading_picks_between_poses_at_the_same_place():
    poses = [
        {'x': 0.0, 'y': 0.0, 'theta': 0.0},
        {'x': 1.0, 'y': 0.0, 'theta': 0.0},
        {'x': 1.0, 'y': 0.0, 'theta': math.pi / 2},
        {'x': 1.0, 'y': 1.0, 'theta': math.pi / 2},
    ]
    assert nearest_pose(poses, 1.0, 0.0, math.pi / 2)[0] == 2
    assert resume_index(poses, 1.0, 0.0, 0.0) == 2


def test_heading_wraps_across_pi():
    poses = [{'x': 0.0, 'y': 0.0, 'theta': math.pi - 0.01}, {'x': 0.0, 'y': 0.0, 'theta': 0.0}]
    assert nearest_pose(poses, 0.0, 0.0, -math.pi + 0.01)[0] == 0


def test_at_the_last_pose_the_path_is_done():
    poses = in_and_out()
    assert resume_index(poses, 0.0, 0.0, math.pi) == 1          # standing on pose 0: pose 1 is next
    assert resume_index(poses[:21], -10.0, 0.0, math.pi) == 21  # one-segment path, at its end


def test_shipped_20x20_path_resumes_reversing_from_the_trailer_end():
    with open(os.path.join(HERE, '..', 'waypoints', 'warehouse_20x20', 'forklift_b.json')) as f:
        data = json.load(f)
    poses = data['poses']
    first_reverse = data['segments'][0]['pose_count']
    end = poses[first_reverse - 1]
    idx = resume_index(poses, end['x'], end['y'], end['theta'])
    assert idx == first_reverse
