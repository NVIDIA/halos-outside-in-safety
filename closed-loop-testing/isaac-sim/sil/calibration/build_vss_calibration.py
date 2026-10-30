#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Build a VSS 2D calibration dataset for a SIL scene from its Isaac Sim camera spawns.

Isaac Sim cameras are pinhole cameras whose pose and optics are fully given by the
cameras config, so no calibration has to be estimated. The output is a VSS sample-data
directory (calibration.json + images/Top.png + images/imageMetadata.json) with one
ROI and one tripwire per loading zone, ids roi-id-N / tripwire-id-N.

    python3 build_vss_calibration.py \
        --cameras ../configs/cameras-40x20.yaml \
        --zones warehouse_40x20/zones.yaml \
        --map ../../../../tools/waypoint-generator/public/maps/warehouse_40x20/config.json \
        --out-dir /tmp/warehouse-40x20-two-docks-3cams-synthetic

Needs numpy and PyYAML. fieldOfViewPolygon is filled when VSS spatialai-data-utils is
importable (PYTHONPATH=<vss>/libs/analytics/spatialai-data-utils), else left out.
"""
import argparse
import json
import math
import shutil
from pathlib import Path

import numpy as np
import yaml

IMAGE_W, IMAGE_H = 1920, 1080
# USD cameras look down -Z with +Y up; VSS / OpenCV cameras look down +Z with +Y down.
USD_TO_CV = np.diag([1.0, -1.0, -1.0])
SAMPLE_PIXELS = [(480, 700), (960, 700), (1440, 700), (480, 1000), (960, 1000), (1440, 1000)]


def _rot(axis, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    if axis == "x":
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == "y":
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def camera_to_world(spawn):
    """Rotation and position of a camera authored as [translate, rotateYXZ]."""
    rx, ry, rz = (float(v) for v in spawn["rotation_yxz_deg"])
    # xformOp:rotateYXZ applies Y first, then X, then Z.
    rotation = _rot("z", rz) @ _rot("x", rx) @ _rot("y", ry)
    return rotation, np.array([float(v) for v in spawn["position"]])


def euler_xyz_deg(rotation):
    """Intrinsic XYZ angles (R = Rx @ Ry @ Rz), the convention of VSS direction3d."""
    b = math.asin(max(-1.0, min(1.0, rotation[0, 2])))
    a = math.atan2(-rotation[1, 2], rotation[2, 2])
    c = math.atan2(-rotation[0, 1], rotation[0, 0])
    return [math.degrees(v) for v in (a, b, c)]


def zone_roi(zone):
    x0, x1, y0, y1 = (float(zone[k]) for k in ("x_min", "x_max", "y_min", "y_max"))
    return {
        "id": f"roi-id-{zone['id']}",
        "roiCoordinates": [{"x": x0, "y": y0}, {"x": x0, "y": y1}, {"x": x1, "y": y1}, {"x": x1, "y": y0}],
        "restrictedObjectTypes": ["Person"],
        "confinedObjectTypes": ["Forklift"],
    }


def zone_tripwire(zone):
    """The wire spans the zone's trailer-side edge; direction points into the trailer."""
    x_wire, y0, y1 = float(zone[zone["trailer_side"]]), float(zone["y_min"]), float(zone["y_max"])
    inward = 1.5 if zone["trailer_side"] == "x_max" else -1.5
    y_mid = (y0 + y1) / 2
    return {
        "id": f"tripwire-id-{zone['id']}",
        "wire": {"p1": {"x": x_wire, "y": y0}, "p2": {"x": x_wire, "y": y1}},
        "direction": {"p1": {"x": x_wire - inward, "y": y_mid}, "p2": {"x": x_wire + inward, "y": y_mid}},
    }


def sensor_entry(camera, scale, translation, rois, tripwires):
    spawn = camera["spawn"]
    fx = float(spawn["focal_length"]) / float(spawn["horizontal_aperture"]) * IMAGE_W
    intrinsic = np.array([[fx, 0, IMAGE_W / 2], [0, fx, IMAGE_H / 2], [0, 0, 1.0]])
    rotation_c2w, position = camera_to_world(spawn)
    rotation = USD_TO_CV @ rotation_c2w.T
    extrinsic = np.hstack([rotation, (-rotation @ position)[:, None]])
    projection = intrinsic @ extrinsic
    homography = np.linalg.inv(projection[:, [0, 1, 3]])

    image_points, world_points = [], []
    for u, v in SAMPLE_PIXELS:
        w = homography @ np.array([u, v, 1.0])
        x, y = w[:2] / w[2]
        image_points.append({"x": u, "y": v})
        world_points.append({"x": float(x), "y": float(y), "z": 0.0})

    attributes = {
        "fps": "30", "depth": "", "fieldOfView": "",
        "direction": str((-float(spawn["rotation_yxz_deg"][2])) % 360.0),
        "direction3d": ",".join(str(a) for a in euler_xyz_deg(rotation_c2w)),
        "source": "vst", "frameWidth": str(IMAGE_W), "frameHeight": str(IMAGE_H),
    }
    return {
        "type": "camera",
        "id": camera["name"],
        "origin": {"lng": 0.0001, "lat": 0.0001},
        "geoLocation": {"lng": 0.0001, "lat": 0.0001},
        "coordinates": {"x": float(position[0]), "y": float(position[1])},
        "scaleFactor": scale,
        "translationToGlobalCoordinates": dict(translation),
        "attributes": [{"name": k, "value": v} for k, v in attributes.items()],
        "place": [{"name": "building", "value": "Warehouse"}, {"name": "room", "value": "Room-1"}],
        "imageCoordinates": image_points,
        "globalCoordinates": world_points,
        "intrinsicMatrix": intrinsic.tolist(),
        "extrinsicMatrix": extrinsic.tolist(),
        "cameraMatrix": projection.tolist(),
        "homography": homography.tolist(),
        "tripwires": tripwires,
        "rois": rois,
    }


def add_fov_polygons(sensors):
    try:
        from spatialai_data_utils.core.cameras.polygon import (
            calculate_camera_frustum_polygon,
            update_sensor_fov_attributes,
        )
    except ImportError:
        print("spatialai-data-utils not importable: fieldOfViewPolygon left out")
        return
    polygons = [
        calculate_camera_frustum_polygon(
            np.array(s["intrinsicMatrix"]), np.array(s["extrinsicMatrix"]),
            height_range=(1.0, 3.0), image_size=(IMAGE_W, IMAGE_H), max_distance=30.0)
        for s in sensors
    ]
    update_sensor_fov_attributes(sensors, polygons)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cameras", required=True, type=Path, help="SIL cameras config (spawn: blocks)")
    parser.add_argument("--zones", required=True, type=Path, help="loading zones YAML")
    parser.add_argument("--map", required=True, type=Path, help="waypoint-generator map config.json")
    parser.add_argument("--out-dir", required=True, type=Path, help="VSS sample-data directory to write")
    args = parser.parse_args()

    cameras = yaml.safe_load(args.cameras.read_text())["cameras"]
    zones = yaml.safe_load(args.zones.read_text())["zones"]
    map_config = json.loads(args.map.read_text())
    scale = float(map_config["scaleFactor"])
    image_h = float(map_config["imageSize"]["height"])
    # The map measures pixelY from the top, VSS from the bottom of the image.
    translation = {"x": float(map_config["translationToGlobalCoordinates"]["x"]),
                   "y": image_h / scale - float(map_config["translationToGlobalCoordinates"]["y"])}

    rois = [zone_roi(z) for z in zones]
    tripwires = [zone_tripwire(z) for z in zones]
    sensors = [sensor_entry(c, scale, translation, rois, tripwires) for c in cameras]
    add_fov_polygons(sensors)

    images = args.out_dir / "images"
    images.mkdir(parents=True, exist_ok=True)
    calibration = {"version": "1.0", "osmURL": "", "calibrationType": "cartesian", "sensors": sensors}
    (args.out_dir / "calibration.json").write_text(json.dumps(calibration, indent=4))
    shutil.copyfile(args.map.parent / map_config["image"], images / "Top.png")
    (images / "imageMetadata.json").write_text(json.dumps(
        {"images": [{"place": "building=Warehouse/room=Room-1", "fileName": "Top.png", "view": "plan-view"}]},
        indent=2))
    print(f"wrote {args.out_dir}: sensors={[s['id'] for s in sensors]} "
          f"rois={[r['id'] for r in rois]} tripwires={[t['id'] for t in tripwires]}")


if __name__ == "__main__":
    main()
