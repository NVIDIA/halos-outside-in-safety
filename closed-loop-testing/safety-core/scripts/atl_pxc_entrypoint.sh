#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Writes the one MDX mapping that --app atl,pxc gives mdx_client, then runs the
# image's entrypoint unchanged.
#
# The image's mixed ATL + PXC mapping is the file safety-core.yml mounts our
# proximity tiers over, so inside the container that path holds proximity rules
# only (EVENT_12..14). The ATL rules (EVENT_0..11) are taken from the image's
# own ATL mapping and joined in front of them here, so they stay the image's.
set -e

ATL_MAPPING=/opt/nvidia/psf/apps/atl/event_mapping_atl.pb.txt
PXC_MAPPING=/opt/nvidia/psf/apps/proximity/proximity_event_mapping.pb.txt
MAPPING=/tmp/event_mapping_atl_pxc.pb.txt

for f in "$ATL_MAPPING" "$PXC_MAPPING"; do
    [[ -r "$f" ]] || { echo "atl_pxc_entrypoint: mapping '$f' not readable" >&2; exit 1; }
done
cat "$ATL_MAPPING" "$PXC_MAPPING" > "$MAPPING"
chmod 644 "$MAPPING"
echo "atl_pxc_entrypoint: $MAPPING: $(grep -c '^rules {' "$MAPPING") rules (ATL + proximity)"

exec /usr/local/bin/entrypoint.sh "$@"
