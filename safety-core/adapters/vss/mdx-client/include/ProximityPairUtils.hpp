/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_PROXIMITY_PAIR_UTILS_HPP
#define MDX_CLIENT_PROXIMITY_PAIR_UTILS_HPP

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace MDXClient {

enum class ProximityGate {
    kInvalid,
    kFalse,
    kTrue,
};

inline bool shouldEvaluateProximityPairGroup(ProximityGate gate,
                                             bool hasNoViolationRule) {
    return gate == ProximityGate::kTrue ||
           (gate == ProximityGate::kFalse && hasNoViolationRule);
}

inline bool shouldEmitProximityPairCandidate(bool isViolation,
                                             bool hasNoViolationRule) {
    return isViolation || hasNoViolationRule;
}

inline bool isRepresentableProximityCoordinate(double value) {
    return std::isfinite(value) &&
           value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
           value <= static_cast<double>(std::numeric_limits<float>::max());
}

inline ProximityGate determineProximityGate(bool socialDistancingPresent,
                                            int32_t proximityDetections,
                                            const std::string& proximityViolation) {
    if (!socialDistancingPresent) {
        return ProximityGate::kFalse;
    }
    if (proximityDetections < 0) {
        return ProximityGate::kInvalid;
    }
    return proximityDetections == 0 && proximityViolation == "false"
        ? ProximityGate::kFalse : ProximityGate::kTrue;
}

inline bool selectProximityThreshold(const std::vector<double>& thresholds,
                                     double distance,
                                     double* selectedThreshold) {
    if (selectedThreshold == nullptr || !std::isfinite(distance) || distance < 0.0) {
        return false;
    }
    for (const double threshold : thresholds) {
        if (!std::isfinite(threshold) || threshold <= 0.0) {
            return false;
        }
        if (distance <= threshold) {
            *selectedThreshold = threshold;
            return true;
        }
    }
    return false;
}

} // namespace MDXClient

#endif
