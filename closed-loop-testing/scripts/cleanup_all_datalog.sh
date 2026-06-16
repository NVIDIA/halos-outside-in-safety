#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Halos Outside-In Safety — data-log cleanup (profile-aware)
#
# Truncates per-run logs (comm-layer + psf-log) for the chosen profile.
# Reads MDX_DATA_DIR from the profile env — same source as setup.sh.
#
# Usage:
#   ./scripts/cleanup_all_datalog.sh <base|sil|hil>   # reads ../../deployments/profiles/<profile>.env

set -e

PROFILE="${1:-}"
if [ -z "$PROFILE" ]; then
    echo "ERROR: profile required. Usage: ./scripts/cleanup_all_datalog.sh <base|sil|hil>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"   # halos-outside-in-safety/
ENV_FILE="$PROJECT_ROOT/deployments/profiles/${PROFILE}.env"

echo "============================================================"
echo "  Halos Outside-In Safety — data cleanup (profile: $PROFILE)"
echo "============================================================"
echo ""

# Load profile env
if [ -f "$ENV_FILE" ]; then
    echo "Loading configuration from $ENV_FILE"
    set -a
    source "$ENV_FILE"
    set +a
else
    echo "ERROR: env file not found: $ENV_FILE"
    exit 1
fi

# Validate MDX_DATA_DIR
if [ -z "$MDX_DATA_DIR" ]; then
    echo "ERROR: MDX_DATA_DIR is not set in .env"
    exit 1
fi

echo ""
echo "Configuration:"
echo "  MDX_DATA_DIR: $MDX_DATA_DIR"
echo ""

# =============================================================================
# Clean Communication Layer directories
# =============================================================================
echo "Cleaning Communication Layer directories..."
COMM_LOG_DIR="${COMM_LOG_DIR:-$MDX_DATA_DIR/comm-layer}"

if [ -d "$COMM_LOG_DIR" ]; then
    echo "  Removing all files in: $COMM_LOG_DIR"
    sudo rm -rf "$COMM_LOG_DIR"/*
    echo "  Communication Layer directory cleaned"
else
    echo "  Communication Layer directory not found: $COMM_LOG_DIR"
fi

# =============================================================================
# Clean PSF log files
# =============================================================================
echo "Cleaning PSF log files..."
PSF_LOG_DIR="${PSF_LOG_DIR:-$MDX_DATA_DIR/psf-log}"

if [ -d "$PSF_LOG_DIR" ]; then
    PSF_LOG_FILE="$PSF_LOG_DIR/pss.log"
    if [ -f "$PSF_LOG_FILE" ]; then
        echo "  Clearing data in: $PSF_LOG_FILE"
        sudo truncate -s 0 "$PSF_LOG_FILE"
        echo "  PSF log file cleared"
    else
        echo "  PSF log file not found: $PSF_LOG_FILE"
    fi
    
    # Remove any other files in PSF log directory
    echo "  Removing other files in: $PSF_LOG_DIR"
    sudo find "$PSF_LOG_DIR" -type f ! -name "pss.log" -delete
    echo "  Other PSF files removed"
else
    echo "  PSF log directory not found: $PSF_LOG_DIR"
fi

echo ""
echo "============================================================"
echo "Cleanup completed successfully!"
echo ""
echo "Cleaned directories:"
echo "  $COMM_LOG_DIR"
echo "  $PSF_LOG_DIR"
echo ""
echo "Note: The directories and pss.log file are preserved, only their contents have been removed."
echo "============================================================"
