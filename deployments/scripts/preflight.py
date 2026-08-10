#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Check that a robot is wired the same way in all four places before launch.

A forklift only works if four lists agree, and they are maintained by hand in
four files that never see each other:

  1. `robots.yaml`          — Isaac builds the prim and the graphs from it
  2. the controller service — publishes that robot's cmd_vel
  3. `waypoints/<id>.json`  — the path the controller follows
  4. `COMM_ROBOT_IDS`       — tells comm-layer to mirror the mute topic

They cannot be collapsed into one file: comm-layer also runs in HIL, where
`robots.yaml` does not exist, and the controller is a container the simulator
knows nothing about. What makes the duplication dangerous is that **every way
of getting it wrong is silent**. Miss the controller and the truck stands
still. Miss the waypoint file and it stands still for a different reason. Miss
`COMM_ROBOT_IDS` and its disc sits on the alarm colour forever. Nothing logs an
error, because from inside each container nothing is wrong.

So this reads all four and says which one disagrees, before Isaac spends three
minutes booting. It answers "did I finish adding the robot", not "is the system
healthy" — it starts nothing and talks to nothing that is running.

    ./preflight.py --robots-config ../closed-loop-testing/isaac-sim/sil/configs/robots-40x20.yaml

`--robots-config` has no default on purpose. Which config Isaac is launched
with is a choice made on the command line and recorded nowhere else, so a
default here would let this check quietly pass against a file the run does not
use — the exact failure it exists to catch.

Runs on the host, not in a container: reading the controller services means
`docker compose config`, and the Isaac container has no docker socket.

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
import sys

DEPLOYMENTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.dirname(DEPLOYMENTS_DIR)
_FORKLIFT_COMMON = os.path.join(
    REPO_ROOT, "closed-loop-testing", "isaac-sim", "sil", "scripts",
    "action_graphs", "forklift_common.py",
)

ERROR, WARN = "ERROR", "warn"


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


def check(common, robots, controllers, comm_ids) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    robot_names = [r["name"] for r in robots]

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
        control = robot.get("control", {}) or {}
        if control.get("enabled") and name not in by_robot_id:
            findings.append((ERROR, (
                f"'{name}' has control enabled but no controller service. It will get a "
                f"prim, a disc and three graphs, and then stand still — nothing publishes "
                f"its cmd_vel. Add a service to forklift-controller.yml with "
                f"ROBOT_ID={name}, and put its profile in COMPOSE_PROFILES."
            )))

        indicator = robot.get("safety_indicator", {}) or {}
        if indicator.get("enabled"):
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

        waypoint = env.get("WAYPOINT_FILE")
        if not waypoint:
            findings.append((ERROR, f"{service_name} has no WAYPOINT_FILE; it has no path to follow."))
        else:
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

        # The compose file calls this out as a silent no-op, and it is: the
        # robot subscribes to <name>/cmd_vel while the controller publishes the
        # bare /cmd_vel, so both trucks would answer to one topic.
        namespaced = any(
            str((r.get("control") or {}).get("cmd_vel_topic", "")).startswith(f"{robot_id}/")
            for r in robots if r["name"] == robot_id
        )
        if namespaced and str(env.get("USE_NAMESPACE", "")).lower() != "true":
            findings.append((ERROR, (
                f"{service_name} has USE_NAMESPACE={env.get('USE_NAMESPACE')!r} while the "
                f"robots config expects {robot_id}/cmd_vel. The controller would publish the "
                f"bare /cmd_vel, which that robot is not listening to."
            )))

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
    args = parser.parse_args()

    robots_yaml = os.path.abspath(args.robots_config)
    env_file = os.path.abspath(args.env_file)
    for path in (robots_yaml, env_file):
        if not os.path.isfile(path):
            raise SystemExit(f"preflight: no such file: {path}")

    common, robots, controllers, comm_ids = collect(robots_yaml, env_file)

    print(f"robots config : {robots_yaml}")
    print(f"env file      : {env_file}")
    print(f"robots        : {', '.join(r['name'] for r in robots) or '<none>'}")
    listed = ", ".join(
        f"{name}({svc['environment']['ROBOT_ID']})"
        for name, svc in sorted(controllers.items())
    )
    print(f"controllers   : {listed or '<none>'}")
    print(f"COMM_ROBOT_IDS: {', '.join(comm_ids) or '<empty>'}")
    print()

    findings = check(common, robots, controllers, comm_ids)
    if not findings:
        print("OK — all four lists agree.")
        return 0

    for level, message in findings:
        print(f"[{level}] {message}\n")
    errors = sum(1 for level, _ in findings if level == ERROR)
    print(f"{errors} error(s), {len(findings) - errors} warning(s).")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
