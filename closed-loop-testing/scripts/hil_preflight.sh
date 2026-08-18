#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# HIL preflight — run on the x86 stimulus host BEFORE bringing the hil profile up.
# Validates the profile env, this host's tooling, and (when reachable) the Thor safety
# host, so a two-host deploy fails here instead of half-way through the bring-up.
#
# Usage (from the repo root):
#   bash closed-loop-testing/scripts/hil_preflight.sh [deployments/profiles/hil.env] [--thor <user@host>]
#
#   --thor   ssh target for the Thor safety host (e.g. <user>@<thor_ip>). Without it,
#            the Thor-side checks are printed as a manual checklist instead of failing.

set -u

ENV_FILE="deployments/profiles/hil.env"
THOR_SSH=""
while [ $# -gt 0 ]; do
    case "$1" in
        --thor) THOR_SSH="${2:?--thor needs <user@host>}"; shift 2 ;;
        *) ENV_FILE="$1"; shift ;;
    esac
done

FAIL=0
warn() { printf 'WARN  %s\n' "$*"; }
ok()   { printf 'ok    %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; FAIL=1; }

# --- 1. Env file present + placeholders filled ---
[ -f "$ENV_FILE" ] || { fail "env file not found: $ENV_FILE"; exit 1; }
set -a; . "$ENV_FILE"; set +a
ok "loaded $ENV_FILE"

for v in MDX_SAMPLE_APPS_DIR MDX_DATA_DIR HOST_IP PEER_HOST_IP; do
    val="${!v:-}"
    case "$val" in
        ''|*'/path/to/'*|*'<'*'>'*) fail "$v not filled in (value: '${val}')" ;;
        *) ok "$v=$val" ;;
    esac
done
[ "${HOST_IP:-}" = "${PEER_HOST_IP:-}" ] && fail "HOST_IP equals PEER_HOST_IP - hil is a two-host profile (use the sil profile for single-host)"

# --- 2. Cross-file invariants ---
case "${COMPOSE_PROFILES:-}" in
    hil|hil,multi-robot|multi-robot,hil) ok "COMPOSE_PROFILES=${COMPOSE_PROFILES}" ;;
    *) warn "COMPOSE_PROFILES is '${COMPOSE_PROFILES:-}' (expected hil, or hil,multi-robot for two forklifts)" ;;
esac
[ -n "${COMM_UDP_PORT:-}" ] || fail "COMM_UDP_PORT not set (the Thor's PSF_CMD_RX_PORT must match it)"
[ -d "${MDX_SAMPLE_APPS_DIR:-/nonexistent}/closed-loop-testing" ] || fail "MDX_SAMPLE_APPS_DIR does not look like the repo root"
[ -d "${MDX_DATA_DIR:-/nonexistent}/collected-assets" ] || warn "MDX_DATA_DIR has no collected-assets/ yet (ngc_artifacts.md §1)"

# --- 3. Local tooling ---
command -v docker >/dev/null 2>&1 && ok "docker present" || fail "docker not found"
docker compose version >/dev/null 2>&1 && ok "docker compose v2 present" || fail "docker compose v2 not found"
if [ -n "${DOCKER_GID:-}" ]; then
    real_gid="$(getent group docker 2>/dev/null | cut -d: -f3)"
    [ -n "$real_gid" ] && [ "$real_gid" != "$DOCKER_GID" ] && warn "DOCKER_GID=$DOCKER_GID but host docker group is $real_gid"
fi

# --- 4. Local clock ---
if command -v timedatectl >/dev/null 2>&1; then
    [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = "yes" ] \
        && ok "local clock NTP-synchronized" \
        || warn "local clock not NTP-synchronized (cross-host log correlation and latency math need synced clocks)"
fi

# --- 5. Thor safety host ---
if [ -n "$THOR_SSH" ]; then
    if ssh -o BatchMode=yes -o ConnectTimeout=8 "$THOR_SSH" true 2>/dev/null; then
        ok "ssh $THOR_SSH reachable"
        [ "$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$THOR_SSH" 'timedatectl show -p NTPSynchronized --value' 2>/dev/null)" = "yes" ] \
            && ok "Thor clock NTP-synchronized" \
            || warn "Thor clock not NTP-synchronized"
        thor_epoch="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$THOR_SSH" 'date +%s' 2>/dev/null)"
        if [ -n "$thor_epoch" ]; then
            skew=$(( $(date +%s) - thor_epoch ))
            [ "${skew#-}" -le 2 ] && ok "clock skew x86<->Thor ${skew}s" || warn "clock skew x86<->Thor ${skew}s (>2s)"
        else
            warn "could not read the Thor clock for the skew check"
        fi
    else
        fail "cannot ssh $THOR_SSH (key-based auth required for the orchestrated flow)"
    fi
    curl -s --max-time 6 -o /dev/null -w '%{response_code}' "http://${PEER_HOST_IP}:30888/vst/api/v1/sensor/list" 2>/dev/null | grep -q '^200$' \
        && ok "Thor VST API :30888 reachable" \
        || warn "Thor VST API :30888 not answering yet (fine if VSS is not up yet)"
    # liveness only: any HTTP response (including 4xx on this POST-only endpoint) counts as up
    curl -s --max-time 6 -o /dev/null "http://${PEER_HOST_IP}:9000/api/v1/stream/add" 2>/dev/null \
        && ok "Thor perception API :9000 reachable" \
        || warn "Thor perception API :9000 not answering yet (fine if VSS is not up yet)"
else
    cat <<'CHECKLIST'
note  --thor not given; verify these on the Thor manually:
      [ ] clock NTP-synchronized (timedatectl)
      [ ] VSS Warehouse 2D up; camera_info.json lists this host's Isaac RTSP URLs (SENSOR_INFO_SOURCE=file)
      [ ] VST API :30888 and perception API :9000 answering
      [ ] profiles/hil-thor.env filled (PSF_CMD_RX_IP = this x86 host, PSF_CMD_RX_PORT = COMM_UDP_PORT)
CHECKLIST
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "preflight PASSED (warnings above, if any, are advisory)"
else
    echo "preflight FAILED - fix the FAIL lines above before compose up"
fi
exit "$FAIL"
