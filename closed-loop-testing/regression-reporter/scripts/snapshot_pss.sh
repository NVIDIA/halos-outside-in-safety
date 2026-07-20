#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Snapshot the bind-mounted PSF host log into a SRR run dir, so PSF internal-state
# evidence survives the next compose restart (which truncates the log file).
#
# Two modes:
#
#   1. Per-scenario (called from phase_analyze right after record stop, before
#      the next scenario's compose_restart wipes the source):
#
#         snapshot_pss.sh <multi-test-dir>/<scn-label> --per-scn
#
#      → copies the full source pss.log into <scn-label>/pss.log. At this
#      instant the source only contains the current scenario's PSF data
#      (previous scenarios' data was already truncated by the prior restart).
#
#   2. Cross-run (end of multi-test, concat per-scn snapshots into a single
#      run-level file used by clip_logs default-fallback):
#
#         snapshot_pss.sh <multi-test-dir>
#
#      → if per-scn snapshots exist under <multi-test-dir>/*/pss.log, concat
#      them (sorted by timestamp) into <multi-test-dir>/pss.log. If no per-scn
#      snapshots exist (e.g. legacy single-scenario flow), fall back to slicing
#      the source by wall-time bounds derived from run-*.parquet mtimes.
#
# Default source pss.log = <sil-data>/psf-log/pss.log, the host bind-mount of the
#   safety-core container's /var/log/pss.log (see the Halos isaac-sim compose /
#   safety-core mounts). Point PSS_LOG_SRC at that host path (or pass
#   <source-pss-log> as the last arg) to override the neutral default below.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: snapshot_pss.sh <run-dir> [--per-scn] [<source-pss-log>]" >&2
  exit 1
fi

RUN_DIR="$1"; shift

MODE="cross-run"
SRC=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --per-scn) MODE="per-scn"; shift ;;
    -*) echo "snapshot_pss: unknown flag $1" >&2; exit 1 ;;
    *)  SRC="$1"; shift ;;
  esac
done
SRC="${SRC:-${PSS_LOG_SRC:-/data/sil-data/psf-log/pss.log}}"

if [[ ! -d "$RUN_DIR" ]]; then
  echo "snapshot_pss: no such dir: $RUN_DIR" >&2; exit 1
fi

OUT="$RUN_DIR/pss.log"

# The run/scenario dirs are created by the srr container (root), so a host-side
# `cp`/redirect into them fails with EPERM. write_out() copies a host-readable
# file to $OUT, falling back to writing THROUGH the srr container (which owns the
# dir as root) by translating the host path to the container's /app/runs mount.
SRR_CONTAINER="${SRR_CONTAINER:-srr}"
write_out() {  # $1 = host-readable source file, $2 = OUT host path
  local src="$1" out="$2"
  if cp "$src" "$out" 2>/dev/null; then
    return 0
  fi
  # host write denied → stream into the container, which writes as root.
  local cdir="/app/runs/${out##*/srr-service/runs/}"
  if cat "$src" | docker exec -i "$SRR_CONTAINER" bash -c "cat > '$cdir'" 2>/dev/null; then
    echo "snapshot_pss: host write denied, wrote via container → $cdir"
    return 0
  fi
  echo "snapshot_pss: FAILED to write $out (host EPERM + container fallback failed)" >&2
  return 1
}

if [[ "$MODE" == "per-scn" ]]; then
  # At call time the source only holds the current scenario (previous
  # scenarios' lines were truncated by the prior compose restart). A plain
  # copy preserves the entire scenario including PSF init lines that no
  # mtime-window heuristic would catch.
  if [[ ! -f "$SRC" ]]; then
    echo "snapshot_pss: source pss.log not found: $SRC" >&2; exit 1
  fi
  write_out "$SRC" "$OUT"
  LINES=$(wc -l < "$SRC")
  SIZE=$(du -h "$SRC" | awk '{print $1}')
  echo "snapshot_pss: copied $SRC → $OUT ($LINES lines, $SIZE) [per-scn]"
  exit 0
fi

# ---- cross-run mode ----

# Prefer concat of per-scn snapshots (preserves PSF data even across restarts).
# Exclude _discarded/ (aborted attempts) and drop exact-duplicate lines: when
# the source log is NOT truncated between scenarios (stack kept up between
# runs), each per-scn snapshot is a cumulative copy of the same source — a
# plain concat would then repeat every line once per overlapping snapshot.
PER_SCN_FILES=()
for f in "$RUN_DIR"/*/pss.log; do
  [[ "$f" == *"_discarded"* ]] && continue
  [[ -f "$f" ]] && PER_SCN_FILES+=( "$f" )
done
if [[ ${#PER_SCN_FILES[@]} -gt 0 ]]; then
  # Concat, sort by leading ISO timestamp (line-stable sort -k1,1), dedupe
  # exact lines (first occurrence wins; output stays timestamp-sorted).
  TMP=$(mktemp)
  cat "${PER_SCN_FILES[@]}" | sort -s -k1,1 | awk '!seen[$0]++' > "$TMP"
  write_out "$TMP" "$OUT"
  LINES=$(wc -l < "$TMP")
  SIZE=$(du -h "$TMP" | awk '{print $1}')
  rm -f "$TMP"
  echo "snapshot_pss: concat ${#PER_SCN_FILES[@]} per-scn snapshot(s) → $OUT ($LINES lines, $SIZE)"
  exit 0
fi

# Fallback (no per-scn snapshots): slice source by mtime window. Works only
# if the source still holds the full multi-test window (i.e. no PSF restart
# happened between scenarios — rare for a multi-scn run).
if [[ ! -f "$SRC" ]]; then
  echo "snapshot_pss: source pss.log not found: $SRC" >&2; exit 1
fi
PAD_S=60
LO=$(find "$RUN_DIR" -name "run-*.parquet" -printf '%T@\n' | sort -n | head -1)
HI=$(find "$RUN_DIR" -name "run-*.parquet" -printf '%T@\n' | sort -n | tail -1)
if [[ -z "${LO:-}" || -z "${HI:-}" ]]; then
  echo "snapshot_pss: no run-*.parquet found under $RUN_DIR" >&2; exit 1
fi
LO=$(awk -v t="$LO" -v p="$PAD_S" 'BEGIN{printf "%d", t - p}')
HI=$(awk -v t="$HI" -v p="$PAD_S" 'BEGIN{printf "%d", t + p}')

TMP=$(mktemp)
TZ=UTC awk -v lo="$LO" -v hi="$HI" '
  /^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\./ {
    iso = substr($0, 1, 19); gsub(/[-T:]/, " ", iso)
    t = mktime(iso)
    if (t == -1) next
    if (t < lo) next
    if (t > hi) exit
    print
  }
' "$SRC" > "$TMP"
write_out "$TMP" "$OUT"
LINES=$(wc -l < "$TMP")
SIZE=$(du -h "$TMP" | awk '{print $1}')
rm -f "$TMP"
echo "snapshot_pss: wrote $OUT ($LINES lines, $SIZE) — window [$LO, $HI] [mtime-slice fallback]"
