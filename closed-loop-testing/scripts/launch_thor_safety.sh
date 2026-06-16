#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Halos Outside-In Safety — launch the Safety Core on an IGX Thor device.
#
# On Thor the Safety Core is hybrid (nv-psf container + host binaries) and is orchestrated by
# /opt/nvidia/psf/bin/launch_hoisa.sh, not by docker compose. This helper reads a profile env
# (deployments/profiles/<profile>.env) and invokes launch_hoisa.sh with the matching flags.
# SDM_TARGET selects CCPLEX vs FSI. The same launcher path serves the base (overlay) profile
# and the HIL safety host. See skills/hoisa-deploy-profile/references/halos_thor.md.
#
# Usage:
#   bash closed-loop-testing/scripts/launch_thor_safety.sh <profile>   # e.g. base-thor

set -e

PROFILE="${1:-}"
if [ -z "$PROFILE" ]; then
    echo "ERROR: profile required. Usage: launch_thor_safety.sh <profile>  (e.g. base-thor)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"   # halos-outside-in-safety/
ENV_FILE="$PROJECT_ROOT/deployments/profiles/${PROFILE}.env"

[ -f "$ENV_FILE" ] || { echo "ERROR: env file not found: $ENV_FILE"; exit 1; }
echo "Loading $ENV_FILE"
set -a; source "$ENV_FILE"; set +a

# --- Required vars ---
for v in HOST_IP PSF_IMAGE PSF_CMD_RX_IP PSF_CMD_RX_PORT PSF_SENSOR_CONFIG; do
    [ -z "${!v}" ] && { echo "ERROR: $v not set in $ENV_FILE"; exit 1; }
done
case "$PSF_IMAGE" in
    *'<'*'>'*) echo "ERROR: PSF_IMAGE still has a placeholder tag ($PSF_IMAGE). Set the aarch64 image first."; exit 1 ;;
esac

# --- Safety Core host install present? ---
LAUNCHER=/opt/nvidia/psf/bin/launch_hoisa.sh
[ -x "$LAUNCHER" ] || { echo "ERROR: $LAUNCHER not found — install the Safety Core Tegra package first (PSF docs HOISA User Guide §1)."; exit 1; }
[ -f "$PSF_SENSOR_CONFIG" ] || { echo "ERROR: sensor config not found: $PSF_SENSOR_CONFIG (copy the template + fill the VST URLs)."; exit 1; }

SDM_TARGET="${SDM_TARGET:-ccplex}"
PSF_LAUNCH_MODE="${PSF_LAUNCH_MODE:-active}"
KAFKA_BROKER="${KAFKA_BROKER:-localhost:9092}"

# --- FSI prerequisite: nvFsiCom daemon must already be running (the launcher does NOT start it) ---
if [ "$SDM_TARGET" = "fsi" ]; then
    if ! pgrep -x nvFsiCom >/dev/null; then
        echo "ERROR: SDM_TARGET=fsi but the nvFsiCom daemon is not running. Start it first:"
        echo "   sudo /opt/nvidia/ccplex_sf/fsi_ccplex_com/nvFsiCom &"
        echo "(FSI also requires the HOISA FSI firmware reflashed — see halos_thor.md §5B.)"
        exit 1
    fi
fi

echo "=== Halos Thor Safety Core launch ==="
echo "  Profile:    $PROFILE"
echo "  Mode:       $PSF_LAUNCH_MODE"
echo "  SDM target: $SDM_TARGET"
echo "  Cmd sink:   ${PSF_CMD_RX_IP}:${PSF_CMD_RX_PORT}"
echo "  Kafka:      $KAFKA_BROKER"
echo ""

# launch_hoisa.sh runs `docker run nv-psf` + spawns the host SDM (ccplex) or the fsicom-agent
# bridge (fsi) + the AI monitor, and installs its own signal-trap cleanup. Stop with
# stop_thor_safety.sh. For fsi, see the fsicom-agent relay-flags note in halos_thor.md §5B.
exec sudo "$LAUNCHER" \
    --mode "$PSF_LAUNCH_MODE" --app atl \
    --sdm-target "$SDM_TARGET" \
    --sensor-config "$PSF_SENSOR_CONFIG" \
    --docker-image "$PSF_IMAGE" \
    --cmd-rx-ip "$PSF_CMD_RX_IP" --cmd-rx-port "$PSF_CMD_RX_PORT" \
    --kafka-broker "$KAFKA_BROKER"
