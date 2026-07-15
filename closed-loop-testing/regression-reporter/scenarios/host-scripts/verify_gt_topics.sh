#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Verify the 4 SRR ground-truth TF topics from host ROS.
# Assumes Isaac Sim scene is loaded, /World/SRRGraph created (add_srr_gt_pubs.py
# was run), and timeline is playing.
#
# Run: source ROS setup first if you haven't (Humble or Jazzy), then:
#   bash scenarios/host-scripts/verify_gt_topics.sh   (from the regression-reporter root)

set -u

export ROS_DOMAIN_ID=74

EXPECTED=(
    /gt/character_0/tf
    /gt/character_1/tf
    /gt/character_2/tf
    /gt/forklift/tf
)

if ! command -v ros2 >/dev/null; then
    echo "FAIL  ros2 CLI not found — source /opt/ros/<distro>/setup.bash first"
    exit 1
fi

echo "=== ROS env ==="
echo "  ROS_DOMAIN_ID=$ROS_DOMAIN_ID"
echo "  ROS_DISTRO=${ROS_DISTRO:-unknown}"
echo "  RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-default}"
echo

echo "=== [1/3] Topic discovery (5s wait for DDS) ==="
sleep 5
LIST=$(ros2 topic list 2>/dev/null)
MISSING=()
for t in "${EXPECTED[@]}"; do
    if grep -qx "$t" <<<"$LIST"; then
        echo "  PASS  $t"
    else
        echo "  FAIL  $t (not visible)"
        MISSING+=("$t")
    fi
done
if [ ${#MISSING[@]} -ne 0 ]; then
    echo
    echo "  ALL discovered topics:"
    grep -E '^/gt|^/tf' <<<"$LIST" | sed 's/^/    /'
    echo
    echo "  Hint: confirm timeline is playing in Isaac Sim, ROS_DOMAIN_ID matches container,"
    echo "        and the host shares network with the isaac-sim container (host network mode)."
    exit 2
fi
echo

echo "=== [2/3] Publish rate (3s sample per topic) ==="
for t in "${EXPECTED[@]}"; do
    HZ=$(timeout 4 ros2 topic hz "$t" 2>&1 | grep -m1 "average rate" || true)
    echo "  $t :  ${HZ:-no rate (no msgs in 4s)}"
done
echo

echo "=== [3/3] Sample message per topic ==="
for t in "${EXPECTED[@]}"; do
    echo "--- $t ---"
    timeout 3 ros2 topic echo --once "$t" 2>/dev/null \
      | grep -E "child_frame_id|translation:|x:|y:|z:|stamp" | head -15
    echo
done

echo "Done."
