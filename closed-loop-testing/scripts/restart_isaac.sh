#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Restart the Isaac Sim scenario on a LIVE SIL stack without wedging its RTSP mounts.
#
# Why this exists: Isaac Sim 6.1's in-process RTSP media fails SDP creation when
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
#                WARM_TIMEOUT_SEC (1200), DS_CONTAINER (vss-rtvi-cv, optional check),
#                SDR_CONTAINER (sdr-controller), REDIS_CONTAINER (redis),
#                DS_URL (http://localhost:9000)
set -u

ISAAC_CONTAINER="${ISAAC_CONTAINER:-isaac-sim}"
VST_CONTAINER="${VST_CONTAINER:-vss-vios-streamprocessing}"
DS_CONTAINER="${DS_CONTAINER:-vss-rtvi-cv}"
SDR_CONTAINER="${SDR_CONTAINER:-sdr-controller}"
REDIS_CONTAINER="${REDIS_CONTAINER:-redis}"
DS_URL="${DS_URL:-http://localhost:9000}"
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

# ---- 2. Mounts to warm-check, from the scenario's cameras config -------------
# The driver takes its cameras from the SCENARIO entry in scenarios.yaml, so the
# warm-check and a re-registration must read the same file.
CONFIGS="$(dirname "$0")/../isaac-sim/sil/configs"
SCENARIO=$($DOCKER exec "$ISAAC_CONTAINER" printenv SCENARIO 2>/dev/null)
CAMS_FILE=cameras.yaml
if [ -n "$SCENARIO" ] && [ -f "$CONFIGS/scenarios.yaml" ]; then
    SCENARIO_CAMS=$(awk -v s="$SCENARIO:" '$1 == s {in_s=1; next} in_s && /^[^ #]/ {in_s=0}
                    in_s && $1 == "cameras:" {print $2; exit}' "$CONFIGS/scenarios.yaml")
    [ -n "$SCENARIO_CAMS" ] && CAMS_FILE=$SCENARIO_CAMS
fi
log "cameras config: $CAMS_FILE${SCENARIO:+ (SCENARIO=$SCENARIO)}"
CAMS_YAML="$CONFIGS/$CAMS_FILE"
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

VST_URL=$($DOCKER exec "$ISAAC_CONTAINER" printenv VST_BASE_URL 2>/dev/null)

ds_vs_vst() {
    # Prints "1 <summary>" when DeepStream holds exactly the sensors VST lists
    # online, "0 <summary>" otherwise. Counting "Active sources" is not enough: a
    # zombie source bin of a deleted sensor still counts, so DeepStream can report
    # 3/3 with one camera missing.
    python3 - "$VST_URL" "$DS_URL" "$N_MOUNTS" <<'PY'
import json, sys, urllib.request
vst, ds, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
def get(url):
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.load(r)
try:
    online = {s['sensorId'] for s in get(vst + '/v1/sensor/list') if s.get('state') == 'online'}
    info = get(ds + '/api/v1/stream/get-stream-info')['stream-info']
    if isinstance(info, dict):
        info = info.get('stream-info', [])
    held = {s['camera_id'] for s in info}
except Exception as e:
    print(f"0 not readable yet ({e.__class__.__name__})")
    sys.exit()
ok = len(online) == n and held == online
print(f"{1 if ok else 0} {len(held & online)}/{n} live, "
      f"{len(held - online)} stale, {len(online - held)} missing")
PY
}

reprovision_sensors() {
    # One clean provisioning round. A restarted DeepStream comes back as an EMPTY
    # REST pipeline, but SDR replays what Redis still holds of earlier sensors, and
    # a source bin of a deleted sensor then comes back as a zombie holding a batch
    # slot. So clear Redis and restart SDR too, then register ONCE: the only
    # camera_add events left are the fresh ones.
    log "clean provisioning: FLUSHALL $REDIS_CONTAINER, restart $SDR_CONTAINER and $DS_CONTAINER..."
    $DOCKER exec "$REDIS_CONTAINER" redis-cli FLUSHALL >/dev/null
    $DOCKER restart "$SDR_CONTAINER" >/dev/null
    $DOCKER restart "$DS_CONTAINER" >/dev/null
    sleep 20
    log "one clean sensor registration round ($CAMS_FILE)..."
    $DOCKER exec "$ISAAC_CONTAINER" bash -lc \
        "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all && sleep 3 && \
         ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --add-from-config /isaac-sim/sil/configs/$CAMS_FILE" \
        2>&1 | tail -2 | while IFS= read -r l; do log "  $l"; done
}

wait_ds_matches() {
    # $1 = number of 10 s polls. Returns 0 once DeepStream holds exactly VST's sensors.
    local state
    for _ in $(seq 1 "$1"); do
        state=$(ds_vs_vst)
        [ "${state%% *}" = 1 ] && { log "DeepStream holds VST's sensors: ${state#* }"; return 0; }
        # It can also die mid-wait — catch that too instead of timing out silently.
        if ! $DOCKER ps --format '{{.Names}}' | grep -qx "$DS_CONTAINER"; then
            log "WARNING: $DS_CONTAINER died while reconnecting — recovering"
            reprovision_sensors
        fi
        sleep 10
    done
    log "DeepStream vs VST: ${state#* }"
    return 1
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
if [ -z "$VST_URL" ]; then
    log "done (VST_BASE_URL not set in $ISAAC_CONTAINER — skipped the DeepStream check)"
    exit 0
fi
# A healthy reconnect takes 10-60 s. Short of that after 2 min, DeepStream either
# restarted without us seeing it (empty pipeline) or holds a zombie of a deleted
# sensor; neither heals by waiting, so re-provision once.
log "waiting for DeepStream to hold VST's $N_MOUNTS sensors (up to 2 min)..."
wait_ds_matches 12 && exit 0
log "WARNING: DeepStream does not hold VST's sensors after 2 min — re-provisioning once"
reprovision_sensors
wait_ds_matches 18 && exit 0
log "WARNING: DeepStream still does not hold VST's sensors — see troubleshooting.md, 'Provisioning Chain Polluted'."
