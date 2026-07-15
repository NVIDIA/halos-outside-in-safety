#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Transcode all clip_logs/<scn>/video.mp4 from H.265 (HEVC) to H.264, in-place.
#
# Why: VST replay records HEVC by default. Firefox + most Chromium builds on
# Linux don't ship software HEVC decoders, so the viewer's <video> element
# spins forever. H.264 plays everywhere.
#
# Usage: ./transcode_to_h264.sh <multi-test-dir> [parallelism=4]
set -euo pipefail

RUN_DIR="${1:?usage: $0 <multi-test-dir> [parallelism=4]}"
PAR="${2:-4}"

if [[ ! -d "$RUN_DIR" ]]; then echo "no such dir: $RUN_DIR" >&2; exit 1; fi
if ! command -v ffmpeg >/dev/null; then echo "ffmpeg not found" >&2; exit 1; fi

mapfile -t VIDS < <(find "$RUN_DIR" -path '*/clip_logs/*/video.mp4' -type f)
TOTAL=${#VIDS[@]}
echo "found $TOTAL clip video(s) under $RUN_DIR"

transcode_one() {
  local src="$1"
  # Skip if already H.264
  local codec
  codec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$src" 2>/dev/null || echo "?")
  if [[ "$codec" == "h264" ]]; then echo "skip (already h264): $src"; return 0; fi

  local tmp="${src%.mp4}.h264.mp4"
  ffmpeg -hide_banner -loglevel warning -nostdin -y \
    -i "$src" \
    -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p \
    -movflags +faststart \
    -an \
    "$tmp" \
    && mv "$tmp" "$src" \
    || { rm -f "$tmp"; echo "FAIL: $src" >&2; return 1; }
  echo "ok: $src"
}
export -f transcode_one

I=0
for v in "${VIDS[@]}"; do
  I=$((I+1))
  printf '\r[%d/%d] %s\n' "$I" "$TOTAL" "${v##*/multi-test-*/}" >&2
  transcode_one "$v" &
  if (( I % PAR == 0 )); then wait; fi
done
wait
echo "done. all $TOTAL clip videos in $RUN_DIR are H.264 now."
