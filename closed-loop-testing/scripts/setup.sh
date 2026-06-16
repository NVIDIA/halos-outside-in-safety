#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Halos Outside-In Safety — deployment setup (profile-aware data dirs)
#
# Creates ONLY the data directories the chosen profile actually uses:
#   base : psf-log                          (safety-core; VST overlay; no Isaac/comm-layer)
#   sil  : psf-log + comm-layer + isaac-cache  (full single-host: +Isaac Sim +comm-layer)
#   hil  : comm-layer + isaac-cache          (x86 stimulus; PSF on Thor, no psf-log here)
#
# base does NOT need isaac-cache (17G) nor the NGC sil-data pull (Isaac scenes/collected-assets).
# sil-data (nvidia/halos-outside-in/sample-sil-data) is a SIL-only prerequisite — pull separately.
#
# Usage:
#   ./scripts/setup.sh <base|sil|hil>     # reads ../../deployments/profiles/<profile>.env

set -e

PROFILE="${1:-}"
if [ -z "$PROFILE" ]; then
    echo "ERROR: profile required. Usage: ./scripts/setup.sh <base|sil|hil>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"   # halos-outside-in-safety/
ENV_FILE="$PROJECT_ROOT/deployments/profiles/${PROFILE}.env"

echo "============================================================"
echo "  Halos Outside-In Safety — setup (profile: $PROFILE)"
echo "============================================================"

# Which services' data dirs does this profile need?
case "$PROFILE" in
    base) NEED_PSF=1 ;;
    sil)  NEED_PSF=1; NEED_COMM=1; NEED_ISAAC=1 ;;
    hil)  NEED_COMM=1; NEED_ISAAC=1 ;;
    *) echo "ERROR: unknown profile '$PROFILE' (base|sil|hil)"; exit 1 ;;
esac

# Load profile env
if [ ! -f "$ENV_FILE" ]; then
    echo "ERROR: env file not found: $ENV_FILE"
    exit 1
fi
echo "Loading $ENV_FILE"
set -a; source "$ENV_FILE"; set +a

# Validate MDX_DATA_DIR (catch unedited template)
if [ -z "$MDX_DATA_DIR" ]; then
    echo "ERROR: MDX_DATA_DIR not set in $ENV_FILE"; exit 1
fi
case "$MDX_DATA_DIR" in
    /path/to/*) echo "ERROR: MDX_DATA_DIR is still the template placeholder ($MDX_DATA_DIR). Edit $ENV_FILE first."; exit 1 ;;
esac
echo "  MDX_DATA_DIR: $MDX_DATA_DIR"
echo ""

sudo mkdir -p "$MDX_DATA_DIR"

# PSF log (base, sil) — safety-core.yml mounts pss.log as a FILE, must pre-exist
if [ -n "$NEED_PSF" ]; then
    PSF_LOG_DIR="${PSF_LOG_DIR:-$MDX_DATA_DIR/psf-log}"
    echo "PSF: $PSF_LOG_DIR (+ pss.log)"
    sudo mkdir -p "$PSF_LOG_DIR"
    sudo touch "$PSF_LOG_DIR/pss.log"
    sudo chmod -R 777 "$PSF_LOG_DIR"
fi

# Communication Layer (sil, hil)
if [ -n "$NEED_COMM" ]; then
    COMM_LOG_DIR="${COMM_LOG_DIR:-$MDX_DATA_DIR/comm-layer}"
    echo "comm-layer: $COMM_LOG_DIR"
    sudo mkdir -p "$COMM_LOG_DIR"
    sudo chmod -R 777 "$COMM_LOG_DIR"
fi

# Isaac Sim cache (sil, hil) — container runs as user 1234:1234
if [ -n "$NEED_ISAAC" ]; then
    ISAAC_CACHE_DIR="${ISAAC_CACHE_DIR:-$MDX_DATA_DIR/isaac-cache}"
    echo "isaac-cache: $ISAAC_CACHE_DIR (chown 1234:1234)"
    for d in cache/ov cache/warp kit-cache data nvidia-omniverse nv; do
        sudo mkdir -p "$ISAAC_CACHE_DIR/$d"
    done
    sudo chown -R 1234:1234 "$ISAAC_CACHE_DIR"
    echo "  NOTE: sil also needs NGC sil-data (Isaac scenes/collected-assets) — pull separately:"
    echo "    ngc registry resource download-version nvidia/halos-outside-in/sample-sil-data:v1.2.1"
fi

echo ""
echo "============================================================"
echo "Setup complete (profile: $PROFILE)"
echo "============================================================"
