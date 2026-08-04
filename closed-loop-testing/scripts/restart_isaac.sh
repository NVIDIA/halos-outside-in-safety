#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Restart the Isaac Sim scenario on a LIVE SIL stack without wedging its RTSP mounts.
#
# Why this exists: Isaac Sim 6.0's in-process RTSP media fails SDP creation when
# >=2 clients DESCRIBE a stream during its cold bind window (right after Play,
# before the first frame). A live VST holds two such clients per mount (the
# proxy ingest client + the liveness prober) and reconnects within milliseconds
# of the port opening, so restarting Isaac under a running VST wedges mounts
# ("has no caps", no self-recovery). Pausing the VST streamprocessing container
# for the boot window avoids the race deterministically. The sensor registry
# (vss-vios-sensor + its database) stays up throughout, so the driver's normal
# registration at render-warm proceeds: it deletes and re-adds the cameras
# (fresh sensor uuids each restart) and SDR re-provisions DeepStream.
#
# Usage (from the host):
#   ./restart_isaac.sh                     # relaunch with the running driver's own args
#   ./restart_isaac.sh -c /isaac-sim/... --start --headless --enable-vst ...   # explicit args
#
# Env overrides: ISAAC_CONTAINER (isaac-sim), VST_CONTAINER (vss-vios-streamprocessing),
#                WARM_TIMEOUT_SEC (1200), DS_CONTAINER (vss-rtvi-cv, optional check)
set -u

ISAAC_CONTAINER="${ISAAC_CONTAINER:-isaac-sim}"
VST_CONTAINER="${VST_CONTAINER:-vss-vios-streamprocessing}"
DS_CONTAINER="${DS_CONTAINER:-vss-rtvi-cv}"
WARM_TIMEOUT_SEC="${WARM_TIMEOUT_SEC:-1200}"

DOCKER="docker"
$DOCKER ps >/dev/null 2>&1 || DOCKER="sudo docker"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# ---- 1. Resolve the driver command ------------------------------------------
if [ "$#" -gt 0 ]; then
    DRIVER_ARGS="$*"
else
    # Read args from the python process itself (not the bash -lc wrapper, whose
    # argv carries the whole quoted command including shell redirections).
    DRIVER_ARGS=$($DOCKER exec "$ISAAC_CONTAINER" bash -c \
        "ps -eo args | grep 'run_actor_sdg.py' | grep -v grep | grep -v 'bash' | head -1" \
        | sed -E 's|^.*run_actor_sdg\.py ||; s| *[0-9]?>.*$||')
    if [ -z "$DRIVER_ARGS" ]; then
        echo "ERROR: no running run_actor_sdg.py to copy args from — pass the driver args explicitly." >&2
        exit 1
    fi
fi
log "driver args: $DRIVER_ARGS"

# ---- 2. Mounts to warm-check, from cameras.yaml if present -------------------
CAMS_YAML="$(dirname "$0")/../isaac-sim/sil/configs/cameras.yaml"
MOUNTS=""
if [ -f "$CAMS_YAML" ]; then
    MOUNTS=$(awk '/^\s*port:/ {p=$2} /^\s*mount_path:/ {print p $2}' "$CAMS_YAML")
fi
[ -n "$MOUNTS" ] || MOUNTS=$'8554/camera\n8555/camera_01\n8556/camera_02'
N_MOUNTS=$(echo "$MOUNTS" | wc -l | tr -d ' ')
log "will wait for $N_MOUNTS mount(s) to deliver"

# ---- 3. Pause VST (kills the DESCRIBE churn; registry persists in the DB) ----
log "stopping $VST_CONTAINER for the Isaac boot window..."
$DOCKER stop "$VST_CONTAINER" >/dev/null

# ---- 4. Restart the driver ---------------------------------------------------
$DOCKER exec "$ISAAC_CONTAINER" bash -c "pgrep -f run_actor_sdg.py | xargs -r kill -9" || true
sleep 5
TS=$(date +%Y%m%d-%H%M%S)
$DOCKER exec -d "$ISAAC_CONTAINER" bash -lc \
    "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/run_actor_sdg.py $DRIVER_ARGS > /tmp/run-restart-$TS.log 2>&1"
log "driver relaunched (log: /tmp/run-restart-$TS.log in the container)"

# ---- 5. Wait until every mount delivers frames (encoder warm) ----------------
log "waiting for mounts to warm (timeout ${WARM_TIMEOUT_SEC}s; first boot compiles shaders)..."
DEADLINE=$(( $(date +%s) + WARM_TIMEOUT_SEC ))
while :; do
    GOOD=0
    while IFS= read -r m; do
        $DOCKER exec "$ISAAC_CONTAINER" timeout 6 ffprobe -v error -rtsp_transport tcp \
            -show_entries stream=codec_name -of csv "rtsp://localhost:$m" >/dev/null 2>&1 \
            && GOOD=$((GOOD + 1))
    done <<< "$MOUNTS"
    [ "$GOOD" -eq "$N_MOUNTS" ] && break
    if [ "$(date +%s)" -ge "$DEADLINE" ]; then
        log "ERROR: only $GOOD/$N_MOUNTS mounts delivering after ${WARM_TIMEOUT_SEC}s."
        log "Restarting $VST_CONTAINER anyway so the stack is not left half-paused."
        $DOCKER start "$VST_CONTAINER" >/dev/null
        exit 1
    fi
    sleep 10
done
log "all $N_MOUNTS mounts delivering — encoder warm"

# ---- 6. Resume VST: its clients now DESCRIBE warm mounts ----------------------
$DOCKER start "$VST_CONTAINER" >/dev/null
log "$VST_CONTAINER restarted"

# ---- 7. DeepStream: recover it if the source drain crashed it, then wait -----
# Draining a live DeepStream's sources to zero (e.g. the driver's own
# delete-all at re-registration, or organic sensor churn) can
# trigger a known intermittent DeepStream abort (std::logic_error /
# "basic_string: construction from null", exit 134). A (re)started DeepStream
# comes back as an EMPTY REST pipeline and nothing replays its sources, so a
# bare `docker start` is not enough — the sensors must be re-registered to emit
# fresh camera_add events, which SDR then pushes into DeepStream (measured:
# 3/3 in ~10 s).

ds_active() {
    # Only trust "Active sources" lines from the CURRENT container run:
    # `docker logs --tail` happily serves pre-restart values.
    local started
    started=$($DOCKER inspect -f '{{.State.StartedAt}}' "$DS_CONTAINER" 2>/dev/null) || { echo 0; return; }
    $DOCKER logs --since "$started" "$DS_CONTAINER" 2>&1 \
        | grep -a "Active sources" | tail -1 | grep -oE '[0-9]+' | tail -1
}

reprovision_sensors() {
    # Deterministic recovery: a (re)started DeepStream always comes back as an
    # EMPTY REST pipeline (all slots freed), so restart it first, then run ONE
    # clean registration round — fresh camera_add events that SDR pushes into
    # the empty pipeline. Never re-register into a possibly-polluted pipeline:
    # the per-source :9000 purges are best-effort only (provisioned urls drift
    # across streamprocessing restarts), and leftovers steal batch slots.
    log "restarting $DS_CONTAINER (guarantees an empty pipeline)..."
    $DOCKER restart "$DS_CONTAINER" >/dev/null
    sleep 15
    log "one clean sensor registration round..."
    $DOCKER exec "$ISAAC_CONTAINER" bash -lc \
        "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all && sleep 3 && \
         ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --add-from-config /isaac-sim/sil/configs/cameras.yaml" \
        2>&1 | tail -2 | while IFS= read -r l; do log "  $l"; done
}

if ! $DOCKER ps --format '{{.Names}}' | grep -qx "$DS_CONTAINER"; then
    if $DOCKER ps -a --format '{{.Names}}' | grep -qx "$DS_CONTAINER"; then
        log "WARNING: $DS_CONTAINER is not running (known DeepStream abort when its sources drain) — recovering"
        reprovision_sensors
    else
        log "done ($DS_CONTAINER not deployed — skipped the DeepStream check)"
        exit 0
    fi
fi
log "waiting for DeepStream to reconnect (up to 5 min)..."
REPROVISIONED_LATE=0
for _ in $(seq 1 30); do
    ACTIVE=$(ds_active)
    [ "${ACTIVE:-0}" -eq "$N_MOUNTS" ] && { log "DeepStream back at $ACTIVE/$N_MOUNTS active sources"; exit 0; }
    # It can also die mid-wait — catch that too instead of timing out silently.
    if ! $DOCKER ps --format '{{.Names}}' | grep -qx "$DS_CONTAINER"; then
        log "WARNING: $DS_CONTAINER died while reconnecting — recovering"
        reprovision_sensors
    fi
    sleep 10
done
# Still short: it may have restarted (empty pipeline) without us seeing the
# death, e.g. Docker's on-failure policy revived it. Re-provision once.
if [ "$REPROVISIONED_LATE" -eq 0 ]; then
    log "WARNING: DeepStream not at $N_MOUNTS/$N_MOUNTS after 5 min — re-provisioning once"
    REPROVISIONED_LATE=1
    reprovision_sensors
    for _ in $(seq 1 18); do
        ACTIVE=$(ds_active)
        [ "${ACTIVE:-0}" -eq "$N_MOUNTS" ] && { log "DeepStream back at $ACTIVE/$N_MOUNTS active sources"; exit 0; }
        sleep 10
    done
fi
log "WARNING: DeepStream not at $N_MOUNTS/$N_MOUNTS yet — check '$DS_CONTAINER' logs."
