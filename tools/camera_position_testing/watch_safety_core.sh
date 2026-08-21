#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Report when Safety Core stops deciding, for as long as a test is recording.
#
# Usage: ./watch_safety_core.sh [seconds-between-samples]
#
# Safety Core can stop publishing mute/unmute decisions mid-recording while every other
# signal stays healthy: behaviour-analytics events keep arriving, the PSS daemon keeps
# heartbeating, /safety/is_muted keeps answering. The recording then scores an undecided
# system against ground truth that expects mute, and the report reads as a bad camera
# placement. It has happened three times here, once for 96 % of a 20-minute scenario.
#
# run_srr_2cam.sh watches this itself and re-records the window. This is the standalone
# version for a test driven by hand, or for a run already in flight.
set -uo pipefail

INTERVAL="${1:-30}"
STALL_S="${STALL_S:-120}"

# The newest decision line, not a count of them: when the decision path stalls the container
# keeps logging received events, so a count over the last N lines still looks alive while the
# timestamp stops moving. Log timestamps are UTC.
age() {
  local ts
  ts=$(docker logs --tail 400 safety-core 2>&1 | grep -a 'Sending decision command' | tail -1 \
       | sed -n 's/^\[\([0-9-]\{10\} [0-9:]\{8\}\).*/\1/p')
  if [ -n "$ts" ]; then
    printf '%s\n' "$(( $(date +%s) - $(date -d "$ts UTC" +%s) ))"
  else
    printf '99999\n'
  fi
}

stalled=0
while true; do
  a="$(age)"
  if [ "$a" -gt "$STALL_S" ]; then
    stalled=$((stalled + 1))
    printf '[%s] safety-core has not decided anything for %ss (sample %d) — this take is junk\n' \
      "$(date +%H:%M:%S)" "$a" "$stalled"
  else
    [ "$stalled" -gt 0 ] && printf '[%s] safety-core is deciding again\n' "$(date +%H:%M:%S)"
    stalled=0
  fi
  sleep "$INTERVAL"
done
