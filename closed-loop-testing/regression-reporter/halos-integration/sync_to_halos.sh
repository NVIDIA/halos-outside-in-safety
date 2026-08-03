#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Sync SRR test fixtures into the sibling Isaac SIL tree.
#
# The isaac-sim container bind-mounts closed-loop-testing/isaac-sim/sil/ at
# /isaac-sim/sil/, so SRR's behavior trees / navmesh / Script-Editor utilities
# have to land in that dir before Isaac starts. SRR no longer ships a custom
# scene — it uses the stock scene already in isaac-sim/sil/scenes/.
#
# This script copies (NOT symlinks — bind-mounts don't always follow
# symlinks, and we want the SIL dir self-sufficient after sync). SRR fixtures
# are never committed there; the copies are deploy-time artifacts.
#
# Usage:
#   ./sync_to_halos.sh [/path/to/closed-loop-testing/isaac-sim/sil]
#
# The target is the Isaac SIL asset tree: <repo>/closed-loop-testing/isaac-sim/sil
# With no arg, it derives from HOISA_ROOT_PATH (export it, or set in the SRR
# .env), then falls back to the sibling ../isaac-sim/sil.
#
# Re-run after:
#   - cloning the repo on a new machine
#   - editing the SRR NavMesh export under scenarios/scenes/navmesh.json
#   - regenerating behavior trees via randomize_paths.py (writes
#     scenarios/behavior-trees/*.bt.json — the canonical source this script copies)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRR_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Sibling Isaac SIL tree in this monorepo: regression-reporter/ and isaac-sim/
# are both under closed-loop-testing/.
DEFAULT_SIL="$(cd "$SRR_ROOT/.." && pwd)/isaac-sim/sil"

# Pick up HOISA_ROOT_PATH from the SRR .env if not already exported.
if [[ -z "${HOISA_ROOT_PATH:-}" && -f "$SRR_ROOT/.env" ]]; then
  set -a; source "$SRR_ROOT/.env"; set +a
fi

# Derive the Isaac SIL tree from HOISA_ROOT_PATH when no explicit arg is given.
SIL_FROM_ROOT="${HOISA_ROOT_PATH:+$HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil}"
SIL="${1:-${SIL_FROM_ROOT:-$DEFAULT_SIL}}"
if [[ -z "$SIL" ]]; then
  echo "✗ No Isaac SIL dir given. Pass it as arg 1 or set HOISA_ROOT_PATH." >&2
  echo "  e.g. $0 <repo>/closed-loop-testing/isaac-sim/sil" >&2
  exit 1
fi
if [[ ! -d "$SIL" ]]; then
  echo "✗ Isaac SIL dir not found: $SIL" >&2
  exit 1
fi

echo "→ Syncing SRR fixtures into the Isaac SIL tree"
echo "  SRR : $SRR_ROOT/scenarios"
echo "  SIL : $SIL"
echo

# 1. Behavior trees (IRA 1.6) + navmesh
echo "[1/3] behavior-trees/*.bt.json + scenes/navmesh.json  →  $SIL/configs/"
mkdir -p "$SIL/configs"
BT_DIR="$SRR_ROOT/scenarios/behavior-trees"
# behavior-trees/*.bt.json is the canonical source of truth (IRA 1.6 dropped the
# legacy command-file layer). Author/regenerate them with randomize_paths.py;
# here we just copy them into the Isaac SIL configs the container bind-mounts.
if ! ls "$BT_DIR"/*.bt.json >/dev/null 2>&1; then
  echo "✗ No behavior trees in $BT_DIR — generate them first (scenarios/tools/randomize_paths.py)." >&2
  exit 1
fi
cp -v "$BT_DIR"/*.bt.json "$SIL/configs/" 2>/dev/null || true
cp -v "$SRR_ROOT"/scenarios/scenes/navmesh.json "$SIL/configs/" 2>/dev/null || true
# Seed the FIXED active names the config references (srr_char{0,1,2}.bt.json) so a
# manual Isaac start works before run_multi.sh swaps a scenario in. Default: in-roi.
SEED="${SRR_SEED_SCENARIO:-in-roi}"
for i in 0 1 2; do
  if [[ -f "$SIL/configs/srr_${SEED}_char${i}.bt.json" ]]; then
    cp -f "$SIL/configs/srr_${SEED}_char${i}.bt.json" "$SIL/configs/srr_char${i}.bt.json"
  fi
done
echo "  seeded active trees (srr_char{0,1,2}.bt.json) from scenario: $SEED"

# 2. Script-Editor utilities (flatten — container expects flat dir)
echo
echo "[2/3] isaac-scripts/**/*.py  →  $SIL/scripts/isaac/  (flat)"
mkdir -p "$SIL/scripts/isaac"
find "$SRR_ROOT/scenarios/isaac-scripts" -name '*.py' -not -path '*/__pycache__/*' \
  -exec cp -v {} "$SIL/scripts/isaac/" \;

# 3. default_config_ros.yaml — copy the SRR template over the STOCK config.
#    This is a DEPLOY-TIME working-tree change of a tracked file (exactly like
#    deployments/profiles/sil.env) — do NOT commit it. run_multi.sh then seds
#    simulation_duration into it per scenario. Recover the stock config any time
#    with:  git -C <repo> checkout origin/develop -- <this file>.
echo
echo "[3/3] default_config_ros.yaml  →  $SIL/configs/  (SRR template — deploy-time, do NOT commit)"
TEMPLATE="$SCRIPT_DIR/default_config_ros.yaml.template"
TARGET="$SIL/configs/default_config_ros.yaml"
if [[ ! -f "$TEMPLATE" ]]; then
  echo "✗ SRR template missing: $TEMPLATE" >&2
  exit 1
fi
cp -v "$TEMPLATE" "$TARGET"

echo
echo "✓ done. SRR fixtures are in $SIL"
