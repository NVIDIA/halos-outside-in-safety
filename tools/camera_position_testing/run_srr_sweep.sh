#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Full 5-scenario SRR sweep for one camera placement, into its own run dir.
#
# Usage: ./run_srr_sweep.sh <label>      e.g. ./run_srr_sweep.sh gateF
#
# The guard below is the reason this wrapper exists. A pose change is only real
# once BOTH the Isaac cameras.yaml and the VSS calibration.json have moved: Isaac
# renders from the yaml, while VSS back-projects detections through the
# calibration's extrinsics. Change one and not the other and nothing errors —
# the pipeline stays healthy and simply reports wrong world positions, which
# looks like a perception regression rather than a config mistake. So identify
# which candidate the installed calibration is, and refuse to run unless the
# installed yaml is that same candidate.
set -uo pipefail

LABEL="${1:?usage: run_srr_sweep.sh <label>}"

# Everything is derived from where this script sits, so a fresh clone needs no editing:
# tools/camera_position_testing/ -> repo root. VSS is assumed to be a sibling checkout;
# override VSS_ROOT (or CALIBRATION_JSON directly) if it lives elsewhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOISA_ROOT_PATH="${HOISA_ROOT_PATH:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
VSS_ROOT="${VSS_ROOT:-$(dirname "$HOISA_ROOT_PATH")/video-search-and-summarization}"
CAND_DIR="${CAND_DIR:-$SCRIPT_DIR/candidates}"
CAMS_YAML_HOST="$HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil/configs/cameras.yaml"
CAL_JSON="${CALIBRATION_JSON:-$VSS_ROOT/deploy/docker/industry-profiles/warehouse-operations/warehouse-2d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic/calibration.json}"
SCRIPTS="$SCRIPT_DIR"
RUNS_HOST="$HOISA_ROOT_PATH/closed-loop-testing/regression-reporter/srr-service/runs"
VIEWER_DATA="${VIEWER_DATA:-$HOISA_ROOT_PATH/tools/srr-debug-viewer/data}"

[ -f "$CAL_JSON" ] || { echo "no calibration at $CAL_JSON — set VSS_ROOT or CALIBRATION_JSON"; exit 1; }
[ -d "$CAND_DIR" ] || { echo "no candidate dir at $CAND_DIR — set CAND_DIR to where your cal-*.json live"; exit 1; }

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

log "checking installed cameras.yaml agrees with installed calibration.json"
python3 - "$CAL_JSON" "$CAMS_YAML_HOST" "$CAND_DIR" <<'PY' || exit 1
import json, re, sys, glob, os
cal_p, yaml_p, cand = sys.argv[1:4]

def extr(p):
    d = json.load(open(p))
    return [[round(v, 6) for row in s["extrinsicMatrix"] for v in row] for s in d["sensors"]]

def poses(p):
    """(position, rotation) per camera — rotation included on purpose.

    Matching on position alone calls two candidates the same placement when they
    stand in the same spot and look in different directions, which is precisely the
    push-out sweep: every D has a re-aimed twin of some earlier candidate. That
    makes the guard either name the wrong candidate or, worse, wave through a yaml
    and a calibration that disagree about where the cameras point — the failure it
    exists to catch.
    """
    txt = open(p).read()
    return [(tuple(round(float(x), 3) for x in pos.split(",")),
             tuple(round(float(x), 3) for x in rot.split(",")))
            for pos, rot in re.findall(
                r"position:\s*\[([^\]]+)\][^}]*?rotation_yxz_deg:\s*\[([^\]]+)\]", txt)]

installed = extr(cal_p)
match = [f for f in sorted(glob.glob(os.path.join(cand, "cal-*.json")))
         if extr(f) == installed]
if not match:
    sys.exit(f"FATAL calibration {cal_p} matches no candidate in {cand}; "
             "regenerate it with generate_calibration.py for the pose you intend")
cal_name = os.path.basename(match[0])
ymatch = [f for f in sorted(glob.glob(os.path.join(cand, "cameras-*.yaml")))
          if poses(f) == poses(yaml_p)]
if not ymatch:
    sys.exit(f"FATAL installed cameras.yaml poses {poses(yaml_p)} match no candidate yaml")
yaml_name = os.path.basename(ymatch[0])
# cal-gate-M.json ↔ cameras-gate-M.yaml
if cal_name[len("cal-"):-len(".json")] != yaml_name[len("cameras-"):-len(".yaml")]:
    sys.exit(f"FATAL mismatch: calibration is {cal_name} but cameras.yaml is {yaml_name}.\n"
             "Isaac would render one pose while VSS back-projects through another.")
print(f"  OK both are '{cal_name}' / '{yaml_name}' — poses {poses(yaml_p)}")
PY

TS=$(date +%Y%m%d-%H%M%S)
export RUN_BASE="multi-test-$TS-$LABEL"
mkdir -p "$RUNS_HOST/$RUN_BASE"
log "run dir: $RUNS_HOST/$RUN_BASE"

# Register with the viewer up front so partial results are browsable mid-sweep.
if [ -d "$VIEWER_DATA" ]; then
  ln -sfn "$RUNS_HOST/$RUN_BASE" "$VIEWER_DATA/$RUN_BASE"
  python3 - "$VIEWER_DATA" <<'PY'
import json, os, sys
d = sys.argv[1]
runs = sorted(x for x in os.listdir(d) if x.startswith("multi-test-"))
json.dump({"runs": runs}, open(os.path.join(d, "index.json"), "w"))
print(f"  viewer index: {runs}")
PY
fi

# Durations follow FULL_PRESET in run_multi.sh.
exec bash "$SCRIPTS/run_srr_2cam.sh" \
  in-roi:300 psf-edge:300 psf-clear:300 balanced:600 fast:1200
