/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SDM_GATEWAY_DECISION_SEQUENCE_HPP
#define SDM_GATEWAY_DECISION_SEQUENCE_HPP

#include "NvPSDGatewayProtocol.h"

#include <cstdint>

enum SdmGatewayDecisionSequenceResult
{
    SDM_GATEWAY_DECISION_SEQUENCE_ACCEPTED = 0,
    SDM_GATEWAY_DECISION_SEQUENCE_GAP,
    SDM_GATEWAY_DECISION_SEQUENCE_DUPLICATE_OR_STALE,
    SDM_GATEWAY_DECISION_SEQUENCE_INVALID
};

struct SdmGatewayDecisionSequenceState
{
    bool initialized{false};
    std::uint64_t gatewayEpoch{0U};
    std::uint32_t lastGatewayTxSeq{0U};
};

static inline void sdmGatewayDecisionSequenceReset(SdmGatewayDecisionSequenceState* state)
{
    if (state == nullptr)
        return;
    state->initialized = false;
    state->gatewayEpoch = 0U;
    state->lastGatewayTxSeq = 0U;
}

static inline SdmGatewayDecisionSequenceResult sdmGatewayDecisionSequenceObserve(
    SdmGatewayDecisionSequenceState* state,
    std::uint64_t gatewayEpoch,
    std::uint32_t gatewayTxSeq)
{
    if (state == nullptr ||
        !NvPSDGatewayDecisionPacketEpochIsValid(gatewayEpoch) ||
        !NvPSDGatewayDecisionPacketSequenceIsValid(gatewayTxSeq))
    {
        return SDM_GATEWAY_DECISION_SEQUENCE_INVALID;
    }

    if (!state->initialized || gatewayEpoch != state->gatewayEpoch)
    {
        state->initialized = true;
        state->gatewayEpoch = gatewayEpoch;
        state->lastGatewayTxSeq = gatewayTxSeq;
        return SDM_GATEWAY_DECISION_SEQUENCE_ACCEPTED;
    }

    const std::uint32_t last = state->lastGatewayTxSeq;
    if (last < NVPSD_GATEWAY_DECISION_TX_SEQ_MAX && gatewayTxSeq <= last)
        return SDM_GATEWAY_DECISION_SEQUENCE_DUPLICATE_OR_STALE;

    const std::uint32_t expected =
        (last >= NVPSD_GATEWAY_DECISION_TX_SEQ_MAX)
            ? NVPSD_GATEWAY_DECISION_TX_SEQ_MIN
            : (last + 1U);

    state->lastGatewayTxSeq = gatewayTxSeq;
    if (gatewayTxSeq != expected)
        return SDM_GATEWAY_DECISION_SEQUENCE_GAP;

    return SDM_GATEWAY_DECISION_SEQUENCE_ACCEPTED;
}

#endif /* SDM_GATEWAY_DECISION_SEQUENCE_HPP */
