/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROXIMITY_SDM_CONFIG_H
#define PROXIMITY_SDM_CONFIG_H

#include <cstddef>
#include <string>
#include "pss_protocol.h"

/* Agility humanoid, forklift, transporter, Fourier humanoid, each paired with
 * Person and ordered NORMAL, REDUCE, STOP. Selection and event codes are
 * startup configuration; tier semantics and decision policy are unchanged. */
constexpr std::size_t PROXIMITY_PAIR_GROUP_COUNT = 4U;
constexpr std::size_t PROXIMITY_TIERS_PER_GROUP = 3U;
constexpr std::size_t PROXIMITY_EVENT_COUNT =
    PROXIMITY_PAIR_GROUP_COUNT * PROXIMITY_TIERS_PER_GROUP;

struct ProximitySdmConfig {
    EventType events[PROXIMITY_EVENT_COUNT];
};

/* Transactional startup load. pair_groups selects complete three-tier groups;
 * without it, the original three groups (nine keys) remain required. Selected
 * keys must use distinct EVENT_0..EVENT_23 codes. Unselected groups have all
 * three slots set to EVENT_UNKNOWN and must have no keys in the config.
 * At least one group is required. out is untouched on failure. */
bool proximitySdmConfigLoad(const std::string& path, ProximitySdmConfig& out,
                           std::string& err);
const char* proximityEventKey(std::size_t index);

#endif /* PROXIMITY_SDM_CONFIG_H */
