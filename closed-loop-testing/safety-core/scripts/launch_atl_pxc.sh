#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# ATL and proximity side by side in one PSF container (PSF_APP=both).
#
# The image's launch_psf.sh starts one decision-maker per container. Running a
# second container instead does not work: both would bind the gateway's UDP port
# on the host network, and mdx_client joins the fixed Kafka consumer groups
# mdx_client_events / mdx_client_frames, so two of them split the frames
# between each other. What can be shared is shared:
#
#   nvpsd_gateway   one; up to 10 SDMs register with it, each for its own events
#   nvpss_daemon    one
#   mdx_client      one, on the ATL mapping and the proximity mapping concatenated
#   atl_sdm         EVENT_0..5  -> 0xA2 packets
#   proximity_sdm   EVENT_8..10 -> 0xA5 packets
#
# Both SDMs send to the same --cmd_rx port; comm-layer tells them apart by the
# packet identifier. SAIM is not wired here: --saim-mode must be skip.
#
# Run as the nvidia user, from atl_pxc_entrypoint.sh.

set -u

PSF=/opt/nvidia/psf
ATL_MAPPING=$PSF/apps/atl/event_mapping_atl.pb.txt
PXC_MAPPING=$PSF/apps/proximity/proximity_event_mapping.pb.txt
MAPPING=/tmp/event_mapping_atl_pxc.pb.txt

APP="" SAIM_MODE="" CMD_RX_IP="" CMD_RX_PORT="" SENSOR_CONFIG="" KAFKA_BROKER=""
HB_STALE_MS=5000 HB_PERIOD_MS=5500
while [[ $# -gt 0 ]]; do
    case $1 in
        --app)           APP="$2"; shift 2 ;;
        --saim-mode)     SAIM_MODE="$2"; shift 2 ;;
        --cmd_rx_ip)     CMD_RX_IP="$2"; shift 2 ;;
        --cmd_rx_port)   CMD_RX_PORT="$2"; shift 2 ;;
        --sensor-config) SENSOR_CONFIG="$2"; shift 2 ;;
        --broker)        KAFKA_BROKER="$2"; shift 2 ;;
        --hb_stale_ms)   HB_STALE_MS="$2"; shift 2 ;;
        --hb_period_ms)  HB_PERIOD_MS="$2"; shift 2 ;;
        *) echo "launch_atl_pxc: unknown option '$1'" >&2; exit 1 ;;
    esac
done

[[ "$APP" == "both" ]] || { echo "launch_atl_pxc: --app must be 'both', got '$APP'" >&2; exit 1; }
[[ "$SAIM_MODE" == "skip" ]] || { echo "launch_atl_pxc: only --saim-mode skip is supported, got '$SAIM_MODE'" >&2; exit 1; }
[[ -r "$SENSOR_CONFIG" ]] || { echo "launch_atl_pxc: sensor config '$SENSOR_CONFIG' not readable" >&2; exit 1; }
for f in "$ATL_MAPPING" "$PXC_MAPPING"; do
    [[ -r "$f" ]] || { echo "launch_atl_pxc: mapping '$f' not readable" >&2; exit 1; }
done

# rsyslog writes /var/log/pss.log; launch_psf.sh starts it the same way.
sudo systemctl start rsyslog || { echo "launch_atl_pxc: failed to start rsyslog" >&2; exit 1; }

cat "$ATL_MAPPING" "$PXC_MAPPING" > "$MAPPING"
echo "Mapping $MAPPING: $(grep -c '^rules {' "$MAPPING") rules (ATL + proximity)"

PIDS=()
start() {
    "$@" &
    local pid=$!
    PIDS+=("$pid")
    sleep 1
    kill -0 "$pid" 2>/dev/null || { echo "launch_atl_pxc: $1 failed to start" >&2; stop 1; }
    echo "$1 started with PID $pid"
}
stop() {
    for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null; done
    sleep 2
    for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null; done
    exit "${1:-143}"
}
trap 'stop 143' TERM INT

SDM_ARGS=(--decision_interval_ms 0 --hb_stale_ms "$HB_STALE_MS" --hb_period_ms "$HB_PERIOD_MS")
[[ -n "$CMD_RX_IP" ]] && SDM_ARGS+=(--cmd_rx_ip "$CMD_RX_IP")
[[ -n "$CMD_RX_PORT" ]] && SDM_ARGS+=(--cmd_rx_port "$CMD_RX_PORT")
MDX_ARGS=(--config "$MAPPING" --sensor-config "$SENSOR_CONFIG")
[[ -n "$KAFKA_BROKER" ]] && MDX_ARGS+=(--broker "$KAFKA_BROKER")

echo "Starting ATL + Proximity Control applications..."
start $PSF/bin/nvpsd_gateway
start $PSF/bin/nvpss_daemon
start $PSF/apps/atl/atl_sdm "${SDM_ARGS[@]}"
start $PSF/apps/proximity/proximity_sdm "${SDM_ARGS[@]}"
start $PSF/apps/mdx-client/mdx_client "${MDX_ARGS[@]}"
echo "All processes started."

# One process gone is the whole app gone: exit so the restart policy brings
# the container back as a unit.
while true; do
    for pid in "${PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "launch_atl_pxc: process $pid exited" >&2
            stop 1
        fi
    done
    sleep 1 & wait $!
done
