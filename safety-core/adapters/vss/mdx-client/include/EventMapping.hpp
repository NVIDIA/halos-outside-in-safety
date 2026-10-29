/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Event-mapping configuration decoded into plain C++ fields once at startup.
 * The configuration is static for the process lifetime, so the frames parser and
 * the reporter read this instead of walking the protobuf config per frame and
 * per candidate alert.
 */

#ifndef MDX_CLIENT_EVENT_MAPPING_HPP
#define MDX_CLIENT_EVENT_MAPPING_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "CoMovementTracker.hpp"
#include "NvPSFMsgCodec.h"

namespace MDXClient {

// One rules[] entry of the event-mapping config. Every field a consumer needs is
// held by value, so matching a candidate performs no codec access and no
// allocation.
struct EventMappingRule {
    std::string name;
    std::string messageSource;
    std::string alertType;
    std::string eventType;
    std::string objectType;
    std::string ruleId;
    std::string restrictedFilter;
    std::string confinedFilter;
    std::string socialFilter;
    std::string outputEvent;
    std::string configuredPrimaryType;
    std::string configuredSecondaryType;
    double distanceThreshold = 0.0;
    int32_t scaleFactor = 0;
    // distance_threshold_meters and proximity_violation are optional: presence
    // distinguishes a configured value from a defaulted one.
    bool distanceThresholdPresent = false;
    bool proximityViolation = false;
    bool proximityViolationPresent = false;

    // Co-movement rules only. Absent config fields keep the tracker's defaults,
    // so a rule may declare just the type pair and the output event.
    CoMovementParams coMovement;

    // Derived from the fields above at compile time. Every consumer previously
    // recomputed these identically per frame.
    std::string primaryType;
    bool hasViolationFilter = false;
    bool isPairRule = false;
    bool isCoMovementRule = false;
};

// All valid pair rules sharing one unordered (primary, secondary) type pair.
struct ProximityPairRuleGroup {
    std::string primaryType;
    std::string secondaryType;
    std::vector<double> thresholds;  // ascending
    bool hasNoViolationRule = false;
};

// One usable co-movement rule, resolved into the roles the tracker works in.
struct CoMovementRuleGroup {
    std::string carrierType;
    std::string riderType;
    CoMovementParams params;
};

// Immutable after compileEventMapping returns, so both consumer threads may read
// it concurrently without synchronization.
struct EventMapping {
    std::vector<EventMappingRule> rules;
    std::vector<ProximityPairRuleGroup> proximityPairGroups;
    std::vector<CoMovementRuleGroup> coMovementGroups;
};

// Decode every rule and derive the proximity pair groups. This is the only place
// that reads the event-mapping config through the codec.
bool compileEventMapping(const NvPSFMsgCodecMsg* config, EventMapping* mapping);

// Reject a configuration whose rules are individually unsafe or mutually
// inconsistent. Operates purely on compiled rules.
bool validateEventMapping(const EventMapping& mapping);

} // namespace MDXClient

#endif
