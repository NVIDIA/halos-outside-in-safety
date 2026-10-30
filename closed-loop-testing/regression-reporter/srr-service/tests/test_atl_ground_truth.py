# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""The Phase 1 ground truth follows the ATL rule PSF decides on.

MUTE iff the forklift is in the trailer, no person is in the trailer and no
person is in the ROI. Frames that rest on a person at the ROI edge, and frames
within the settling allowance of an expected-state change, are not scored; a
lag is measured only within its own expected-state interval, and not for an
interval begun by a person crossing the ROI edge. Run in the srr image:

    docker run --rm -v $PWD:/svc -w /svc --entrypoint python3 srr-service:phase1 \
      -m pytest -q -p no:cacheprovider tests
"""

import json
import os
import sys

import pandas as pd

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from srr.aggregator import analyze_clip, load_roi  # noqa: E402

HZ = 30
FAR = (0.0, 0.0)            # outside the ROI and the trailer
TRAILER = (11.0, -15.0)     # past the tripwire at x = 9.574
OUTSIDE_TRAILER = (8.0, -15.0)
ROI_CENTRE = (7.2, -15.1)   # >2 m from every ROI edge
ROI_EDGE = (5.0, -15.1)     # 0.12 m inside the x = 4.877 edge
ROI_EDGE_OUT = (4.8, -15.1)  # 0.08 m outside the same edge


def write_clip(tmp_path, n, forklift, chars, muted):
    """forklift(i) / chars(i) -> positions, muted(i) -> bool, one row per 1/30 s."""
    rows = []
    for i in range(n):
        fx, fy = forklift(i)
        row = {"arrival_wall_time": 1000.0 + i / HZ, "sim_time": i / HZ,
               "forklift_x": fx, "forklift_y": fy,
               "psf_command": json.dumps({"command_name": "MUTE" if muted(i) else "UNMUTE"}),
               "is_muted": muted(i), "ba_events_json": "[]", "detections_json": None,
               "tracker_state_json": None, "bev_frame_id": None, "bev_create_time": None,
               "ba_positions_json": None}
        for c, (x, y) in enumerate(chars(i)):
            row[f"char_{c}_x"], row[f"char_{c}_y"] = x, y
        rows.append(row)
    scenes = tmp_path / "run" / "scenes"
    scenes.mkdir(parents=True)
    path = scenes / "scn_0000.parquet"
    pd.DataFrame(rows).to_parquet(path)
    return path


def score(path):
    roi, tw = load_roi(None)
    v, df = analyze_clip(path, roi, tw)
    return v, df


def test_a_person_in_the_trailer_expects_unmute(tmp_path):
    path = write_clip(tmp_path, 300, lambda i: TRAILER,
                      lambda i: [TRAILER, FAR, FAR], lambda i: False)
    v, df = score(path)
    assert not df["expected_mute"].any()
    assert v.pct_char_in_trailer == 100.0
    assert v.match_rate == 100.0 and v.verdict == "PASS"


def test_a_person_on_the_roi_edge_is_not_scored(tmp_path):
    # Nobody near the ROI for 5 s, then a person on its edge; the system stays muted.
    path = write_clip(tmp_path, 300, lambda i: TRAILER,
                      lambda i: [FAR if i < 150 else ROI_EDGE, FAR, FAR], lambda i: True)
    v, _ = score(path)
    assert v.n_unscored_edge_frames == 150
    assert v.match_rate == 100.0
    assert v.raw_match_rate == 50.0


def test_a_person_well_inside_the_roi_is_scored(tmp_path):
    path = write_clip(tmp_path, 300, lambda i: TRAILER,
                      lambda i: [FAR if i < 150 else ROI_CENTRE, FAR, FAR], lambda i: True)
    v, _ = score(path)
    assert v.n_unscored_edge_frames == 0
    assert v.n_over_mute_frames == 150 - HZ  # the first second after the change settles
    assert v.verdict == "FAIL"


def test_a_lag_within_the_settling_allowance_is_not_a_mismatch(tmp_path):
    enter, lag = 60, 24  # 0.8 s
    path = write_clip(tmp_path, 300, lambda i: TRAILER if i >= enter else OUTSIDE_TRAILER,
                      lambda i: [FAR, FAR, FAR], lambda i: i >= enter + lag)
    v, _ = score(path)
    assert v.match_rate == 100.0
    assert v.raw_match_rate < 100.0
    assert v.max_reaction_mute_ms == 800.0


def test_a_lag_beyond_the_settling_allowance_still_counts(tmp_path):
    enter, lag = 60, 48  # 1.6 s
    path = write_clip(tmp_path, 300, lambda i: TRAILER if i >= enter else OUTSIDE_TRAILER,
                      lambda i: [FAR, FAR, FAR], lambda i: i >= enter + lag)
    v, _ = score(path)
    assert v.n_under_mute_frames == lag - HZ
    assert v.max_reaction_mute_ms == 1600.0


def test_a_missed_interval_is_not_answered_by_a_later_one(tmp_path):
    # The forklift is in the trailer for 0.1 s at 2.0 s, and again from 5.0 s;
    # the system mutes only at 5.3 s.
    def forklift(i):
        return TRAILER if 60 <= i < 63 or i >= 150 else OUTSIDE_TRAILER
    path = write_clip(tmp_path, 300, forklift, lambda i: [FAR, FAR, FAR], lambda i: i >= 159)
    v, _ = score(path)
    assert v.missed_intervals == [("MUTE", 2.0, 0.1)]
    assert v.max_reaction_mute_ms == 300.0


def test_a_person_stepping_across_the_roi_edge_is_not_timed(tmp_path):
    # The person on the edge steps just outside for 0.1 s at 2.0 s; the system stays unmuted.
    path = write_clip(tmp_path, 300, lambda i: TRAILER,
                      lambda i: [ROI_EDGE_OUT if 60 <= i < 63 else ROI_EDGE, FAR, FAR],
                      lambda i: False)
    v, _ = score(path)
    assert v.missed_intervals == []
    assert v.n_untimed_edge_intervals == 3
    assert v.n_unmute_transitions == 0


def test_the_forklift_entering_with_a_person_at_the_roi_edge_is_timed(tmp_path):
    enter, lag = 60, 48  # 1.6 s
    path = write_clip(tmp_path, 300, lambda i: TRAILER if i >= enter else OUTSIDE_TRAILER,
                      lambda i: [ROI_EDGE_OUT, FAR, FAR], lambda i: i >= enter + lag)
    v, _ = score(path)
    assert v.n_untimed_edge_intervals == 0
    assert v.max_reaction_mute_ms == 1600.0
