/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSD_INTERNAL_HPP
#define NVPSD_INTERNAL_HPP

#include "NvPSD.h"

#include <cstdint>

static inline bool NvPSDIsDecisionResponseTimeoutMsValid(uint32_t timeoutMs)
{
    return timeoutMs >= NVPSD_DECISION_RESPONSE_TIMEOUT_MS_MIN &&
           timeoutMs <= NVPSD_DECISION_RESPONSE_TIMEOUT_MS_MAX;
}

static inline bool NvPSDIsCompleteDecisionResponseSize(int receivedBytes)
{
    return receivedBytes == static_cast<int>(sizeof(DecisionResponse));
}

static inline bool NvPSDDecisionResponseMatchesRequest(const DecisionRequest& request,
                                                       const DecisionResponse& response)
{
    return request.requestId != 0U &&
           response.decisionId != 0U &&
           response.decisionId == request.requestId;
}

enum class NvPSDDecisionResponseReceiveAction : uint8_t
{
    ACCEPT = 0U,
    DISCARD_AND_CONTINUE
};

static inline NvPSDDecisionResponseReceiveAction NvPSDDecisionResponseReceiveActionFor(
    const DecisionRequest& request,
    const DecisionResponse& response,
    int receivedBytes)
{
    if (!NvPSDIsCompleteDecisionResponseSize(receivedBytes) ||
        !NvPSDDecisionResponseMatchesRequest(request, response))
    {
        return NvPSDDecisionResponseReceiveAction::DISCARD_AND_CONTINUE;
    }

    return NvPSDDecisionResponseReceiveAction::ACCEPT;
}

#endif /* NVPSD_INTERNAL_HPP */
