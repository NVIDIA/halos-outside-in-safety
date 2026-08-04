#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# SRR multi-test orchestrator — runs N scenarios sequentially with the demo log format.
#
# Invoked by the hoisa-generate-regression-report skill (NOT meant to run standalone
# without the skill orchestration — agent-driven readiness checks happen in
# the skill, not in this script).
#
# Usage:
#   ./run_multi.sh in-roi psf-edge balanced            # default 5 min each
#   ./run_multi.sh in-roi:300 balanced:600 fast:180    # explicit durations (seconds)
#   ./run_multi.sh all                                 # all 5 at 5 min each
#   ./run_multi.sh all:300                             # all 5 at 5 min (explicit)
#   ./run_multi.sh full                                # recommended sweep, per-scenario
#                                                      #   durations (see FULL_PRESET):
#                                                      #   in-roi/psf-edge/psf-clear 5m,
#                                                      #   balanced 10m, fast 20m
#
# Default duration if not specified: 300 seconds (5 minutes).
#
# Each scenario name maps to 3 behavior trees (IRA 1.6, one per pedestrian):
#   - srr_<name>_char{0,1,2}.bt.json  in $SIL_DIR/configs/
#     (GitHub layout: <halos-repo>/closed-loop-testing/isaac-sim/sil/configs/)
#   authored in scenarios/behavior-trees/ (regenerate with
#   scenarios/tools/randomize_paths.py) and synced into configs/ by
#   halos-integration/sync_to_halos.sh.
#
# This script does the bash plumbing. The skill is responsible for:
#   - pre-checks (compose state, ROS topics, opcua port)
#   - automated phase-gate readiness checks (no fixed sleep)
#   - demo log header lines and final rollup

set -euo pipefail

# Self-locate: SCRIPT_DIR = .../isaac-sim-sil/scripts, REPO_ROOT = .../isaac-sim-sil.
# Scripts live at repo root so manual users can invoke them without the skill.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Load .env files in precedence: repo .env (sets HOISA_ROOT_PATH + SRR-specific),
# then the deployment profile env for shared values (HOST_IP, ROS_DOMAIN_ID,
# VST_BASE_URL, ...).
if [ -f "$REPO_ROOT/.env" ]; then
  set -a; source "$REPO_ROOT/.env"; set +a
fi
# One var to rule them all: HOISA_ROOT_PATH = the halos-outside-in-safety repo
# root. The deployment dir, profile env, and Isaac SIL tree all derive from it.
HOISA_ROOT_PATH="${HOISA_ROOT_PATH:?HOISA_ROOT_PATH not set — define in $REPO_ROOT/.env}"
COMPOSE_DIR="$HOISA_ROOT_PATH/deployments"
# Shared infra vars live in profiles/<profile>.env; default to the sil profile
# (override PROFILE_ENV to use a different one).
PROFILE_ENV="${PROFILE_ENV:-$COMPOSE_DIR/profiles/sil.env}"
if [ -f "$PROFILE_ENV" ]; then
  set -a; source "$PROFILE_ENV"; set +a
fi

SRR_DIR="${SRR_SERVICE_DIR:-$REPO_ROOT/srr-service}"
# Isaac SIL asset tree the isaac-sim container bind-mounts at /isaac-sim/sil.
SIL_DIR="$HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil"
ISAAC_CONFIG="${SIL_DIR}/configs/default_config_ros.yaml"
SCENE_LOG_BASE="/tmp/isaac-scenario"
LIVE_MON="$SCRIPT_DIR/live_clip_monitor.py"
VST_MGR="${SIL_DIR}/scripts/vst_sensor_manager.py"

# Host-side path of the srr container's /app/runs mount, for host tools like
# snapshot_pss.sh. Mirrors srr-service/docker-compose.yml's
#   ${RUNS_HOST_DIR:-./runs}:/app/runs   (RUNS_HOST_DIR resolved under $SRR_DIR).
# Previously hardcoded to a stale ~/Documents path that no longer exists, so the
# pss.log snapshot silently skipped and every clip ended up with pss_raw=0.
RUNS_HOST_BASE="${RUNS_HOST_DIR:-$SRR_DIR/runs}"
case "$RUNS_HOST_BASE" in
  /*) ;;                                            # already absolute
  *)  RUNS_HOST_BASE="$SRR_DIR/${RUNS_HOST_BASE#./}";;  # relative → resolve under $SRR_DIR
esac

DEFAULT_DURATION_S=300   # 5 minutes — overridable per scenario via "name:seconds"

# Scenarios the default sweep expands to (`all` / `full`) — the 5 randomized
# stress cases.
SWEEP_NAMES="in-roi psf-edge psf-clear balanced fast"

# All accepted scenario names (validation). `fixed` is the deterministic baseline:
# runnable by explicit name (`run_multi.sh fixed`) but intentionally NOT part of
# `all`/`full` — it's always-in-ROI (mute untestable, inflates blended headline)
# and would just add wall clock to a regression sweep.
VALID_NAMES="$SWEEP_NAMES fixed"

# Recommended full sweep — short scenarios stay at 5 min; balanced/fast need
# longer to exercise enough forklift trailer crossings + walking cycles. Tokens
# below are fed straight through expand_tokens. Override a single scenario by
# naming it explicitly instead of "full" (e.g. `run_multi.sh fast:1800`).
FULL_PRESET="in-roi:300 psf-edge:300 psf-clear:300 balanced:600 fast:1200"

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
PSF_WARMUP_S=30

# ----- helpers -----

log() { printf '  [%s] %s\n' "$(date +%M:%S)" "$*"; }
hdr() { printf '\n> %s\n' "$*"; }

require_args() {
  if [ "$#" -lt 1 ]; then
    echo "usage: $0 <scenario1> [scenario2 ...]" >&2
    echo "       $0 all" >&2
    exit 2
  fi
}

is_valid_name() {
  for v in $VALID_NAMES; do [ "$v" = "$1" ] && return 0; done
  return 1
}

# Parse one token "<name>" or "<name>:<seconds>" or "all" / "all:<seconds>"
# Output one line per scenario: "<name> <seconds>"
expand_tokens() {
  local default_s="$DEFAULT_DURATION_S"
  for tok in "$@"; do
    # "full" preset → recurse into expand_tokens with the per-scenario durations.
    if [ "$tok" = "full" ]; then
      expand_tokens $FULL_PRESET
      continue
    fi
    local name="${tok%%:*}" sec=""
    if [ "$tok" != "$name" ]; then
      sec="${tok#*:}"
    fi
    [ -z "$sec" ] && sec="$default_s"
    if [ "$name" = "all" ]; then
      # `all` runs the default sweep only (SWEEP_NAMES); `fixed` is opt-in by name.
      for v in $SWEEP_NAMES; do printf '%s %s\n' "$v" "$sec"; done
    else
      if ! is_valid_name "$name"; then
        echo "unknown scenario: $name" >&2; exit 2
      fi
      printf '%s %s\n' "$name" "$sec"
    fi
  done
}

label_for() {
  local name="$1" sec="$2"
  printf '%s-%dmin' "$name" $((sec / 60))
}

# ----- per-scenario phases (skill orchestrates the agent checks between these) -----

phase_vst_purge() {
  # Each Halos restart makes Isaac re-register Camera/Camera_01/Camera_02 with
  # NEW sensorIds, but VST never drops the previous ones — stale dup sensors pile
  # up (18+ after a few scenarios). With dups present VST does not emit a clean
  # camera_streaming event for the "new" same-named camera, so the SDR never
  # provisions perception (sits at 0 active sources) and the parquet is empty
  # from scenario 2 onward. Purge ALL VST sensors while Isaac is down so the next
  # registration lands in a clean VST and the SDR fires a fresh remove→add cycle.
  if [ ! -f "$VST_MGR" ]; then
    log "VST purge skipped (manager not found: $VST_MGR)"; return 0
  fi
  log "Purging VST sensors (clean slate for next Isaac registration)..."
  VST_BASE_URL="${VST_BASE_URL:-http://127.0.0.1:30888/vst/api}" \
    python3 "$VST_MGR" --delete-all >/dev/null 2>&1 || log "  VST purge WARN (non-fatal)"
  sleep 5   # let VST emit camera_remove → SDR deprovision before new Isaac comes up
}

phase_compose_restart() {
  log "Halos compose down..."
  ( cd "$COMPOSE_DIR" && docker compose --env-file "$PROFILE_ENV" down >/dev/null 2>&1 ) || true
  phase_vst_purge
  log "Halos compose up..."
  ( cd "$COMPOSE_DIR" && docker compose --env-file "$PROFILE_ENV" up -d ) >/dev/null
  log "SRR compose restarting..."
  ( cd "$SRR_DIR"   && docker compose down >/dev/null 2>&1 && docker compose up -d ) >/dev/null
}

phase_set_behavior_tree() {
  # $1 = scenario name (e.g. "in-roi"); $2 = recording seconds.
  #
  # IRA 1.6 (Isaac Sim 6.0) removed character command files — each SRR pedestrian
  # is a behavior tree. default_config_ros.yaml references FIXED tree names
  # (srr_char{0,1,2}.bt.json); we select a scenario by copying that scenario's 3
  # trees (srr_<name>_char{0,1,2}.bt.json) onto the fixed names, then set
  # simulation_duration.
  local name="$1" rec_s="$2"
  local cfg_dir="${SIL_DIR}/configs"
  local i src dst
  for i in 0 1 2; do
    src="${cfg_dir}/srr_${name}_char${i}.bt.json"
    dst="${cfg_dir}/srr_char${i}.bt.json"
    if [[ ! -s "$src" ]]; then
      echo "FATAL: behavior tree missing for scenario '$name': $src" >&2
      echo "  generate + sync it: scenarios/tools/randomize_paths.py then sync_to_halos.sh" >&2
      exit 3
    fi
    cp -f "$src" "$dst"
  done

  # simulation_duration (seconds) must outlast scene-load + PSF warmup + the
  # recording window, with buffer. Old schema used frames (simulation_length).
  # 1800 s buffer = scene-load (+ RTSP-wait up to 240 s) + up to TWO scene-ready windows (the gate's
  # reprovision-retry path can burn ~700 s of wall clock per window, the
  # nominal 360 s timeout plus per-poll docker-exec/consume latency the
  # 'waited' counter does not account for) + reprovision sleeps + PSF warm-up.
  # Budget: ~180-240 s load + 2×~700 s windows + ~28 s reprovision ≈ 1670 s.
  # With the old +600 the sim could hit simulation_duration MID-RECORDING
  # precisely when the reprovision retry succeeded late — silently truncating
  # GT with no guard to catch it. A longer timeline is harmless: the
  # per-scenario compose restart tears the scene down long before it ends.
  local sim_dur=$(( rec_s + PSF_WARMUP_S + 1800 ))
  sed -i "s|^\(\s*\)simulation_duration:.*|\1simulation_duration: ${sim_dur}.0|" "$ISAAC_CONFIG"

  # Sanity check — the duration line must now carry the new value.
  local got
  got=$(grep -E "^\s*simulation_duration:" "$ISAAC_CONFIG" || true)
  if [[ "$got" != *"${sim_dur}.0"* ]]; then
    echo "FATAL: simulation_duration substitution failed for $name" >&2
    echo "  line: $got" >&2
    exit 3
  fi
  log "behavior trees → srr_${name}_char{0,1,2}.bt.json; simulation_duration=${sim_dur}s"
}

phase_start_scene() {
  local label="$1" log_path="${SCENE_LOG_BASE}-${TIMESTAMP}-${label}.log"
  log "Scene loading..."
  # SRR ground-truth publisher — opt-in Halos ActionGraph builder. --srr-gt tells
  # run_actor_sdg.py to build /World/SRRGraph (/gt/*/tf) from its post-setup
  # builder block (action_graphs/srr_ground_truth.py), AFTER IRA has spawned the
  # SRR char groups + runtime_patches repositioned them, then pump live Fabric
  # transforms. The flag defaults OFF, so a normal Halos run is unaffected.
  docker exec -d isaac-sim bash -c "
    ./python.sh /isaac-sim/sil/scripts/run_actor_sdg.py \
      -c /isaac-sim/sil/configs/default_config_ros.yaml \
      --start --headless --enable-vst \
      --cameras-config /isaac-sim/sil/configs/cameras.yaml \
      --srr-gt \
      > $log_path 2>&1
  "
}

# --- VST reconnect workaround -------------------------------------------------
# In SENSOR_INFO_SOURCE=file mode VST spams RTSP reconnects at Isaac's cameras
# while the stream still "has no caps", so Isaac fails to create the SDP and
# perception sits at 0 active sources (empty parquet). Confirmed fix on 2
# machines: let Isaac bring up its RTSP server first, THEN restart the VST/vios
# ingestion stack + the perception container so they reconnect to streams that
# already have caps. Set SKIP_VST_WORKAROUND=1 to bypass (nvstreamer-source
# deployments don't need it).
VST_RTSP_PORTS="${VST_RTSP_PORTS:-8554 8555 8556}"
VST_WAIT_TIMEOUT_S="${VST_WAIT_TIMEOUT_S:-240}"
phase_vst_workaround() {
  if [ -n "${SKIP_VST_WORKAROUND:-}" ]; then
    log "VST workaround skipped (SKIP_VST_WORKAROUND set)."; return 0
  fi
  log "VST workaround: waiting for Isaac RTSP ports (${VST_RTSP_PORTS}; timeout ${VST_WAIT_TIMEOUT_S}s)..."
  local waited=0 step=10 nports up p
  nports=$(echo $VST_RTSP_PORTS | wc -w)
  while [ "$waited" -lt "$VST_WAIT_TIMEOUT_S" ]; do
    up=0
    for p in $VST_RTSP_PORTS; do
      # /dev/tcp connect test — no dependency on ss/netstat being installed.
      if (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then up=$((up + 1)); fi
    done
    if [ "$up" -ge "$nports" ]; then log "  RTSP ports listening after ${waited}s."; break; fi
    sleep "$step"; waited=$((waited + step))
  done
  [ "$waited" -ge "$VST_WAIT_TIMEOUT_S" ] && log "  WARN RTSP-port wait timed out; applying workaround anyway."
  local vios rtvi
  vios=$(docker ps --format '{{.Names}}' | grep -E 'vss-vios-(sensor|streamprocessing|ingress)$' || true)
  rtvi=$(docker ps -a --format '{{.Names}}' | grep -xE 'vss-rtvi-cv' || true)
  if [ -n "$vios" ]; then
    log "  restarting VST/vios: $(echo $vios | tr '\n' ' ')"
    docker restart $vios >/dev/null 2>&1 || true
    sleep 12
  else
    log "  WARN no vss-vios-* containers found; skipping vios restart."
  fi
  if [ -n "$rtvi" ]; then
    log "  restarting perception: $rtvi"
    docker restart $rtvi >/dev/null 2>&1 || docker start $rtvi >/dev/null 2>&1 || true
  else
    log "  WARN vss-rtvi-cv not found; skipping perception restart."
  fi
  log "VST workaround applied."
}

# Bare-run scene-ready gate (when the skill isn't orchestrating the agent
# check). After a compose restart the scene takes ~3 min to load; until then the
# perception pipeline emits nothing useful, so a "has any message" check gives a
# false-positive. We therefore require a REAL perception signal before recording.
# Falls back to proceeding after SCENE_READY_TIMEOUT_S so we never hang forever.
#
# Profile-aware (2D vs 3D). The topic we poll is chosen by the deploy MODE
# (resolve_scene_ready_topic, below): 2d -> mdx-raw (DeepStream), 3d/mv3dt ->
# mdx-bev (Sparse4D/BEV). Both carry the same nv.Frame protobuf, so one decoder
# covers both and we always require decoded detections > 0 (a real perception
# signal, not "has any message"). MODE comes from the VSS warehouse env, so the
# gate never hardcodes a mode-specific topic.
#
# The counter is CUMULATIVE, not "consecutive". Perception legitimately
# interleaves EMPTY frames (e.g. a sparse scene where only one actor is in FOV,
# like `fast`), so the old "2 consecutive hits, hard-reset on any miss" gate
# false-timed-out on such scenes and burned the whole timeout before proceeding —
# even though detections were flowing. We now accumulate hits and only decay the
# counter after several consecutive empty polls (never below 0), so intermittent
# empties don't wipe progress. Tunables: SCENE_READY_HITS (non-empty polls needed)
# and SCENE_READY_MISS_DECAY (consecutive empties that drop one hit).
SCENE_READY_TIMEOUT_S="${SCENE_READY_TIMEOUT_S:-360}"
SCENE_READY_HITS="${SCENE_READY_HITS:-2}"
SCENE_READY_MISS_DECAY="${SCENE_READY_MISS_DECAY:-3}"
# Which Kafka topic carries the perception signal depends on the deploy MODE.
# Resolve it once, in priority order:
#   1. explicit SCENE_READY_TOPIC override (set in .env to force a topic)
#   2. auto-derive from the VSS warehouse env's MODE (ENV_VSS_PATH): the same
#      nv.Frame protobuf flows on mdx-raw for 2d (DeepStream) and mdx-bev for
#      3d/mv3dt (Sparse4D/BEV), so one decoder covers both — only the topic
#      differs by mode. Reading MODE from the VSS deployment's own env keeps a
#      single source of truth (no 2D/3D flag duplicated on the SRR side).
#   3. fall back to mdx-bev (prior default) when neither is available.
resolve_scene_ready_topic() {
  if [ -n "${SCENE_READY_TOPIC:-}" ]; then printf '%s' "$SCENE_READY_TOPIC"; return; fi
  local mode=""
  [ -n "${ENV_VSS_PATH:-}" ] && [ -f "$ENV_VSS_PATH" ] && \
    mode=$(grep -sE '^MODE=' "$ENV_VSS_PATH" | head -1 | cut -d= -f2 | tr -d ' "'"'"'' || true)
  case "$mode" in
    2d)        printf 'mdx-raw' ;;
    3d|mv3dt)  printf 'mdx-bev' ;;
    *)         printf 'mdx-bev' ;;
  esac
}
phase_wait_scene_ready() {
  # Pick the perception topic by deploy MODE (resolve_scene_ready_topic);
  # decode it and require real detections > 0. One code path covers 2d (mdx-raw)
  # and 3d/mv3dt (mdx-bev) since both carry the same nv.Frame protobuf.
  local topic probe_py signal
  topic="$(resolve_scene_ready_topic)"
  signal="perception detections (${topic})"
  probe_py="
from kafka import KafkaConsumer
from srr.kafka_consumer import parse_mdx_bev_bytes
c=KafkaConsumer('${topic}', bootstrap_servers='localhost:9092', auto_offset_reset='latest', value_deserializer=None, consumer_timeout_ms=8000)
n=0
# Bound the scan: consumer_timeout_ms only fires when the topic goes IDLE, so a
# busy topic streaming all-EMPTY frames would otherwise block this loop (and the
# whole gate poll) indefinitely. ~500 messages is one 15-20 s look at a 30 Hz feed.
for i, m in enumerate(c):
    if (parse_mdx_bev_bytes(m.value).get('detections') or []): n+=1
    if n>=1 or i>=500: break
import sys; sys.exit(0 if n>=1 else 1)
"
  if _scene_ready_wait_once; then return 0; fi
  # A first-pass timeout is more often the empty-DeepStream-pipeline race than
  # a slow scene (healthy scenarios reach ready in well under a minute): the
  # per-scenario perception restart leaves a (re)started DeepStream with an
  # EMPTY REST pipeline, and the SDR is event-driven with no reconcile-on-start,
  # so if its stream push raced the restart nothing ever retries and the gate
  # times out on a stack that will never produce a signal. Re-provision once,
  # give the gate one more full window, then fall through with the historical
  # WARN (verdict-side guards treat that exactly as before).
  # NOTE: this intermediate line must NOT contain the substring
  # "WARN scene-ready timeout" — downstream log distillers pair one terminal
  # scene-ready line per scenario (the success line or that WARN marker).
  log "WARN scene-ready first-pass timed out — reprovisioning perception (empty-pipeline race), then retrying once..."
  phase_reprovision_perception || true
  if _scene_ready_wait_once; then return 0; fi
  log "WARN scene-ready timeout — proceeding anyway (data may be empty)."
}

# One full scene-ready wait window. Reads $signal/$probe_py resolved by
# phase_wait_scene_ready (bash dynamic scoping — only called from there).
# Returns 0 when the perception signal is flowing, 1 on timeout.
_scene_ready_wait_once() {
  log "Waiting for scene-ready (${signal}, need ${SCENE_READY_HITS} non-empty polls, timeout ${SCENE_READY_TIMEOUT_S}s)..."
  local waited=0 step=10 hits=0 misses=0
  while [ "$waited" -lt "$SCENE_READY_TIMEOUT_S" ]; do
    if docker exec srr python3 -c "$probe_py" >/dev/null 2>&1; then
      hits=$((hits + 1)); misses=0
      log "  signal seen (${hits}/${SCENE_READY_HITS}) at T+${waited}s"
      [ "$hits" -ge "$SCENE_READY_HITS" ] && { log "scene-ready: perception signal flowing."; return 0; }
    else
      misses=$((misses + 1))
      # Tolerate intermittent empty polls: only decay one hit after
      # SCENE_READY_MISS_DECAY consecutive empties, and never below 0 — no hard
      # reset (that was the false-timeout bug on sparse scenes).
      if [ "$misses" -ge "$SCENE_READY_MISS_DECAY" ] && [ "$hits" -gt 0 ]; then
        hits=$((hits - 1)); misses=0
        log "  ${SCENE_READY_MISS_DECAY} empty polls → decay to ${hits}/${SCENE_READY_HITS} at T+${waited}s"
      fi
    fi
    sleep "$step"; waited=$((waited + step))
  done
  return 1
}

phase_reprovision_perception() {
  # Deterministic recovery for the empty-DeepStream-pipeline race (see the
  # first-pass timeout comment in phase_wait_scene_ready). Same recipe as
  # restart_isaac.sh reprovision_sensors(): restart DeepStream first — a
  # (re)started vss-rtvi-cv always comes back as an EMPTY REST pipeline; never
  # re-register into a possibly polluted one — then ONE clean VST registration
  # round inside the isaac-sim container so VST emits fresh camera_add events
  # for the SDR to push. The Isaac driver and its RTSP streams are untouched
  # (encoders stay warm, so no cold-window race), and this runs strictly
  # BEFORE recording starts.
  local rtvi
  rtvi=$(docker ps -a --format '{{.Names}}' | grep -xE 'vss-rtvi-cv' || true)
  if [ -z "$rtvi" ]; then
    log "  reprovision skipped (vss-rtvi-cv not found)"; return 1
  fi
  log "  reprovision: restarting ${rtvi} (guarantees an empty pipeline)..."
  docker restart "$rtvi" >/dev/null 2>&1 || docker start "$rtvi" >/dev/null 2>&1 || true
  sleep 15
  log "  reprovision: one clean VST sensor registration round..."
  docker exec isaac-sim bash -lc \
    "cd /isaac-sim && ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all && sleep 3 && \
     ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --add-from-config /isaac-sim/sil/configs/cameras.yaml" \
    >/dev/null 2>&1 || { log "  reprovision WARN: sensor registration round failed"; return 1; }
  sleep 10   # let the SDR push the fresh adds into the empty pipeline
}

phase_psf_warmup() {
  log "PSF warm-up (${PSF_WARMUP_S} s)..."
  sleep "$PSF_WARMUP_S"
}

phase_record_start() {
  # ros2 is not on PATH for a bare `docker exec` (no entrypoint/source), so
  # source the ROS env inside the container before calling the service.
  #
  # The first ros2 call after a fresh srr-container restart races DDS service
  # discovery; the start used to be swallowed by >/dev/null, leaving recording
  # OFF for the whole window (parquet = 0 rows). Warm discovery, then retry the
  # start until the service confirms "recording →" in its response.
  docker exec srr bash -lc \
    "source /opt/ros/*/setup.bash && ros2 service list >/dev/null 2>&1 || true" >/dev/null 2>&1
  local ok="" tries=0 out=""
  while [ "$tries" -lt 6 ]; do
    out=$(docker exec srr bash -lc \
      "source /opt/ros/*/setup.bash && ros2 service call /srr/record std_srvs/srv/SetBool '{data: true}' 2>&1" || true)
    if printf '%s' "$out" | grep -q "recording →"; then ok=1; break; fi
    tries=$((tries + 1)); log "  record-start not confirmed, retry ${tries}/6..."; sleep 5
  done
  if [ -z "$ok" ]; then
    echo "FATAL: /srr/record true never confirmed after retries:" >&2
    printf '%s\n' "$out" >&2
  fi
  log "/srr/record true → recording active."
}

phase_record_loop() {
  local rec_s="$1"
  local elapsed=0 step
  while [ "$elapsed" -lt "$rec_s" ]; do
    step=$(( rec_s - elapsed < 60 ? rec_s - elapsed : 60 ))
    sleep "$step"
    elapsed=$((elapsed + step))
    log "Recording... (T+${elapsed}s / ${rec_s}s)"
  done
}

phase_record_stop() {
  docker exec srr bash -lc \
    "source /opt/ros/*/setup.bash && ros2 service call /srr/record std_srvs/srv/SetBool '{data: false}'" \
    >/dev/null 2>&1
  log "/srr/record false → recording stopped."
}

phase_analyze() {
  local label="$1"
  log "Moving parquet to scenario subdir..."
  docker exec srr bash -c "
    set -e
    cd /app/runs
    mkdir -p multi-test-${TIMESTAMP}/${label}
    mv \$(ls -t run-*.parquet | head -1) multi-test-${TIMESTAMP}/${label}/
  "

  # Per-scenario pss.log snapshot — MUST happen before the next scenario's
  # compose restart truncates the source log. End-of-run concat picks these up.
  local snap_script="$SCRIPT_DIR/snapshot_pss.sh"
  local scn_host_dir="${RUNS_HOST_BASE}/multi-test-${TIMESTAMP}/${label}"
  if [[ -x "$snap_script" && -d "$scn_host_dir" ]]; then
    log "snapshotting pss.log (per-scn)..."
    # The per-scenario PSF forensic log is destroyed by the next scenario's
    # compose restart, so a failed snapshot loses it permanently. Fail the sweep
    # by default instead of silently warning; set SRR_PSS_STRICT=0
    # to downgrade to a non-fatal warning (tolerate one lost log, keep sweeping).
    if ! bash "$snap_script" "$scn_host_dir" --per-scn; then
      _pss_src="${PSF_LOG_DIR:-${MDX_DATA_DIR:-/data/sil-data}/psf-log}/pss.log"
      if [[ "${SRR_PSS_STRICT:-1}" == "0" ]]; then
        log "WARN snapshot_pss FAILED for ${label} (src=${_pss_src}) — PSF forensic log for this scenario is MISSING (SRR_PSS_STRICT=0, continuing)"
      else
        log "ERROR snapshot_pss FAILED for ${label} (src=${_pss_src}) — PSF forensic log for this scenario would be LOST; aborting the sweep (set SRR_PSS_STRICT=0 to downgrade to a warning)"
        exit 1
      fi
    fi
    # Refresh the run-level concat right away, not only at end-of-multi-test:
    # if the run is aborted or resumed later, a stale run-level pss.log makes
    # clip_logs slice every clip recorded after its last timestamp to 0 lines.
    bash "$snap_script" "${RUNS_HOST_BASE}/multi-test-${TIMESTAMP}" \
      || log "snapshot_pss cross-run WARN (non-fatal)"
  fi

  # Guard the flush-vs-read race: the recorder's writer.close() footer may not
  # be durably visible the instant /srr/record false returns. tw_split then dies
  # with "Parquet magic bytes not found in footer" → 0 clips for a perfectly
  # good recording. Wait until pyarrow can open the footer (≤30s) before split.
  log "waiting for parquet footer to settle..."
  docker exec srr bash -c "
    cd /app
    for i in \$(seq 1 30); do
      if python3 -c \"import glob,pyarrow.parquet as pq; pq.ParquetFile(glob.glob('/app/runs/multi-test-${TIMESTAMP}/${label}/run-*.parquet')[0])\" 2>/dev/null; then
        exit 0
      fi
      sleep 1
    done
    echo 'WARN: parquet footer not readable after 30s' >&2
  "

  log "tw_split running..."
  docker exec srr bash -c "
    cd /app && python3 -m srr.tw_split \
      /app/runs/multi-test-${TIMESTAMP}/${label}/run-*.parquet \
      /app/runs/multi-test-${TIMESTAMP}/${label}/scenes \
      --source gt
  " >/dev/null

  local n
  n=$(docker exec srr bash -c "ls /app/runs/multi-test-${TIMESTAMP}/${label}/scenes/scn_*.parquet 2>/dev/null | wc -l")
  log "tw_split: ${n} clips identified."

  log "aggregator running (--top-level, idempotent — fixes scene-boundary spillover)..."
  docker exec srr bash -c "
    cd /app && python3 -m srr.aggregator \
      --runs-dir /app/runs/multi-test-${TIMESTAMP} \
      --top-level
  " >/dev/null
  log "aggregator done."

  log "vst_video pulling per-clip MP4..."
  sleep 15  # let the VST recorder flush the segment after record-stop, or the last clip's MP4 can come up short/missing
  # Videos go INSIDE the run dir so per-clip reports' [../videos/scn_X.mp4] resolve.
  docker exec srr bash -c "
    cd /app && python3 -m srr.utils.vst_video split-run \
      --run-dir /app/runs/multi-test-${TIMESTAMP}/${label} \
      --out-dir /app/runs/multi-test-${TIMESTAMP}/${label}/videos
  " >/dev/null
  log "Per-clip videos saved."
}

# ----- main -----

require_args "$@"
# SCENARIOS will be array of "name seconds" pairs (one per line)
mapfile -t SCENARIOS < <(expand_tokens "$@")

hdr "Generating testing plan..."
total_rec=0
echo "  Selected:"
for entry in "${SCENARIOS[@]}"; do
  read -r s d <<<"$entry"
  total_rec=$((total_rec + d))
  printf '    - %s (%d min)\n' "$s" $((d / 60))
done
echo "  Total recording: $((total_rec / 60)) min"
echo "  Output base: /app/runs/multi-test-${TIMESTAMP}/"

hdr "Planning complete."

hdr "Launching SIL test runs..."

# Clear stale run-*.parquet stragglers in /app/runs root before starting. Each
# recording lands as /app/runs/run-<ts>.parquet then phase_analyze moves it into
# the scenario dir, so root is normally empty between scenarios. A leftover from
# an aborted run or manual /srr/record testing can have a newer mtime than a
# failed/empty recording, and phase_analyze's `ls -t run-*.parquet | head -1`
# would then mis-attribute it to the scenario (observed: scenario 1 grabbed a
# stale 768-row debugging parquet). Deleting the run dir also clears these.
docker exec srr bash -c 'rm -f /app/runs/run-*.parquet' >/dev/null 2>&1 || true

# Isaac Sim must be the SOLE camera source for SRR. nvstreamer (the VSS
# sample-video player) publishes the same camera names; running it alongside
# Isaac churns the SDR→perception provisioning across Halos restarts (perception
# ends up at 0 active sources → empty parquet) and lets the scene-ready gate
# false-positive on sample-video detections. Stop it once up front — it belongs
# to the VSS stack which this script never restarts, so it stays down for the
# whole run. Harmless if already stopped or absent.
NVSTREAMER=$(docker ps --format '{{.Names}}' | grep -i nvstreamer || true)
if [ -n "$NVSTREAMER" ]; then
  log "Stopping nvstreamer ($NVSTREAMER) so Isaac is the sole camera source..."
  docker stop $NVSTREAMER >/dev/null 2>&1 || true
fi

i=0
N=${#SCENARIOS[@]}
for entry in "${SCENARIOS[@]}"; do
  read -r s rec_s <<<"$entry"
  i=$((i + 1))
  label=$(label_for "$s" "$rec_s")

  printf '\n  Test %d/%d starts: %s (%d min)\n' "$i" "$N" "$s" $((rec_s / 60))

  phase_compose_restart
  phase_set_behavior_tree "$s" "$rec_s"
  phase_start_scene "$label"

  # Once Isaac's RTSP server is up, bounce VST/vios + perception so they
  # reconnect to streams that already have caps (see phase_vst_workaround).
  phase_vst_workaround

  # Skill SHOULD insert agent-driven scene-ready check here. For bare runs we
  # poll the perception topic so we don't record before the scene is streaming.
  phase_wait_scene_ready

  phase_psf_warmup

  # Live monitor
  source /opt/ros/jazzy/setup.bash 2>/dev/null || true
  export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-74}"
  TW_X_VAL="${TW_X:-9.574}"
  nohup python3 "$LIVE_MON" --tw-x "$TW_X_VAL" --label "$label" \
    > "/tmp/live-clip-${TIMESTAMP}-${label}.log" 2>&1 &
  LIVE_PID=$!

  phase_record_start
  phase_record_loop "$rec_s"
  phase_record_stop

  kill -TERM "$LIVE_PID" 2>/dev/null || true
  wait "$LIVE_PID" 2>/dev/null || true

  # Don't let one bad scenario (e.g. empty parquet) abort the whole sweep.
  phase_analyze "$label" || log "WARN analyze failed for ${label} — continuing to next scenario"
  printf '  Test %d/%d completes.\n' "$i" "$N"
done

hdr "All test runs complete."

# Cross-run rollup — aggregator --top-level produces summary.md at runs-dir root
# plus refreshes each <run>/reports/summary.md with run-level BA pool matching.
log "running cross-run aggregator (--top-level)..."
docker exec srr bash -c "
  cd /app && python3 -m srr.aggregator \
    --runs-dir /app/runs/multi-test-${TIMESTAMP} \
    --top-level
" >/dev/null
log "top-level summary written: /app/runs/multi-test-${TIMESTAMP}/summary.md"

# Concat per-scenario pss.log snapshots into a single run-level pss.log used
# by clip_logs default fallback. Per-scn snapshots were taken in phase_analyze
# right after each record stop (before the next compose restart truncated the
# source), so this preserves every scenario's PSF data.
SNAP_SCRIPT="$(dirname "$0")/snapshot_pss.sh"
HOST_RUN_DIR="${RUNS_HOST_BASE}/multi-test-${TIMESTAMP}"
if [[ -x "$SNAP_SCRIPT" && -d "$HOST_RUN_DIR" ]]; then
  log "concatenating per-scn pss.log snapshots..."
  bash "$SNAP_SCRIPT" "$HOST_RUN_DIR" || log "snapshot_pss WARN: $?"
fi

echo "  Output: /app/runs/multi-test-${TIMESTAMP}/"
echo "  Top-level summary: /app/runs/multi-test-${TIMESTAMP}/summary.md"
echo "  Skill should read references/06_interpret_report.md to interpret."
