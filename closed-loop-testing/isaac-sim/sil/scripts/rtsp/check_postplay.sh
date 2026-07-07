#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Halos RTSP — Post-Play verification (after Play timeline + Action Graph build)
# --------------------------------------------------------------------------------
# Usage:
#   ./check_postplay.sh                                # cameras.yaml, host=localhost
#   ./check_postplay.sh my_cameras.yaml                # specific yaml
#   ./check_postplay.sh cameras.yaml <remote-host-ip>  # remote host
#   ISAAC_RTSP_CAMERAS_YAML=path ./check_postplay.sh
#
# Checks C1-C11 verify RTSP streams actually serve the expected spec.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YAML="${1:-${ISAAC_RTSP_CAMERAS_YAML:-$SCRIPT_DIR/cameras.yaml}}"
HOST="${2:-localhost}"

if [[ ! -f "$YAML" ]]; then
    echo "ERROR: YAML not found: $YAML"
    exit 1
fi

# Parse cameras list
CAMS_JSON=$(python3 -c "
import yaml, json
with open('$YAML') as f: cfg = yaml.safe_load(f)
print(json.dumps([{'name': c['name'], 'port': c['port'], 'mount_path': c['mount_path']}
                  for c in cfg.get('cameras', [])]))
")

N_CAMS=$(echo "$CAMS_JSON" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))")

echo "================================================================"
echo "Halos RTSP Post-Play Verification"
echo "  Config: $YAML"
echo "  Host: $HOST"
echo "  Cameras: $N_CAMS"
echo "================================================================"

pass()  { printf "  \033[32m[PASS]\033[0m %s\n" "$1"; }
warn()  { printf "  \033[33m[WARN]\033[0m %s\n" "$1"; }
fail()  { printf "  \033[31m[FAIL]\033[0m %s\n" "$1"; }
info()  { printf "  [INFO] %s\n" "$1"; }

# Iterate cameras helper
foreach_cam() {
    python3 -c "
import sys, json
cams = json.loads('''$CAMS_JSON''')
for c in cams:
    print(f\"{c['name']}|{c['port']}|{c['mount_path']}\")
"
}

# ---------- C1 Ports LISTEN ----------
echo
echo "[C1] Ports LISTEN on host"
while IFS='|' read -r NAME PORT MOUNT; do
    L=$(ss -tln 2>/dev/null | grep -E ":${PORT}\b" || true)
    if [[ -n "$L" ]]; then
        pass "$NAME port $PORT LISTEN"
    else
        fail "$NAME port $PORT NOT LISTEN — server not bound / not Played / Action Graph wire missing"
    fi
done < <(foreach_cam)

# ---------- C2 ffprobe stream responsive ----------
echo
echo "[C2] ffprobe stream metadata"
if ! command -v ffprobe >/dev/null; then
    fail "ffprobe not installed"
else
    while IFS='|' read -r NAME PORT MOUNT; do
        URL="rtsp://${HOST}:${PORT}${MOUNT}"
        META=$(timeout 10 ffprobe -v error -show_streams -of json "$URL" 2>&1)
        CODEC=$(echo "$META" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['streams'][0].get('codec_name','?')) if d.get('streams') else print('-')" 2>/dev/null || echo "-")
        WIDTH=$(echo "$META" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['streams'][0].get('width','?')) if d.get('streams') else print('-')" 2>/dev/null || echo "-")
        HEIGHT=$(echo "$META" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['streams'][0].get('height','?')) if d.get('streams') else print('-')" 2>/dev/null || echo "-")
        FPS=$(echo "$META" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['streams'][0].get('r_frame_rate','?')) if d.get('streams') else print('-')" 2>/dev/null || echo "-")
        if [[ "$CODEC" == "h264" && "$WIDTH" =~ ^[0-9]+$ ]]; then
            pass "$NAME $URL: codec=$CODEC ${WIDTH}x${HEIGHT} fps=$FPS"
        else
            fail "$NAME $URL: ffprobe failed (codec=$CODEC width=$WIDTH)"
            info "  raw: $(echo "$META" | head -3 | tr '\n' ' ')"
        fi
    done < <(foreach_cam)
fi

# ---------- C3/C7 FPS + bitrate (5s sample) ----------
echo
echo "[C3/C7] FPS + bitrate (5s sample per cam)"
if command -v ffprobe >/dev/null; then
    while IFS='|' read -r NAME PORT MOUNT; do
        URL="rtsp://${HOST}:${PORT}${MOUNT}"
        # Use simpler: 5-second capture to /dev/null, parse fps + bitrate from ffmpeg stderr
        FPS_LINE=$(timeout 12 ffmpeg -y -i "$URL" -t 5 -f null - 2>&1 | grep -E "frame=" | tail -1)
        if [[ -z "$FPS_LINE" ]]; then
            warn "$NAME no frame stats captured"
            continue
        fi
        FPS_NUM=$(echo "$FPS_LINE" | grep -oE "fps= *[0-9.]+" | grep -oE "[0-9.]+" | head -1)
        SPEED=$(echo "$FPS_LINE" | grep -oE "speed= *[0-9.]+x?" | grep -oE "[0-9.]+" | head -1)
        FPS_NUM=${FPS_NUM:-0}
        SPEED=${SPEED:-0}
        SPEED_OK=$(awk -v s="$SPEED" 'BEGIN { print (s+0 >= 0.95) ? 1 : 0 }')
        FPS_OK=$(awk -v f="$FPS_NUM" 'BEGIN { print (f+0 >= 28) ? 1 : 0 }')
        if [[ "$SPEED_OK" == "1" && "$FPS_OK" == "1" ]]; then
            pass "$NAME 5s sample: fps=$FPS_NUM speed=${SPEED}x (real-time, OK)"
        elif [[ "$SPEED_OK" == "0" && "$FPS_OK" == "0" ]]; then
            fail "$NAME wall-clock cap: fps=$FPS_NUM speed=${SPEED}x — Isaac sim slower than real-time. Downstream DS will see <30 fps regardless of SDP/SPS declaration. Check GPU load / scene complexity."
        else
            warn "$NAME borderline: fps=$FPS_NUM speed=${SPEED}x — investigate scene complexity / GPU load"
        fi
        info "  raw: $(echo "$FPS_LINE" | tr -s ' ')"
    done < <(foreach_cam)
else
    warn "ffmpeg/ffprobe missing skip"
fi

# ---------- C4 NVENC sessions active (per-process) ----------
echo
echo "[C4] NVENC sessions active (≥ #cameras)"
if command -v nvidia-smi >/dev/null; then
    # pmon shows per-process encoder/decoder usage
    PMON=$(timeout 5 nvidia-smi pmon -c 1 -s u 2>/dev/null | tail -n +3 || echo "")
    if [[ -n "$PMON" ]]; then
        info "Per-process util snapshot (pmon, columns: gpu pid type sm mem enc dec ...):"
        echo "$PMON" | while IFS= read -r line; do info "  $line"; done
        # Filter rows where enc > 0
        ENC_ACTIVE=$(echo "$PMON" | awk '$6 > 0 {count++} END {print count+0}')
        if [[ $ENC_ACTIVE -ge $N_CAMS ]]; then
            pass "$ENC_ACTIVE process(es) doing NVENC encode ≥ $N_CAMS cameras"
        else
            warn "Only $ENC_ACTIVE process(es) NVENC-encoding ($N_CAMS expected). Possible CPU h264 fallback or nvidia-smi pmon sampling miss"
        fi
    else
        warn "nvidia-smi pmon empty (perm or unsupported)"
    fi
else
    warn "nvidia-smi missing"
fi

# ---------- C5 Isaac log ERROR scan ----------
echo
echo '[C5] Isaac log scan (manual — check terminal stdout of running Isaac Sim)'
info 'Suggested grep: grep -iE "ERROR|CRITICAL|fail|rtsp" Isaac-stdout.log'

# ---------- C6 All streams parallel ----------
echo
echo "[C6] Parallel access — all N streams at once"
PIDS=""
PASS_COUNT=0
TMPDIR=$(mktemp -d)
while IFS='|' read -r NAME PORT MOUNT; do
    URL="rtsp://${HOST}:${PORT}${MOUNT}"
    (timeout 8 ffprobe -v error -show_streams "$URL" > "$TMPDIR/$NAME.out" 2>&1 && echo OK > "$TMPDIR/$NAME.result" || echo FAIL > "$TMPDIR/$NAME.result") &
    PIDS="$PIDS $!"
done < <(foreach_cam)
wait $PIDS
while IFS='|' read -r NAME PORT MOUNT; do
    R=$(cat "$TMPDIR/$NAME.result" 2>/dev/null || echo MISSING)
    if [[ "$R" == "OK" ]]; then
        pass "$NAME parallel ffprobe OK"
        PASS_COUNT=$((PASS_COUNT+1))
    else
        fail "$NAME parallel ffprobe FAIL"
    fi
done < <(foreach_cam)
rm -rf "$TMPDIR"
info "Parallel: $PASS_COUNT/$N_CAMS PASS"

# ---------- C8 Latency (skip — needs visual verify) ----------
echo
echo "[C8] Latency end-to-end (manual)"
info "Suggested: ffplay rtsp://... + visual delay check, or clock-in-frame technique"

# ---------- C9 Stability re-check after 60s ----------
echo
echo "[C9] Stability — re-check ports + ffprobe after 60s? [skip / re-run script]"
info "Re-run after 60s. Compare FPS + bitrate. Must be bounded, no drift."

# ---------- C10 Memory ----------
echo
echo "[C10] GPU memory used"
if command -v nvidia-smi >/dev/null; then
    nvidia-smi --query-gpu=index,memory.used,memory.total --format=csv,noheader 2>/dev/null | while IFS= read -r line; do info "$line"; done
    info "Re-check after few min — flat/bounded = OK, ascending = leak"
fi

# ---------- C11 SEI sim-time metadata (per-frame JSON) ----------
echo
echo "[C11] SEI sim-time NAL units (per-frame metadata, UUID aa71e48f...)"
SEI_PY="$SCRIPT_DIR/sei_decode.py"
if [[ ! -f "$SEI_PY" ]]; then
    warn "sei_decode.py not found at $SEI_PY — skip"
elif ! command -v ffmpeg >/dev/null; then
    warn "ffmpeg missing — skip"
else
    TMPDIR_SEI=$(mktemp -d)
    while IFS='|' read -r NAME PORT MOUNT; do
        URL="rtsp://${HOST}:${PORT}${MOUNT}"
        H264_OUT="$TMPDIR_SEI/${NAME}.h264"
        if ! timeout 10 ffmpeg -y -loglevel quiet -i "$URL" -t 1 -c:v copy -f h264 "$H264_OUT" </dev/null 2>/dev/null; then
            warn "$NAME ffmpeg capture failed"
            continue
        fi
        OUT=$(python3 "$SEI_PY" "$H264_OUT" --quiet 2>&1)
        EC=$?
        UUID_N=$(echo "$OUT" | awk -F': ' '/UUID matched/ {print $2}')
        SCHEMA_N=$(echo "$OUT" | awk -F': ' '/Schema OK/ {print $2}')
        SEI_TOTAL=$(echo "$OUT" | awk -F': ' '/SEI NALs:/ {print $2}')
        if [[ $EC -eq 0 && ${SCHEMA_N:-0} -gt 0 ]]; then
            # Print sample frame_num + sim_time from first match for sanity check
            SAMPLE=$(python3 "$SEI_PY" "$H264_OUT" --json 2>&1 | head -1 | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(f\"frame_num={d.get('frame_num')} sim_time_ns={d.get('publish_sim_time_ns')}\")" 2>/dev/null || echo "?")
            pass "$NAME SEI: ${SEI_TOTAL} NALs, ${UUID_N} UUID match, ${SCHEMA_N} schema OK — $SAMPLE"
        elif [[ ${UUID_N:-0} -gt 0 ]]; then
            warn "$NAME UUID matched but no valid JSON payload (UUID=$UUID_N, schema_ok=$SCHEMA_N)"
        else
            fail "$NAME no SEI with target UUID found (total SEI=$SEI_TOTAL, UUID=$UUID_N) — SEI not emitted or useRawEncoding=true"
        fi
    done < <(foreach_cam)
    rm -rf "$TMPDIR_SEI"
    info "SEI carries publish_sim_time_ns + frame_num (replaces the wall-clock workaround)"
fi

echo
echo "================================================================"
echo "Post-Play verification done."
echo "Re-run script a few times after stream stable to check C9 stability."
echo "================================================================"
