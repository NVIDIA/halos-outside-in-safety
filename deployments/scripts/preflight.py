#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Check that a robot is wired the same way everywhere before launch.

A forklift only works if four lists agree, and they are maintained by hand in
four files that never see each other:

  1. `robots.yaml`          — Isaac builds the prim and the graphs from it
  2. the controller service — publishes that robot's cmd_vel
  3. `waypoints/<id>.json`  — the path the controller follows
  4. `COMM_ROBOT_IDS`       — tells comm-layer to mirror the mute topic

Two more decide whether a passing run means anything, and are not checked here:
`cameras.yaml` places the sensors, and VSS's `calibration.json` states where
those sensors think they are. A scene paired with another scene's calibration
produces frames, detections and PSF verdicts, all of them about the wrong
warehouse. That pair is asserted by hand — see the scene<->calibration table in
skills/hoisa-deploy-profile/references/test_scenario.md.

The four cannot be collapsed into one file: comm-layer also runs in HIL, where
`robots.yaml` does not exist, and the controller is a container the simulator
knows nothing about. What makes the duplication dangerous is that **every way
of getting it wrong is silent**. Miss the controller and the truck stands
still. Miss the waypoint file and it stands still for a different reason. Miss
`COMM_ROBOT_IDS` and its disc sits on the alarm colour forever. Nothing logs an
error, because from inside each container nothing is wrong.

So this reads all four and says which one disagrees, before Isaac spends three
minutes booting. It answers "did I finish adding the robot", not "is the system
healthy" — it starts nothing and talks to nothing that is running.

    ./preflight.py --robots-config ../closed-loop-testing/isaac-sim/sil/configs/robots-40x20.yaml \
        --scene ../closed-loop-testing/isaac-sim/sil/scenes/warehouse_40x20_two_loading_dock.usd

Names are checked, and so is the one number that has actually broken: a
waypoint file's `origin` against where the truck stands. The controller
subtracts that origin from every world pose, so a file from another scene stays
"valid" and the truck simply drives a lane nobody validated. For a robot with a
`spawn:` block both numbers are in configs this script already reads; for a
truck baked into the scene USD the pose is in the scene, which is what
`--scene` is for.

`--robots-config` has no default on purpose. Which config Isaac is launched
with is a choice made on the command line and recorded nowhere else, so a
default here would let this check quietly pass against a file the run does not
use — the exact failure it exists to catch.

Runs on the host, not in a container: reading the controller services means
`docker compose config`, and the Isaac container has no docker socket. The host
usually has no `pxr`, so a baked pose that cannot be read is reported as a
warning rather than skipped silently.

Not checked here: whether a robot's prim exists. `forklift_overlay.py` creates
it from the same `spawn:` block, and refuses at launch when the scene already
has one, so that pair cannot drift.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import yaml
import sys

DEPLOYMENTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.dirname(DEPLOYMENTS_DIR)
_FORKLIFT_COMMON = os.path.join(
    REPO_ROOT, "closed-loop-testing", "isaac-sim", "sil", "scripts",
    "action_graphs", "forklift_common.py",
)

ERROR, WARN = "ERROR", "warn"

# Below this, a mismatch is rounding in a hand-authored config rather than a
# retargeting mistake. The two real ones found by hand in this branch were 1.0 m
# and 4.63 m out, so the threshold does not need to be tight to catch them.
ORIGIN_TOLERANCE_M = 0.05


def _load_forklift_common():
    """Import the Isaac-side helpers by path.

    Imported rather than reimplemented so the mute-topic rule has one
    definition. A second copy here would drift from the builder's, and this
    check would then confirm a wiring that does not exist.
    """
    spec = importlib.util.spec_from_file_location("forklift_common", _FORKLIFT_COMMON)
    if spec is None or spec.loader is None:
        raise SystemExit(f"preflight: cannot import {_FORKLIFT_COMMON}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _compose_config(env_file: str) -> dict:
    """The compose config as it would actually start, profiles applied."""
    try:
        out = subprocess.run(
            ["docker", "compose", "--env-file", env_file, "config", "--format", "json"],
            cwd=DEPLOYMENTS_DIR, capture_output=True, text=True, timeout=120,
        )
    except FileNotFoundError:
        raise SystemExit("preflight: docker not found; this check runs on the host")
    if out.returncode != 0:
        raise SystemExit(
            f"preflight: `docker compose config` failed for {env_file}:\n{out.stderr.strip()}"
        )
    return json.loads(out.stdout)


def _host_path_of(service: dict, container_path: str) -> str | None:
    """Where a path inside the container lives on the host, via its mounts."""
    best = None
    for volume in service.get("volumes", []):
        target, source = volume.get("target"), volume.get("source")
        if not target or not source:
            continue
        if container_path == target or container_path.startswith(target.rstrip("/") + "/"):
            # Longest matching target wins, the way the kernel resolves mounts.
            if best is None or len(target) > len(best[0]):
                best = (target, source)
    if best is None:
        return None
    target, source = best
    return os.path.join(source, os.path.relpath(container_path, target))


def _waypoint_origin(path: str):
    """((x, y), None) the controller subtracts from this file, or (None, why not)."""
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        return None, f"cannot be read as JSON ({e})"
    origin = data.get("origin")
    if not isinstance(origin, dict):
        return None, "has no 'origin' block"
    try:
        return (float(origin["world_x"]), float(origin["world_y"])), None
    except (KeyError, TypeError, ValueError):
        return None, f"has a malformed 'origin': {origin!r}"


def _spawn_position(robot: dict):
    """((x, y), None) where a `spawn:` block puts this truck, or (None, why not)."""
    position = (robot.get("spawn") or {}).get("position")
    if not isinstance(position, (list, tuple)) or len(position) < 2:
        return None, f"spawn.position is not [x, y, z]: {position!r}"
    try:
        return (float(position[0]), float(position[1])), None
    except (TypeError, ValueError):
        return None, f"spawn.position is not numeric: {position!r}"


def _baked_positions(scene_path: str, prim_paths: list[str]):
    """({prim_path: (x, y)}, None) read from the scene, or ({}, why not).

    `Usd.Stage.LoadNone` keeps this cheap: the truck's translate is authored in the
    scene's own layer, so none of the payloads behind it need to open. The host
    normally has no USD Python at all, which is a reason to warn rather than to
    silently treat the pose as agreeing.
    """
    try:
        from pxr import Gf, Usd, UsdGeom
    except ImportError:
        return {}, ("no `pxr` on the host, so the baked spawn poses cannot be read. "
                    "Compare the scene's xformOp:translate against the waypoint "
                    "origins by hand, or run this where USD Python is available")
    try:
        stage = Usd.Stage.Open(scene_path, Usd.Stage.LoadNone)
    except Exception as e:  # noqa: BLE001 - USD raises several unrelated types
        return {}, f"cannot open {scene_path} as a stage ({e})"
    if stage is None:
        return {}, f"cannot open {scene_path} as a stage"

    out = {}
    for prim_path in prim_paths:
        prim = stage.GetPrimAtPath(prim_path)
        if not prim or not prim.IsValid():
            continue
        matrix = UsdGeom.Xformable(prim).GetLocalTransformation()
        translation = Gf.Transform(matrix).GetTranslation()
        out[prim_path] = (float(translation[0]), float(translation[1]))
    return out, None


def collect(robots_yaml: str, env_file: str):
    common = _load_forklift_common()
    robots, _ = common.load_and_validate_robots_yaml(robots_yaml)
    config = _compose_config(env_file)
    services = config.get("services", {})

    # Any service carrying ROBOT_ID is a controller, rather than matching on
    # the service name: a third truck's service will be named by whoever adds
    # it, and this check exists precisely for the moment someone adds one.
    controllers = {
        name: svc for name, svc in services.items()
        if "ROBOT_ID" in (svc.get("environment") or {})
    }
    comm_ids: list[str] = []
    for svc in services.values():
        raw = (svc.get("environment") or {}).get("ROS_ROBOT_IDS")
        if raw:
            comm_ids = [x.strip() for x in raw.split(",") if x.strip()]
            break
    return common, robots, controllers, comm_ids


def _scenario_value(svc, key):
    """One key of the scenario this service names, or None.

    The scenario is what a deployment actually sets; WAYPOINTS_MAP and
    ROBOTS_CONFIG exist only to override it for a one-off run. Reading it here
    keeps the check looking at what the container will do rather than at the
    variables that happen to be spelled out.
    """
    scenario_id = (svc["environment"].get("SCENARIO") or "").strip()
    if not scenario_id:
        return None
    # Through the mount first, so an unusual layout is honoured; then the repo
    # this script lives in, because a profile env still carrying its placeholder
    # paths is the normal state of a fresh checkout.
    candidates = []
    host_dir = _host_path_of(svc, "/app/robots")
    if host_dir:
        candidates.append(os.path.join(host_dir, "scenarios.yaml"))
    candidates.append(os.path.join(
        REPO_ROOT, "closed-loop-testing", "isaac-sim", "sil", "configs",
        "scenarios.yaml"))
    path = next((c for c in candidates if os.path.isfile(c)), None)
    if path is None:
        return None
    with open(path) as handle:
        return ((yaml.safe_load(handle) or {}).get(scenario_id) or {}).get(key)


def _check_origin(service_name, robot, waypoint, host_path, baked, baked_reason):
    """Findings for one controller's waypoint origin against where its truck stands."""
    name = robot["name"]
    origin, why = _waypoint_origin(host_path)
    if origin is None:
        return [(WARN, f"{service_name}: {waypoint} {why}, so its origin cannot be "
                       f"checked against '{name}'.")]

    if "spawn" in robot:
        pose, why = _spawn_position(robot)
        source = "spawn.position in the robots config"
        if pose is None:
            return [(ERROR, f"'{name}' {why} — the overlay will refuse this at launch.")]
    else:
        pose = baked.get(robot["articulation_prim"])
        source = f"the baked translate at {robot['articulation_prim']}"
        if pose is None:
            reason = baked_reason or (
                f"{robot['articulation_prim']} is not in the scene — it is neither "
                f"baked nor declared with a spawn: block"
            )
            return [(WARN, f"'{name}': cannot compare {waypoint}'s origin "
                           f"({origin[0]}, {origin[1]}) against the scene: {reason}.")]

    dx, dy = origin[0] - pose[0], origin[1] - pose[1]
    delta = (dx * dx + dy * dy) ** 0.5
    if delta <= ORIGIN_TOLERANCE_M:
        return []
    return [(ERROR, (
        f"'{name}' stands at ({pose[0]}, {pose[1]}) per {source}, but {waypoint} "
        f"declares origin ({origin[0]}, {origin[1]}) — {delta:.3f} m apart "
        f"(dx={dx:+.3f}, dy={dy:+.3f}). The controller subtracts the origin from "
        f"every world pose, so the truck drives the right shape in the wrong place "
        f"and nothing logs it. This is what a waypoint set from another scene looks "
        f"like: point the scenario's `waypoints:` at this warehouse."
    ))]


def check(common, robots, controllers, comm_ids, scene_path=None) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    robot_names = [r["name"] for r in robots]
    by_name = {r["name"]: r for r in robots}

    # One stage open for the whole fleet, and only when a robot is actually baked.
    baked: dict[str, tuple[float, float]] = {}
    baked_reason = None
    baked_robots = [r for r in robots if "spawn" not in r]
    if baked_robots:
        if scene_path is None:
            baked_reason = (
                "no --scene was given, and these trucks are baked into the scene USD "
                "rather than declared with a spawn: block"
            )
        else:
            baked, baked_reason = _baked_positions(
                scene_path, [r["articulation_prim"] for r in baked_robots]
            )

    by_robot_id: dict[str, list[str]] = {}
    for service_name, svc in controllers.items():
        by_robot_id.setdefault(svc["environment"]["ROBOT_ID"], []).append(service_name)

    for robot_id, service_names in sorted(by_robot_id.items()):
        if len(service_names) > 1:
            findings.append((ERROR, (
                f"{', '.join(sorted(service_names))} all drive '{robot_id}'. Two "
                f"controllers publishing one cmd_vel fight each other, and the truck "
                f"moves in a way neither waypoint file explains."
            )))
        if robot_id not in robot_names:
            findings.append((ERROR, (
                f"controller {service_names[0]} drives '{robot_id}', which is not in "
                f"the robots config. Isaac builds no graph for it, so its cmd_vel goes "
                f"nowhere — rename it, or add the robot."
            )))

    for robot in robots:
        name = robot["name"]
        if common.is_section_enabled(robot, "control") and name not in by_robot_id:
            findings.append((ERROR, (
                f"'{name}' has control enabled but no controller service. It will get a "
                f"prim, a disc and three graphs, and then stand still — nothing publishes "
                f"its cmd_vel. Add a service to forklift-controller.yml with "
                f"ROBOT_ID={name}, and put its profile in COMPOSE_PROFILES."
            )))

        if common.is_section_enabled(robot, "safety_indicator"):
            topic = common.resolve_muted_topic(robot)
            # A scene naming the global topic opted out of the mirrors and needs
            # no entry; only a derived per-robot topic depends on the list.
            if topic != "/safety/is_muted" and name not in comm_ids:
                findings.append((ERROR, (
                    f"'{name}' listens on {topic}, which comm-layer is not publishing: "
                    f"COMM_ROBOT_IDS is {', '.join(comm_ids) or '<empty>'}. Its disc will "
                    f"sit on the alarm colour for the whole run with nothing in any log. "
                    f"Add {name} to COMM_ROBOT_IDS in the env file."
                )))

    for name in comm_ids:
        if name not in robot_names:
            findings.append((WARN, (
                f"COMM_ROBOT_IDS lists '{name}', which this robots config does not have. "
                f"Harmless — a topic with no subscriber — and expected if the list is "
                f"shared across scenes."
            )))

    for service_name, svc in sorted(controllers.items()):
        env = svc["environment"]
        robot_id = env["ROBOT_ID"]

        # WAYPOINT_FILE is an explicit override. Normally the container derives
        # the path from the map and its own ROBOT_ID, so resolve it the same way.
        waypoint = env.get("WAYPOINT_FILE")
        if not waypoint:
            map_id = env.get("WAYPOINTS_MAP") or _scenario_value(svc, "waypoints")
            if not map_id:
                findings.append((ERROR, (
                    f"{service_name} has neither WAYPOINT_FILE nor WAYPOINTS_MAP; "
                    f"it has no path to follow."
                )))
            else:
                waypoint = f"/app/waypoints/{map_id}/{robot_id}.json"
        if waypoint:
            host_path = _host_path_of(svc, waypoint)
            if host_path is None:
                findings.append((WARN, (
                    f"{service_name}: cannot tell where {waypoint} comes from — no volume "
                    f"mounts it. Check by hand that the file reaches the container."
                )))
            elif not os.path.isfile(host_path):
                findings.append((ERROR, (
                    f"{service_name} ({robot_id}) points at {waypoint}, which does not "
                    f"exist on the host at {host_path}. The controller starts and the "
                    f"truck never moves."
                )))
            elif robot_id in by_name:
                findings += _check_origin(
                    service_name, by_name[robot_id], waypoint, host_path,
                    baked, baked_reason,
                )

    return findings


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check a robot is wired the same in robots.yaml, the controller "
                    "service, its waypoint file and COMM_ROBOT_IDS.")
    parser.add_argument("--robots-config", required=True,
                        help="the robots.yaml Isaac will be launched with (no default: "
                             "checking the wrong one is the failure this catches)")
    parser.add_argument("--env-file", default=os.path.join(DEPLOYMENTS_DIR, "profiles", "sil.env"),
                        help="deployment profile passed to docker compose (default: profiles/sil.env)")
    parser.add_argument("--scene",
                        help="the scene USD, needed only to read the pose of a truck "
                             "baked into it rather than declared with a spawn: block")
    args = parser.parse_args()

    robots_yaml = os.path.abspath(args.robots_config)
    env_file = os.path.abspath(args.env_file)
    scene_path = os.path.abspath(args.scene) if args.scene else None
    for path in (robots_yaml, env_file, *(p for p in (scene_path,) if p)):
        if not os.path.isfile(path):
            raise SystemExit(f"preflight: no such file: {path}")

    common, robots, controllers, comm_ids = collect(robots_yaml, env_file)

    print(f"robots config : {robots_yaml}")
    print(f"env file      : {env_file}")
    print(f"scene         : {scene_path or '<not given>'}")
    print(f"robots        : {', '.join(r['name'] for r in robots) or '<none>'}")
    listed = ", ".join(
        f"{name}({svc['environment']['ROBOT_ID']})"
        for name, svc in sorted(controllers.items())
    )
    print(f"controllers   : {listed or '<none>'}")
    print(f"COMM_ROBOT_IDS: {', '.join(comm_ids) or '<empty>'}")
    print()

    findings = check(common, robots, controllers, comm_ids, scene_path)
    if not findings:
        print("OK — all four lists agree, and every waypoint origin matches its truck.")
        return 0

    for level, message in findings:
        print(f"[{level}] {message}\n")
    errors = sum(1 for level, _ in findings if level == ERROR)
    print(f"{errors} error(s), {len(findings) - errors} warning(s).")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
