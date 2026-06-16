#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

INSTALL_PREFIX="/opt/nvidia/psf"
BIN_DIR="${INSTALL_PREFIX}/bin"
APPS_DIR="${INSTALL_PREFIX}/apps"
CONFIG_DIR="${INSTALL_PREFIX}/configs"

APP=""
SENSOR_CONFIG="${CONFIG_DIR}/sensor_config.conf"
KAFKA_BROKER=""
CMD_RX_IP=""
CMD_RX_PORT=""

declare -a BG_PIDS=()

usage() {
    cat <<USAGE
Usage: $(basename "$0") --app <atl|proximity|pxc> [options]

Options:
  --sensor-config <file>   Sensor config mounted or installed in the container
  --broker <address>       Kafka broker passed to mdx_client
  --cmd_rx_ip <ip>         Command receiver IP passed to the SDM app
  --cmd_rx_port <port>     Command receiver port passed to the SDM app
  -h, --help               Show this help
USAGE
}

shutdown() {
    for pid in "${BG_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done

    sleep 1

    for pid in "${BG_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
}

trap 'shutdown; exit 143' SIGTERM
trap 'shutdown; exit 130' SIGINT

require_arg() {
    if [[ $# -lt 2 || "$2" == --* ]]; then
        echo "Error: $1 requires an argument" >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app) require_arg "$@"; APP="$2"; shift 2 ;;
        --sensor-config) require_arg "$@"; SENSOR_CONFIG="$2"; shift 2 ;;
        --broker) require_arg "$@"; KAFKA_BROKER="$2"; shift 2 ;;
        --cmd_rx_ip) require_arg "$@"; CMD_RX_IP="$2"; shift 2 ;;
        --cmd_rx_port) require_arg "$@"; CMD_RX_PORT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ "$APP" == "pxc" ]]; then
    APP="proximity"
fi

if [[ "$APP" != "atl" && "$APP" != "proximity" ]]; then
    echo "Error: --app must be atl, proximity, or pxc" >&2
    exit 1
fi

if [[ ! -f "$SENSOR_CONFIG" ]]; then
    echo "Error: sensor config not found: $SENSOR_CONFIG" >&2
    exit 1
fi

mkdir -p /run/nvpsf
chmod 1777 /run/nvpsf 2>/dev/null || true

start_process() {
    "$@" &
    local pid=$!
    BG_PIDS+=("$pid")
    echo "Started $1 with PID $pid"
}

start_process "${BIN_DIR}/nvpsd_gateway"
start_process "${BIN_DIR}/nvpss_daemon"

SDM_ARGS=()
if [[ -n "$CMD_RX_IP" ]]; then
    SDM_ARGS+=(--cmd_rx_ip "$CMD_RX_IP")
fi
if [[ -n "$CMD_RX_PORT" ]]; then
    SDM_ARGS+=(--cmd_rx_port "$CMD_RX_PORT")
fi

if [[ "$APP" == "atl" ]]; then
    start_process "${APPS_DIR}/atl/atl_sdm" "${SDM_ARGS[@]}"
    MDX_CONFIG="${APPS_DIR}/atl/event_mapping_atl.pb.txt"
else
    start_process "${APPS_DIR}/proximity/proximity_sdm" "${SDM_ARGS[@]}"
    MDX_CONFIG="${APPS_DIR}/proximity/proximity_event_mapping.pb.txt"
fi

MDX_ARGS=(--config "$MDX_CONFIG" --sensor-config "$SENSOR_CONFIG")
if [[ -n "$KAFKA_BROKER" ]]; then
    MDX_ARGS+=(--broker "$KAFKA_BROKER")
fi
start_process "${APPS_DIR}/mdx-client/mdx_client" "${MDX_ARGS[@]}"

while true; do
    for pid in "${BG_PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Process $pid exited unexpectedly" >&2
            shutdown
            exit 1
        fi
    done
    sleep 1
done
