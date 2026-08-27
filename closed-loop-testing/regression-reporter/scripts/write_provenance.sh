#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Record what produced a run, into the run directory itself.
#
#     write_provenance.sh <run-dir>
#
# Writes <run-dir>/provenance.json and copies the geometry the run will be
# scored against (calibration.json, cameras.yaml) next to it.
#
# Two things this exists to catch, both of which have happened:
#
#   1. Container code that is in no image. `docker cp` into a live srr
#      container is a documented workflow, and a compose recreate silently
#      reverts it, so consecutive scenarios of one sweep can run different
#      code. Nothing in the run output said which. This hashes the container's
#      /app/srr against the host tree and records both.
#
#   2. Reports that carry metrics but not the geometry behind them. The
#      aggregator and tw_split read the live calibration mount on every
#      invocation, so a run scored today and re-scored after a calibration swap
#      gives different verdicts with nothing in the run dir to explain it. Two
#      rows of a pushout sweep are identical from the report alone.
#
# Never fatal: a run that produced good data should not be thrown away because
# provenance capture hit a snag. Failures are recorded inside the JSON.

set -uo pipefail

RUN_DIR="${1:-}"
if [[ -z "$RUN_DIR" ]]; then
  echo "usage: write_provenance.sh <run-dir>" >&2
  exit 1
fi
mkdir -p "$RUN_DIR" 2>/dev/null || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${HOISA_ROOT_PATH:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
SRR_HOST_SRC="$REPO_ROOT/closed-loop-testing/regression-reporter/srr-service/srr"
SRR_CONTAINER="${SRR_CONTAINER:-srr}"

note() { printf '  [provenance] %s\n' "$*"; }

# ---- git ----
GIT_COMMIT=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo "")
GIT_BRANCH=$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
GIT_DIRTY="unknown"
if [[ -n "$GIT_COMMIT" ]]; then
  if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    GIT_DIRTY="true"
  else
    GIT_DIRTY="false"
  fi
fi

# ---- container images ----
# Tag alone is not identity: srr-service:phase1 is rebuilt in place, so record
# the image id and repo digest that the container is actually running.
inspect_image() {  # $1 = container name → "<image-ref>|<image-id>|<repo-digest>"
  local ctr="$1" ref id dig
  ref=$(docker inspect --format '{{.Config.Image}}' "$ctr" 2>/dev/null || echo "")
  id=$(docker inspect --format '{{.Image}}' "$ctr" 2>/dev/null || echo "")
  dig=$(docker inspect --format '{{if .RepoDigests}}{{index .RepoDigests 0}}{{end}}' "$id" 2>/dev/null || echo "")
  printf '%s|%s|%s\n' "$ref" "$id" "$dig"
}

# ---- code identity ----
# One expression, run on both sides, because two transcriptions of "hash the
# tree" drift: the first pair differed only in sort order (the host's locale
# collates __init__.py after clip_logs.py, the container's C locale before it)
# and reported a mismatch for two identical trees. LC_ALL=C pins the collation,
# paths are relative to the tree root so they read the same in both places, and
# hashing path alongside content means a rename is a difference too.
CODE_DIGEST_CMD='cd "$0" 2>/dev/null || exit 1
find . -name "*.py" -type f -print0 2>/dev/null \
  | LC_ALL=C sort -z \
  | xargs -0 sha256sum 2>/dev/null \
  | LC_ALL=C sort \
  | sha256sum | cut -d" " -f1'

HOST_CODE_SHA=""
if [[ -d "$SRR_HOST_SRC" ]]; then
  HOST_CODE_SHA=$(bash -c "$CODE_DIGEST_CMD" "$SRR_HOST_SRC" 2>/dev/null || echo "")
fi

CTR_CODE_SHA=""
if docker inspect "$SRR_CONTAINER" >/dev/null 2>&1; then
  CTR_CODE_SHA=$(docker exec "$SRR_CONTAINER" bash -c "$CODE_DIGEST_CMD" /app/srr \
    2>/dev/null | tr -d '\r' || echo "")
fi

CODE_MATCH="unknown"
if [[ -n "$HOST_CODE_SHA" && -n "$CTR_CODE_SHA" ]]; then
  if [[ "$HOST_CODE_SHA" == "$CTR_CODE_SHA" ]]; then
    CODE_MATCH="true"
  else
    CODE_MATCH="false"
    note "WARN the srr container is not running the host's srr/ tree."
    note "     host      ${HOST_CODE_SHA:0:16}"
    note "     container ${CTR_CODE_SHA:0:16}"
    note "     Results will not be reproducible from this repo state. Rebuild"
    note "     (docker compose build srr) unless the difference is intended."
  fi
fi

# ---- geometry actually in force ----
# Ask the container what it is actually reading rather than trusting an env var
# to still describe it. The aggregator scores against /app/calibration.json, so
# whatever is mounted there is the geometry in force, whatever CALIBRATION_JSON
# happens to say.
CAL_SRC="${CALIBRATION_JSON:-}"
if [[ -z "$CAL_SRC" ]] && docker inspect "$SRR_CONTAINER" >/dev/null 2>&1; then
  CAL_SRC=$(docker inspect "$SRR_CONTAINER" \
    --format '{{range .Mounts}}{{if eq .Destination "/app/calibration.json"}}{{.Source}}{{end}}{{end}}' \
    2>/dev/null || echo "")
fi
CAMS_SRC="${CAMERAS_YAML:-$REPO_ROOT/closed-loop-testing/isaac-sim/sil/configs/cameras.yaml}"
CAL_COPIED="false"
CAMS_COPIED="false"
if [[ -n "$CAL_SRC" && -f "$CAL_SRC" ]]; then
  cp "$CAL_SRC" "$RUN_DIR/calibration-used.json" 2>/dev/null && CAL_COPIED="true"
fi
if [[ -f "$CAMS_SRC" ]]; then
  cp "$CAMS_SRC" "$RUN_DIR/cameras-used.yaml" 2>/dev/null && CAMS_COPIED="true"
fi
[[ "$CAL_COPIED" == "true" ]] || note "WARN no calibration copied — set CALIBRATION_JSON so the run records its geometry"

SRR_IMG=$(inspect_image "$SRR_CONTAINER")
PSF_IMG=$(inspect_image "safety-core")

# Values go through the environment rather than being interpolated into the
# script text: they are paths, command lines and image refs, and quoting them
# into a heredoc is how this kind of script grows a syntax error nobody hits
# until the one run that matters.
P_RUN_DIR="$RUN_DIR" \
P_GIT_COMMIT="$GIT_COMMIT" P_GIT_BRANCH="$GIT_BRANCH" P_GIT_DIRTY="$GIT_DIRTY" \
P_SRR_IMG="$SRR_IMG" P_PSF_IMG="$PSF_IMG" \
P_HOST_CODE_SHA="$HOST_CODE_SHA" P_CTR_CODE_SHA="$CTR_CODE_SHA" P_CODE_MATCH="$CODE_MATCH" \
P_CAL_SRC="$CAL_SRC" P_CAL_COPIED="$CAL_COPIED" \
P_CAMS_SRC="$CAMS_SRC" P_CAMS_COPIED="$CAMS_COPIED" \
python3 - <<'PYEOF'
import json, os, time

env = os.environ.get
run_dir = env("P_RUN_DIR", "")

def split_img(s):
    ref, iid, dig = (s.split("|", 2) + ["", "", ""])[:3]
    return {"ref": ref or None, "image_id": iid or None, "repo_digest": dig or None}

doc = {
    "written_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "timezone": time.strftime("%Z%z"),
    "host": os.uname().nodename,
    "run_dir": run_dir,
    "git": {
        "commit": env("P_GIT_COMMIT") or None,
        "branch": env("P_GIT_BRANCH") or None,
        "dirty": env("P_GIT_DIRTY"),
    },
    "images": {
        "srr": split_img(env("P_SRR_IMG", "")),
        "safety_core": split_img(env("P_PSF_IMG", "")),
    },
    "code": {
        "host_srr_sha256": env("P_HOST_CODE_SHA") or None,
        "container_srr_sha256": env("P_CTR_CODE_SHA") or None,
        "container_matches_host": env("P_CODE_MATCH"),
    },
    "geometry": {
        "calibration_source": env("P_CAL_SRC") or None,
        "calibration_copied": env("P_CAL_COPIED") == "true",
        "cameras_source": env("P_CAMS_SRC") or None,
        "cameras_copied": env("P_CAMS_COPIED") == "true",
    },
    "invocation": {
        "command": env("SRR_RUN_CMDLINE") or None,
        "psf_app": env("PSF_APP") or None,
        "profile_env": env("PROFILE_ENV") or None,
        "scenarios": env("SRR_RUN_SCENARIOS") or None,
        "num_streams": env("NUM_STREAMS") or None,
        "ros_domain_id": env("ROS_DOMAIN_ID") or None,
        "pss_strict": env("SRR_PSS_STRICT") or None,
    },
}

path = os.path.join(run_dir, "provenance.json")
try:
    with open(path, "w") as fh:
        json.dump(doc, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"  [provenance] wrote {path}")
except OSError as exc:
    print(f"  [provenance] WARN could not write {path}: {exc}")
PYEOF
