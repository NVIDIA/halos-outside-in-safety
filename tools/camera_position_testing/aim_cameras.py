#!/usr/bin/env python3
"""Write a candidate `cameras.yaml` from mount points and what they should look at.

Why this exists
---------------
Searching for a camera placement means asking "put a camera on the gate post, 4.5 m up,
pointing at the middle of the safety zone" and seeing what that covers. `cameras.yaml`
does not accept that question: it stores `rotation_yxz_deg`, a USD `xformOp:rotateYXZ`
triple. Hand-converting an aim point into one of those, per camera, per iteration, is
both slow and the easiest place in this whole loop to make a silent mistake — a sign
error aims the camera at the ceiling and the coverage number that comes back is a real
number for the wrong scene.

So this is the inverse of the rotation convention `generate_calibration.py` consumes,
wrapped in the loop it is actually used in: place cameras, aim them, emit a config that
Isaac and the calibration generator both read.

The convention, and why it is inverted this way
-----------------------------------------------
USD `rotateYXZ` composes as ``Rz @ Rx @ Ry`` on column vectors, and a USD camera looks
down its own -Z. With ry = 0 (no roll — a rolled safety camera has no purpose here), the
world-space direction the camera faces is::

    forward = Rz(rz) @ Rx(rx) @ (0, 0, -1) = (-sin rz * sin rx,  cos rz * sin rx,  -cos rx)

which inverts, for a wanted unit `forward`, to::

    rx = acos(-forward.z)          depression from straight up, 90 deg = horizontal
    rz = atan2(-forward.x, forward.y)

Every emitted pose is checked by composing it forward again and comparing against the
requested direction, because the cost of that check is nothing and the cost of a wrong
sign is a day of screening the wrong geometry.

Scope
-----
This writes camera *poses*. It does not touch `calibration.json` — that is
`generate_calibration.py`, which must be re-run against the file this produces, or VSS
keeps back-projecting through the poses the cameras used to have. `screen_fov.py` then
scores the result without launching Isaac.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from typing import Any

import yaml

Vector = list[float]

# Anything below this and "which way is it facing" stops having an answer: a camera
# pointing straight down has no yaw, and one at its own aim point has no direction at all.
MIN_AIM_DISTANCE_M = 0.05

# A convention error — a dropped sign, Rx and Rz swapped — misses by tens of degrees, so
# this only has to be tight enough to separate that from arithmetic noise.
ROUNDTRIP_TOLERANCE_DEG = 1e-4


def unit(v: Vector) -> Vector:
    n = math.dist(v, [0.0, 0.0, 0.0])
    return [c / n for c in v]


def aim_to_rotation(position: Vector, target: Vector) -> Vector:
    """Return the `rotation_yxz_deg` that points a USD camera at `target`."""
    delta = [t - p for t, p in zip(target, position)]
    if math.dist(delta, [0.0, 0.0, 0.0]) < MIN_AIM_DISTANCE_M:
        raise SystemExit(f"aim point {target} is on top of the camera at {position}")

    fx, fy, fz = unit(delta)
    rx = math.degrees(math.acos(max(-1.0, min(1.0, -fz))))
    rz = math.degrees(math.atan2(-fx, fy))
    return [rx, 0.0, rz]


def rotation_to_aim(rotation_yxz_deg: Vector) -> Vector:
    """Forward direction of a camera at this rotation — the inverse of aim_to_rotation."""
    rx, _ry, rz = (math.radians(a) for a in rotation_yxz_deg)
    return [-math.sin(rz) * math.sin(rx), math.cos(rz) * math.sin(rx), -math.cos(rx)]


def angle_between_deg(a: Vector, b: Vector) -> float:
    """Angle via atan2(|a x b|, a.b) — acos of the dot product loses most of its
    significant digits exactly where this is used, on two nearly identical directions."""
    cross = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
    return math.degrees(math.atan2(math.dist(cross, [0.0, 0.0, 0.0]),
                                   sum(x * y for x, y in zip(a, b))))


def check_roundtrip(name: str, position: Vector, target: Vector, rotation: Vector) -> None:
    wanted = unit([t - p for t, p in zip(target, position)])
    error_deg = angle_between_deg(wanted, rotation_to_aim(rotation))
    if error_deg > ROUNDTRIP_TOLERANCE_DEG:
        raise SystemExit(f"{name}: composing the emitted rotation gives a direction "
                         f"{error_deg:.3e} deg off the aim point — convention mismatch")


def parse_point(text: str, default_z: float | None = None) -> Vector:
    parts = [p for p in text.replace(" ", "").split(",") if p]
    if len(parts) == 2 and default_z is not None:
        parts.append(str(default_z))
    if len(parts) != 3:
        raise SystemExit(f"expected x,y,z (got {text!r})")
    try:
        return [float(p) for p in parts]
    except ValueError:
        raise SystemExit(f"non-numeric coordinate in {text!r}") from None


def parse_assignment(text: str, default_z: float | None = None) -> tuple[str, Vector]:
    name, _, point = text.partition("=")
    if not name or not point:
        raise SystemExit(f"expected NAME=x,y,z (got {text!r})")
    return name, parse_point(point, default_z)


def emit_yaml(cameras: list[dict[str, Any]], scene: Any, rtsp: dict[str, Any],
              header: list[str]) -> str:
    """Write the config by hand rather than yaml.dump, so the provenance survives.

    A candidate config is worthless a week later if it does not say what it was trying
    and what has to happen before it means anything — and yaml.dump drops comments.
    """
    out = [*header, "", "cameras:"]
    for cam in cameras:
        spawn = cam["spawn"]
        aim = cam["_aim"]
        out += [
            f"  - name: {cam['name']}",
            f"    camera_prim: {cam['camera_prim']}",
            f"    port: {cam['port']}",
            f"    mount_path: {cam['mount_path']}",
            f"    # aimed at ({aim[0]:.3f}, {aim[1]:.3f}, {aim[2]:.3f}), "
            f"{cam['_range']:.2f} m away, {cam['_depression']:.1f} deg below horizontal",
            "    spawn:",
            "      position: [{:.9f}, {:.9f}, {:.9f}]".format(*spawn["position"]),
            "      rotation_yxz_deg: [{:.6f}, {:.6f}, {:.6f}]".format(
                *spawn["rotation_yxz_deg"]),
            f"      focal_length: {spawn['focal_length']}",
            f"      horizontal_aperture: {spawn['horizontal_aperture']}",
            f"      vertical_aperture: {spawn['vertical_aperture']}",
            "",
        ]

    if scene is not None:
        out += ["# Scenes these world-space poses are measured in, checked by",
                "# camera_loader.assert_scene_matches() against the launch's stage.",
                "scene:"]
        out += [f"  - {s}" for s in (scene if isinstance(scene, list) else [scene])]
        out.append("")

    out += ["rtsp:", f"  host: {rtsp.get('host', '${HOST_IP}')}", ""]
    return "\n".join(out)


def build(args: argparse.Namespace) -> str:
    template = yaml.safe_load(Path(args.template).read_text())
    by_name = {c["name"]: c for c in template.get("cameras", [])}

    mounts = dict(parse_assignment(m) for m in args.mount)
    aims = dict(parse_assignment(a, default_z=args.aim_z) for a in args.aim_for)
    shared_aim = parse_point(args.aim, default_z=args.aim_z) if args.aim else None

    unknown = set(mounts) - set(by_name)
    if unknown:
        raise SystemExit(f"{args.template} has no camera named {', '.join(sorted(unknown))} "
                         f"(has: {', '.join(by_name)})")

    kept = [name for name in by_name if name not in args.drop]
    if not kept:
        raise SystemExit("every camera was dropped — nothing to write")

    cameras = []
    for name in kept:
        cam = dict(by_name[name])
        spawn = dict(cam.get("spawn") or {})
        if not spawn:
            raise SystemExit(f"{name} has no spawn block in the template to build on")

        position = mounts.get(name, [float(v) for v in spawn["position"]])
        target = aims.get(name, shared_aim)
        if target is None:
            raise SystemExit(f"nothing to aim {name} at — pass --aim or --aim-for {name}=x,y")

        rotation = aim_to_rotation(position, target)
        check_roundtrip(name, position, target, rotation)

        spawn["position"] = position
        spawn["rotation_yxz_deg"] = rotation
        if args.focal is not None:
            spawn["focal_length"] = args.focal
        cam["spawn"] = spawn
        cam["_aim"] = target
        cam["_range"] = math.dist(position, target)
        cam["_depression"] = 90.0 - rotation[0]
        cameras.append(cam)

    hfov = 2 * math.degrees(math.atan(
        float(cameras[0]["spawn"]["horizontal_aperture"]) / 2
        / float(cameras[0]["spawn"]["focal_length"])))

    header = [
        "# CANDIDATE camera placement — generated by tools/calibration-generator/aim_cameras.py",
        "#",
        f"# {args.label}" if args.label else "# (no label given)",
        "#",
        f"# Derived from {Path(args.template).name} by moving the cameras listed below and",
        f"# aiming each one at a point on the floor. focal_length "
        f"{cameras[0]['spawn']['focal_length']} mm is {hfov:.1f} deg horizontal FOV.",
        "#",
        "# This file describes poses ONLY. Before it means anything downstream:",
        "#   1. generate_calibration.py --cameras-config <this file> --template <shipped",
        "#      calibration.json>   — otherwise VSS back-projects through the old poses and",
        "#      every world position it reports is wrong while looking healthy.",
        "#   2. screen_fov.py --calibration <that output>   — coverage, before burning a run.",
        "#   3. recreate (not restart) the VSS perception and safety-core containers.",
    ]
    if args.drop:
        header += ["#",
                   f"# Dropped from the template: {', '.join(sorted(args.drop))}. Running with",
                   "# fewer cameras than the template also needs NUM_STREAMS, sensor_config.conf,",
                   "# the SENSORS default in tw_split.py and the sensor list in calibration.json",
                   "# (generate_calibration.py --drop-missing-sensors) to agree."]

    return emit_yaml(cameras, template.get("scene"), template.get("rtsp", {}), header)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--template", required=True,
                        help="cameras.yaml to take names, ports, prims and apertures from")
    parser.add_argument("--mount", action="append", default=[], metavar="NAME=x,y,z",
                        help="move this camera here (repeatable); unlisted cameras keep "
                             "their template position and are only re-aimed")
    parser.add_argument("--aim", metavar="x,y[,z]",
                        help="point every camera at this world point")
    parser.add_argument("--aim-for", action="append", default=[], metavar="NAME=x,y[,z]",
                        help="aim one camera somewhere else (repeatable), e.g. a "
                             "diagonal arrangement where each camera covers the far side")
    parser.add_argument("--aim-z", type=float, default=0.0,
                        help="z of an aim point given as x,y — 0.0 is the floor (default), "
                             "1.0 aims at torso height instead")
    parser.add_argument("--focal", type=float,
                        help="focal length in mm for every camera (default: keep template's)")
    parser.add_argument("--drop", action="append", default=[], metavar="NAME",
                        help="leave this camera out entirely (repeatable)")
    parser.add_argument("--label", help="one line saying what this candidate is testing")
    parser.add_argument("--output", default="-",
                        help="where to write the config ('-' for stdout, the default)")
    args = parser.parse_args()

    text = build(args)
    if args.output == "-":
        sys.stdout.write(text)
    else:
        Path(args.output).write_text(text)
        print(f"wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
