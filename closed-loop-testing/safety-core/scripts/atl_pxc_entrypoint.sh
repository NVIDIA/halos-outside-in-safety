#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# The image's entrypoint with one change: it hands off to launch_atl_pxc.sh
# instead of launch_psf.sh. Its docker-group, cron and rsyslog setup is reused
# as shipped rather than copied, so it cannot drift from the image.
set -e

SRC=/usr/local/bin/entrypoint.sh
DST=/tmp/entrypoint_atl_pxc.sh
sed 's#/opt/nvidia/psf/bin/launch_psf.sh#/opt/hoisa/launch_atl_pxc.sh#' "$SRC" > "$DST"
if ! grep -q '/opt/hoisa/launch_atl_pxc.sh' "$DST"; then
    echo "atl_pxc_entrypoint: $SRC no longer execs launch_psf.sh; cannot swap the launcher" >&2
    exit 1
fi
exec bash "$DST" "$@"
