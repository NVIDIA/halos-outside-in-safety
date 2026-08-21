#!/usr/bin/env python3
"""Draw the tripwire and work zone onto camera frames, straight from a calibration.

Two questions this answers before a run is spent. Does the placement see the whole tripwire —
a wire that leaves the frame has crossings no amount of tuning will recover. And, when the
zone is being pushed away from the wall, what does each distance actually look like from
these cameras, including how the two --roi-follow rules differ.

Nothing is rendered: the cameras do not move while the zone is pushed out, so one frame per
camera plus the projected geometry is the whole picture.

Usage:
    ./preview_geometry.py --calibration cal.json \\
        --shot Camera=shots/Camera.png --shot Camera_01=shots/Camera_01.png \\
        --pushout 0 1 2 3 --output preview.png
"""

from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from generate_calibration import push_out_geometry
from screen_fov import Sensor, project

WIRE = (255, 70, 70)
WIRE_AT_ZERO = (190, 190, 190)
ROI_EDGE = (255, 215, 0)
ROI_SHIFT = (0, 225, 255)
FONT_DIR = "/usr/share/fonts/truetype/dejavu"


def font(name: str, size: int):
    try:
        return ImageFont.truetype(f"{FONT_DIR}/{name}", size)
    except OSError:
        return ImageFont.load_default()


def pushed(calibration: dict, distance: float, roi_follow: str) -> dict:
    """The calibration as a run at this distance would receive it."""
    out = copy.deepcopy(calibration)
    for sensor in out["sensors"]:
        push_out_geometry(sensor, distance, roi_follow)
    return out


def geometry(calibration: dict, sensor_id: str):
    entry = next(s for s in calibration["sensors"] if s["id"] == sensor_id)
    wire = entry["tripwires"][0]["wire"]
    roi = entry["rois"][0]["roiCoordinates"]
    return ([(wire["p1"]["x"], wire["p1"]["y"]), (wire["p2"]["x"], wire["p2"]["y"])],
            [(p["x"], p["y"]) for p in roi])


def in_frame_percent(sensor: Sensor, a, b, steps: int = 400) -> float:
    seen = 0
    for i in range(steps + 1):
        t = i / steps
        u, v, depth = project(sensor.k, sensor.r, sensor.t,
                              [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, 0.0])
        seen += depth > 0 and 0 <= u < sensor.width and 0 <= v < sensor.height
    return 100.0 * seen / (steps + 1)


def polyline(draw, sensor: Sensor, path, colour, width: int, close: bool) -> None:
    """Draw a floor path, sampled so the frame edge cuts it where it should.

    A world segment projects to an image segment only while it stays in front of the camera.
    Endpoints alone would happily draw a line across the frame standing for geometry behind
    the lens, so sample and drop the points with non-positive depth.
    """
    corners = list(path) + [path[0]] if close else list(path)
    for a, b in zip(corners, corners[1:]):
        steps = max(2, int(math.dist(a, b) / 0.05))
        previous = None
        for i in range(steps + 1):
            t = i / steps
            u, v, depth = project(sensor.k, sensor.r, sensor.t,
                                  [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, 0.0])
            here = (u, v) if depth > 0 and abs(u) < 1e5 and abs(v) < 1e5 else None
            if previous and here:
                draw.line([previous, here], fill=colour, width=width)
            previous = here


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--calibration", type=Path, required=True)
    ap.add_argument("--shot", action="append", default=[], metavar="SENSOR=path",
                    help="one frame per sensor, as captured by capture_camera_shots.sh")
    ap.add_argument("--pushout", type=float, nargs="+", default=[0.0], metavar="D",
                    help="tripwire push-out distances to draw, one row each (default 0)")
    ap.add_argument("--roi-follow", choices=("none", "edge", "shift", "both"), default="both",
                    help="which ROI rule to draw (default both, for comparing them)")
    ap.add_argument("--output", type=Path, default=Path("preview.png"))
    ap.add_argument("--tile-width", type=int, default=960)
    args = ap.parse_args()

    shots = {}
    for item in args.shot:
        name, _, path = item.partition("=")
        if not name or not path:
            print(f"expected SENSOR=path (got {item!r})")
            return 1
        shots[name] = Path(path)
    if not shots:
        print("pass at least one --shot SENSOR=path")
        return 1

    calibration = json.loads(args.calibration.read_text())
    first = Image.open(next(iter(shots.values())))
    frame = first.size
    tile_w = args.tile_width
    tile_h = round(tile_w * frame[1] / frame[0])
    band_h = 74

    head, mono, big = font("DejaVuSans-Bold.ttf", 26), font("DejaVuSansMono.ttf", 15), \
        font("DejaVuSansMono-Bold.ttf", 28)

    sheet = Image.new("RGB", (len(shots) * tile_w, len(args.pushout) * (tile_h + band_h)),
                      "black")
    draw = ImageDraw.Draw(sheet)
    base_wire, _ = geometry(pushed(calibration, 0.0, "none"), next(iter(shots)))

    for row, distance in enumerate(args.pushout):
        top = row * (tile_h + band_h)
        variants = {}
        if args.roi_follow in ("edge", "both"):
            variants["edge"] = pushed(calibration, distance, "edge")
        if args.roi_follow in ("shift", "both"):
            variants["shift"] = pushed(calibration, distance, "shift")
        if args.roi_follow == "none":
            variants["none"] = pushed(calibration, distance, "none")
        reference = next(iter(variants.values()))
        wire, _ = geometry(reference, next(iter(shots)))

        draw.rectangle([0, top, sheet.size[0], top + band_h], fill=(20, 40, 70))
        draw.text((16, top + 8), f"D = {distance:.0f} m", font=head, fill="white")
        draw.text((150, top + 12), f"tripwire at {args.calibration.name}: "
                                   f"x {wire[0][0]:.3f}, y {wire[0][1]:.3f} → {wire[1][1]:.3f}",
                  font=mono, fill=(215, 230, 245))
        legend = [("tripwire at this D", WIRE), ("tripwire at D=0", WIRE_AT_ZERO)]
        legend += [("zone, edge follows (shrinks)", ROI_EDGE)] if "edge" in variants else []
        legend += [("zone, whole zone shifts", ROI_SHIFT)] if "shift" in variants else []
        x = 150
        for text, colour in legend:
            draw.line([(x, top + 50), (x + 24, top + 50)], fill=colour, width=5)
            draw.text((x + 30, top + 41), text, font=mono, fill=(215, 230, 245))
            x += 30 + int(draw.textlength(text, font=mono)) + 26

        for column, (sensor_id, shot_path) in enumerate(shots.items()):
            tile = Image.open(shot_path).convert("RGB")
            layer = ImageDraw.Draw(tile)
            sensor = Sensor(next(s for s in reference["sensors"] if s["id"] == sensor_id),
                            frame)
            polyline(layer, sensor, base_wire, WIRE_AT_ZERO, 5, False)
            for mode, colour in (("shift", ROI_SHIFT), ("edge", ROI_EDGE), ("none", ROI_EDGE)):
                if mode in variants:
                    polyline(layer, sensor, geometry(variants[mode], sensor_id)[1],
                             colour, 7, True)
            wire_here, _ = geometry(reference, sensor_id)
            polyline(layer, sensor, wire_here, WIRE, 12, False)
            layer.text((14, 14), f"{sensor_id}   tripwire in frame "
                                 f"{in_frame_percent(sensor, *wire_here):.0f}%",
                       font=big, fill="white", stroke_width=4, stroke_fill="black")
            sheet.paste(tile.resize((tile_w, tile_h)), (column * tile_w, top + band_h))

    sheet.save(args.output)
    print(f"wrote {args.output}  {sheet.size[0]}x{sheet.size[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
