#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Grab one frame per camera for a candidate placement, without running a full test.
#
# Usage: ./capture_camera_shots.sh <cameras.yaml> [output-dir]
#
# This is the step that cannot be skipped. The offline screener (screen_fov.py) models a
# clear line of sight to an empty floor, so it cannot see pipework, cages or the panel above
# the dock: in the shipped scene it ranked y = -9.5 best of everything tried while the render
# showed wall structures covering 5 % of the zone at foot level. Every offline number is an
# upper bound until a frame confirms it.
#
# Isaac is launched without VST, so this does not disturb any registration state, but it does
# take over the isaac-sim container — do not run it while a test is recording.
set -uo pipefail

YAML="${1:?usage: capture_camera_shots.sh <cameras.yaml> [output-dir]}"
OUT="${2:-./shots}"
WARMUP="${WARMUP:-150}"

THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOISA_ROOT_PATH="${HOISA_ROOT_PATH:-$(cd "$THIS_DIR/../.." && pwd)}"
CFG_DIR="$HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil/configs"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# Ports and mount paths come from the config itself, so a candidate with a different camera
# count needs no arguments here.
mounts() {
  python3 - "$YAML" <<'PY'
import sys, yaml
cams = yaml.safe_load(open(sys.argv[1]))["cameras"]
for c in cams:
    print(f"{c['name']} {c['port']}/{str(c['mount_path']).lstrip('/')}")
PY
}

mkdir -p "$OUT"
log "installing $(basename "$YAML") as the live cameras.yaml"
cp "$YAML" "$CFG_DIR/cameras.yaml" || exit 1

docker stop vss-vios-streamprocessing >/dev/null 2>&1
docker exec isaac-sim bash -lc \
  "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all" \
  >/dev/null 2>&1
docker exec isaac-sim bash -lc "pgrep -f run_actor_sdg.py | xargs -r kill -9" >/dev/null 2>&1
sleep 5

log "launching Isaac headless"
docker exec -d isaac-sim bash -lc "cd /isaac-sim && ./python.sh \
  /isaac-sim/sil/scripts/run_actor_sdg.py -c /isaac-sim/sil/configs/default_config_ros.yaml \
  --start --headless --cameras-config /isaac-sim/sil/configs/cameras.yaml \
  --srr-gt --postprocessing-preset baseline > /tmp/isaac-shots.log 2>&1"

# Do not poll the RTSP mounts while the render warms up. Isaac 6.0 self-hosts one RTSP
# server per camera and answers DESCRIBE before the encoder has produced a frame; a client
# arriving in that window gets "stream has no caps" and then keeps the media wedged by
# re-asking, so the probe meant to detect readiness is what prevents it. Wait, then ask once.
log "warming the render silently for ${WARMUP}s"
sleep "$WARMUP"

rc=0
while read -r name mount; do
  [ -n "$name" ] || continue
  if docker exec isaac-sim bash -lc \
      "ffmpeg -v error -rtsp_transport tcp -i rtsp://localhost:$mount -frames:v 1 /tmp/shot.png -y" \
      2>&1 | tail -1 | grep -q .; then
    log "  $name: no frame from rtsp://localhost:$mount"
    rc=1
    continue
  fi
  docker cp "isaac-sim:/tmp/shot.png" "$OUT/$name.png" >/dev/null 2>&1 \
    && log "  wrote $OUT/$name.png" || { log "  $name: copy failed"; rc=1; }
done < <(mounts)

log "done — look for structures covering the zone, and check the tripwire is framed end to end"
exit "$rc"
