#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
set -e
source /opt/ros/jazzy/setup.bash

echo "🚜 Forklift Controller | Robot: ${ROBOT_ID} | RMW: ${RMW_IMPLEMENTATION}"

ARGS="--robot-id ${ROBOT_ID}"
ARGS="${ARGS} --speed ${BASE_SPEED:-1.5}"
ARGS="${ARGS} --angular-speed ${ANGULAR_SPEED:-0.4}"
ARGS="${ARGS} --heading-offset ${HEADING_OFFSET:-0}"
ARGS="${ARGS} --end-tolerance ${END_TOLERANCE:-1.0}"
ARGS="${ARGS} --end-pose-count ${END_POSE_COUNT:-5}"
ARGS="${ARGS} --spiral-timeout ${SPIRAL_TIMEOUT:-10.0}"

[ -n "${WAYPOINT_FILE}" ] && ARGS="${ARGS} --path ${WAYPOINT_FILE}"
[ "${USE_NAMESPACE}" != "true" ] && ARGS="${ARGS} --no-namespace"
[ "${LOOP_PATH}" == "true" ] && ARGS="${ARGS} --loop"
[ "${NO_INVERT}" == "true" ] && ARGS="${ARGS} --no-invert"

exec python3 /app/robot_controller.py ${ARGS} "$@"
