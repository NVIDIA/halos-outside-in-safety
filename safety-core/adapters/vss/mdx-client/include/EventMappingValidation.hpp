/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_EVENT_MAPPING_VALIDATION_HPP
#define MDX_CLIENT_EVENT_MAPPING_VALIDATION_HPP

#include "common.hpp"

#include <cctype>
#include <cmath>
#include <set>
#include <string>

namespace MDXClient {

inline bool isValidViolationFilter(const std::string& filter) {
    return filter.empty() || filter == "any" || filter == "true" || filter == "false";
}

inline bool isValidRoiClearObjectType(const std::string& restrictedFilter,
                                      const std::string& confinedFilter,
                                      const std::string& objectType) {
    (void)restrictedFilter;
    (void)confinedFilter;
    (void)objectType;
    return true;
}

inline bool matchesConfiguredStringCondition(const std::string& configuredValue,
                                             const std::string& candidateValue) {
    if (configuredValue.empty()) {
        return true;
    }
    if (candidateValue.empty() || configuredValue.size() != candidateValue.size()) {
        return false;
    }
    for (size_t index = 0; index < configuredValue.size(); ++index) {
        const unsigned char configuredCharacter =
            static_cast<unsigned char>(configuredValue[index]);
        const unsigned char candidateCharacter =
            static_cast<unsigned char>(candidateValue[index]);
        if (std::tolower(configuredCharacter) != std::tolower(candidateCharacter)) {
            return false;
        }
    }
    return true;
}

inline bool isValidProximityPairRuleId(const std::string& ruleId) {
    return !ruleId.empty() && ruleId.size() < IDENTIFIER_NAME_LENGTH;
}

inline std::string canonicalProximityPairRuleId(const std::string& ruleId) {
    std::string canonical = ruleId;
    for (size_t index = 0; index < canonical.size(); ++index) {
        canonical[index] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(canonical[index])));
    }
    return canonical;
}

inline bool insertUniqueProximityPairRuleId(std::set<std::string>* ruleIds,
                                            const std::string& ruleId) {
    return ruleIds != nullptr && isValidProximityPairRuleId(ruleId) &&
           ruleIds->insert(canonicalProximityPairRuleId(ruleId)).second;
}

inline std::string selectOutputRuleIdentifier(AlertCandidateKind candidateKind,
                                              const std::string& candidateRuleId,
                                              const std::string& configuredRuleId) {
    if (candidateKind != AlertCandidateKind::kProximityPair) {
        return candidateRuleId;
    }
    return isValidProximityPairRuleId(configuredRuleId)
        ? configuredRuleId : std::string();
}

inline std::string canonicalProximityTypePair(const std::string& primaryType,
                                              const std::string& secondaryType) {
    std::string first = primaryType;
    std::string second = secondaryType.empty() ? primaryType : secondaryType;
    for (size_t index = 0; index < first.size(); ++index) {
        first[index] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(first[index])));
    }
    for (size_t index = 0; index < second.size(); ++index) {
        second[index] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(second[index])));
    }
    if (second < first) {
        first.swap(second);
    }
    return first + "\x1f" + second;
}

inline bool isProximityPairRule(const std::string& alertType,
                                const std::string& configuredPrimaryType,
                                const std::string& configuredSecondaryType,
                                bool proximityViolationPresent,
                                bool distanceThresholdPresent) {
    return proximityViolationPresent || distanceThresholdPresent ||
           !configuredPrimaryType.empty() || !configuredSecondaryType.empty() ||
           matchesConfiguredStringCondition("no_violation", alertType);
}

inline bool isValidProximityPairRule(const std::string& messageSource,
                                     const std::string& alertType,
                                     const std::string& primaryType,
                                     const std::string& secondaryType,
                                     bool proximityViolationPresent,
                                     bool proximityViolation,
                                     bool distanceThresholdPresent,
                                     double distanceThreshold,
                                     bool hasViolationFilter) {
    (void)secondaryType;
    if (hasViolationFilter || primaryType.empty() || !proximityViolationPresent ||
        !matchesConfiguredStringCondition("mdx-frames", messageSource) ||
        !matchesConfiguredStringCondition("social_distancing", alertType)) {
        return false;
    }
    if (!proximityViolation) {
        return !distanceThresholdPresent;
    }
    return distanceThresholdPresent && std::isfinite(distanceThreshold) &&
           distanceThreshold > 0.0;
}

inline bool matchesConfiguredProximityPair(const std::string& primaryType,
                                           const std::string& secondaryType,
                                           const std::string& firstCandidateType,
                                           const std::string& secondCandidateType) {
    if (primaryType.empty()) {
        return false;
    }
    const std::string effectiveSecondaryType = secondaryType.empty()
        ? primaryType : secondaryType;
    return (matchesConfiguredStringCondition(primaryType, firstCandidateType) &&
            matchesConfiguredStringCondition(effectiveSecondaryType,
                                              secondCandidateType)) ||
           (matchesConfiguredStringCondition(primaryType, secondCandidateType) &&
            matchesConfiguredStringCondition(effectiveSecondaryType,
                                              firstCandidateType));
}

inline bool insertUniqueProximityPairRule(
        std::set<std::pair<std::string, double>>* thresholds,
        std::set<std::string>* noViolationPairs,
        const std::string& primaryType,
        const std::string& secondaryType,
        bool proximityViolation,
        double distanceThreshold) {
    if (thresholds == nullptr || noViolationPairs == nullptr ||
        primaryType.empty()) {
        return false;
    }
    const std::string pairKey = canonicalProximityTypePair(primaryType, secondaryType);
    if (proximityViolation) {
        if (!std::isfinite(distanceThreshold) || distanceThreshold <= 0.0) {
            return false;
        }
        return thresholds->insert(std::make_pair(pairKey, distanceThreshold)).second;
    }
    return noViolationPairs->insert(pairKey).second;
}

inline bool isValidViolationRuleScope(const std::string& restrictedFilter,
                                      const std::string& confinedFilter,
                                      const std::string& socialFilter,
                                      const std::string& messageSource,
                                      const std::string& alertType) {
    if (!isValidViolationFilter(restrictedFilter) ||
        !isValidViolationFilter(confinedFilter) ||
        !isValidViolationFilter(socialFilter)) {
        return false;
    }
    const bool hasRestrictedFilter = !restrictedFilter.empty();
    const bool hasConfinedFilter = !confinedFilter.empty();
    const bool hasSocialFilter = !socialFilter.empty();
    const int filterCount = static_cast<int>(hasRestrictedFilter) +
                            static_cast<int>(hasConfinedFilter) +
                            static_cast<int>(hasSocialFilter);
    if (filterCount > 1) {
        return false;
    }
    if (filterCount == 0) {
        return true;
    }
    if (!matchesConfiguredStringCondition("mdx-frames", messageSource)) {
        return false;
    }
    return (hasRestrictedFilter || hasConfinedFilter)
        ? matchesConfiguredStringCondition("roi", alertType)
        : matchesConfiguredStringCondition("social_distancing", alertType);
}

inline bool violationRuleMatchesCandidateScope(const std::string& restrictedFilter,
                                               const std::string& confinedFilter,
                                               const std::string& socialFilter,
                                               const std::string& configuredMessageSource,
                                               const std::string& configuredAlertType,
                                               const std::string& candidateMessageSource,
                                               const std::string& candidateAlertType) {
    if (!isValidViolationRuleScope(restrictedFilter, confinedFilter, socialFilter,
                                   configuredMessageSource, configuredAlertType)) {
        return false;
    }
    const bool hasRoiFilter = !restrictedFilter.empty() || !confinedFilter.empty();
    const bool hasSocialFilter = !socialFilter.empty();
    if (!hasRoiFilter && !hasSocialFilter) {
        return true;
    }
    if (!matchesConfiguredStringCondition("mdx-frames", candidateMessageSource)) {
        return false;
    }
    return hasRoiFilter
        ? matchesConfiguredStringCondition("roi", candidateAlertType)
        : matchesConfiguredStringCondition("social_distancing", candidateAlertType);
}

inline bool violationRuleMatchesCandidateKind(const std::string& restrictedFilter,
                                             const std::string& confinedFilter,
                                             const std::string& socialFilter,
                                             AlertCandidateKind candidateKind) {
    const bool hasRestrictedFilter = !restrictedFilter.empty();
    const bool hasConfinedFilter = !confinedFilter.empty();
    const bool hasSocialFilter = !socialFilter.empty();
    const int filterCount = static_cast<int>(hasRestrictedFilter) +
                            static_cast<int>(hasConfinedFilter) +
                            static_cast<int>(hasSocialFilter);
    if (filterCount == 0) {
        return true;
    }
    if (filterCount != 1) {
        return false;
    }
    if (hasRestrictedFilter) {
        return candidateKind == AlertCandidateKind::kRestrictedRoi;
    }
    if (hasConfinedFilter) {
        return candidateKind == AlertCandidateKind::kConfinedRoi;
    }
    return candidateKind == AlertCandidateKind::kFrameSocial;
}

} // namespace MDXClient

#endif
