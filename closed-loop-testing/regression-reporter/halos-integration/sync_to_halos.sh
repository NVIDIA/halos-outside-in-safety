#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Sync SRR test fixtures into the sibling Halos Isaac SIL tree.
#
# The isaac-sim container bind-mounts closed-loop-testing/isaac-sim/sil/ at
# /isaac-sim/sil/, so SRR's scenes / behavior trees / navmesh / Script-Editor
# utilities have to land in that dir before Isaac starts.
#
# This script copies (NOT symlinks — bind-mounts don't always follow
# symlinks, and we want the SIL dir self-sufficient after sync). SRR fixtures
# are never committed there; the copies are deploy-time artifacts.
#
# Usage:
#   ./sync_to_halos.sh [/path/to/closed-loop-testing/isaac-sim/sil]
#
# The target is the Halos Isaac SIL asset tree: <repo>/closed-loop-testing/isaac-sim/sil
# With no arg, falls back to HALOS_SIL_DIR (export it, or set in the SRR .env),
# then to the sibling ../isaac-sim/sil.
#
# Re-run after:
#   - cloning the repo on a new machine
#   - editing any SRR scene / NavMesh under scenarios/
#   - regenerating behavior trees via randomize_paths.py (writes
#     scenarios/behavior-trees/*.bt.json — the canonical source this script copies)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRR_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Sibling Isaac SIL tree in this monorepo: regression-reporter/ and isaac-sim/
# are both under closed-loop-testing/.
DEFAULT_SIL="$(cd "$SRR_ROOT/.." && pwd)/isaac-sim/sil"

# Pick up HALOS_SIL_DIR from the SRR .env if not already exported.
if [[ -z "${HALOS_SIL_DIR:-}" && -f "$SRR_ROOT/.env" ]]; then
  set -a; source "$SRR_ROOT/.env"; set +a
fi

HALOS_SIL="${1:-${HALOS_SIL_DIR:-$DEFAULT_SIL}}"
if [[ -z "$HALOS_SIL" ]]; then
  echo "✗ No Isaac SIL dir given. Pass it as arg 1 or set HALOS_SIL_DIR." >&2
  echo "  e.g. $0 <repo>/closed-loop-testing/isaac-sim/sil" >&2
  exit 1
fi
if [[ ! -d "$HALOS_SIL" ]]; then
  echo "✗ Halos sil dir not found: $HALOS_SIL" >&2
  exit 1
fi

echo "→ Syncing SRR fixtures into Halos compose"
echo "  SRR  : $SRR_ROOT/scenarios"
echo "  Halos: $HALOS_SIL"
echo

# 1. Scenes
echo "[1/4] scenes/  →  $HALOS_SIL/scenes/"
mkdir -p "$HALOS_SIL/scenes"
cp -v "$SRR_ROOT"/scenarios/scenes/*.usd "$HALOS_SIL/scenes/" 2>/dev/null || true

# 2. Behavior trees (IRA 1.6) + navmesh
echo
echo "[2/4] behavior-trees/*.bt.json + scenes/navmesh.json  →  $HALOS_SIL/configs/"
mkdir -p "$HALOS_SIL/configs"
BT_DIR="$SRR_ROOT/scenarios/behavior-trees"
# behavior-trees/*.bt.json is the canonical source of truth (IRA 1.6 dropped the
# legacy command-file layer). Author/regenerate them with randomize_paths.py;
# here we just copy them into the Isaac SIL configs the container bind-mounts.
if ! ls "$BT_DIR"/*.bt.json >/dev/null 2>&1; then
  echo "✗ No behavior trees in $BT_DIR — generate them first (scenarios/tools/randomize_paths.py)." >&2
  exit 1
fi
cp -v "$BT_DIR"/*.bt.json "$HALOS_SIL/configs/" 2>/dev/null || true
cp -v "$SRR_ROOT"/scenarios/scenes/navmesh.json "$HALOS_SIL/configs/" 2>/dev/null || true
# Seed the FIXED active names the config references (srr_char{0,1,2}.bt.json) so a
# manual Isaac start works before run_multi.sh swaps a scenario in. Default: in-roi.
SEED="${SRR_SEED_SCENARIO:-in-roi}"
for i in 0 1 2; do
  if [[ -f "$HALOS_SIL/configs/srr_${SEED}_char${i}.bt.json" ]]; then
    cp -f "$HALOS_SIL/configs/srr_${SEED}_char${i}.bt.json" "$HALOS_SIL/configs/srr_char${i}.bt.json"
  fi
done
echo "  seeded active trees (srr_char{0,1,2}.bt.json) from scenario: $SEED"

# 3. Script-Editor utilities (flatten — container expects flat dir)
echo
echo "[3/4] isaac-scripts/**/*.py  →  $HALOS_SIL/scripts/isaac/  (flat)"
mkdir -p "$HALOS_SIL/scripts/isaac"
find "$SRR_ROOT/scenarios/isaac-scripts" -name '*.py' -not -path '*/__pycache__/*' \
  -exec cp -v {} "$HALOS_SIL/scripts/isaac/" \;

# 4. default_config_ros.yaml — copy the SRR template over Halos' STOCK config.
#    This is a DEPLOY-TIME working-tree change of a tracked Halos file (exactly
#    like deployments/profiles/sil.env) — do NOT commit it to Halos. run_multi.sh
#    then seds simulation_duration into it per scenario. Recover the stock config
#    any time with:  git -C <halos-repo> checkout origin/develop -- <this file>.
echo
echo "[4/4] default_config_ros.yaml  →  $HALOS_SIL/configs/  (SRR template — deploy-time, do NOT commit to Halos)"
TEMPLATE="$SCRIPT_DIR/default_config_ros.yaml.template"
TARGET="$HALOS_SIL/configs/default_config_ros.yaml"
if [[ ! -f "$TEMPLATE" ]]; then
  echo "✗ SRR template missing: $TEMPLATE" >&2
  exit 1
fi
cp -v "$TEMPLATE" "$TARGET"

echo
echo "✓ done. SRR fixtures are in $HALOS_SIL"
