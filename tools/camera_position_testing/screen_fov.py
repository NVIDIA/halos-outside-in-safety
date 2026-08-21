#!/usr/bin/env python3
"""Score a candidate camera placement for coverage, without launching Isaac Sim.

Why this exists
---------------
Finding a viable gate-mounted camera pose is a search, and the only check available today is
"launch Isaac, look at the rendered streams". That is minutes per candidate, which is too
slow to explore position x orientation x focal length. Most candidates fail for a reason
that is pure geometry and needs no renderer: the object is out of frame, or its feet are.

So this screens candidates against the derived `calibration.json` in milliseconds. It does
not replace the visual check — it removes the candidates that could never have passed it.

What "visible" means here
-------------------------
A target is modelled as a vertical segment at a floor position, from z = 0 to
`--target-height`. Two levels are reported, because they fail for different reasons and
break different things:

``foot``  the floor contact point is in frame. The 2D profile recovers world position by
          back-projecting that point through the ground homography, so without it there is
          no position at all — the detection cannot be placed in the ROI or tested against
          the tripwire.
``body``  foot *and* head are in frame, so the bounding box is not clipped by the frame
          edge. This is the level the "close object partially out of the FOV" concern is
          about, and the one predicted to fail for a person standing directly under a
          gate-mounted camera.

Since the frame is convex and a straight world segment projects to a straight image
segment, testing the two endpoints settles the whole segment.

What the tripwire push-out D does, and does not, change here
------------------------------------------------------------
The ROI coverage block cannot respond to D at all, and that is not an opinion: a tripwire is
virtual geometry, so moving it changes nothing about what a camera can see, and the cells
sampled are the ROI's, not the wire's. Sweeping `--tripwire-pushout` leaves that block
bit-identical while the ROI is held fixed. (With `--roi-follow edge` the scores *appear* to
improve, but only because shrinking the ROI away from the gate discards the cells nearest
the cameras, which are the blind ones — an artefact of measuring a smaller region.)

The wire stations do move with the wire, so the approach run and the view angle are reported
per D. The view angle is the one that responds usefully: pushing the wire out from under a
gate-mounted camera is precisely a trade of proximity for a shallower look at the crossing.

That still does not choose D. Where a shallow-enough angle becomes a detection that
behaviour analytics places correctly is a property of code outside this repo, and only an
SRR run measures it. What this narrows is which D values are worth a run.

What it does not model
----------------------
Occlusion by scene geometry (racks, pillars, the trailer, other forklifts) — everything here
is a clear line of sight to an empty floor. Blind spots caused by *objects* are exactly what
the xlsx pro/con table argues about, and they need the renderer. Detector behaviour is not
modelled either: being in frame is necessary, not sufficient. Treat every number here as an
upper bound.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

from generate_calibration import Matrix, Vector, outward_normal, transpose

APPROACH_LIMIT_M = 8.0
APPROACH_STEP_M = 0.05


def project(k: Matrix, r: Matrix, t: Vector, point: Vector) -> tuple[float, float, float]:
    """World point -> (u, v, depth). Depth <= 0 means behind the camera."""
    cam = [sum(r[i][j] * point[j] for j in range(3)) + t[i] for i in range(3)]
    depth = cam[2]
    if depth <= 1e-9:
        return math.nan, math.nan, depth
    return (k[0][0] * cam[0] + k[0][2] * cam[2]) / depth, \
           (k[1][1] * cam[1] + k[1][2] * cam[2]) / depth, depth


class Sensor:
    """A camera read back from the calibration VSS will actually consume."""

    def __init__(self, entry: dict[str, Any], fallback: tuple[int, int]) -> None:
        self.id = entry.get("id", "?")
        self.k = entry["intrinsicMatrix"]
        extrinsic = entry["extrinsicMatrix"]
        self.r = [row[:3] for row in extrinsic]
        self.t = [row[3] for row in extrinsic]
        self.centre = [-v for v in _matvec(transpose(self.r), self.t)]
        self.width = int(_attribute(entry, "frameWidth") or fallback[0])
        self.height = int(_attribute(entry, "frameHeight") or fallback[1])

    def in_frame(self, point: Vector) -> bool:
        u, v, depth = project(self.k, self.r, self.t, point)
        return depth > 0 and 0.0 <= u < self.width and 0.0 <= v < self.height

    def sees_foot(self, x: float, y: float) -> bool:
        return self.in_frame([x, y, 0.0])

    def sees_body(self, x: float, y: float, height: float) -> bool:
        return self.in_frame([x, y, 0.0]) and self.in_frame([x, y, height])


def _matvec(m: Matrix, v: Vector) -> Vector:
    return [sum(m[i][k] * v[k] for k in range(len(v))) for i in range(len(m))]


def _attribute(entry: dict[str, Any], name: str) -> str | None:
    for attr in entry.get("attributes", []):
        if attr.get("name") == name:
            return attr.get("value")
    return None


def point_in_polygon(x: float, y: float, polygon: list[dict[str, Any]]) -> bool:
    """Ray casting. Boundary cases are irrelevant at grid resolution."""
    inside = False
    count = len(polygon)
    for i in range(count):
        ax, ay = polygon[i]["x"], polygon[i]["y"]
        bx, by = polygon[(i + 1) % count]["x"], polygon[(i + 1) % count]["y"]
        if (ay > y) != (by > y) and x < ax + (y - ay) / (by - ay) * (bx - ax):
            inside = not inside
    return inside


def coverage(sensors: list[Sensor], cells: list[tuple[float, float]], height: float,
             cell_area: float) -> dict[str, Any]:
    """Coverage of `cells`, per sensor and combined, plus how redundant that coverage is.

    `sole` counts cells only one camera can see at body level. Those are the cells where a
    single occluder — a pallet stack, the trailer, another forklift — takes coverage to
    zero, and occlusion is precisely what this script cannot model. So a configuration that
    reaches 100% mostly via `sole` cells is not equivalent to one that reaches it with two
    cameras everywhere, even though both print 100%.
    """
    per_sensor = {s.id: {"foot": 0, "body": 0} for s in sensors}
    union = {"foot": 0, "body": 0, "sole": 0}
    for x, y in cells:
        any_foot = False
        body_count = 0
        for sensor in sensors:
            foot = sensor.sees_foot(x, y)
            body = foot and sensor.sees_body(x, y, height)
            per_sensor[sensor.id]["foot"] += foot
            per_sensor[sensor.id]["body"] += body
            any_foot |= foot
            body_count += body
        union["foot"] += any_foot
        union["body"] += body_count > 0
        union["sole"] += body_count == 1
    total = len(cells) or 1
    return {"total": len(cells),
            "per_sensor": {k: {"foot": 100.0 * d["foot"] / total,
                               "body": 100.0 * d["body"] / total,
                               "blind_area": (total - d["body"]) * cell_area}
                           for k, d in per_sensor.items()},
            "union": {m: 100.0 * v / total for m, v in union.items()}}


def approach_run(sensors: list[Sensor], station: tuple[float, float],
                 outward: tuple[float, float], height: float) -> float:
    """Metres of continuous body-level visibility immediately outside the tripwire.

    This is the geometric form of the requirement to push the tripwire out far
    enough "to ensure enough detection points": a target walking in towards the wire must
    stay fully in frame over the final stretch of its approach, or there are no frames in
    which the crossing can be established. Zero means the crossing is undetectable no
    matter what the detector does.
    """
    x, y = station
    dx, dy = outward
    distance = 0.0
    while distance <= APPROACH_LIMIT_M:
        px, py = x + dx * distance, y + dy * distance
        if not any(s.sees_body(px, py, height) for s in sensors):
            return distance
        distance += APPROACH_STEP_M
    return APPROACH_LIMIT_M


def wire_view_angles(sensors: list[Sensor], station: tuple[float, float]) -> list[float]:
    """Depression angle of the shallowest camera that can see the floor point here.

    Being in frame is not the whole story at a gate mount. The 2D profile recovers world
    position by back-projecting the floor contact point through the ground homography, and
    the steeper the view, the less that point behaves: at 90 degrees the target stands on
    its own contact point and occludes it, so the recovered position walks by however tall
    the target is. That is the mechanism behind the "blind spot on one side of the tripwire"
    reported from a gate mount, and the reason D exists as a knob at all.

    Sorted shallowest first, because one camera with a usable angle is enough to place the
    target. The second entry matters as much as the first though, and on a gate mount it is
    the entry that tells the truth: a station directly under one camera gets its shallow
    view from the camera at the *far* post, whose line of sight crosses the whole ROI and is
    therefore the one a forklift is most likely to block. Nothing here models occlusion, so
    read the second angle as "what is left when the comfortable view is taken away".

    This is the geometric *input* to that mechanism, not a prediction of it. Where the
    angle stops being usable is a property of behaviour analytics and the detector, and
    only an SRR run measures it. Read this as which D values are worth spending a run on.
    """
    x, y = station
    return sorted(math.degrees(math.atan2(s.centre[2], math.hypot(s.centre[0] - x,
                                                                  s.centre[1] - y)))
                  for s in sensors if s.sees_foot(x, y))


def ascii_map(sensors: list[Sensor], bounds: tuple[float, float, float, float],
              step: float, height: float, roi: list[dict[str, Any]],
              wire: dict[str, Any]) -> str:
    """Body-level coverage as a floor plan. North up, east right, as seen from above."""
    xmin, xmax, ymin, ymax = bounds
    wire_p1, wire_p2 = wire["p1"], wire["p2"]
    rows = []
    y = ymax
    while y >= ymin:
        row = []
        x = xmin
        while x <= xmax:
            near_wire = _near_segment(x, y, wire_p1, wire_p2, step * 0.7)
            covered = sum(s.sees_body(x, y, height) for s in sensors)
            on_camera = any(math.dist((x, y), (s.centre[0], s.centre[1])) < step for s in sensors)
            if on_camera:
                char = "C"
            elif near_wire:
                char = "T" if covered else "!"
            elif covered >= 2:
                char = "#"
            elif covered == 1:
                char = "+"
            elif any(s.sees_foot(x, y) for s in sensors):
                char = "-"
            elif point_in_polygon(x, y, roi):
                char = "x"
            else:
                char = " "
            row.append(char)
            x += step
        rows.append(f"  y={y:7.2f} |{''.join(row)}|")
        y -= step
    legend = ("    # body seen by 2+   + by 1   - foot only   x ROI but blind   "
              "T wire covered   ! wire blind   C camera")
    axis = f"    x from {xmin:.2f} to {xmax:.2f} m, step {step:.2f} m"
    return "\n".join([*rows, axis, legend])


def _near_segment(x: float, y: float, p1: dict[str, Any], p2: dict[str, Any],
                  tolerance: float) -> bool:
    ax, ay, bx, by = p1["x"], p1["y"], p2["x"], p2["y"]
    dx, dy = bx - ax, by - ay
    length_sq = dx * dx + dy * dy
    if length_sq < 1e-12:
        return math.dist((x, y), (ax, ay)) <= tolerance
    s = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / length_sq))
    return math.dist((x, y), (ax + s * dx, ay + s * dy)) <= tolerance


def grid_cells(polygon: list[dict[str, Any]], step: float) -> list[tuple[float, float]]:
    xs = [p["x"] for p in polygon]
    ys = [p["y"] for p in polygon]
    cells = []
    y = min(ys)
    while y <= max(ys):
        x = min(xs)
        while x <= max(xs):
            if point_in_polygon(x, y, polygon):
                cells.append((x, y))
            x += step
        y += step
    return cells


def wire_stations(wire: dict[str, Any], count: int) -> list[tuple[float, float]]:
    p1, p2 = wire["p1"], wire["p2"]
    if count == 1:
        return [((p1["x"] + p2["x"]) / 2.0, (p1["y"] + p2["y"]) / 2.0)]
    return [(p1["x"] + (p2["x"] - p1["x"]) * i / (count - 1),
             p1["y"] + (p2["y"] - p1["y"]) * i / (count - 1)) for i in range(count)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--calibration", type=Path, required=True,
                        help="calibration.json to screen, as produced by "
                             "generate_calibration.py")
    parser.add_argument("--target-height", type=float, default=1.8, metavar="M",
                        help="height of the target that must stay in frame; 1.8 for a "
                             "standing person, ~2.5 for a forklift mast (default 1.8)")
    parser.add_argument("--grid", type=float, default=0.25, metavar="M",
                        help="ROI sampling step in metres (default 0.25)")
    parser.add_argument("--stations", type=int, default=9, metavar="N",
                        help="points along the tripwire at which to measure the approach "
                             "run (default 9)")
    parser.add_argument("--map-step", type=float, default=0.0, metavar="M",
                        help="print an ASCII coverage map at this step; 0 disables it")
    parser.add_argument("--resolution", default="1920x1080", metavar="WxH",
                        help="fallback for sensors whose calibration carries no "
                             "frameWidth / frameHeight")
    args = parser.parse_args(argv)

    calibration = json.loads(args.calibration.read_text())
    entries = calibration.get("sensors") or []
    if not entries:
        raise SystemExit(f"{args.calibration} has no sensors")
    fallback = tuple(int(v) for v in args.resolution.lower().split("x"))
    sensors = [Sensor(e, fallback) for e in entries]

    geometry = next((e for e in entries if e.get("tripwires") and e.get("rois")),
                    calibration)
    wires = geometry.get("tripwires") or []
    rois = geometry.get("rois") or []
    if not wires or not rois:
        raise SystemExit("no tripwire / ROI found — screening needs both to score anything")
    wire = wires[0]["wire"]
    roi = rois[0]["roiCoordinates"]
    outward = outward_normal(wires[0])

    print(f"Screening {args.calibration}")
    print(f"  target height {args.target_height:.2f} m, ROI grid {args.grid:.2f} m, "
          f"{len(sensors)} sensor(s)\n")

    cells = grid_cells(roi, args.grid)
    scores = coverage(sensors, cells, args.target_height, args.grid * args.grid)
    stations = wire_stations(wire, args.stations)

    print(f"ROI coverage over {scores['total']} sampled cells "
          f"({scores['total'] * args.grid * args.grid:.1f} m2)")
    print(f"  {'sensor':<12} {'foot %':>8} {'body %':>8} {'blind':>9}  centre")
    for sensor in sensors:
        row = scores["per_sensor"][sensor.id]
        print(f"  {sensor.id:<12} {row['foot']:8.1f} {row['body']:8.1f} "
              f"{row['blind_area']:7.1f}m2  "
              f"({sensor.centre[0]:6.2f},{sensor.centre[1]:7.2f},{sensor.centre[2]:5.2f})")
    print(f"  {'ANY':<12} {scores['union']['foot']:8.1f} {scores['union']['body']:8.1f}")
    print(f"  of which only one camera can see: {scores['union']['sole']:.1f}% of the ROI "
          "— one occluder there and coverage is zero")

    print(f"\nTarget standing on the tripwire, and its approach run outward "
          f"({outward[0]:+.2f},{outward[1]:+.2f})")
    runs = []
    angles = []
    for x, y in stations:
        run = approach_run(sensors, (x, y), outward, args.target_height)
        runs.append(run)
        on_wire = any(s.sees_body(x, y, args.target_height) for s in sensors)
        capped = "+" if run >= APPROACH_LIMIT_M else " "
        seen_at = wire_view_angles(sensors, (x, y))
        if seen_at:
            angles.append(seen_at)
        best = f"{seen_at[0]:5.1f}" if seen_at else "  n/a"
        fallback = f"{seen_at[1]:5.1f}" if len(seen_at) > 1 else "  n/a"
        print(f"  station ({x:6.2f},{y:7.2f})  on wire {'yes' if on_wire else 'NO ':>3}   "
              f"approach run {run:5.2f}{capped} m   view {best} deg, "
              f"fallback {fallback} deg")
    print(f"  worst approach run over {len(runs)} stations: {min(runs):.2f} m")
    if angles:
        steepest_best = max(a[0] for a in angles)
        steepest_fallback = max((a[1] for a in angles if len(a) > 1), default=None)
        print(f"  steepest station is viewed at {steepest_best:.1f} deg below horizontal "
              "(90 = straight down, where the target hides its own contact point)")
        if steepest_fallback is not None:
            print(f"  with the shallowest camera occluded, the steepest station falls back "
                  f"to {steepest_fallback:.1f} deg")
    if min(runs) == 0.0:
        print("  -> at least one station is blind at the wire itself: no frames in which a "
              "crossing there could be established. Push the tripwire out further, widen "
              "the FOV, or move the cameras back.")

    if args.map_step > 0:
        xs = [p["x"] for p in roi] + [wire["p1"]["x"], wire["p2"]["x"]] + \
             [s.centre[0] for s in sensors]
        ys = [p["y"] for p in roi] + [wire["p1"]["y"], wire["p2"]["y"]] + \
             [s.centre[1] for s in sensors]
        margin = 2.0
        print("\nBody-level coverage map")
        print(ascii_map(sensors, (min(xs) - margin, max(xs) + margin,
                                  min(ys) - margin, max(ys) + margin),
                        args.map_step, args.target_height, roi, wire))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
