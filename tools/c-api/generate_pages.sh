#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Regenerate the C API reference pages in docs/c-api/ from the headers published in safety-core/.
#
# Usage: tools/c-api/generate_pages.sh
#
# headers.tsv names the integrator headers. Each one found under safety-core/ is copied into a flat
# directory, as the development package installs them in /opt/nvidia/psf/include; Doxygen reads them
# and writes XML, and doxyxml_to_mdx.py writes one Fern page per header. The docs list every page in
# docs/c-api/ under API Documentation, so adding or removing a header needs no navigation change.
# The C API reference workflow runs this when a change to the headers is merged and opens a pull
# request with the result.
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
REPO="$(readlink -f "$HERE/../..")"
OUT="$REPO/docs/c-api"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/include"
mkdir -p "$SRC"

command -v doxygen >/dev/null 2>&1 || { echo "error: doxygen is not installed" >&2; exit 1; }

RELEASE="$(sed -n 's/^tag:[[:space:]]*//p' "$REPO/release_metadata.yaml" | head -n 1)"

found=0
while IFS=$'\t' read -r name _ _; do
    [[ -z "${name}" || "${name}" == \#* ]] && continue
    path="$(find "$REPO/safety-core" -name "$name" -type f -print -quit)"
    if [[ -z "$path" ]]; then
        echo "not published in safety-core/, skipped: $name" >&2
        continue
    fi
    cp "$path" "$SRC/"
    found=$((found + 1))
done < "$HERE/headers.tsv"
(( found > 0 )) || { echo "error: none of the headers in headers.tsv exist under safety-core/" >&2; exit 1; }

(cd "$HERE" && DOXY_SRC="$SRC" DOXY_OUT="$WORK" doxygen Doxyfile)
if [ -s "$WORK/warnings.log" ]; then
    echo "Doxygen warnings:" >&2
    sed "s|$SRC/||g" "$WORK/warnings.log" >&2
fi

python3 "$HERE/doxyxml_to_mdx.py" "$WORK/xml" "$HERE/headers.tsv" "$OUT" "${RELEASE:-unknown}"
