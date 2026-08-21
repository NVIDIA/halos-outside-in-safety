#!/usr/bin/env python3
"""Derive a VSS `calibration.json` from the Isaac Sim `cameras.yaml` camera poses.

Why this exists
---------------
Moving the SIL cameras invalidates the VSS perception calibration: `cameras.yaml`
carries a note that its values replicate the baked production cameras 1:1 precisely so
that the shipped `warehouse-loading-dock-3cams-synthetic` calibration stays valid. Once a
camera moves, the camera matrices in `calibration.json` describe a camera that no longer
exists, and perception drifts *silently* — detections still look fine but world positions,
ROI membership and tripwire crossings are wrong.

The normal remedy is the VSS AutoMagicCalib (AMC) UI: upload video, click point
correspondences, run the solver, export. That exists to recover an *unknown* camera pose
from real footage. In simulation the pose is not unknown — we choose it in `cameras.yaml` —
so the calibration is a closed-form coordinate change, not an estimation problem. This
module implements that closed form, which makes a camera-position sweep scriptable.

Verified against the shipped calibration
----------------------------------------
Because the current `cameras.yaml` reproduces the baked cameras exactly, the shipped
`calibration.json` is a ground truth for those poses. Running `--verify` against it gives:

    intrinsicMatrix   fx = width * focal_length / horizontal_aperture   err 1.4e-05
                      (residual is only the rounding of horizontal_aperture in the YAML)
    extrinsicMatrix   camera centre -R^T @ t vs spawn.position           err 2e-15
                      rotation R                                         err 6e-08

`cameraMatrix` and `homography` are *not* reproduced to that precision, and should not be:
in the shipped file they were least-squares fitted from six hand-clicked point pairs, not
derived from the pose. The evidence is internal to that file — its `cameraMatrix` disagrees
with its own `intrinsicMatrix @ extrinsicMatrix` by up to 3.5%, while reprojecting its own
six click points to 0.1 px, and those points sit up to 2.2 m off the floor plane they are
meant to lie on. This module emits `cameraMatrix = K @ [R|t]` and the exact ground-plane
homography, which are geometrically correct rather than fitted.

Conventions resolved empirically
--------------------------------
USD `xformOp:rotateYXZ` composes as ``Rz @ Rx @ Ry`` on column vectors. USD cameras look
down -Z with +Y up; the CV convention used by `extrinsicMatrix` looks down +Z with +Y down.
Hence::

    R = diag(1, -1, -1) @ (Rz @ Rx @ Ry).T
    t = -R @ camera_position

`homography` is image -> world on the ground plane (z = 0), i.e. the inverse of
``K @ [r1 r2 t]``. A homography is scale-invariant; this module normalises H[2][2] to 1.

Tripwire push-out and the ROI
-----------------------------
The tripwire lives in this file too, in world coordinates, so `--tripwire-pushout D` moves
it here rather than in a separate edit. In the shipped scene the ROI's east edge is
*coincident* with the tripwire (both x = 9.574), so pushing the wire west detaches the two
unless the ROI is moved with it — see `--roi-follow`. Whether it should be is a scope
question for whoever owns the test plan, not something this module decides.

Fields this module does not compute
-----------------------------------
`scaleFactor` and `translationToGlobalCoordinates` are identical across all three sensors
in the shipped file — they describe the floorplan (`Top.png`) raster, not any camera — so
they are carried over from the template unchanged, as is `Top.png` itself.

`fieldOfViewPolygon` is the visible floor footprint and *does* depend on camera pose. VSS
ships its own frustum generator for it, which reads only `intrinsicMatrix` and
`extrinsicMatrix`; pass ``--fov-polygon frustum`` to invoke it (requires the VSS checkout
and `shapely`). The shipped polygons additionally encode occlusion by scene geometry as
interior rings, which the frustum generator does not reproduce.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import sys
from pathlib import Path
from typing import Any

import yaml

Matrix = list[list[float]]
Vector = list[float]

DEFAULT_RESOLUTION = (1920, 1080)


# --------------------------------------------------------------------------- linear algebra
# Pure python on purpose: this has to run on a bare SIL host, where numpy is often absent.


def matmul(a: Matrix, b: Matrix) -> Matrix:
    return [[sum(a[i][k] * b[k][j] for k in range(len(b))) for j in range(len(b[0]))]
            for i in range(len(a))]


def transpose(m: Matrix) -> Matrix:
    return [[m[j][i] for j in range(len(m))] for i in range(len(m[0]))]


def matvec(m: Matrix, v: Vector) -> Vector:
    return [sum(m[i][k] * v[k] for k in range(len(v))) for i in range(len(m))]


def rotation(axis: str, degrees: float) -> Matrix:
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    if axis == "x":
        return [[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]]
    if axis == "y":
        return [[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]
    if axis == "z":
        return [[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]]
    raise ValueError(f"unknown axis {axis!r}")


def invert3(m: Matrix) -> Matrix:
    (a, b, c), (d, e, f), (g, h, i) = m[0], m[1], m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-15:
        raise ValueError("singular matrix")
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def max_abs_diff(a: Matrix, b: Matrix) -> float:
    return max(abs(a[i][j] - b[i][j]) for i in range(len(a)) for j in range(len(a[0])))


# ------------------------------------------------------------------------------ calibration


def intrinsic_matrix(focal_length: float, horizontal_aperture: float,
                     resolution: tuple[int, int]) -> Matrix:
    """Pinhole K. Isaac renders square pixels sized by the horizontal aperture, so
    `vertical_aperture` does not enter fy (confirmed against the shipped calibration)."""
    width, height = resolution
    f_px = width * focal_length / horizontal_aperture
    return [[f_px, 0.0, width / 2.0],
            [0.0, f_px, height / 2.0],
            [0.0, 0.0, 1.0]]


def extrinsic_matrix(position: Vector, rotation_yxz_deg: Vector) -> tuple[Matrix, Vector]:
    """Return (R, t) in the CV convention from a USD rotateYXZ pose."""
    rx, ry, rz = rotation_yxz_deg
    usd = matmul(matmul(rotation("z", rz), rotation("x", rx)), rotation("y", ry))
    flip = [[1.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, -1.0]]
    r = matmul(flip, transpose(usd))
    t = [-v for v in matvec(r, position)]
    return r, t


def camera_matrix(k: Matrix, r: Matrix, t: Vector) -> Matrix:
    """K @ [R|t], normalised so the last element is 1 (the shipped file's convention)."""
    p = matmul(k, [r[i] + [t[i]] for i in range(3)])
    scale = p[2][3]
    return [[v / scale for v in row] for row in p]


def ground_homography(k: Matrix, r: Matrix, t: Vector) -> Matrix:
    """Image -> world homography on the z = 0 plane, normalised so H[2][2] == 1."""
    world_to_image = matmul(k, [[r[i][0], r[i][1], t[i]] for i in range(3)])
    h = invert3(world_to_image)
    return [[v / h[2][2] for v in row] for row in h]


def camera_direction_deg(rotation_yxz_deg: Vector) -> float:
    """Compass-style yaw stored in the `direction` attribute (verified: -rotation_z)."""
    return -rotation_yxz_deg[2]


def camera_direction3d(rotation_yxz_deg: Vector) -> str:
    """`direction3d` is informational — nothing in VSS reads it (grepped against 3.2.1).
    The shipped values came from the AMC fit and differ from the true pose by up to 30
    degrees, so this reports the true pose instead."""
    rx, ry, rz = rotation_yxz_deg
    return f"{ry},{-rx},{rz}"


# ------------------------------------------------------------------------------------ I/O


def load_cameras(path: Path) -> dict[str, dict[str, Any]]:
    """Map camera name -> its `spawn` block, skipping cameras with no spawn."""
    config = yaml.safe_load(path.read_text())
    cameras: dict[str, dict[str, Any]] = {}
    for cam in config.get("cameras", []):
        spawn = cam.get("spawn")
        if spawn is None:
            continue
        cameras[cam["name"]] = spawn
    if not cameras:
        raise SystemExit(f"{path}: no cameras with a `spawn:` block — nothing to derive from")
    return cameras


def set_attribute(sensor: dict[str, Any], name: str, value: str) -> None:
    for attr in sensor.setdefault("attributes", []):
        if attr.get("name") == name:
            attr["value"] = value
            return
    sensor["attributes"].append({"name": name, "value": value})


def get_attribute(sensor: dict[str, Any], name: str) -> str | None:
    for attr in sensor.get("attributes", []):
        if attr.get("name") == name:
            return attr.get("value")
    return None


ON_WIRE_TOLERANCE_M = 1e-6


def outward_normal(wire: dict[str, Any]) -> tuple[float, float]:
    """Unit vector pointing away from the trailer.

    `direction` p1 -> p2 points into the trailer, so the outward shift is its negation.
    """
    direction = wire.get("direction")
    if not direction:
        raise SystemExit(f"tripwire {wire.get('id')!r} has no `direction` — cannot "
                         "determine which side is inside the trailer")
    dx = direction["p2"]["x"] - direction["p1"]["x"]
    dy = direction["p2"]["y"] - direction["p1"]["y"]
    norm = math.hypot(dx, dy)
    if norm < 1e-12:
        raise SystemExit(f"tripwire {wire.get('id')!r} has a degenerate `direction`")
    return -dx / norm, -dy / norm


def distance_to_line(point: dict[str, Any], p1: dict[str, Any],
                     p2: dict[str, Any]) -> float:
    """Perpendicular distance from `point` to the infinite line through p1, p2."""
    dx, dy = p2["x"] - p1["x"], p2["y"] - p1["y"]
    norm = math.hypot(dx, dy)
    if norm < 1e-12:
        return math.dist((point["x"], point["y"]), (p1["x"], p1["y"]))
    return abs(dy * (point["x"] - p1["x"]) - dx * (point["y"] - p1["y"])) / norm


def push_out_geometry(container: dict[str, Any], distance: float,
                      roi_follow: str) -> None:
    """Translate the tripwires of `container` `distance` metres away from the trailer.

    Works on whichever level holds the geometry: the 2D profile keeps `tripwires` / `rois`
    per sensor, the 3D profile keeps them at the top level of the calibration.

    `roi_follow` decides what happens to an ROI whose edge is coincident with the tripwire
    (in the shipped scene the ROI's east edge and the tripwire are both x = 9.574):

    ``none``   leave the ROI alone. The tripwire detaches from the ROI edge, so the strip
               between them belongs to the ROI while being on the trailer side of the wire.
    ``edge``   move only the ROI vertices lying on the tripwire, keeping the two attached.
               The ROI shrinks by `distance` x wire length.
    ``shift``  translate the whole ROI with the tripwire. Attached, area preserved, and the
               far edge moves too — the protected region relocates into the warehouse.

    Which one is correct is an open question for the test's author (ANALYSIS.md 7 Q4), so
    the caller must choose; there is no defensible default beyond "change nothing".
    """
    if distance == 0.0:
        return
    wires = container.get("tripwires") or []
    rois = container.get("rois") or []
    if roi_follow != "none" and len(wires) > 1:
        raise SystemExit(f"--roi-follow {roi_follow} is ambiguous with {len(wires)} "
                         "tripwires: it cannot tell which wire an ROI edge belongs to. "
                         "Use --roi-follow none and move the ROIs by hand.")

    for wire in wires:
        nx, ny = outward_normal(wire)
        ox, oy = nx * distance, ny * distance
        original = copy.deepcopy(wire["wire"])
        for block in ("wire", "direction"):
            for point in wire[block].values():
                point["x"] += ox
                point["y"] += oy

        if roi_follow == "none":
            continue
        for roi in rois:
            for vertex in roi.get("roiCoordinates", []):
                if roi_follow == "edge" and distance_to_line(
                        vertex, original["p1"], original["p2"]) > ON_WIRE_TOLERANCE_M:
                    continue
                vertex["x"] += ox
                vertex["y"] += oy


def build(template: dict[str, Any], cameras: dict[str, dict[str, Any]],
          resolution: tuple[int, int], tripwire_pushout: float, roi_follow: str,
          drop_missing: bool) -> dict[str, Any]:
    result = copy.deepcopy(template)
    width, height = resolution

    template_ids = {s.get("id") for s in result.get("sensors", [])}
    unknown = set(cameras) - template_ids
    if unknown:
        raise SystemExit(f"cameras.yaml has cameras absent from the template: {sorted(unknown)}. "
                         "Add them to the template first — this tool updates sensors, it does "
                         "not invent scene metadata (place, origin, scaleFactor).")

    missing = sorted(template_ids - set(cameras))
    if missing and drop_missing:
        result["sensors"] = [s for s in result["sensors"] if s.get("id") in cameras]
        print(f"  removed {len(missing)} sensor(s) absent from cameras.yaml: "
              f"{', '.join(missing)}", file=sys.stderr)
        print("    calibration.json is only one of the places the stream count is pinned. "
              "Also update NUM_STREAMS in the VSS overrides, the rows in "
              "safety-core/configs/sensor_config.conf, the SENSORS default in tw_split.py, "
              "and the VST registrations — otherwise the stack waits for a stream that "
              "never arrives.", file=sys.stderr)
    elif missing:
        print(f"  ! no spawn block in cameras.yaml for: {', '.join(missing)}", file=sys.stderr)
        print("    their camera matrices are left at the template's values and are now "
              "inconsistent with the cameras Isaac renders. Pass --drop-missing-sensors if "
              "you meant to run with fewer cameras.", file=sys.stderr)

    for sensor in result.get("sensors", []):
        spawn = cameras.get(sensor.get("id"))
        if spawn is None:
            continue

        position = [float(v) for v in spawn["position"]]
        rot = [float(v) for v in spawn["rotation_yxz_deg"]]
        k = intrinsic_matrix(float(spawn["focal_length"]),
                            float(spawn["horizontal_aperture"]), resolution)
        r, t = extrinsic_matrix(position, rot)

        sensor["intrinsicMatrix"] = k
        sensor["extrinsicMatrix"] = [r[i] + [t[i]] for i in range(3)]
        sensor["cameraMatrix"] = camera_matrix(k, r, t)
        sensor["homography"] = ground_homography(k, r, t)
        sensor["coordinates"] = {"x": position[0], "y": position[1]}

        set_attribute(sensor, "direction", str(camera_direction_deg(rot)))
        set_attribute(sensor, "direction3d", camera_direction3d(rot))
        set_attribute(sensor, "frameWidth", str(width))
        set_attribute(sensor, "frameHeight", str(height))

    # A tripwire is scene geometry replicated across every sensor, not a property of any one
    # camera, so it moves even in sensors whose pose was left untouched — otherwise the same
    # wire would sit at two different places in one file. The top-level call is a no-op for
    # the 2D profile (geometry is per sensor) and does the work for the 3D one.
    for sensor in result.get("sensors", []):
        push_out_geometry(sensor, tripwire_pushout, roi_follow)
    push_out_geometry(result, tripwire_pushout, roi_follow)

    return result


def regenerate_fov_polygons(calibration: dict[str, Any], vss_root: Path,
                            resolution: tuple[int, int]) -> None:
    """Delegate to VSS's own frustum generator, which reads only intrinsic + extrinsic."""
    lib = vss_root / "libs" / "analytics" / "spatialai-data-utils"
    if not lib.is_dir():
        raise SystemExit(f"{lib} not found — check --vss-root")
    sys.path.insert(0, str(lib))
    try:
        from spatialai_data_utils.core.cameras.polygon import fill_sensor_fov_polygons
    except ImportError as exc:
        raise SystemExit(
            f"cannot import the VSS frustum generator ({exc}).\n"
            "It needs numpy and shapely, which a bare SIL host usually lacks. Either run "
            "this script inside the srr container (it ships both), or use "
            "--fov-polygon keep and regenerate the polygons separately.") from exc
    filled, skipped, missing = fill_sensor_fov_polygons(
        calibration["sensors"], use_frustum=True, image_size=resolution)
    print(f"  fieldOfViewPolygon: {filled} regenerated, {skipped} skipped, {missing} absent")


def verify(template: dict[str, Any], cameras: dict[str, dict[str, Any]],
           resolution: tuple[int, int]) -> int:
    """Re-derive the template's own matrices and report the residuals."""
    print("Verifying derived matrices against the template "
          "(meaningful only while cameras.yaml still matches it)\n")
    worst = 0.0
    for sensor in template.get("sensors", []):
        spawn = cameras.get(sensor.get("id"))
        if spawn is None or "extrinsicMatrix" not in sensor:
            continue
        position = [float(v) for v in spawn["position"]]
        rot = [float(v) for v in spawn["rotation_yxz_deg"]]
        k = intrinsic_matrix(float(spawn["focal_length"]),
                            float(spawn["horizontal_aperture"]), resolution)
        r, t = extrinsic_matrix(position, rot)

        k_err = max_abs_diff(k, sensor["intrinsicMatrix"])
        e_ref = sensor["extrinsicMatrix"]
        r_err = max_abs_diff(r, [row[:3] for row in e_ref])
        centre = [-v for v in matvec(transpose([row[:3] for row in e_ref]),
                                    [row[3] for row in e_ref])]
        c_err = math.dist(centre, position)

        print(f"  {sensor['id']:<10}  intrinsic {k_err:.3e}   rotation {r_err:.3e}   "
              f"camera centre {c_err:.3e} m")
        worst = max(worst, r_err, c_err)

    tolerance = 1e-5
    if worst <= tolerance:
        print(f"\nOK — pose reproduced to {worst:.3e} (tolerance {tolerance:.0e})")
        return 0
    print(f"\nFAIL — worst residual {worst:.3e} exceeds {tolerance:.0e}. Either cameras.yaml "
          "no longer matches this template, or a convention changed.", file=sys.stderr)
    return 1


def parse_resolution(text: str) -> tuple[int, int]:
    try:
        width, height = (int(v) for v in text.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected WIDTHxHEIGHT, got {text!r}") from None
    return width, height


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cameras-config", type=Path, required=True,
                        help="Isaac SIL cameras.yaml (source of truth for the poses)")
    parser.add_argument("--template", type=Path, required=True,
                        help="existing calibration.json supplying the scene metadata "
                             "(place, origin, scaleFactor, ROI/tripwire geometry)")
    parser.add_argument("--output", type=Path,
                        help="where to write the derived calibration.json ('-' for stdout)")
    parser.add_argument("--resolution", type=parse_resolution,
                        default=DEFAULT_RESOLUTION, metavar="WxH",
                        help="render resolution, must match the RTSP streams "
                             f"(default {DEFAULT_RESOLUTION[0]}x{DEFAULT_RESOLUTION[1]})")
    parser.add_argument("--tripwire-pushout", type=float, default=0.0, metavar="D",
                        help="metres to translate every tripwire away from the trailer, "
                             "along the outward normal of its `direction` arrow")
    parser.add_argument("--roi-follow", choices=("none", "edge", "shift"), default="none",
                        help="what an ROI does when the tripwire is pushed out: 'none' "
                             "leaves it (tripwire detaches from the ROI edge); 'edge' moves "
                             "only the ROI vertices lying on the tripwire (ROI shrinks); "
                             "'shift' translates the whole ROI (area preserved)")
    parser.add_argument("--drop-missing-sensors", action="store_true",
                        help="delete template sensors that have no spawn block in "
                             "cameras.yaml, for running with fewer cameras than the "
                             "template describes")
    parser.add_argument("--fov-polygon", choices=("keep", "frustum"), default="keep",
                        help="'keep' carries the template's polygons over unchanged (stale "
                             "once a camera moves); 'frustum' regenerates them with VSS's "
                             "own generator (needs --vss-root and shapely)")
    parser.add_argument("--vss-root", type=Path,
                        help="VSS checkout, required by --fov-polygon frustum")
    parser.add_argument("--verify", action="store_true",
                        help="re-derive the template's matrices and report residuals "
                             "instead of writing output")
    args = parser.parse_args(argv)

    cameras = load_cameras(args.cameras_config)
    template = json.loads(args.template.read_text())

    if args.verify:
        return verify(template, cameras, args.resolution)

    if args.output is None:
        parser.error("--output is required unless --verify is given")

    calibration = build(template, cameras, args.resolution, args.tripwire_pushout,
                        args.roi_follow, args.drop_missing_sensors)

    if args.fov_polygon == "frustum":
        if args.vss_root is None:
            parser.error("--fov-polygon frustum requires --vss-root")
        regenerate_fov_polygons(calibration, args.vss_root, args.resolution)
    else:
        print("  fieldOfViewPolygon: carried over from the template — STALE if a camera "
              "moved. Use --fov-polygon frustum to regenerate.", file=sys.stderr)

    payload = json.dumps(calibration, indent=2)
    if str(args.output) == "-":
        print(payload)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n")
        print(f"  wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
