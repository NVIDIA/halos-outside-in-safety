#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Where this robot's run parameters come from, and in what order.

Three sources, highest first:

1. a command-line flag, for a run started by hand outside compose;
2. the robot's own block in the same `robots.yaml` Isaac was launched with,
   folded with its `models:` entry;
3. the built-in defaults below.

Resolution records which source each value came from. A truck that drives oddly
then says why in its first ten log lines, instead of costing a bisect.

The fleet file is parsed by `action_graphs.forklift_common`, the same loader
Isaac uses. One implementation of the `models:` merge, so the two sides cannot
drift into disagreeing about what a config means.
"""

from __future__ import annotations

import os
import sys
from typing import Any, Optional

# Where the Isaac script tree is mounted. The loader lives there rather than
# being copied here: a second copy is a second set of merge semantics.
ISAAC_SCRIPTS_DIR = os.environ.get("ISAAC_SCRIPTS_DIR", "/app/isaac")

DRIVE_DEFAULTS = {
    "speed": 1.5,             # m/s along the path
    "angular_speed": 0.4,     # rad/s while turning
    "heading_offset": 180.0,  # degrees; ForkliftB faces -X in its own frame
    "loop": True,             # replay forever — needs a closed path
    "no_invert": False,       # true skips the reverse leg
    "end_tolerance": 1.0,     # m from the final pose that counts as arrived
    "end_pose_count": 5,      # poses from the end where end_tolerance applies
    "spiral_timeout": 10.0,   # s circling a missed waypoint before giving up
}


def _loader():
    """The Isaac-side module both this file and the graph builders read."""
    if ISAAC_SCRIPTS_DIR not in sys.path:
        sys.path.insert(0, ISAAC_SCRIPTS_DIR)
    try:
        from action_graphs import forklift_common
    except ImportError as exc:
        raise RuntimeError(
            f"cannot import the fleet loader from {ISAAC_SCRIPTS_DIR} ({exc}). "
            f"The forklift-controller service mounts the Isaac script tree; check "
            f"that mount, or set ISAAC_SCRIPTS_DIR."
        ) from exc
    return forklift_common


def load_fleet(robots_config_path: str) -> list[dict]:
    """Every robot in the config, each already folded with its model."""
    common = _loader()
    load_and_validate_robots_yaml = common.load_and_validate_robots_yaml
    KNOWN_DRIVE_KEYS = common.KNOWN_DRIVE_KEYS
    # The loader names the knobs, this file gives them values; the two lists
    # must not drift.
    if set(KNOWN_DRIVE_KEYS) != set(DRIVE_DEFAULTS):
        raise RuntimeError(
            f"drive knobs disagree: forklift_common knows {sorted(KNOWN_DRIVE_KEYS)}, "
            f"fleet_config defaults {sorted(DRIVE_DEFAULTS)}"
        )
    robots, _ = load_and_validate_robots_yaml(robots_config_path)
    return robots


def find_robot(robots: list[dict], robot_id: str, robots_config_path: str) -> dict:
    """This robot's block. Absent is fatal, and says what the file does hold.

    Running on defaults because the name was misspelled looks exactly like
    running correctly, right up until the truck drives someone else's lane.
    """
    for robot in robots:
        if robot.get("name") == robot_id:
            return robot
    names = ", ".join(sorted(str(r.get("name")) for r in robots)) or "(none)"
    raise SystemExit(
        f"[fleet] ROBOT_ID '{robot_id}' is not in {robots_config_path}. "
        f"That file declares: {names}. ROBOT_ID must equal a robots.yaml block "
        f"name and the scene prim leaf name."
    )


def resolve_drive(cli: dict[str, Any], robot: Optional[dict]) -> tuple[dict, dict]:
    """(values, source-of-each-value) for the drive knobs."""
    fleet = (robot or {}).get("drive") or {}
    values: dict[str, Any] = {}
    sources: dict[str, str] = {}
    for key, fallback in DRIVE_DEFAULTS.items():
        if cli.get(key) is not None:
            values[key], sources[key] = cli[key], "flag"
        elif key in fleet:
            values[key], sources[key] = fleet[key], "robots.yaml"
        else:
            values[key], sources[key] = fallback, "default"
    return values, sources


def format_sources(values: dict, sources: dict) -> str:
    """One line naming every value and where it came from."""
    return " ".join(f"{k}={values[k]}({sources[k]})" for k in sorted(values))


def resolve_waypoint_file(root: str, map_id: str, robot_id: str) -> str:
    """This robot's route: `<root>/<map>/<robot>.json`.

    One directory per route set, one file per robot, named after the robot. A
    scenario that needs different routes points `waypoints:` at a different
    directory — there is no override layer to reason about.
    """
    return os.path.join(root, map_id, f"{robot_id}.json")


def resolve_topics(robot: Optional[dict], robot_id: str) -> dict:
    """The topic names this truck uses, from its own block in the fleet file.

    Isaac builds its graphs from the same two keys, so reading them rather than
    rebuilding the strings is what stops the two sides drifting. Without a fleet
    file — a run started by hand — the namespaced convention still applies.
    """
    if robot is None:
        names = {"cmd_vel": f"{robot_id}/cmd_vel", "odom": f"{robot_id}/odom"}
    else:
        common = _loader()
        names = {"cmd_vel": common.resolve_cmd_vel_topic(robot),
                 "odom": common.resolve_odom_topic(robot)}
    return {k: v if v.startswith("/") else f"/{v}" for k, v in names.items()}


def load_scenario(configs_dir: str, scenario_id: str) -> dict:
    """One entry of scenarios.yaml, read by the same parser Isaac uses."""
    return _loader().load_scenario(configs_dir, scenario_id)
