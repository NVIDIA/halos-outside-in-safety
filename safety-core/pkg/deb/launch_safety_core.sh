#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# SAIM and PCM use the same launch/log supervision in this launcher.
psf_resolve_monitor_modes() {
    local name mode
    for name in SAIM PCM; do
        if [[ "$name" == SAIM ]]; then mode="$SAIM_MODE"; else mode="$PCM_MODE"; fi
        case "$mode" in
            learn|active|skip) ;;
            *) echo "Error: --${name,,}-mode is required and must be learn, active, or skip" >&2; return 1 ;;
        esac
    done
    # A learner never becomes ACTIVE implicitly. Run the control plane when
    # either monitor is ACTIVE, or when both monitors are explicitly skipped.
    CONTROL_PLANE=0
    if [[ "$SAIM_MODE" == active || "$PCM_MODE" == active ||
          ( "$SAIM_MODE" == skip && "$PCM_MODE" == skip ) ]]; then
        CONTROL_PLANE=1
    fi
}

# PSS owns required-source liveness policy. Independent monitor modes must
# agree with that policy whenever this launcher starts the control plane. A LEARN
# monitor does not supply ACTIVE verdicts; it must not be required by a
# concurrently running PSS daemon. Pure LEARN jobs do not start PSS.
nvpss_required_flag() { # $1=key; echoes 0 or 1, or explains and returns 1
    local key="$1" n value
    if [[ ! -r "$NVPSS_CONF" ]]; then
        echo "Error: ${NVPSS_CONF} not found or not readable." >&2
        return 1
    fi
    n="$(grep -cE "^[[:space:]]*${key}[[:space:]]*=" "$NVPSS_CONF" || true)"
    if [[ "$n" -eq 0 ]]; then
        echo "Error: ${key} is missing from ${NVPSS_CONF}." >&2
        echo "       Add '${key} = 1' for ACTIVE or '= 0' for LEARN/skip." >&2
        return 1
    fi
    if [[ "$n" -gt 1 ]]; then
        echo "Error: ${key} is defined more than once in ${NVPSS_CONF}. Keep exactly one." >&2
        return 1
    fi
    value="$(grep -E "^[[:space:]]*${key}[[:space:]]*=" "$NVPSS_CONF" \
             | cut -d= -f2- | sed 's/#.*//' | tr -d '[:space:]')"
    if ! [[ "$value" =~ ^[0-9]{1,3}$ ]] || (( 10#$value > 1 )); then
        echo "Error: ${key} = '${value}' in ${NVPSS_CONF} is not 0 or 1." >&2
        return 1
    fi
    echo $(( 10#$value ))
}

psf_validate_required_monitors() {
    if [[ "$CONTROL_PLANE" == 0 ]]; then return 0; fi
    REQUIRED_SAFETY_MONITOR="$(nvpss_required_flag required_safety_monitor)" || return 1
    REQUIRED_PERCEPTION_MONITOR="$(nvpss_required_flag required_perception_monitor)" || return 1
    local label mode key required active
    for label in SAIM PCM; do
        if [[ "$label" == SAIM ]]; then
            mode="$SAIM_MODE"; key=required_safety_monitor; required="$REQUIRED_SAFETY_MONITOR"
        else
            mode="$PCM_MODE"; key=required_perception_monitor; required="$REQUIRED_PERCEPTION_MONITOR"
        fi
        active=0
        if [[ "$mode" == active ]]; then active=1; fi
        if [[ "$required" != "$active" ]]; then
            echo "Error: --${label,,}-mode ${mode} conflicts with ${key} = ${required} in ${NVPSS_CONF}." >&2
            echo "       Set ${key} = ${active} for this mode, or select the mode matching the deployment's required sources." >&2
            return 1
        fi
    done
    echo "Required sources: safety_monitor=${REQUIRED_SAFETY_MONITOR}" \
         "perception_monitor=${REQUIRED_PERCEPTION_MONITOR} (from ${NVPSS_CONF})"
}

declare -a PSF_MONITOR_PIDS=()
declare -A PSF_MONITOR_LABEL=() PSF_MONITOR_MODE=() PSF_MONITOR_LOG_PID=()
declare -A PSF_MONITOR_DONE=() PSF_MONITOR_ROOT=()
declare -A PSF_MONITOR_LOG_EOF=()

psf_prepare_monitor_log() {
    local log_file="$1"
    if [[ -e "$log_file" && ! -f "$log_file" ]]; then
        echo "Error: monitor log is not a regular file: ${log_file}" >&2
        return 1
    fi
    # Open as the same user that runs tee. Test the actual filesystem/mount
    # permission, not just mode bits, before launching any safety processes.
    if ! (: >> "$log_file"); then
        echo "Error: monitor log is not writable: ${log_file}" >&2
        return 1
    fi
}

psf_start_monitor() {
    local label="$1" mode="$2" log_file="$3" need_root="$4"
    shift 4
    psf_prepare_monitor_log "$log_file" || return 1
    local log_fd log_pid pid
    # Retain both PIDs so a failed tee is noticed. exit makes ENOSPC/EIO stop
    # the writer instead of silently continuing with only stdout. The
    # supervisor treats its exit as a failure.
    exec {log_fd}> >(exec tee --output-error=exit -a "$log_file") || return 1
    log_pid=$!
    "$@" </dev/null >&"$log_fd" 2>&1 {log_fd}>&- &
    pid=$!
    exec {log_fd}>&-
    PSF_MONITOR_PIDS+=("$pid")
    PSF_MONITOR_LABEL[$pid]="$label"
    PSF_MONITOR_MODE[$pid]="$mode"
    PSF_MONITOR_LOG_PID[$pid]="$log_pid"
    PSF_MONITOR_DONE[$pid]=0
    PSF_MONITOR_ROOT[$pid]="$need_root"
    PSF_MONITOR_LOG_EOF[$pid]=0
    echo "${label} started in ${mode^^} mode with PID ${pid}"
}

psf_monitor_signal() {
    local sig="$1" pid="$2"
    if [[ "${PSF_MONITOR_ROOT[$pid]:-0}" == 1 ]]; then
        sudo -n kill -"$sig" "$pid" 2>/dev/null
    else
        kill -"$sig" "$pid" 2>/dev/null
    fi
}

psf_poll_monitors() {
    local i pid log_pid status log_done
    for i in "${!PSF_MONITOR_PIDS[@]}"; do
        pid="${PSF_MONITOR_PIDS[$i]}"
        log_pid="${PSF_MONITOR_LOG_PID[$pid]}"
        log_done=0
        if ! kill -0 "$log_pid" 2>/dev/null; then
            status=0
            wait "$log_pid" || status=$?
            if [[ "$status" != 0 ]]; then
                echo "Error: ${PSF_MONITOR_LABEL[$pid]} log writer exited (status ${status})" >&2
                return 1
            fi
            log_done=1
        fi
        if [[ "${PSF_MONITOR_DONE[$pid]}" == 0 ]] && ! psf_monitor_signal 0 "$pid"; then
            status=0
            wait "$pid" || status=$?
            PSF_MONITOR_DONE[$pid]=1
            if [[ "${PSF_MONITOR_MODE[$pid]}" != learn || "$status" != 0 ]]; then
                echo "Error: ${PSF_MONITOR_LABEL[$pid]} exited (status ${status})" >&2
                return 1
            fi
        fi
        if [[ "$log_done" == 1 ]]; then
            if [[ "${PSF_MONITOR_DONE[$pid]}" != 1 ]]; then
                # EOF may arrive just before a successful learner exits. Allow
                # one polling interval for that race, but never leave a live
                # monitor indefinitely without its writer.
                if [[ "${PSF_MONITOR_MODE[$pid]}" == learn && "${PSF_MONITOR_LOG_EOF[$pid]}" == 0 ]]; then
                    PSF_MONITOR_LOG_EOF[$pid]=1
                    continue
                fi
                echo "Error: ${PSF_MONITOR_LABEL[$pid]} log writer closed while the monitor is still running" >&2
                return 1
            fi
            echo "${PSF_MONITOR_LABEL[$pid]} completed LEARN successfully"
            unset 'PSF_MONITOR_PIDS[i]' 'PSF_MONITOR_LABEL[$pid]' 'PSF_MONITOR_MODE[$pid]'
            unset 'PSF_MONITOR_LOG_PID[$pid]' 'PSF_MONITOR_DONE[$pid]' 'PSF_MONITOR_ROOT[$pid]'
            unset 'PSF_MONITOR_LOG_EOF[$pid]'
        fi
    done
    return 0
}

psf_wait_for_learners() {
    while [[ ${#PSF_MONITOR_PIDS[@]} -gt 0 ]]; do
        psf_poll_monitors || return 1
        if [[ ${#PSF_MONITOR_PIDS[@]} -gt 0 ]]; then sleep 0.1; fi
    done
}

psf_stop_monitors() {
    local pid log_pid elapsed alive
    for pid in "${PSF_MONITOR_PIDS[@]}"; do
        if [[ "${PSF_MONITOR_DONE[$pid]}" == 0 ]]; then
            psf_monitor_signal TERM "$pid" || true
        fi
    done
    # Let readers flush final lines after the monitor closes its pipe.
    for ((elapsed=0; elapsed<10; ++elapsed)); do
        alive=0
        for pid in "${PSF_MONITOR_PIDS[@]}"; do
            if psf_monitor_signal 0 "$pid" || kill -0 "${PSF_MONITOR_LOG_PID[$pid]}" 2>/dev/null; then
                alive=1
            fi
        done
        if [[ "$alive" == 0 ]]; then break; fi
        sleep 1
    done
    for pid in "${PSF_MONITOR_PIDS[@]}"; do
        log_pid="${PSF_MONITOR_LOG_PID[$pid]}"
        psf_monitor_signal KILL "$pid" || true
        kill -KILL "$log_pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        wait "$log_pid" 2>/dev/null || true
    done
    PSF_MONITOR_PIDS=()
}

# Allow regression checks to source the launch functions without starting services.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then return 0; fi

source "$(dirname "$(readlink -f -- "${BASH_SOURCE[0]}")")/sdm_launch_common.sh" || exit 1

declare -a BG_PIDS=()

shutdown() {
    local exit_code="${1:-0}"
    trap - EXIT SIGTERM SIGINT
    echo "Stopping PSF processes..."

    for pid in "${BG_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "Stopping process $pid"
            kill -SIGTERM "$pid" 2>/dev/null
        fi
    done

    psf_stop_monitors

    local timeout=10
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        local all_dead=true
        for pid in "${BG_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                all_dead=false
                break
            fi
        done

        if [ "$all_dead" = true ]; then
            echo "All processes stopped cleanly"
            exit "$exit_code"
        fi

        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo "Timeout reached, force killing remaining processes"
    for pid in "${BG_PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null
    done

    if [[ "$exit_code" == 0 ]]; then exit_code=143; fi
    exit "$exit_code"
}

trap 'shutdown 143' SIGTERM
trap 'shutdown 130' SIGINT

PSF_PREFIX="/opt/nvidia/psf"
PSF_BIN="${PSF_PREFIX}/bin"
PSF_APPS="${PSF_PREFIX}/apps"

APP=""
SDM_EXTERNAL=0
SDM_CONFIG_LIST=""
SDM_LOG_LIST=""
EVENT_MAPPING=""
CONFIGURED_SDMS=0
CMD_RX_IP=""
CMD_RX_PORT=""
# atl_proximity only: the combined app runs two SDMs, so the forklift-to-person
# SDM needs its own PLC command port. Left empty means "use the binary's own
# default" (12346), which is already chosen not to collide with ATL's 12345.
PROXIMITY_CMD_RX_PORT=""
PAIR_TTL_MS=""
# atl_dual only: two ATL SDMs, one per zone. Both are the same atl_sdm binary
# with the same built-in PLC port default, so unlike atl_proximity there is no
# non-colliding default to fall back on -- zone 2's port is required.
ZONE2_CMD_RX_PORT=""
KAFKA_BROKER=""
SENSOR_CONFIG=""
HB_STALE_MS=5000
HB_PERIOD_MS=5500
SAIM_MODE=""              # required: skip | active | learn
PCM_MODE=""               # required: skip | active | learn, independent of SAIM
GPU_ID=""                 # optional GPU pin, exported as CUDA_VISIBLE_DEVICES
LEARN_DURATION=300        # learn capture window in seconds; range [1, 3600]
BASELINE_DIR="${PSF_BIN}/saim_baseline" # Per-sensor baselines. Bind-mounted from the host under Docker.
PCM_BASELINE_DIR="$(dirname -- "${PCM_BASELINE_CONFIG:-${PSF_BIN}/pcm_baseline/pcm_baseline.cfg}")"
PCM_CONFIG="${PCM_MONITOR_CONFIG:-${PSF_BIN}/pcm_monitor.cfg}"
THRESHOLD_CONFIG="${PSF_BIN}/thresholds.cfg"
# Operator-selected SDM configs, like --sensor-config. The package ships only
# bin/atl_sdm_zone{1,2}.conf.sample templates, never an active default. Under
# Docker these flags name the container paths of read-only config bind mounts.
ATL_ZONE1_CONF=""
ATL_ZONE2_CONF=""
# The SDM binaries' built-in PLC command ports, named here so an app that starts
# TWO of them can be checked for a port collision before either is launched.
# The resolved value is also what gets passed on the command line below, so the
# port this script checked is the port the SDM actually binds.
#   atl_sdm            decision-makers/atl/sdm/ATL.cpp (plcPort = 12345)
#   atl_proximity_sdm  decision-makers/atl_proximity/sdm/ccplex/AtlProximity.cpp
#                      (kDefaultPlcPort = 12346)
ATL_SDM_BUILTIN_CMD_RX_PORT=12345
ATL_PROXIMITY_SDM_BUILTIN_CMD_RX_PORT=12346
LOG_DIR="/var/log/psf"
SAIM_LOG_FILE="${LOG_DIR}/saim.log"
PCM_LOG_FILE="${LOG_DIR}/pcm.log"

require_arg() {
    if [ "$#" -lt 2 ] || [[ -z "$2" || "$2" == --* ]]; then
        echo "Error: $1 requires an argument" >&2
        exit 1
    fi
}

usage() {
    echo "Usage: $0 --saim-mode <skip|active|learn> --pcm-mode <skip|active|learn> [options]"
    echo "  Select each monitor independently. LEARN exits after saving baselines; it never becomes ACTIVE."
    echo "  The control plane runs when either monitor is ACTIVE or both are skipped."
    echo "  In control-plane runs, nvpss.conf required_safety_monitor / required_perception_monitor"
    echo "  must match the selected modes: 1 for active, 0 for learn/skip."
    echo "  --app <atl|pxc|atl,atl|atl,pxc|pxc,atl|pxc,pxc|atl_dual|cjs|atl_proximity> (control plane only)"
    echo "  --sensor-config FILE          Required for SAIM or the control plane"
    echo "  --sdm-config <file[,file]>     Required ATL/PXC configs, in --app order"
    echo "  --cmd_rx_port <port[,port]>    One port per SDM; distinct ports required for two SDMs"
    echo "  --sdm-log-file <file[,file]>   Optional native log paths, in --app order (default /var/log/psf)"
    echo "  --event-mapping <file>         Optional MDX mapping matching the SDM event bindings"
    echo "  --atl-zone1-conf / --atl-zone2-conf / --zone2_cmd_rx_port remain supported for ATL launches"
    echo "  --sdm-external                 Control plane only; launch the SDMs separately"
    echo "  --cmd_rx_ip IP --broker ADDR --hb_stale_ms MS --hb_period_ms MS"
    echo "  --proximity_cmd_rx_port PORT --pair_ttl_ms MS (atl_proximity only)"
    echo "  --gpu ID --learn-duration SEC --baseline-dir DIR"
    echo "  --pcm-baseline-dir DIR        PCM baselines (default /opt/nvidia/psf/bin/pcm_baseline)"
    echo "  --pcm-config FILE             ACTIVE PCM config (default /opt/nvidia/psf/bin/pcm_monitor.cfg)"
}

resolve_atl_config() {
    local option="$1" config="$2"
    if [[ -z "$config" ]]; then
        echo "Error: ${option} is required for --app ${APP} when launching an ATL SDM" >&2
        echo "       Copy/edit ${PSF_BIN}/atl_sdm_zone{1,2}.conf.sample and pass the active config path." >&2
        return 1
    fi
    if [[ ! -f "$config" || ! -r "$config" ]]; then
        echo "Error: ${option} '${config}' not found or not readable" >&2
        echo "       Under Docker, bind-mount the config read-only and pass its container path." >&2
        return 1
    fi
    # The control plane later changes into LOG_DIR; preserve relative inputs.
    readlink -f -- "$config"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --app)
            require_arg "$@"; APP="$2"; shift 2 ;;
        --sdm-external)
            SDM_EXTERNAL=1; shift ;;
        --sdm-config)
            require_arg "$@"; SDM_CONFIG_LIST="$2"; shift 2 ;;
        --sdm-log-file)
            require_arg "$@"; SDM_LOG_LIST="$2"; shift 2 ;;
        --event-mapping)
            require_arg "$@"; EVENT_MAPPING="$2"; shift 2 ;;
        --cmd_rx_ip)
            require_arg "$@"; CMD_RX_IP="$2"; shift 2 ;;
        --cmd_rx_port)
            require_arg "$@"; CMD_RX_PORT="$2"; shift 2 ;;
        --proximity_cmd_rx_port)
            require_arg "$@"; PROXIMITY_CMD_RX_PORT="$2"; shift 2 ;;
        --pair_ttl_ms)
            require_arg "$@"; PAIR_TTL_MS="$2"; shift 2 ;;
        --zone2_cmd_rx_port)
            require_arg "$@"; ZONE2_CMD_RX_PORT="$2"; shift 2 ;;
        --broker)
            require_arg "$@"; KAFKA_BROKER="$2"; shift 2 ;;
        --sensor-config)
            require_arg "$@"; SENSOR_CONFIG="$2"; shift 2 ;;
        --atl-zone1-conf)
            require_arg "$@"; ATL_ZONE1_CONF="$2"; shift 2 ;;
        --atl-zone2-conf)
            require_arg "$@"; ATL_ZONE2_CONF="$2"; shift 2 ;;
        --hb_stale_ms)
            require_arg "$@"; HB_STALE_MS="$2"; shift 2 ;;
        --hb_period_ms)
            require_arg "$@"; HB_PERIOD_MS="$2"; shift 2 ;;
        --saim-mode)
            require_arg "$@"; SAIM_MODE="$2"; shift 2 ;;
        --pcm-mode)
            require_arg "$@"; PCM_MODE="$2"; shift 2 ;;
        --pcm-baseline-dir)
            require_arg "$@"; PCM_BASELINE_DIR="$2"; shift 2 ;;
        --pcm-config)
            require_arg "$@"; PCM_CONFIG="$2"; shift 2 ;;
        --gpu)
            require_arg "$@"; GPU_ID="$2"; shift 2 ;;
        --learn-duration)
            require_arg "$@"; LEARN_DURATION="$2"; shift 2 ;;
        --baseline-dir)
            require_arg "$@"; BASELINE_DIR="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "Error: Unknown option '$1'" >&2
            usage >&2
            exit 1
            ;;
    esac
done

psf_resolve_monitor_modes || exit 1

# Learning-only jobs do not need an application or the control plane.
if [[ "$CONTROL_PLANE" == 1 ]]; then
    if [[ -z "$APP" ]]; then
        echo "Error: --app parameter is required" >&2
        usage >&2
        exit 1
    fi

    case "$APP" in
        atl|atl_dual|atl,atl|pxc|atl,pxc|pxc,atl|pxc,pxc) CONFIGURED_SDMS=1 ;;
        cjs|atl_proximity) ;;
        *) echo "Error: unsupported app '$APP'" >&2; exit 1 ;;
    esac
    if [[ "$CONFIGURED_SDMS" != 1 && ( -n "$SDM_CONFIG_LIST" || -n "$SDM_LOG_LIST" || -n "$EVENT_MAPPING" || "$SDM_EXTERNAL" == 1 ) ]]; then
        echo "Error: instance config/log/mapping options and --sdm-external require an ATL/PXC app combination" >&2
        exit 1
    fi

    # atl_proximity runs two SDMs against one gateway, so their PLC command
    # ports must differ or the two command streams land on one receiver where
    # the opcodes are indistinguishable.
    if [[ "$APP" == "atl_proximity" ]]; then
        # Compare the ports the two SDMs will ACTUALLY bind, not the ones that
        # happen to be typed. An omitted flag means that SDM takes its binary's
        # built-in port, so "--cmd_rx_port 12346" alone would otherwise put
        # atl_sdm and atl_proximity_sdm both on 12346, silently.
        ATL_EFFECTIVE_CMD_RX_PORT="${CMD_RX_PORT:-$ATL_SDM_BUILTIN_CMD_RX_PORT}"
        PROXIMITY_EFFECTIVE_CMD_RX_PORT="${PROXIMITY_CMD_RX_PORT:-$ATL_PROXIMITY_SDM_BUILTIN_CMD_RX_PORT}"
        if [[ "$ATL_EFFECTIVE_CMD_RX_PORT" == "$PROXIMITY_EFFECTIVE_CMD_RX_PORT" ]]; then
            echo "Error: atl_sdm and atl_proximity_sdm would both use PLC port ${ATL_EFFECTIVE_CMD_RX_PORT}." >&2
            echo "       Their command streams would land on one receiver carrying" >&2
            echo "       indistinguishable opcodes. Set --cmd_rx_port and" >&2
            echo "       --proximity_cmd_rx_port to different ports (defaults are" >&2
            echo "       ${ATL_SDM_BUILTIN_CMD_RX_PORT} and ${ATL_PROXIMITY_SDM_BUILTIN_CMD_RX_PORT})." >&2
            exit 1
        fi
    elif [[ -n "$PROXIMITY_CMD_RX_PORT" || -n "$PAIR_TTL_MS" ]]; then
        echo "Error: --proximity_cmd_rx_port and --pair_ttl_ms apply only to --app atl_proximity" >&2
        exit 1
    fi

    if [[ "$CONFIGURED_SDMS" == 1 ]]; then
        psf_sdm_legacy_args "$APP" "$SDM_CONFIG_LIST" "$CMD_RX_PORT" \
            "$ATL_ZONE1_CONF" "$ATL_ZONE2_CONF" "$ZONE2_CMD_RX_PORT" || exit 1
        SDM_CONFIG_LIST="$PSF_SDM_CONFIG_LIST"
        CMD_RX_PORT="$PSF_SDM_PORT_LIST"
        NEED_SDM_CONFIGS=1
        if [[ "$SDM_EXTERNAL" == 1 ]]; then NEED_SDM_CONFIGS=0; fi
        # External SDMs still supply their read-only subscription bindings to PCM.
        if [[ "$SDM_EXTERNAL" == 1 && "$PCM_MODE" != skip && -n "$SDM_CONFIG_LIST" ]]; then
            NEED_SDM_CONFIGS=1
        fi
        psf_sdm_prepare "$APP" "$SDM_CONFIG_LIST" "$CMD_RX_PORT" "$SDM_LOG_LIST" \
            "$LOG_DIR" "$NEED_SDM_CONFIGS" || exit 1
        if [[ -z "$EVENT_MAPPING" ]]; then
            EVENT_MAPPING="$(psf_sdm_default_mapping "$APP" "$PSF_APPS")" || exit 1
        fi
        if [[ ! -f "$EVENT_MAPPING" || ! -r "$EVENT_MAPPING" ]]; then
            echo "Error: event mapping '$EVENT_MAPPING' is not readable" >&2
            exit 1
        fi
        EVENT_MAPPING="$(readlink -f -- "$EVENT_MAPPING")" || exit 1
    else
        if [[ -n "$ZONE2_CMD_RX_PORT" || -n "$ATL_ZONE2_CONF" ]]; then
            echo "Error: zone-two options require --app atl_dual or atl,atl" >&2
            exit 1
        fi
        if [[ "$APP" == "atl_proximity" ]]; then
            ATL_ZONE1_CONF="$(resolve_atl_config --atl-zone1-conf "$ATL_ZONE1_CONF")" || exit 1
        fi
    fi
fi

# A learning-only PCM may inspect the selected consumers without starting them.
# An app-free learner remains supported and records only camera/timing data.
if [[ "$CONTROL_PLANE" == 0 && "$PCM_MODE" != skip ]]; then
    # Match ACTIVE's binding selection before learning the contract. Receiver
    # ports are unused by a learning-only job.
    psf_sdm_legacy_args "$APP" "$SDM_CONFIG_LIST" "" \
        "$ATL_ZONE1_CONF" "$ATL_ZONE2_CONF" "" || exit 1
    SDM_CONFIG_LIST="$PSF_SDM_CONFIG_LIST"
    case "$APP" in
        atl|atl_dual|atl,atl|pxc|atl,pxc|pxc,atl|pxc,pxc)
            if [[ -z "$EVENT_MAPPING" ]]; then
                EVENT_MAPPING="$(psf_sdm_default_mapping "$APP" "$PSF_APPS")" || exit 1
            fi ;;
    esac
    if [[ -n "$SDM_CONFIG_LIST" ]]; then
        if [[ "$SDM_CONFIG_LIST" == ,* || "$SDM_CONFIG_LIST" == *, || "$SDM_CONFIG_LIST" == *,,* ]]; then
            echo "Error: --sdm-config requires non-empty comma-separated paths" >&2; exit 1
        fi
        IFS=, read -r -a SDM_CONFIGS <<< "$SDM_CONFIG_LIST"
    fi
    if [[ -n "$EVENT_MAPPING" ]]; then
        if [[ ! -f "$EVENT_MAPPING" || ! -r "$EVENT_MAPPING" ]]; then
            echo "Error: PCM event mapping '$EVENT_MAPPING' is not readable" >&2; exit 1
        fi
        EVENT_MAPPING="$(readlink -f -- "$EVENT_MAPPING")" || exit 1
    elif [[ ${#SDM_CONFIGS[@]} -gt 0 ]]; then
        echo "Error: PCM SDM configs require --app or --event-mapping" >&2; exit 1
    fi
    for ((PCM_CONFIG_INDEX=0; PCM_CONFIG_INDEX<${#SDM_CONFIGS[@]}; ++PCM_CONFIG_INDEX)); do
        if [[ ! -f "${SDM_CONFIGS[$PCM_CONFIG_INDEX]}" || ! -r "${SDM_CONFIGS[$PCM_CONFIG_INDEX]}" ]]; then
            echo "Error: PCM SDM config '${SDM_CONFIGS[$PCM_CONFIG_INDEX]}' is not readable" >&2; exit 1
        fi
        SDM_CONFIGS[$PCM_CONFIG_INDEX]="$(readlink -f -- "${SDM_CONFIGS[$PCM_CONFIG_INDEX]}")" || exit 1
    done
fi

if [[ "$CONTROL_PLANE" == 1 || "$SAIM_MODE" != skip ]]; then
    if [[ -z "$SENSOR_CONFIG" ]]; then
        echo "Error: --sensor-config is required" >&2
        exit 1
    fi

    if [[ ! -f "$SENSOR_CONFIG" ]] || [[ ! -r "$SENSOR_CONFIG" ]]; then
        echo "Error: sensor config file '${SENSOR_CONFIG}' not found or not readable" >&2
        exit 1
    fi

    SENSOR_CONFIG="$(readlink -f "$SENSOR_CONFIG")"
fi

# ---------------------------------------------------------------------------
# Deployment topology: READ, never re-declared.
#
# nvpss.conf already states it, and the daemon refuses to start without it.
# Taking it as a launcher flag instead would create a second declaration that
# can disagree with the first, and the two disagreements are not symmetric:
#
#   launcher 3D / conf 2D  the daemon has no group, mdx-client sends
#                          group-named events, and every one is rejected by
#                          ValidateEventSource. Loud.
#   launcher 2D / conf 3D  per-camera events validate fine (the camera rows are
#                          still in sensor_config), but the SDM reviewed set is
#                          the group, so nothing ever moves the failed mask. The
#                          stack looks healthy and the latch is inert. Silent.
#
# Reading the single declaration removes that class rather than validating it
# after the fact. nvpss.conf is bind-mounted read-only in the container; this
# only ever reads it.
#
# NVPSS_CONF must match the daemon's default (NVPSS_DAEMON_DEFAULT_CONF in
# components/event-integrator/daemon/include/pss_daemon.h): the daemon is
# started below without --conf.
NVPSS_CONF="${PSF_BIN}/nvpss.conf"
SENSOR_PIPELINES_CONFIG="${PSF_BIN}/sensor_pipelines_config.conf"

resolve_control_plane_topology() {
    if [[ ! -r "$NVPSS_CONF" ]]; then
        echo "Error: ${NVPSS_CONF} not found or not readable." >&2
        echo "       It declares deploymentMode, which selects the identity file" >&2
        echo "       mdx-client resolves Kafka sensor ids against. The daemon reads" >&2
        echo "       the same file and would refuse to start a moment later." >&2
        exit 1
    fi

    # Last assignment wins, matching how the daemon's parser resolves a repeated
    # key -- though the daemon rejects duplicates outright, so a file that reaches
    # here with two is already going to be refused.
    DEPLOYMENT_MODE="$(grep -E '^[[:space:]]*deploymentMode[[:space:]]*=' "$NVPSS_CONF" \
                       | tail -1 | cut -d= -f2- | tr -d '[:space:]' | tr '[:lower:]' '[:upper:]')"

    case "$DEPLOYMENT_MODE" in
        2D|3D) ;;
        "")
            echo "Error: deploymentMode is missing from ${NVPSS_CONF}." >&2
            echo "       Add 'deploymentMode = 2D' or '= 3D'. It is mandatory and is" >&2
            echo "       not inferred from which files are present." >&2
            exit 1 ;;
        *)
            echo "Error: deploymentMode '${DEPLOYMENT_MODE}' in ${NVPSS_CONF} is not recognised." >&2
            echo "       Accepted values are 2D and 3D." >&2
            exit 1 ;;
    esac

    # In 3D the Kafka sensor id is the BEV group, not a camera, so mdx-client must
    # resolve against sensor_pipelines_config.conf. Exactly one of the two flags is
    # accepted by mdx-client; supplying both, or neither, is refused there.
    #
    # Checked here, before anything is spawned, not at the point of use:
    # mdx_identity_args is consumed through process substitution, where an exit
    # only ends the subshell, and by then the daemon, gateway and SDMs are running.
    if [[ "$DEPLOYMENT_MODE" == "3D" && ! -r "$SENSOR_PIPELINES_CONFIG" ]]; then
        echo "Error: deploymentMode=3D but ${SENSOR_PIPELINES_CONFIG} is not readable." >&2
        echo "       In 3D this file names the BEV group that Kafka reports under," >&2
        echo "       and mdx-client cannot resolve any Kafka sensor id without it." >&2
        exit 1
    fi
    echo "Deployment mode: ${DEPLOYMENT_MODE} (from ${NVPSS_CONF})"
}
if [[ "$CONTROL_PLANE" == 1 ]]; then resolve_control_plane_topology; fi
psf_validate_required_monitors || exit 1

# PSS instance id of the one mdx_client this launcher runs.
# It makes the client identifiable to PSS: if it stops, every SDM latches until
# this instance is back. It must be covered by required_mdx_instances in
# nvpss.conf, which the shipped file sets to 1.
MDX_PSS_INSTANCE_ID=1

# Pure: emits arguments, never exits. The preflight above is what guarantees the
# 3D file is readable by the time this runs.
mdx_identity_args() {
    if [[ "$DEPLOYMENT_MODE" == "3D" ]]; then
        printf '%s\n' --sensor-pipelines-config "$SENSOR_PIPELINES_CONFIG"
    else
        printf '%s\n' --sensor-config "$SENSOR_CONFIG"
    fi
    printf '%s\n' --instance-id "$MDX_PSS_INSTANCE_ID"
}

if [[ "$PCM_MODE" != skip ]]; then
    if [[ ! -x "${PSF_BIN}/nv_perception_monitor" ]]; then
        echo "Error: nv_perception_monitor is missing or not executable" >&2
        exit 1
    fi
    if [[ ! -d "$PCM_BASELINE_DIR" || ! -r "$PCM_BASELINE_DIR" || ! -x "$PCM_BASELINE_DIR" ]]; then
        echo "Error: PCM baseline folder '${PCM_BASELINE_DIR}' is not readable (bind-mount it or pass --pcm-baseline-dir)" >&2
        exit 1
    fi
    PCM_BASELINE_DIR="$(readlink -f -- "$PCM_BASELINE_DIR")" || exit 1
    if [[ "$PCM_MODE" == active ]]; then
        if [[ ! -f "$PCM_CONFIG" || ! -r "$PCM_CONFIG" ]]; then
            echo "Error: PCM config '${PCM_CONFIG}' not found or not readable" >&2
            exit 1
        fi
        PCM_CONFIG="$(readlink -f -- "$PCM_CONFIG")" || exit 1
    fi
fi

if [[ "$SAIM_MODE" == learn || "$PCM_MODE" == learn ]]; then
    if ! [[ "$LEARN_DURATION" =~ ^[0-9]+$ ]] || [[ "$LEARN_DURATION" -lt 1 || "$LEARN_DURATION" -gt 3600 ]]; then
        echo "Error: --learn-duration must be an integer in [1, 3600]" >&2
        exit 1
    fi
fi

# ACTIVE needs a per-sensor baseline. A learned <sensor>_baseline.cfg (from a prior
# LEARN run) always wins; when it is absent, SAIM falls back to the shipped
# <sensor>_baseline.cfg.default template. Only bail if NEITHER is present.
if [[ "$SAIM_MODE" == "active" ]]; then
    if [[ ! -r "$THRESHOLD_CONFIG" ]]; then
        echo "Error: thresholds.cfg not readable at '${THRESHOLD_CONFIG}'" >&2
        exit 1
    fi
    if [[ ! -d "$BASELINE_DIR" ]]; then
        echo "Error: SAIM baseline folder '${BASELINE_DIR}' does not exist (bind-mount it or pass --baseline-dir)" >&2
        exit 1
    fi
    if ! compgen -G "${BASELINE_DIR}/*_baseline.cfg" >/dev/null \
       && ! compgen -G "${BASELINE_DIR}/*_baseline.cfg.default" >/dev/null; then
        echo "Error: no SAIM baselines (*_baseline.cfg or *_baseline.cfg.default) found in '${BASELINE_DIR}'" >&2
        echo "       Run learn mode first (--saim-mode learn) to generate them" >&2
        exit 1
    fi
fi

if [[ "$SAIM_MODE" != skip ]]; then
    if [[ ! -x "${PSF_BIN}/safety_monitor" ]]; then
        echo "Error: safety_monitor is missing or not executable" >&2
        exit 1
    fi
    psf_prepare_monitor_log "$SAIM_LOG_FILE" || exit 1
fi
if [[ "$PCM_MODE" != skip ]]; then
    psf_prepare_monitor_log "$PCM_LOG_FILE" || exit 1
fi

if ! [[ "$HB_STALE_MS" =~ ^[0-9]+$ ]] || [[ "$HB_STALE_MS" -lt 100 || "$HB_STALE_MS" -gt 600000 ]]; then
    echo "Error: --hb_stale_ms must be an integer in [100, 600000]" >&2
    exit 1
fi

if ! [[ "$HB_PERIOD_MS" =~ ^[0-9]+$ ]] || [[ "$HB_PERIOD_MS" -lt 100 || "$HB_PERIOD_MS" -gt 600000 ]]; then
    echo "Error: --hb_period_ms must be an integer in [100, 600000]" >&2
    exit 1
fi

if [[ "$CONFIGURED_SDMS" == 1 ]]; then
    psf_sdm_check_log_inputs "$SENSOR_CONFIG" "$SENSOR_PIPELINES_CONFIG" \
        "$NVPSS_CONF" "$EVENT_MAPPING" "$THRESHOLD_CONFIG" "$PCM_CONFIG" || exit 1
fi
# Inside the container systemctl is a no-op shim; the entrypoint has already
# started rsyslogd.
if ! sudo -n systemctl start rsyslog; then
    echo "Error: Failed to start rsyslog service" >&2
    exit 1
fi

start_psf_process() {
    "$@" &
    local pid=$!
    BG_PIDS+=("$pid")

    # safety_monitor is wrapped in "sudo setpriv ... -- <binary>" on aarch64 to
    # attach device-node groups; report the real binary after the "--", not the
    # sudo wrapper. Every other caller passes the binary as $1.
    local name="$1"
    if [ "$name" = "sudo" ]; then
        local all="$*"
        name="${all##* -- }"
        name="${name%% *}"
    fi

    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "Error: Failed to start $name" >&2
        exit 1
    fi
    echo "$name started with PID $pid"
}

enter_baseline_dir() {
    if [[ ! -d "$BASELINE_DIR" ]]; then
        echo "Error: SAIM baseline folder '${BASELINE_DIR}' does not exist (bind-mount it or pass --baseline-dir)" >&2
        exit 1
    fi
    if [[ "$SAIM_MODE" == learn ]]; then
        sudo -n chown "$(id -u):$(id -g)" "$BASELINE_DIR" 2>/dev/null || true
        if [[ ! -w "$BASELINE_DIR" ]]; then
            echo "Error: SAIM baseline folder '${BASELINE_DIR}' is not writable for LEARN" >&2
            exit 1
        fi
    fi
    if ! cd "$BASELINE_DIR"; then
        echo "Error: cannot change into SAIM baseline folder '${BASELINE_DIR}'" >&2
        exit 1
    fi
}

# safety_monitor launch prefix. On aarch64 the GPU nodes are group-owned by host
# gids (video/render) that the non-root user isn't in, and the container
# entrypoint's runuser drop discards docker's --group-add. Re-attach exactly those
# gids (read off the injected nodes) via sudo+setpriv so SAIM can open them while
# staying non-root. x86 nodes are world-accessible, so no prefix is needed.
SAIM_LAUNCH=("${PSF_BIN}/safety_monitor")
if [[ "$SAIM_MODE" != skip && "$(uname -m)" == "aarch64" ]]; then
    SAIM_DEV_GIDS=$(stat -c '%g' /dev/nvmap /dev/dri/renderD128 2>/dev/null | sort -un | paste -sd,)
    if [[ -n "$SAIM_DEV_GIDS" ]]; then
        SAIM_LAUNCH=(sudo -n setpriv --reuid="$(id -u)" --regid="$(id -g)" --groups "$SAIM_DEV_GIDS"
                     -- "${PSF_BIN}/safety_monitor")
    fi
fi

PCM_LAUNCH=(env "PCM_MONITOR_CONFIG=${PCM_CONFIG}" "${PSF_BIN}/nv_perception_monitor")
PCM_ARGS=(--baseline-config "${PCM_BASELINE_DIR}/pcm_baseline.cfg")
if [[ -n "$KAFKA_BROKER" ]]; then PCM_ARGS+=(--kafka-broker "$KAFKA_BROKER"); fi
if [[ -n "$EVENT_MAPPING" && ( "$CONFIGURED_SDMS" == 1 || "$CONTROL_PLANE" == 0 ) ]]; then
    PCM_ARGS+=(--event-mapping "$EVENT_MAPPING")
    for PCM_SDM_CONFIG in "${SDM_CONFIGS[@]}"; do PCM_ARGS+=(--sdm-config "$PCM_SDM_CONFIG"); done
fi
# The container entrypoint's runuser drops docker's supplemental groups. Keep
# the current identity and re-attach the mounted Docker socket's group so the
# PCM container health check can query it.
if [[ "$PCM_MODE" == active && -S /var/run/docker.sock \
      && ( ! -r /var/run/docker.sock || ! -w /var/run/docker.sock ) ]]; then
    PCM_DOCKER_GID="$(stat -c '%g' /var/run/docker.sock)" || exit 1
    PCM_GROUP_IDS="$(id -G | tr ' ' ',')"
    PCM_LAUNCH=(sudo -n setpriv --reuid="$(id -u)" --regid="$(id -g)"
                --groups "${PCM_GROUP_IDS},${PCM_DOCKER_GID}" -- "${PCM_LAUNCH[@]}")
fi

# A failed monitor startup must also stop the control plane already started
# by this invocation. Preserve the failing exit status through cleanup.
trap 'shutdown "$?"' EXIT

if [[ "$PCM_MODE" == learn ]]; then
    # Only a dedicated baseline directory may have ownership adjusted.
    if [[ ! -w "$PCM_BASELINE_DIR" && "$PCM_BASELINE_DIR" != / \
          && "$PCM_BASELINE_DIR" != /opt && "$PCM_BASELINE_DIR" != /opt/nvidia \
          && "$PCM_BASELINE_DIR" != "$PSF_PREFIX" \
          && "$PCM_BASELINE_DIR" != "$PSF_BIN" ]]; then
        sudo -n chown "$(id -u):$(id -g)" "$PCM_BASELINE_DIR" 2>/dev/null || true
    fi
    if [[ ! -w "$PCM_BASELINE_DIR" ]]; then
        echo "Error: PCM baseline folder '${PCM_BASELINE_DIR}' is not writable for LEARN" >&2
        exit 1
    fi
fi

# Resolve SAIM's directory before the control plane changes the working directory.
if [[ "$SAIM_MODE" != skip ]]; then
    ORIGINAL_DIR="$PWD"
    enter_baseline_dir
    BASELINE_DIR="$PWD"
    cd "$ORIGINAL_DIR" || exit 1
fi

start_selected_monitors() {
    local original_dir="$PWD"
    local -a saim_args pcm_args
    if [[ "$SAIM_MODE" != skip ]]; then
        saim_args=(--mode "${SAIM_MODE^^}" --sensor-config "$SENSOR_CONFIG")
        if [[ "$SAIM_MODE" == learn ]]; then
            saim_args+=(--learn-duration "$LEARN_DURATION")
        else
            saim_args+=(--threshold-config "$THRESHOLD_CONFIG")
        fi
        if [[ -n "$GPU_ID" ]]; then
            saim_args+=(--gpu "$GPU_ID")
            export CUDA_VISIBLE_DEVICES="$GPU_ID"
        fi
        cd "$BASELINE_DIR" || return 1
        psf_start_monitor SAIM "$SAIM_MODE" "$SAIM_LOG_FILE" 0 \
            "${SAIM_LAUNCH[@]}" "${saim_args[@]}" || return 1
        cd "$original_dir" || return 1
    else
        echo "Skipping SAIM (--saim-mode skip)"
    fi
    if [[ "$PCM_MODE" != skip ]]; then
        pcm_args=(--mode "${PCM_MODE^^}" "${PCM_ARGS[@]}")
        if [[ "$PCM_MODE" == learn ]]; then pcm_args+=(--learn-duration "$LEARN_DURATION"); fi
        psf_start_monitor PCM "$PCM_MODE" "$PCM_LOG_FILE" 0 \
            "${PCM_LAUNCH[@]}" "${pcm_args[@]}" || return 1
    else
        echo "Skipping PCM (--pcm-mode skip)"
    fi
}

if [[ "$CONTROL_PLANE" == 0 ]]; then
    start_selected_monitors || exit 1
    psf_wait_for_learners || exit 1
    echo "All selected monitors completed LEARN successfully"
    exit 0
fi

# The gateway <-> SDM link is AF_UNIX SOCK_SEQPACKET and is not selectable, so
# there is nothing to pass: nvpsd_gateway and every SDM hardcode it.

# The rendezvous directory holds all three PSF sockets (nvpssd, nvpssd_to_psd
# and nvpsd_gateway). Three separate properties, checked rather than assumed,
# because each fails differently:
#
#   EXISTS      -- the transport does not create it, and the packaged
#                  tmpfiles.d entry only runs on a real boot, which a container
#                  does not have.
#   WRITABLE    -- bind() needs the write bit on the CONTAINING directory; a
#                  directory without it fails bind() with EACCES.
#   MODE 1777   -- matches usr/lib/tmpfiles.d/safety-core.conf. The sticky bit
#                  is NOT about bind: a 0777 directory binds perfectly well. It
#                  is what stops one process deleting another's socket, after
#                  which the path is free for an impostor to bind. Wrong mode is
#                  therefore a warning, not a failure.
if [[ -L /run/nvpsf ]]; then
    # A symlink passes -d when it points at a directory. Refused because this is
    # the rendezvous point for credential-validated IPC, and a redirected path
    # is not something a launcher should silently accept.
    echo "Error: /run/nvpsf is a symlink; refusing to use it as the PSF socket" >&2
    echo "       rendezvous point. Remove it and let tmpfiles.d or this script" >&2
    echo "       create a real directory." >&2
    exit 1
fi
# `sudo -n`, never a bare sudo: this launcher is an entrypoint and must not stop
# for a password prompt, which sudo writes to /dev/tty where 2>/dev/null does
# not hide it. The container image grants NOPASSWD, so -n costs nothing there.
#
# Plain first, sudo second: when this process already owns the directory -- a dev
# checkout, or a root runtime -- sudo is never invoked at all.
if [[ ! -d /run/nvpsf ]]; then
    mkdir -p /run/nvpsf 2>/dev/null || sudo -n mkdir -p /run/nvpsf 2>/dev/null || true
fi
if [[ ! -d /run/nvpsf ]]; then
    echo "Error: /run/nvpsf does not exist and could not be created; the PSS" >&2
    echo "       daemon and gateway have nowhere to bind their sockets" >&2
    exit 1
fi
# Unconditional, because a directory that ALREADY existed with the wrong mode
# must be corrected too. Best-effort, because a non-root process cannot chmod a
# root-owned directory even when the mode is already correct. The verification
# below is what decides.
chmod 1777 /run/nvpsf 2>/dev/null || sudo -n chmod 1777 /run/nvpsf 2>/dev/null || true

NVPSF_DIR_MODE=$(stat -c %a /run/nvpsf 2>/dev/null || echo "unknown")
if [[ ! -w /run/nvpsf || ! -x /run/nvpsf ]]; then
    echo "Error: /run/nvpsf is not writable by $(id -un) (mode ${NVPSF_DIR_MODE}," >&2
    echo "       owner $(stat -c %U:%G /run/nvpsf 2>/dev/null || echo unknown))." >&2
    echo "       PSF processes bind their AF_UNIX sockets in this directory and" >&2
    echo "       would fail at bind() with EACCES. Fix on the host with:" >&2
    echo "         sudo chmod 1777 /run/nvpsf" >&2
    exit 1
fi
if [[ "$NVPSF_DIR_MODE" != "1777" ]]; then
    echo "Warning: /run/nvpsf mode is ${NVPSF_DIR_MODE}, expected 1777 (see" >&2
    echo "         usr/lib/tmpfiles.d/safety-core.conf). Sockets will bind, but without" >&2
    echo "         the sticky bit one process can delete another's socket." >&2
fi

# SDMs use relative log filenames. Start the control plane from LOG_DIR so
# atl_sdm.log, atl_proximity_sdm.log, and pxc_sdm.log are written under
# /var/log/psf.
if ! cd "${LOG_DIR}"; then
    echo "Error: cannot enter ${LOG_DIR} for component logs" >&2
    exit 1
fi

# Configured ATL/PXC instances share one control plane and one MDX mapping.
if [[ "$CONFIGURED_SDMS" == 1 ]]; then
    if [[ "$SDM_EXTERNAL" != 1 ]]; then
        # Fail before starting the control plane if a requested native log
        # cannot be created. Custom parent directories must already exist.
        for SDM_LOG in "${SDM_LOGS[@]}"; do
            if ! touch -- "$SDM_LOG"; then
                echo "Error: cannot create SDM log '$SDM_LOG'" >&2
                exit 1
            fi
        done
    fi
    echo "Starting configured SDM application: $APP"
    # Daemon first: it owns the socket the gateway connects to.
    start_psf_process "${PSF_BIN}/nvpss_daemon"
    start_psf_process "${PSF_BIN}/nvpsd_gateway"
    if [[ "$SDM_EXTERNAL" == 1 ]]; then
        echo "SDMs run outside this launcher; skipping local SDM startup"
    else
        for ((SDM_INDEX=0; SDM_INDEX<${#SDM_APPS[@]}; ++SDM_INDEX)); do
            SDM_BINARY="$(psf_sdm_binary "${SDM_APPS[$SDM_INDEX]}" "$PSF_APPS")" || exit 1
            # Event-driven decisions, periodic repeat off.
            SDM_ARGS=(--decision_interval_ms 0
                      --hb_stale_ms "$HB_STALE_MS" --hb_period_ms "$HB_PERIOD_MS"
                      --config "${SDM_CONFIGS[$SDM_INDEX]}"
                      --log_file "${SDM_LOGS[$SDM_INDEX]}"
                      --cmd_rx_port "${SDM_PORTS[$SDM_INDEX]}")
            if [[ -n "$CMD_RX_IP" ]]; then SDM_ARGS+=(--cmd_rx_ip "$CMD_RX_IP"); fi
            start_psf_process "$SDM_BINARY" "${SDM_ARGS[@]}"
        done
    fi
    MDX_CLIENT_ARGS=(--config "$EVENT_MAPPING")
    while IFS= read -r _arg; do MDX_CLIENT_ARGS+=("$_arg"); done < <(mdx_identity_args)
    if [[ -n "$KAFKA_BROKER" ]]; then MDX_CLIENT_ARGS+=(--broker "$KAFKA_BROKER"); fi
    start_psf_process "${PSF_APPS}/mdx-client/mdx_client" "${MDX_CLIENT_ARGS[@]}"
elif [[ "$APP" == "cjs" ]]; then
    echo "Starting Crossing Junction Safety application..."
    # Daemon first: it owns the PSS->PSD AF_UNIX socket that nvpsd_gateway
    # connects to, and connect() fails until the daemon has bound and listened.
    start_psf_process "${PSF_BIN}/nvpss_daemon"
    start_psf_process "${PSF_BIN}/nvpsd_gateway"
    SDM_ARGS=()
    if [[ -n "$CMD_RX_IP" ]]; then
        SDM_ARGS+=(--cmd_rx_ip "$CMD_RX_IP")
    fi
    if [[ -n "$CMD_RX_PORT" ]]; then
        SDM_ARGS+=(--cmd_rx_port "$CMD_RX_PORT")
    fi
    SDM_ARGS+=(--decision_interval_ms 0)
    SDM_ARGS+=(--hb_stale_ms "$HB_STALE_MS" --hb_period_ms "$HB_PERIOD_MS")
    start_psf_process "${PSF_APPS}/crossing-junction-safety-monitor/crossing_junction_safety_monitor_sdm" "${SDM_ARGS[@]}"
    MDX_CLIENT_ARGS=(--config "${PSF_APPS}/crossing-junction-safety-monitor/crossing_junction_safety_event_mapping.pb.txt")
    # The helper supplies the PSS instance id: without it the client registers
    # anonymously, and a site whose nvpss.conf requires instance 1 latches every
    # SDM once the heartbeat budget runs out (never_registered). It also picks
    # the 3D identity file, where a literal --sensor-config would leave every
    # group-addressed event unresolvable.
    while IFS= read -r _arg; do MDX_CLIENT_ARGS+=("$_arg"); done < <(mdx_identity_args)
    if [[ -n "$KAFKA_BROKER" ]]; then
        MDX_CLIENT_ARGS+=(--broker "$KAFKA_BROKER")
    fi
    start_psf_process "${PSF_APPS}/mdx-client/mdx_client" "${MDX_CLIENT_ARGS[@]}"
elif [[ "$APP" == "atl_proximity" ]]; then
    # Combined ATL + forklift-to-person proximity for the ATL dataset.
    #
    # Two SDMs share one gateway and one mdx_client. The single event mapping
    # event_mapping_atl_proximity.pb.txt carries both event sets, and each SDM
    # registers with the gateway only for the event types it consumes, so each
    # sees its own slice:
    #   atl_sdm            EVENT_0..EVENT_5   (ATL, reused unmodified)
    #   atl_proximity_sdm  EVENT_8..EVENT_10  (forklift/person proximity)
    #
    # Only one mdx_client may run: it is the sole Kafka consumer and publishes
    # into PSS, which fans out to both SDMs via the gateway.
    echo "Starting combined ATL + forklift/person proximity application..."
    start_psf_process "${PSF_BIN}/nvpss_daemon"
    start_psf_process "${PSF_BIN}/nvpsd_gateway"

    COMMON_SDM_ARGS=(--decision_interval_ms 0
                     --hb_stale_ms "$HB_STALE_MS" --hb_period_ms "$HB_PERIOD_MS")
    if [[ -n "$CMD_RX_IP" ]]; then
        COMMON_SDM_ARGS+=(--cmd_rx_ip "$CMD_RX_IP")
    fi

    # zone1.conf is EVENT_0..EVENT_5 plus the three reviewed pipelines, which is
    # exactly the binding this combined deployment's ATL half uses. There is
    # only one ATL zone here. Both ports are the resolved values compared above.
    ATL_SDM_ARGS=("${COMMON_SDM_ARGS[@]}" --config "$ATL_ZONE1_CONF"
                  --cmd_rx_port "$ATL_EFFECTIVE_CMD_RX_PORT")
    start_psf_process "${PSF_APPS}/atl/atl_sdm" "${ATL_SDM_ARGS[@]}"

    PXC_SDM_ARGS=("${COMMON_SDM_ARGS[@]}"
                  --cmd_rx_port "$PROXIMITY_EFFECTIVE_CMD_RX_PORT")
    if [[ -n "$PAIR_TTL_MS" ]]; then
        PXC_SDM_ARGS+=(--pair_ttl_ms "$PAIR_TTL_MS")
    fi
    start_psf_process "${PSF_APPS}/atl_proximity/atl_proximity_sdm" "${PXC_SDM_ARGS[@]}"

    MDX_CLIENT_ARGS=(--config "${PSF_APPS}/atl_proximity/event_mapping_atl_proximity.pb.txt")
    while IFS= read -r _arg; do MDX_CLIENT_ARGS+=("$_arg"); done < <(mdx_identity_args)
    if [[ -n "$KAFKA_BROKER" ]]; then
        MDX_CLIENT_ARGS+=(--broker "$KAFKA_BROKER")
    fi
    start_psf_process "${PSF_APPS}/mdx-client/mdx_client" "${MDX_CLIENT_ARGS[@]}"
fi

start_selected_monitors || exit 1
sleep 1
psf_poll_monitors || exit 1

echo "All processes started. Waiting for shutdown signal..."

while true; do
    psf_poll_monitors || shutdown 1
    for pid in "${BG_PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Process $pid has exited unexpectedly"
            shutdown 1
        fi
    done

    # Sleep briefly and let signals interrupt.
    sleep 1 &
    wait $!
done
