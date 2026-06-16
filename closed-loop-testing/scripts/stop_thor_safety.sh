#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Halos Outside-In Safety — stop the Safety Core on an IGX Thor device.
#
# Sends SIGTERM to launch_hoisa.sh so its trap handler cleans up the host binaries and the
# nv-psf container; falls back to explicit kills. Uses `pkill -x <basename>` (exact match),
# never `pkill -f <pattern>` — over SSH the latter can match this shell's own argv and kill
# the session. Leaves the nvFsiCom daemon running (start/stop it separately for FSI).
#
# Usage:
#   bash closed-loop-testing/scripts/stop_thor_safety.sh

echo "=== Halos Thor Safety Core stop ==="

# 1. Graceful: SIGTERM launch_hoisa.sh → its trap cleans up children + nv-psf
if pgrep -x launch_hoisa.sh >/dev/null; then
    echo "SIGTERM launch_hoisa.sh ..."
    sudo pkill -TERM -x launch_hoisa.sh || true
    for _ in $(seq 1 10); do pgrep -x launch_hoisa.sh >/dev/null || break; sleep 1; done
fi

# 2. Fallback: explicit kills (exact basename) if the trap did not finish
for p in atl_sdm proximity_sdm safety_monitor fsicom-agent atl_sdm_cmd_receiver; do
    sudo pkill -x "$p" 2>/dev/null || true
done

# 3. Stop the nv-psf container
sudo docker stop nv-psf 2>/dev/null || true
sudo docker rm nv-psf 2>/dev/null || true

# 4. Verify
sleep 1
REMAIN=$(ps -eo comm | grep -cE '^(launch_hoisa\.sh|atl_sdm|proximity_sdm|safety_monitor|fsicom-agent)$' || true)
if [ "$REMAIN" -eq 0 ] && [ -z "$(sudo docker ps -q -f name=nv-psf)" ]; then
    echo "Stopped."
else
    echo "WARNING: some processes/containers may still be alive:"
    ps -eo pid,comm | grep -E '(launch_hoisa|atl_sdm|safety_monitor|fsicom)' | grep -v grep || true
    sudo docker ps -f name=nv-psf || true
fi
