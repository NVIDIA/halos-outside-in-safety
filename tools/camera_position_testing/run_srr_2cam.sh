#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Unattended SRR scenario runner. Defaults to a 2-camera deployment; set SENSORS
# and MOUNTS together for any other camera count (see below).
#
# Why this exists next to run_multi.sh: run_multi hardcodes cameras.yaml and the
# 8554-8556 RTSP wait, and neither it nor
# restart_isaac.sh restarts sdr-controller. On this host that one omission is
# what breaks every run: sdr-controller replays camera_status_change events from
# its Redis stream, so after a DeepStream restart it re-provisions sensor uuids
# that no longer exist. With NUM_STREAMS=3 those dead entries (typically a
# long-gone Camera_02) occupy all three source slots and the two live cameras
# never get provisioned — DeepStream sits at 0 active sources with a full PERF
# table reading 0.00000. Restarting sdr-controller together with DeepStream
# drops the backlog so only the fresh registrations land.
#
# The second trap this encodes: /opt/storage holds only the ONNX, no .engine, so
# every DeepStream restart rebuilds the TensorRT engine and publishes nothing for
# several minutes. "Active sources : 2" appears well before inference is alive,
# so the gate below requires a non-zero FPS per stream, and recording never opens
# on an ungated pipeline (a run that starts blind writes 5 minutes of nulls).
#
# Usage:
#   ./run_srr_2cam.sh                      # psf-edge at 300 s
#   ./run_srr_2cam.sh psf-edge balanced:600
#
# Results land in the run dir below, alongside the scenarios already recorded,
# so the aggregator rolls all of them into one cross-scenario summary.
set -uo pipefail

# Paths derive from where this script sits — tools/camera_position_testing/ -> repo root — so
# a fresh clone needs no editing. VSS is assumed to be a sibling checkout of the Halos repo.
THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOISA_ROOT_PATH="${HOISA_ROOT_PATH:-$(cd "$THIS_DIR/../.." && pwd)}"
VSS_ROOT="${VSS_ROOT:-$(dirname "$HOISA_ROOT_PATH")/video-search-and-summarization}"
export HOISA_ROOT_PATH
export CALIBRATION_JSON="${CALIBRATION_JSON:-$VSS_ROOT/deploy/docker/industry-profiles/warehouse-operations/warehouse-2d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic/calibration.json}"
export SENSORS="${SENSORS:-Camera,Camera_01}"
# Perception topic the readiness gate samples: the 2D profile publishes per-camera
# detections to mdx-raw, the 3D (sparse4d) profile publishes fused BEV frames to
# mdx-bev instead. Both carry the same protobuf frame, so only the name changes.
PERCEPTION_TOPIC="${PERCEPTION_TOPIC:-mdx-raw}"
# Seconds to let DeepStream come back up before re-registering the cameras. The
# 2D engine loads in well under 30 s; sparse4d needs roughly 90, and registering
# while its REST server is still down leaves the source slots empty.
REPROVISION_WARMUP="${REPROVISION_WARMUP:-30}"
# Seconds to let VST close and register the recording chunk covering the window
# just recorded, before asking it for clip MP4s. See the note in analyze().
VIDEO_FLUSH_WAIT="${VIDEO_FLUSH_WAIT:-90}"
# This host's LAN address: the RTSP URLs Isaac publishes and the Kafka broker both carry it,
# and it changes per host and per reboot. deployments/profiles/sil.env holds it on a deployed
# host, so read it from there when the caller has not exported one.
PROFILE_ENV="${PROFILE_ENV:-$HOISA_ROOT_PATH/deployments/profiles/sil.env}"
if [ -z "${HOST_IP:-}" ] && [ -f "$PROFILE_ENV" ]; then
  HOST_IP="$(grep -E '^HOST_IP=' "$PROFILE_ENV" | tail -1 | cut -d= -f2)"
fi
HOST_IP="${HOST_IP:?HOST_IP not set and none found in $PROFILE_ENV}"
export KAFKA_BROKERS="${KAFKA_BROKERS:-$HOST_IP:9092}"
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-0}"

# One run dir per invocation unless the caller is adding scenarios to an existing one.
RUN_BASE="${RUN_BASE:-multi-test-$(date +%Y%m%d-%H%M%S)}"
CFG_DIR="$HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil/configs"
SRR_DIR="$HOISA_ROOT_PATH/closed-loop-testing/regression-reporter/srr-service"
# snapshot_pss.sh belongs to the reporter, not to this toolkit, and stays where it is.
SCRIPT_DIR="$HOISA_ROOT_PATH/closed-loop-testing/regression-reporter/scripts"
RUNS_HOST="$SRR_DIR/runs"
CAMS_YAML=/isaac-sim/sil/configs/cameras.yaml
# Isaac mounts: port/mount_path pairs the cameras.yaml above declares, in the same
# order as SENSORS. Override both together when running a different camera count,
# e.g. the stock 3-camera config:
#   SENSORS=Camera,Camera_01,Camera_02 \
#   MOUNTS="8554/camera 8555/camera_01 8556/camera_02" ./run_srr_2cam.sh ...
MOUNTS="${MOUNTS:-8554/camera 8555/camera_01}"
EXPECTED_STREAMS=$(printf '%s\n' $MOUNTS | wc -l)
RECORD_DEFAULT=300

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }

# ---------------------------------------------------------------- perception --
# FPS per stream from the newest PERF block. Prints one "name=fps" per line.
ds_fps() {
  # One PERF block carries one line per stream; keep the last three blocks.
  local keep=$((3 * EXPECTED_STREAMS))
  docker logs --tail 200 vss-rtvi-cv 2>&1 \
    | grep -a -A$((EXPECTED_STREAMS + 4)) 'PERF' | grep -aE 'stream_name' | tail -"$keep" \
    | awk '{print $NF"="$1}'
}

# True only when EVERY camera in SENSORS reports a non-zero FPS. Checking the
# names explicitly matters: a stale registration often occupies a slot, and taking
# "the last N PERF lines" can read that phantom instead of a real camera.
# "Active sources" is not trusted at all — it goes green while the TensorRT
# engine is still building and nothing is being inferred yet.
ds_healthy() {
  local out name
  out="$(ds_fps)"
  for name in ${SENSORS//,/ }; do
    printf '%s\n' "$out" | grep -qE "^${name}=(0*[1-9][0-9]*\.|0\.[0-9]*[1-9])" || return 1
  done
  return 0
}

# Camera name bound to each DeepStream source slot, one "slot=name" per line.
ds_slots() {
  docker exec vss-rtvi-cv curl -s --max-time 8 \
    http://localhost:9000/api/v1/stream/get-stream-info 2>/dev/null \
  | python3 -c "
import json,sys
try: info=json.load(sys.stdin)['stream-info']['stream-info']
except Exception: sys.exit(0)
for s in sorted(info, key=lambda s: s['source_id']): print(f\"{s['source_id']}={s['camera_name']}\")"
}

# Every sensor must hold exactly one slot. Two slots can end up bound to the SAME
# camera after enough re-registration churn, and nothing downstream complains:
# stream count, names and FPS all look right, while sparse4d fuses two copies of
# one view against two different poses and publishes empty BEV frames forever.
ds_slots_distinct() {
  local slots names
  slots="$(ds_slots)"
  names="$(printf '%s\n' "$slots" | cut -d= -f2 | sort)"
  [ "$(printf '%s\n' "$names" | wc -l)" -eq "$EXPECTED_STREAMS" ] || return 1
  [ "$(printf '%s\n' "$names" | uniq | wc -l)" -eq "$EXPECTED_STREAMS" ] || return 1
  return 0
}

# Every camera we intend to record must own at least one source slot.
ds_slots_cover_sensors() {
  local slots name
  slots="$(ds_slots | cut -d= -f2)"
  for name in ${SENSORS//,/ }; do
    printf '%s\n' "$slots" | grep -qx "$name" || return 1
  done
  return 0
}

# How strict the slot map has to be depends on what consumes it. sparse4d fuses the
# slots into one BEV, so two slots holding the same camera feed it the same image
# under two poses and it stops detecting anything — worth a reprovision. The 2D
# detector runs each slot independently, and the deployment keeps NUM_STREAMS=3 from
# the shipped 3-camera profile, so a leftover third slot duplicating a camera is
# routine and harmless; failing the run over it just throws away good scenarios.
ds_slots_ok() {
  case "$PERCEPTION_TOPIC" in
    mdx-bev) ds_slots_distinct ;;
    *)       ds_slots_cover_sensors ;;
  esac
}

# Restart DeepStream *and* sdr-controller (drops the stale event backlog), then
# one clean VST registration round so only the current cameras get provisioned.
reprovision() {
  # The sensor add/remove history lives in a Redis stream that sdr-controller
  # replays on restart, stale entries included; those replays are what steal
  # slots and produce the duplicate binding above. Flush before restarting so
  # the only events in the stream are the ones this round creates.
  log "  reprovision: flushing sensor event backlog (redis)"
  docker exec redis redis-cli FLUSHALL >/dev/null 2>&1
  log "  reprovision: restarting vss-rtvi-cv + sdr-controller"
  docker restart sdr-controller >/dev/null 2>&1
  docker restart vss-rtvi-cv >/dev/null 2>&1
  sleep "$REPROVISION_WARMUP"
  log "  reprovision: one clean VST registration round"
  docker exec isaac-sim bash -lc \
    "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all >/dev/null 2>&1; sleep 3; \
     ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --add-from-config $CAMS_YAML" 2>&1 | tail -2
}

# Poll for live inference. The TensorRT rebuild dominates this wait.
wait_perception() {
  local timeout="${1:-600}" waited=0
  while [ "$waited" -lt "$timeout" ]; do
    if ds_healthy; then
      log "  perception live: $(ds_fps | tr '\n' ' ')"
      return 0
    fi
    sleep 20; waited=$((waited + 20))
    [ $((waited % 120)) -eq 0 ] && log "  waiting on perception (T+${waited}s): $(ds_fps | tr '\n' ' ' )"
  done
  return 1
}

# ---------------------------------------------------------------------- isaac --
isaac_driver_running() {
  [ "$(docker exec isaac-sim bash -lc 'pgrep -cf run_actor_sdg.py' 2>/dev/null || echo 0)" -gt 0 ]
}

# Pause VST for Isaac's RTSP boot window, relaunch the driver, wait for both
# mounts to actually deliver, then let VST back in. Isaac 6.0 fails to create
# the SDP if VST probes a stream that has no caps yet, and never recovers.
start_isaac() {
  local label="$1" log_path="/tmp/isaac-$label-$(date +%Y%m%d-%H%M%S).log"
  log "  stopping vss-vios-streamprocessing for the boot window"
  docker stop vss-vios-streamprocessing >/dev/null 2>&1
  docker exec isaac-sim bash -lc "pgrep -f run_actor_sdg.py | xargs -r kill -9" >/dev/null 2>&1
  sleep 5
  log "  launching Isaac (log in container: $log_path)"
  docker exec -d isaac-sim bash -lc "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/run_actor_sdg.py \
    -c /isaac-sim/sil/configs/default_config_ros.yaml \
    --start --headless --enable-vst \
    --cameras-config $CAMS_YAML \
    --srr-gt --postprocessing-preset baseline > $log_path 2>&1"

  local waited=0 ok
  while [ "$waited" -lt 1200 ]; do
    ok=0
    for m in $MOUNTS; do
      docker exec isaac-sim timeout 6 ffprobe -v error -rtsp_transport tcp \
        "rtsp://localhost:$m" >/dev/null 2>&1 && ok=$((ok + 1))
    done
    [ "$ok" -ge "$EXPECTED_STREAMS" ] && { log "  all $EXPECTED_STREAMS mounts delivering after ${waited}s"; break; }
    sleep 20; waited=$((waited + 20))
  done
  [ "$ok" -ge "$EXPECTED_STREAMS" ] || { docker start vss-vios-streamprocessing >/dev/null 2>&1; return 1; }
  docker start vss-vios-streamprocessing >/dev/null 2>&1
  log "  vss-vios-streamprocessing back up"
  sleep 10
}

# ----------------------------------------------------------------- srr gates --
gt_publishers() {
  docker exec srr bash -lc "source /opt/ros/*/setup.bash 2>/dev/null; \
    for t in /gt/forklift/tf /gt/character_0/tf /gt/character_1/tf /gt/character_2/tf; do \
      timeout 8 ros2 topic info \$t 2>/dev/null | grep -c 'Publisher count: [1-9]'; done" 2>/dev/null \
    | awk '{s+=$1} END {print s+0}'
}

mdx_raw_detections() {
  docker exec srr python3 -c "
from kafka import KafkaConsumer
from srr.kafka_consumer import parse_mdx_bev_bytes
c=KafkaConsumer('$PERCEPTION_TOPIC',bootstrap_servers='$KAFKA_BROKERS',auto_offset_reset='latest',
                value_deserializer=None,consumer_timeout_ms=15000)
n=t=0
for m in c:
    t+=1
    if (parse_mdx_bev_bytes(m.value).get('detections') or []): n+=1
    if n>=2 or t>=400: break
c.close(); print(n)" 2>/dev/null | tail -1
}

# --------------------------------------------------------------------- phases --
select_trees() {
  local name="$1" i
  for i in 0 1 2; do
    [ -f "$CFG_DIR/srr_${name}_char${i}.bt.json" ] || die "missing tree srr_${name}_char${i}.bt.json"
    cp -f "$CFG_DIR/srr_${name}_char${i}.bt.json" "$CFG_DIR/srr_char${i}.bt.json"
  done
  for i in 0 1 2; do
    diff -q "$CFG_DIR/srr_char${i}.bt.json" "$CFG_DIR/srr_${name}_char${i}.bt.json" >/dev/null \
      || die "tree $i did not take"
  done
  log "  behavior trees → $name"
}

set_sim_duration() {
  local secs=$(( $1 + 30 + 1800 ))
  sed -i "s|^\(\s*\)simulation_duration:.*|\1simulation_duration: ${secs}.0|" \
    "$CFG_DIR/default_config_ros.yaml"
  grep -q "simulation_duration: ${secs}.0" "$CFG_DIR/default_config_ros.yaml" \
    || die "simulation_duration sed did not match"
  log "  simulation_duration=${secs}s"
}

restart_srr_stack() {
  docker restart safety-core >/dev/null 2>&1
  ( cd "$SRR_DIR" && docker compose down >/dev/null 2>&1 && docker compose up -d >/dev/null 2>&1 )
  sleep 12
  local s
  s=$(docker exec srr env 2>/dev/null | grep '^SENSORS=' | cut -d= -f2)
  [ "$s" = "$SENSORS" ] || die "srr came up with SENSORS=$s (wanted $SENSORS)"
  log "  safety-core + srr restarted (SENSORS=$s)"
}

# Latest FPS per camera, one line "<name> <fps>" — the newest PERF block only.
ds_fps_latest() {
  docker logs --tail 200 vss-rtvi-cv 2>&1 \
    | grep -a -A$((EXPECTED_STREAMS + 4)) 'PERF' | grep -aE 'stream_name' \
    | tail -"$EXPECTED_STREAMS" | awk '{print $NF, $1}'
}

# Cameras with no live source at all, by best fps across the slots they occupy.
#
# Per-slot zeroes are not per-camera death. The deployment runs NUM_STREAMS=3 against two
# registered cameras, and a spare slot sometimes binds to a camera that already has one
# ("slots: 0=Camera_01 1=Camera_01 2=Camera"). The spare carries no frames and sits at 0
# fps for the whole run while both cameras feed normally. Reading the slot instead of the
# camera aborts a perfectly good 20-minute take on a phantom.
ds_dead_cameras() {
  ds_fps_latest | awk '{ if (!($1 in best) || $2 > best[$1]) best[$1] = $2 }
                       END { for (name in best) if (best[name] + 0 == 0) print name }'
}

# Seconds since Safety Core last published a mute/unmute decision, 99999 if it never has.
#
# Reads the newest decision line rather than counting lines: when the decision path stalls,
# the container keeps logging received events and the PSS daemon keeps heartbeating, so a
# count over the last N lines still looks alive while the timestamp stops moving. Log
# timestamps are UTC.
psf_decision_age() {
  local ts
  ts=$(docker logs --tail 400 safety-core 2>&1 | grep -a 'Sending decision command' | tail -1 \
       | sed -n 's/^\[\([0-9-]\{10\} [0-9:]\{8\}\).*/\1/p')
  if [ -n "$ts" ]; then
    printf '%s\n' "$(( $(date +%s) - $(date -d "$ts UTC" +%s) ))"
  else
    printf '99999\n'
  fi
}

record_window() {
  local secs="$1" out elapsed=0 dead strikes=0 age psf_strikes=0
  out=$(docker exec srr bash -lc \
    "source /opt/ros/*/setup.bash && ros2 service call /srr/record std_srvs/srv/SetBool '{data: true}' 2>&1")
  printf '%s' "$out" | grep -q 'recording →' || die "record start not confirmed: $out"
  log "  recording started (${secs}s)"
  while [ "$elapsed" -lt "$secs" ]; do
    sleep 60; elapsed=$((elapsed + 60))
    log "    T+${elapsed}/${secs}s  fps: $(ds_fps | tr '\n' ' ')"

    # A camera can stop feeding mid-window and the recording carries on regardless:
    # one 1200 s scenario kept going for 16 minutes after a camera hit 0 fps, and the
    # clips it produced look like a placement that misses half its tripwire crossings
    # rather than the half-blind rig it actually was. Two consecutive dead samples is
    # not a hiccup, so stop the take instead of banking unusable footage.
    dead="$(ds_dead_cameras | tr '\n' ' ')"
    if [ -n "${dead// /}" ]; then
      strikes=$((strikes + 1))
      log "    no frames from: $dead (strike $strikes/2)"
      if [ "$strikes" -ge 2 ]; then
        docker exec srr bash -lc \
          "source /opt/ros/*/setup.bash && ros2 service call /srr/record std_srvs/srv/SetBool '{data: false}'" \
          >/dev/null 2>&1
        log "    aborting this take: a camera dropped out"
        return 1
      fi
    else
      strikes=0
    fi

    # Safety Core can stop deciding mid-window while every other signal stays healthy. It
    # goes on receiving BA events and publishing heartbeats, so nothing upstream complains,
    # and the take keeps scoring an unmuted system against ground truth that expects mute.
    # Three scenarios were lost that way, one deaf for 96% of its 20 minutes. In a healthy
    # take decisions arrive at ~30 Hz, so two minutes of silence is a stall, not a lull.
    # Not checked in the first two minutes: safety-core has just restarted and has nothing
    # to decide about until the forklift and the actors are moving.
    if [ "$elapsed" -ge 120 ]; then
      age="$(psf_decision_age)"
      if [ "$age" -gt 120 ]; then
        psf_strikes=$((psf_strikes + 1))
        log "    safety-core has not decided anything for ${age}s (strike $psf_strikes/2)"
        if [ "$psf_strikes" -ge 2 ]; then
          docker exec srr bash -lc \
            "source /opt/ros/*/setup.bash && ros2 service call /srr/record std_srvs/srv/SetBool '{data: false}'" \
            >/dev/null 2>&1
          log "    aborting this take: safety-core stopped deciding"
          return 2
        fi
      else
        psf_strikes=0
      fi
    fi
  done
  docker exec srr bash -lc \
    "source /opt/ros/*/setup.bash && ros2 service call /srr/record std_srvs/srv/SetBool '{data: false}'" 2>&1 | tail -1
}

analyze() {
  local label="$1" secs="$2" rows
  mkdir -p "$RUNS_HOST/$RUN_BASE/$label"
  docker exec srr bash -c "mv \$(ls -t /app/runs/run-*.parquet | head -1) /app/runs/$RUN_BASE/$label/" \
    || die "no parquet to move"
  rows=$(docker exec srr python3 -c "
import pandas as pd, glob
p=sorted(glob.glob('/app/runs/$RUN_BASE/$label/run-*.parquet'))[-1]
print(len(pd.read_parquet(p)))" 2>/dev/null | tail -1)
  log "  parquet rows: $rows (expect ~$((secs * 30)))"
  [ "${rows:-0}" -lt $((secs * 15)) ] && log "  WARN row count is less than half of expected — recording was interrupted"

  bash "$SCRIPT_DIR/snapshot_pss.sh" "$RUNS_HOST/$RUN_BASE/$label" --per-scn 2>&1 | tail -1
  bash "$SCRIPT_DIR/snapshot_pss.sh" "$RUNS_HOST/$RUN_BASE" 2>&1 | tail -1

  docker exec srr bash -c "cd /app && python3 -m srr.tw_split \
    /app/runs/$RUN_BASE/$label/run-*.parquet /app/runs/$RUN_BASE/$label/scenes --source gt" 2>&1 | tail -2

  # Videos before the aggregator, not after: the per-clip table only links an
  # MP4 that already exists on disk when the report is written, so pulling
  # afterwards leaves every video cell of this scenario empty until some later
  # run happens to regenerate the summary.
  #
  # VST commits a recording chunk to postgres only when the chunk closes, which
  # trails the end of the window by up to a minute. Asking earlier returns 404
  # for every scene even though the footage lands on disk moments later, so wait
  # before the pull rather than debug an empty video column afterwards.
  log "  waiting ${VIDEO_FLUSH_WAIT}s for VST to commit the recording chunk"
  sleep "$VIDEO_FLUSH_WAIT"
  docker exec srr bash -c "cd /app && python3 -m srr.utils.vst_video split-run \
    --run-dir /app/runs/$RUN_BASE/$label --out-dir /app/runs/$RUN_BASE/$label/videos" 2>&1 | tail -3
  local nvid
  nvid=$(ls "$RUNS_HOST/$RUN_BASE/$label/videos"/scn_*.mp4 2>/dev/null | wc -l)
  log "  videos pulled: $nvid"
  # A camera name maps to several VST stream ids once sensors have been
  # re-registered, and only the newest holds live footage. Zero clips here means
  # the pull resolved to the wrong id; recover with the id whose timeline covers
  # this run: vst_video list, then split-run --stream-id <id>.
  [ "$nvid" -eq 0 ] && log "  WARN no videos: check 'vst_video list' for the stream id covering this window"

  docker exec srr bash -c "cd /app && python3 -m srr.aggregator \
    --runs-dir /app/runs/$RUN_BASE --top-level" 2>&1 | tail -3
}

run_scenario() {
  local label="$1" secs="$2" attempt
  log "=== scenario $label (${secs}s) ==="
  select_trees "$label"
  set_sim_duration "$secs"
  restart_srr_stack
  start_isaac "$label" || die "$label: Isaac mounts never came up"

  for attempt in 1 2 3; do
    if wait_perception 420; then break; fi
    log "  perception not live (attempt $attempt/3) — reprovisioning"
    reprovision
    if wait_perception 600; then break; fi
    [ "$attempt" -eq 3 ] && die "$label: perception never produced FPS; NOT recording"
  done

  # Live FPS is not proof that the detector sees anything. Restarting Isaac hands
  # DeepStream a new pair of RTSP sessions, and on the 3D profile the frames then
  # keep flowing at full rate while sparse4d publishes empty frames forever — the
  # camera projections it fused at start no longer belong to these sources. Only
  # bouncing perception re-binds them, so treat an empty topic as a reprovision
  # trigger rather than a fatal error.
  local dets gts try
  gts=$(gt_publishers)
  for try in 1 2 3; do
    if ds_slots_ok; then
      dets=$(mdx_raw_detections)
      log "  gate: $PERCEPTION_TOPIC detections=$dets, gt publishers=$gts/4, slots: $(ds_slots | tr '\n' ' ') (try $try/3)"
      [ "${dets:-0}" -ge 1 ] && break
    else
      dets=0
      log "  gate: source slots do not cover every camera: $(ds_slots | tr '\n' ' ') (try $try/3)"
    fi
    [ "$try" -eq 3 ] && die "$label: $PERCEPTION_TOPIC has no decoded detections; NOT recording"
    log "  frames flow but nothing is detected — reprovisioning perception"
    reprovision
    wait_perception 600 || die "$label: perception never came back after reprovision"
  done
  [ "${gts:-0}" -ge 4 ] || die "$label: only $gts/4 GT publishers; NOT recording"

  log "  safety-core warm-up 30s"
  sleep 30
  record_window "$secs"
  case $? in
    0) ;;
    2)  # Safety Core went deaf. Bouncing perception does not help; the decision path is
        # what died, so restart safety-core and srr and take the window again.
      log "  safety-core stopped deciding mid-take — restarting safety-core + srr, one retry"
      restart_srr_stack
      sleep 30
      record_window "$secs" \
        || die "$label: safety-core stopped deciding twice; refusing to bank undecided footage"
      ;;
    *)
      log "  a camera dropped out mid-take — reprovisioning, one retry"
      reprovision
      wait_perception 600 || die "$label: perception never came back after a camera dropped"
      sleep 30
      record_window "$secs" \
        || die "$label: a camera dropped out twice; refusing to bank half-blind footage"
      ;;
  esac
  analyze "$label" "$secs"
  log "=== scenario $label done ==="
}

# ------------------------------------------------------------------------ main --
[ $# -gt 0 ] || set -- psf-edge
log "run base: $RUNS_HOST/$RUN_BASE"
for tok in "$@"; do
  name="${tok%%:*}"; secs="${tok#*:}"
  [ "$secs" = "$name" ] && secs="$RECORD_DEFAULT"
  run_scenario "$name" "$secs"
done

log "all requested scenarios complete"
log "summary: $RUNS_HOST/$RUN_BASE/summary.md"
