#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
set -e
source /opt/ros/jazzy/setup.bash

echo "🚜 Forklift Controller | Robot: ${ROBOT_ID} | RMW: ${RMW_IMPLEMENTATION}"

# FORKLIFT_WAYPOINTS_DIR used to name one warehouse's directory and be mounted
# as the whole waypoints root. The tree is now mounted whole and the warehouse
# picked inside, so an old value would nest one level too deep and the truck
# would drive a lane nobody validated — the exact silent failure this pairing
# has always risked. Refuse instead.
if [ -n "${FORKLIFT_WAYPOINTS_DIR:-}" ]; then
    echo "ERROR: FORKLIFT_WAYPOINTS_DIR is no longer used." >&2
    echo "  Set WAYPOINTS_MAP=<map id> instead, e.g. WAYPOINTS_MAP=warehouse_40x20." >&2
    echo "  The whole waypoints tree is mounted; the map is selected in the container." >&2
    exit 1
fi

ARGS="--robot-id ${ROBOT_ID}"

# The scenario names the fleet file and the map; robots.yaml then says how each
# truck drives. Drive knobs are not forwarded from the environment: one env var
# would set every truck in the fleet at once.
[ -n "${SCENARIO:-}" ]      && ARGS="${ARGS} --scenario ${SCENARIO}"
[ -n "${WAYPOINTS_MAP:-}" ] && ARGS="${ARGS} --map ${WAYPOINTS_MAP}"
# One file for this container, set by docker_run.sh. Safe here because a
# `docker run` env is per container; compose does not set it, where one
# value would hand every truck in the fleet the same route.
[ -n "${WAYPOINT_FILE:-}" ] && ARGS="${ARGS} --path ${WAYPOINT_FILE}"

exec python3 /app/robot_controller.py ${ARGS} "$@"
