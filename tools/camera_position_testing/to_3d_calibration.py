#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Turn a 2D-profile calibration.json into one the 3D (sparse4d) profile accepts.

The 3D profile fuses every camera into a single BEV group, so behaviour analytics
resolves ROIs and tripwires through `sensors[].group` and through top-level
`rois` / `tripwires` arrays that name their `sensors` and `groups`. The 2D profile
instead carries that geometry per sensor. Both forms are valid in the shared
calibration schema, and the loader merges the global arrays into every sensor it
names, so this writes BOTH:

  * per-sensor `rois` / `tripwires` are left exactly as the 2D file had them,
    because SRR's aggregator, tw_split and clip_logs all read the geometry from
    `sensors[0]` and would otherwise see an empty scene;
  * top-level `rois` / `tripwires` mirror that geometry with `groups` pointing at
    the BEV group, which is the path the 3D app actually uses.

Each sensor also gains the BEV `group` block and the region `place` entry that the
3D profile requires (`bp-configurator-3d` runs with CHECK_SENSOR_IN_CALIBRATION_FILE=true),
built per skills/hoisa-deploy-profile/references/calibration_3d.md.

Usage:
  ./to_3d_calibration.py --input cal-gate-I-z3.2.json --output cal-3d-I-z3.2.json
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

BEV_GROUP = "bev-sensor-1"


def group_for(sensor: dict, name: str) -> dict:
    """BEV group block for one sensor, per calibration_3d.md.

    `origin` and `dimensions` are not free-form bookkeeping: sparse4d runs with
    recentering=True and rebuilds its BEV grid from them, so inventing values
    breaks multi-view fusion — each camera lands in a differently-shifted frame
    and the fused detections collapse, while a single camera still looks fine.
    Take both from the fields the 2D calibration already carries.
    """
    t = sensor["translationToGlobalCoordinates"]
    pts = sensor.get("globalCoordinates") or []
    if not pts:
        raise SystemExit(f"FATAL sensor {sensor['id']} has no globalCoordinates to size the BEV group")
    xs = [float(p["x"]) for p in pts]
    ys = [float(p["y"]) for p in pts]
    return {
        "name": name,
        "alias": "area-1",
        "type": "bev",
        "origin": [float(t["x"]), float(t["y"])],
        "dimensions": [min(xs), min(ys), max(xs), max(ys)],
    }


def with_z(points: list[dict]) -> list[dict]:
    """Top-level ROIs in the 3D samples carry an explicit floor-level z."""
    return [{"x": p["x"], "y": p["y"], "z": p.get("z", 0.0)} for p in points]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", type=Path, required=True,
                    help="2D-profile calibration.json to convert")
    ap.add_argument("--output", type=Path, required=True,
                    help="where to write the 3D-profile calibration.json")
    ap.add_argument("--group", default=BEV_GROUP,
                    help=f"BEV group name every sensor joins (default: {BEV_GROUP})")
    args = ap.parse_args()

    cal = json.loads(args.input.read_text())
    sensor_ids = [s["id"] for s in cal["sensors"]]

    global_rois: list[dict] = []
    global_tripwires: list[dict] = []
    seen_rois: set[str] = set()
    seen_tripwires: set[str] = set()

    for s in cal["sensors"]:
        s["group"] = group_for(s, args.group)
        # `place` gains the region level the 3D profile expects; the 2D file only
        # carries building and room.
        place = s.setdefault("place", [])
        if not any(entry.get("name") == "region" for entry in place):
            place.append({"name": "region", "value": "Region-1"})
        for roi in s.get("rois", []):
            if roi["id"] in seen_rois:
                continue
            seen_rois.add(roi["id"])
            entry = {
                "id": roi["id"],
                "type": roi.get("type", "hazard_zone"),
                "roiCoordinates": with_z(roi["roiCoordinates"]),
                "sensors": list(sensor_ids),
                "groups": [args.group],
            }
            for key in ("restrictedObjectTypes", "confinedObjectTypes"):
                if roi.get(key):
                    entry[key] = roi[key]
            global_rois.append(entry)
        for tw in s.get("tripwires", []):
            if tw["id"] in seen_tripwires:
                continue
            seen_tripwires.add(tw["id"])
            global_tripwires.append({
                "id": tw["id"],
                "wire": tw["wire"],
                "direction": tw["direction"],
                "sensors": list(sensor_ids),
                "groups": [args.group],
            })

    cal["rois"] = global_rois
    if global_tripwires:
        cal["tripwires"] = global_tripwires

    args.output.write_text(json.dumps(cal, indent=4))
    print(f"wrote {args.output}")
    print(f"  sensors      : {', '.join(sensor_ids)} → group '{args.group}'")
    for s in cal["sensors"]:
        g = s["group"]
        print(f"    {s['id']:<10} origin {g['origin']}  dimensions {[round(v, 3) for v in g['dimensions']]}")
    print(f"  top-level    : {len(global_rois)} roi(s), {len(global_tripwires)} tripwire(s)")
    print(f"  per-sensor   : kept as-is so SRR still reads geometry from sensors[0]")


if __name__ == "__main__":
    main()
