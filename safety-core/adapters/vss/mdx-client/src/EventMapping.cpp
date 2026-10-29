/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "EventMapping.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <utility>

#include "EventMappingValidation.hpp"
#include "NvPSB.h"

namespace MDXClient {

namespace {

std::string readStringField(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_STRING && result.data.s != nullptr) {
        std::string value(result.data.s);
        free(const_cast<char*>(result.data.s));
        return value;
    }
    return std::string();
}

double readDoubleField(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_DOUBLE) {
        return result.data.d;
    }
    if (result.type == NvPSF_VALUE_FLOAT) {
        return static_cast<double>(result.data.f);
    }
    return 0.0;
}

int32_t readInt32Field(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_INT32) {
        return result.data.i32;
    }
    return 0;
}

// Absent leaves the caller's default in place; present but of an unexpected type
// is treated as absent, and the range check in isValidCoMovementParams is what
// rejects a value that is present and wrong.
void readOptionalDoubleField(const NvPSFMsgCodecMsg* rule, const char* field, double* value) {
    if (rule == nullptr || field == nullptr || value == nullptr ||
        !NvPSFMsgCodecGetFieldPresence(rule, field)) {
        return;
    }
    const NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_DOUBLE) {
        *value = result.data.d;
    } else if (result.type == NvPSF_VALUE_FLOAT) {
        *value = static_cast<double>(result.data.f);
    }
}

void readOptionalInt32Field(const NvPSFMsgCodecMsg* rule, const char* field, int32_t* value) {
    if (rule == nullptr || field == nullptr || value == nullptr ||
        !NvPSFMsgCodecGetFieldPresence(rule, field)) {
        return;
    }
    const NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_INT32) {
        *value = result.data.i32;
    }
}

bool readOptionalBoolField(const NvPSFMsgCodecMsg* rule, const char* field, bool* value) {
    if (rule == nullptr || field == nullptr || value == nullptr ||
        !NvPSFMsgCodecGetFieldPresence(rule, field)) {
        return false;
    }
    const NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type != NvPSF_VALUE_BOOL) {
        return false;
    }
    *value = result.data.b;
    return true;
}

EventMappingRule readRule(const NvPSFMsgCodecMsg* ruleMsg) {
    EventMappingRule rule;
    rule.name = readStringField(ruleMsg, "name");
    rule.messageSource = readStringField(ruleMsg, "message_source");
    rule.alertType = readStringField(ruleMsg, "alert_type");
    rule.eventType = readStringField(ruleMsg, "event_type");
    rule.objectType = readStringField(ruleMsg, "object_type");
    rule.ruleId = readStringField(ruleMsg, "rule_id");
    rule.restrictedFilter = readStringField(ruleMsg, "restricted_area_violation");
    rule.confinedFilter = readStringField(ruleMsg, "confined_area_violation");
    rule.socialFilter = readStringField(ruleMsg, "social_distancing_violation");
    rule.outputEvent = readStringField(ruleMsg, "output_event");
    rule.configuredPrimaryType = readStringField(ruleMsg, "object_type_primary");
    rule.configuredSecondaryType = readStringField(ruleMsg, "object_type_secondary");
    rule.distanceThreshold = readDoubleField(ruleMsg, "distance_threshold_meters");
    rule.distanceThresholdPresent = NvPSFMsgCodecGetFieldPresence(
        ruleMsg, "distance_threshold_meters");
    rule.scaleFactor = readInt32Field(ruleMsg, "scale_factor");
    rule.proximityViolationPresent = readOptionalBoolField(
        ruleMsg, "proximity_violation", &rule.proximityViolation);

    readOptionalDoubleField(ruleMsg, "comovement_max_distance_meters",
                            &rule.coMovement.maxDistanceMeters);
    readOptionalDoubleField(ruleMsg, "comovement_max_offset_sd_meters",
                            &rule.coMovement.maxOffsetSdMeters);
    readOptionalInt32Field(ruleMsg, "comovement_window_ms", &rule.coMovement.windowMs);
    readOptionalInt32Field(ruleMsg, "comovement_min_samples", &rule.coMovement.minSamples);
    readOptionalInt32Field(ruleMsg, "comovement_min_span_ms", &rule.coMovement.minSpanMs);
    readOptionalDoubleField(ruleMsg, "comovement_min_forklift_travel_meters",
                            &rule.coMovement.minCarrierTravelMeters);
    readOptionalDoubleField(ruleMsg, "comovement_min_box_containment",
                            &rule.coMovement.minBoxContainment);
    readOptionalInt32Field(ruleMsg, "comovement_enter_frames", &rule.coMovement.enterFrames);
    readOptionalInt32Field(ruleMsg, "comovement_exit_frames", &rule.coMovement.exitFrames);
    readOptionalInt32Field(ruleMsg, "comovement_latch_ms", &rule.coMovement.latchMs);

    rule.primaryType = rule.configuredPrimaryType.empty()
        ? rule.objectType : rule.configuredPrimaryType;
    rule.hasViolationFilter = !rule.restrictedFilter.empty() ||
                              !rule.confinedFilter.empty() ||
                              !rule.socialFilter.empty();
    rule.isPairRule = isProximityPairRule(
        rule.alertType, rule.configuredPrimaryType, rule.configuredSecondaryType,
        rule.proximityViolationPresent, rule.distanceThresholdPresent);
    rule.isCoMovementRule = isCoMovementAlertType(rule.alertType);
    return rule;
}

bool isUsableCoMovementRule(const EventMappingRule& rule) {
    return rule.isCoMovementRule &&
           isValidCoMovementRule(rule.messageSource, rule.alertType, rule.primaryType,
                                 rule.configuredSecondaryType, rule.proximityViolationPresent,
                                 rule.distanceThresholdPresent, rule.hasViolationFilter,
                                 rule.outputEvent) &&
           isValidProximityPairRuleId(rule.ruleId) &&
           isValidCoMovementParams(rule.coMovement);
}

void buildCoMovementGroups(const std::vector<EventMappingRule>& rules,
                           std::vector<CoMovementRuleGroup>* groups) {
    for (size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
        const EventMappingRule& rule = rules[ruleIndex];
        if (!isUsableCoMovementRule(rule)) {
            continue;
        }
        CoMovementRuleGroup group;
        group.carrierType = rule.primaryType;
        group.riderType = rule.configuredSecondaryType;
        group.params = rule.coMovement;
        groups->push_back(group);
    }
}

bool isUsablePairRule(const EventMappingRule& rule) {
    return rule.isPairRule &&
           isValidProximityPairRule(rule.messageSource, rule.alertType, rule.primaryType,
                                    rule.configuredSecondaryType,
                                    rule.proximityViolationPresent, rule.proximityViolation,
                                    rule.distanceThresholdPresent, rule.distanceThreshold,
                                    rule.hasViolationFilter) &&
           isValidProximityPairRuleId(rule.ruleId);
}

void buildProximityPairGroups(const std::vector<EventMappingRule>& rules,
                              std::vector<ProximityPairRuleGroup>* groups) {
    std::unordered_map<std::string, size_t> groupIndexes;
    for (size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
        const EventMappingRule& rule = rules[ruleIndex];
        if (!isUsablePairRule(rule)) {
            continue;
        }
        const std::string pairKey = canonicalProximityTypePair(
            rule.primaryType, rule.configuredSecondaryType);
        std::unordered_map<std::string, size_t>::const_iterator groupIndex =
            groupIndexes.find(pairKey);
        if (groupIndex == groupIndexes.end()) {
            ProximityPairRuleGroup group;
            group.primaryType = rule.primaryType;
            group.secondaryType = rule.configuredSecondaryType;
            groups->push_back(group);
            groupIndex = groupIndexes.insert(
                std::make_pair(pairKey, groups->size() - 1U)).first;
        }
        if (rule.proximityViolation) {
            (*groups)[groupIndex->second].thresholds.push_back(rule.distanceThreshold);
        } else {
            (*groups)[groupIndex->second].hasNoViolationRule = true;
        }
    }
    for (size_t groupIndex = 0; groupIndex < groups->size(); ++groupIndex) {
        std::vector<double>& thresholds = (*groups)[groupIndex].thresholds;
        std::sort(thresholds.begin(), thresholds.end());
    }
}

} // namespace

bool compileEventMapping(const NvPSFMsgCodecMsg* config, EventMapping* mapping) {
    if (config == nullptr || mapping == nullptr) {
        return false;
    }
    mapping->rules.clear();
    mapping->proximityPairGroups.clear();
    mapping->coMovementGroups.clear();

    // An empty rules list compiles successfully; the caller rejects it, keeping
    // the "no rules in config" diagnostic in one place.
    const int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    if (rulesCount > 0) {
        mapping->rules.reserve(static_cast<size_t>(rulesCount));
    }
    for (int ruleIndex = 0; ruleIndex < rulesCount; ++ruleIndex) {
        NvPSFMsgCodecMsg* ruleMsg = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", ruleIndex);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &ruleMsg) != NvPSFMSGCODEC_SUCCESS ||
            ruleMsg == nullptr) {
            if (ruleMsg != nullptr) {
                NvPSFMsgCodecFreeMsg(ruleMsg);
            }
            mapping->rules.clear();
            NvPSBWriteData(NVPSB_LOG_ERR, "Invalid event mapping rule", "");
            return false;
        }
        const EventMappingRule rule = readRule(ruleMsg);
        NvPSFMsgCodecFreeMsg(ruleMsg);
        mapping->rules.push_back(rule);
    }
    buildProximityPairGroups(mapping->rules, &mapping->proximityPairGroups);
    buildCoMovementGroups(mapping->rules, &mapping->coMovementGroups);
    return true;
}

bool validateEventMapping(const EventMapping& mapping) {
    std::set<std::pair<std::string, double>> proximityThresholds;
    std::set<std::string> proximityNoViolationPairs;
    // Shared with the pair rules: both categories emit their configured rule_id as
    // the PSS rule identifier, so the identifier must be unique across them.
    std::set<std::string> proximityRuleIds;
    std::set<std::string> coMovementPairs;
    for (size_t ruleIndex = 0; ruleIndex < mapping.rules.size(); ++ruleIndex) {
        const EventMappingRule& rule = mapping.rules[ruleIndex];
        const bool hasValidPairRule = !rule.isPairRule ||
            isValidProximityPairRule(rule.messageSource, rule.alertType, rule.primaryType,
                                     rule.configuredSecondaryType,
                                     rule.proximityViolationPresent, rule.proximityViolation,
                                     rule.distanceThresholdPresent, rule.distanceThreshold,
                                     rule.hasViolationFilter);
        const bool hasValidPairRuleId = !rule.isPairRule ||
            isValidProximityPairRuleId(rule.ruleId);
        // A co-movement rule is rejected rather than skipped when malformed: the
        // consumer suppresses a stop on its event, so silently dropping the rule
        // would be a change in safety behaviour that nothing reports.
        const bool hasValidCoMovementRule = !rule.isCoMovementRule ||
            isUsableCoMovementRule(rule);
        bool hasUniqueCoMovementPair = true;
        if (hasValidCoMovementRule && rule.isCoMovementRule) {
            hasUniqueCoMovementPair = coMovementPairs.insert(
                canonicalProximityTypePair(rule.primaryType,
                                           rule.configuredSecondaryType)).second &&
                insertUniqueProximityPairRuleId(&proximityRuleIds, rule.ruleId);
        }
        bool hasUniquePairRule = true;
        bool hasUniquePairRuleId = true;
        if (hasValidPairRule && hasValidPairRuleId && rule.isPairRule) {
            hasUniquePairRule = insertUniqueProximityPairRule(
                &proximityThresholds, &proximityNoViolationPairs, rule.primaryType,
                rule.configuredSecondaryType, rule.proximityViolation,
                rule.distanceThreshold);
            if (hasUniquePairRule) {
                hasUniquePairRuleId = insertUniqueProximityPairRuleId(
                    &proximityRuleIds, rule.ruleId);
            }
        }
        const bool valid = isValidViolationFilter(rule.restrictedFilter) &&
                           isValidViolationFilter(rule.confinedFilter) &&
                           isValidViolationFilter(rule.socialFilter) &&
                           isValidViolationRuleScope(rule.restrictedFilter,
                                                     rule.confinedFilter, rule.socialFilter,
                                                     rule.messageSource, rule.alertType) &&
                           isValidRoiClearObjectType(rule.restrictedFilter,
                                                     rule.confinedFilter, rule.objectType) &&
                           hasValidPairRule && hasValidPairRuleId &&
                           hasUniquePairRule && hasUniquePairRuleId &&
                           hasValidCoMovementRule && hasUniqueCoMovementPair;
        if (!valid) {
            NvPSBWriteData(NVPSB_LOG_ERR, "Rejected unsafe event mapping rule", "");
            return false;
        }
    }
    return true;
}

} // namespace MDXClient
