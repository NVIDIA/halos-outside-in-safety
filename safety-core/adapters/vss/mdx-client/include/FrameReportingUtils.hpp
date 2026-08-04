/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_FRAME_REPORTING_UTILS_HPP
#define MDX_CLIENT_FRAME_REPORTING_UTILS_HPP

#include "EventMappingValidation.hpp"

#include <cstdint>
#include <string>

namespace MDXClient {

// Pair candidates must retain both physical objects in PSS fusion metadata.
// This is deliberately keyed by the typed internal candidate kind, never by
// a generated/private rule identifier.
inline bool shouldPopulatePairFusionMetadata(AlertCandidateKind candidateKind) {
    return candidateKind == AlertCandidateKind::kProximityPair;
}

// Frame candidates are rate-selected solely from their immutable parser
// ordinal. This function intentionally has no violation-history input.
inline bool shouldReportFrameCandidate(const std::string& candidateMessageSource,
                                       const std::string& matchedRuleMessageSource,
                                       int32_t scaleFactor,
                                       uint64_t frameOrdinal) {
    if (candidateMessageSource != "mdx-frames" ||
        !matchesConfiguredStringCondition("mdx-frames", matchedRuleMessageSource) ||
        scaleFactor <= 1) {
        return true;
    }
    return frameOrdinal != 0U &&
           frameOrdinal % static_cast<uint64_t>(scaleFactor) == 0U;
}

} // namespace MDXClient

#endif
