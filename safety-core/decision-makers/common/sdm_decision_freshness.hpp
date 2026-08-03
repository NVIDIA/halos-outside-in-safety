/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SDM_DECISION_FRESHNESS_HPP
#define SDM_DECISION_FRESHNESS_HPP

#include <cstdint>

static constexpr std::uint32_t SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT = 7000U;
static constexpr std::uint32_t SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN = 50U;
static constexpr std::uint32_t SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX = 300000U;
static constexpr std::uint32_t SDM_DECISION_FRESHNESS_CHECK_PERIOD_MS = 500U;

static inline bool sdmDecisionFreshnessTimeoutMsIsValid(std::uint32_t timeoutMs)
{
    return timeoutMs >= SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN &&
           timeoutMs <= SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX;
}

static inline bool sdmDecisionFreshnessExpired(std::uint64_t elapsedMs,
                                               std::uint32_t timeoutMs)
{
    return elapsedMs > static_cast<std::uint64_t>(timeoutMs);
}

#endif /* SDM_DECISION_FRESHNESS_HPP */
