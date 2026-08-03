#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Decode H.264 SEI NAL units to extract Isaac Sim 6.0 RTSP sim-time metadata.

Each frame carries an SEI user_data_unregistered NAL with UUID
`aa71e48f-0711-5d80-a247-cd31ca6fa49c` and a JSON payload:
    {"publish_sim_time_ns": <int>, "timestamp_iso8601": <str>,
     "timestamp": <unix_ns>, "frame_num": <int>}

Usage:
    python3 sei_decode.py <sample.h264>            # print first 3 + summary
    python3 sei_decode.py <sample.h264> --json     # one JSON line per match
    python3 sei_decode.py <sample.h264> --quiet    # summary only

Exit codes:
    0 — at least 1 SEI with target UUID + valid JSON
    1 — no SEI NALs, or no UUID match, or all payloads invalid
"""

import json
import re
import sys

TARGET_UUID = bytes.fromhex("aa71e48f07115d80a247cd31ca6fa49c")
REQUIRED_FIELDS = {"publish_sim_time_ns", "timestamp_iso8601", "timestamp", "frame_num"}


def decode(h264_bytes):
    """Walk NAL units. Return (sei_count, matches_list) where matches_list is
    [{"payload_size": int, "data": dict}, ...]."""
    nal_starts = [m.start() for m in re.finditer(b"\x00\x00\x00\x01|\x00\x00\x01", h264_bytes)]
    nal_starts.append(len(h264_bytes))

    sei_count = 0
    matches = []

    for i in range(len(nal_starts) - 1):
        start = nal_starts[i]
        sc_len = 4 if h264_bytes[start:start + 4] == b"\x00\x00\x00\x01" else 3
        nal_start = start + sc_len
        nal_end = nal_starts[i + 1]
        if nal_start >= len(h264_bytes):
            continue
        nal_type = h264_bytes[nal_start] & 0x1F
        if nal_type != 6:
            continue
        sei_count += 1
        p = nal_start + 1
        while p < nal_end - 1:
            pt = 0
            while p < nal_end and h264_bytes[p] == 0xFF:
                pt += 255; p += 1
            if p >= nal_end: break
            pt += h264_bytes[p]; p += 1
            ps = 0
            while p < nal_end and h264_bytes[p] == 0xFF:
                ps += 255; p += 1
            if p >= nal_end: break
            ps += h264_bytes[p]; p += 1
            payload = h264_bytes[p:p + ps]
            p += ps
            if pt == 5 and len(payload) >= 16 and payload[:16] == TARGET_UUID:
                json_bytes = payload[16:].rstrip(b"\x80\x00")
                try:
                    parsed = json.loads(json_bytes.decode("utf-8", errors="replace"))
                    matches.append({"payload_size": ps, "data": parsed})
                except json.JSONDecodeError:
                    matches.append({"payload_size": ps, "data": None})
            if p < nal_end and h264_bytes[p] == 0x80:
                break

    return sei_count, matches


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    path = sys.argv[1]
    flags = set(sys.argv[2:])

    with open(path, "rb") as f:
        data = f.read()

    sei_count, matches = decode(data)
    valid_json = [m for m in matches if m["data"] is not None]
    schema_ok = [m for m in valid_json if REQUIRED_FIELDS.issubset(m["data"].keys())]

    if "--json" in flags:
        for m in valid_json:
            print(json.dumps(m["data"]))
    elif "--quiet" not in flags:
        for i, m in enumerate(valid_json[:3], 1):
            print(f"--- SEI match #{i} (payload_size={m['payload_size']}) ---")
            print(json.dumps(m["data"], indent=2))

    print(f"SEI NALs: {sei_count}")
    print(f"UUID matched (aa71e48f...): {len(matches)}")
    print(f"Valid JSON payload: {len(valid_json)}")
    print(f"Schema OK (all 4 fields): {len(schema_ok)}")

    sys.exit(0 if schema_ok else 1)


if __name__ == "__main__":
    main()
