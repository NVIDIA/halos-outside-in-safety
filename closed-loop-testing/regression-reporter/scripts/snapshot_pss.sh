#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Snapshot the bind-mounted PSF host log into a SRR run dir, so PSF internal-state
# evidence for a scenario survives whatever the next compose restart does to the
# source. Observed behaviour is that the restart does NOT truncate it — the log
# is cumulative across scenarios and grows into the multi-GB range — but the
# stack can be brought up in ways that do, so neither is assumed.
#
# Three modes:
#
#   0. Init (called once before the first scenario):
#
#         snapshot_pss.sh <multi-test-dir> --init
#
#      → creates the run dir host-side (so later host writes into it are not
#      blocked by the srr container's root ownership) and records the source's
#      current size as the run baseline. Everything already in the log at this
#      point predates the run and is never copied into it.
#
#   1. Per-scenario (called from phase_analyze right after record stop, before
#      the next scenario's compose_restart wipes the source):
#
#         snapshot_pss.sh <multi-test-dir>/<scn-label> --per-scn
#
#      → copies the bytes appended since the previous snapshot into
#      <scn-label>/pss.log. The source is NOT reliably truncated between
#      scenarios: whether the restart truncates depends on how the stack was
#      brought up, and when it does not, a full copy makes every scenario a
#      near-complete duplicate of its predecessors. Tracking the offset gives
#      the same evidence either way — a truncation or rotation is detected by
#      inode + head digest and falls back to copying the whole file.
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
    --init)    MODE="init"; shift ;;
    -*) echo "snapshot_pss: unknown flag $1" >&2; exit 1 ;;
    *)  SRC="$1"; shift ;;
  esac
done
# When invoked standalone (not via run_multi.sh, which already `set -a; source`s
# the SIL profile), MDX_DATA_DIR / PSF_LOG_DIR are not in the environment. Load
# them from the SIL deployment profile so the documented per-scenario command
# resolves the SAME data root used to launch SIL. Override the profile path with
# SIL_ENV; a real (non-template) MDX_DATA_DIR is required for this to take effect.
if [[ -z "${PSF_LOG_DIR:-}" && -z "${MDX_DATA_DIR:-}" ]]; then
  _SP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  # Prefer HOISA_ROOT_PATH (same root run_multi.sh derives the profile from) when
  # it is exported; fall back to a script-relative repo root so a bare standalone
  # invocation (no repo .env sourced) still resolves. Default profile: sil.env.
  SIL_ENV="${SIL_ENV:-${HOISA_ROOT_PATH:-$_SP_DIR/../../..}/deployments/profiles/sil.env}"
  if [[ -f "$SIL_ENV" ]]; then
    set -a
    # shellcheck disable=SC1090
    source "$SIL_ENV"
    set +a
    # An unedited template profile yields placeholder paths — and PSF_LOG_DIR is
    # derived from MDX_DATA_DIR, so it inherits the placeholder too. Ignore both
    # so resolution falls through to the documented default instead of /path/to/.
    case "${MDX_DATA_DIR:-}" in /path/to/*) MDX_DATA_DIR="" ;; esac
    case "${PSF_LOG_DIR:-}"  in /path/to/*) PSF_LOG_DIR=""  ;; esac
  fi
fi

# Resolve the source pss.log so a non-default data root still finds the Safety
# Core log. Priority: explicit <source-pss-log> arg > PSS_LOG_SRC override >
# PSF_LOG_DIR (from the profile env) > MDX_DATA_DIR > neutral fallback.
if [[ -z "$SRC" ]]; then
  if   [[ -n "${PSS_LOG_SRC:-}" ]]; then SRC="$PSS_LOG_SRC"
  elif [[ -n "${PSF_LOG_DIR:-}"  ]]; then SRC="$PSF_LOG_DIR/pss.log"
  elif [[ -n "${MDX_DATA_DIR:-}" ]]; then SRC="$MDX_DATA_DIR/psf-log/pss.log"
  else SRC="/data/sil-data/psf-log/pss.log"
  fi
fi

echo "snapshot_pss: resolved source = $SRC" >&2

if [[ "$MODE" == "init" ]]; then
  mkdir -p "$RUN_DIR"
elif [[ ! -d "$RUN_DIR" ]]; then
  echo "snapshot_pss: no such dir: $RUN_DIR" >&2; exit 1
fi

OUT="$RUN_DIR/pss.log"

# Offset bookkeeping lives at the multi-test root: --init and cross-run are
# handed that dir directly, --per-scn a scenario dir one level below it.
if [[ "$MODE" == "per-scn" ]]; then
  STATE_FILE="$(dirname "$RUN_DIR")/.pss_offset"
else
  STATE_FILE="$RUN_DIR/.pss_offset"
fi

# Identify the source well enough to tell "grew" from "was truncated and
# regrew past the old offset" — the latter happens whenever safety-core
# restarts mid-run, and a byte offset alone cannot distinguish the two.
#
# The digest covers only bytes already accounted for by the offset. Those are
# immutable unless the file was replaced, whereas a fixed-size prefix of a log
# smaller than that prefix keeps changing as it grows, which reads as a
# truncation on every call.
HEAD_SAMPLE_MAX=4096
src_inode() { stat -c %i "$SRC" 2>/dev/null || echo 0; }
src_size()  { stat -c %s "$SRC" 2>/dev/null || echo 0; }
head_len()  { local n="$1"; (( n < HEAD_SAMPLE_MAX )) && echo "$n" || echo "$HEAD_SAMPLE_MAX"; }
src_head()  {  # $1 = leading byte count to digest
  local n="$1"
  if (( n <= 0 )); then echo "-"; return; fi
  head -c "$n" "$SRC" 2>/dev/null | sha256sum | cut -d' ' -f1
}

write_state() {  # $1 = inode, $2 = head length, $3 = head digest, $4 = offset
  printf 'PSS_INODE=%s\nPSS_HEADLEN=%s\nPSS_HEAD=%s\nPSS_OFFSET=%s\n' "$1" "$2" "$3" "$4" \
    > "$STATE_FILE" 2>/dev/null \
    || echo "snapshot_pss: WARN could not write $STATE_FILE — next snapshot will copy the whole log" >&2
}

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

# write_out returning 0 is not proof the evidence landed: the container fallback
# reports the exit status of `cat` inside the container, and a copy taken while
# safety-core is still appending can come up short. Check the destination.
verify_out() {  # $1 = OUT host path, $2 = expected byte count
  local out="$1" want="$2" got
  if [[ ! -s "$out" ]]; then
    echo "snapshot_pss: destination missing or empty after write: $out" >&2
    return 1
  fi
  got=$(stat -c %s "$out" 2>/dev/null || echo "")
  if [[ -z "$got" ]]; then
    echo "snapshot_pss: WARN cannot stat $out to confirm size" >&2
    return 0
  fi
  if [[ "$got" != "$want" ]]; then
    echo "snapshot_pss: short write: $out is $got bytes, expected $want" >&2
    return 1
  fi
  return 0
}

if [[ "$MODE" == "init" ]]; then
  # Everything already in the log belongs to whatever ran before this sweep.
  # Recording it as the baseline is what keeps the first scenario's snapshot
  # from dragging the entire pre-run history into the run directory.
  BASE=$(src_size)
  BASE_HL=$(head_len "$BASE")
  write_state "$(src_inode)" "$BASE_HL" "$(src_head "$BASE_HL")" "$BASE"
  echo "snapshot_pss: baseline for $RUN_DIR set at $BASE bytes of $SRC [init]"
  exit 0
fi

if [[ "$MODE" == "per-scn" ]]; then
  # Missing OR empty (0-byte) source both mean no Safety Core evidence for this
  # scenario — fail rather than copy an empty log that would silently "pass".
  if [[ ! -s "$SRC" ]]; then
    echo "snapshot_pss: source pss.log missing or empty: $SRC" >&2; exit 1
  fi

  SRC_SIZE=$(src_size)
  INODE=$(src_inode)

  START=0
  if [[ -f "$STATE_FILE" ]]; then
    PSS_INODE=""; PSS_HEADLEN=0; PSS_HEAD=""; PSS_OFFSET=0
    # shellcheck disable=SC1090
    source "$STATE_FILE"
    if [[ "$PSS_INODE" == "$INODE" \
       && "$PSS_OFFSET" -le "$SRC_SIZE" \
       && "$PSS_HEAD" == "$(src_head "$PSS_HEADLEN")" ]]; then
      START="$PSS_OFFSET"
    else
      echo "snapshot_pss: source was truncated or rotated since the last snapshot — copying from the start" >&2
    fi
  fi

  BYTES=$(( SRC_SIZE - START ))
  if (( BYTES <= 0 )); then
    echo "snapshot_pss: source gained no bytes since the last snapshot ($SRC is $SRC_SIZE bytes) — no Safety Core evidence for this scenario" >&2
    exit 1
  fi

  # Cut at the size read above rather than streaming to EOF: safety-core is
  # still appending, and a snapshot that ends wherever the writer happened to
  # be is not reproducible and can split a line in half.
  TMP=$(mktemp)
  trap 'rm -f "$TMP"' EXIT
  tail -c "+$((START + 1))" "$SRC" | head -c "$BYTES" > "$TMP"

  # A failed write (host EPERM AND container fallback both denied) must propagate,
  # otherwise run_multi's strict sweep-abort never sees the lost snapshot.
  if ! write_out "$TMP" "$OUT"; then
    exit 1
  fi
  if ! verify_out "$OUT" "$BYTES"; then
    exit 1
  fi
  LINES=$(wc -l < "$TMP")
  SIZE=$(du -h "$TMP" | awk '{print $1}')
  NEW_HL=$(head_len "$SRC_SIZE")
  write_state "$INODE" "$NEW_HL" "$(src_head "$NEW_HL")" "$SRC_SIZE"
  echo "snapshot_pss: copied bytes ${START}-${SRC_SIZE} of $SRC → $OUT ($LINES lines, $SIZE) [per-scn]"
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
  trap 'rm -f "$TMP"' EXIT
  cat "${PER_SCN_FILES[@]}" | sort -s -k1,1 | awk '!seen[$0]++' > "$TMP"
  if ! write_out "$TMP" "$OUT"; then
    exit 1
  fi
  if ! verify_out "$OUT" "$(stat -c %s "$TMP")"; then
    exit 1
  fi
  LINES=$(wc -l < "$TMP")
  SIZE=$(du -h "$TMP" | awk '{print $1}')
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
trap 'rm -f "$TMP"' EXIT
# LO/HI are true epoch seconds (find -printf %T@). pss.log timestamps carry a
# UTC offset, so the wall clock in the first 19 characters cannot be compared
# against them directly — read the offset and fold it back in, or every
# comparison is wrong by however far the host sits from UTC.
TZ=UTC awk -v lo="$LO" -v hi="$HI" '
  {
    tok = $1
    if (tok !~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\./) next
    iso = substr(tok, 1, 19); gsub(/[-T:]/, " ", iso)
    t = mktime(iso)
    if (t == -1) next
    if (match(tok, /[+-][0-9][0-9]:[0-9][0-9]$/)) {
      off = substr(tok, RSTART, RLENGTH)
      secs = (substr(off, 2, 2) * 3600) + (substr(off, 5, 2) * 60)
      t += (substr(off, 1, 1) == "+") ? -secs : secs
    }
    if (t < lo) { past = 0; next }
    if (t > hi) {
      # Stop early — this is a multi-GB scan — but not on the first line past
      # the window: the log interleaves several writers and is not perfectly
      # ordered, so a lone late line must not end the slice.
      if (++past > 10000) exit
      next
    }
    past = 0
    print
  }
' "$SRC" > "$TMP"
if ! write_out "$TMP" "$OUT"; then
  exit 1
fi
if ! verify_out "$OUT" "$(stat -c %s "$TMP")"; then
  exit 1
fi
LINES=$(wc -l < "$TMP")
SIZE=$(du -h "$TMP" | awk '{print $1}')
echo "snapshot_pss: wrote $OUT ($LINES lines, $SIZE) — window [$LO, $HI] [mtime-slice fallback]"
