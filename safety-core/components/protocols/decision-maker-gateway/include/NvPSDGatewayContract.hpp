/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSD_GATEWAY_CONTRACT_HPP
#define NVPSD_GATEWAY_CONTRACT_HPP

#include "NvPSDGatewayProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <unordered_set>

static constexpr uint8_t NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT = 32U;
static constexpr uint32_t NVPSD_GATEWAY_PSS_REQUEST_ID_MIN = 1U;
static constexpr uint32_t NVPSD_GATEWAY_PSS_REQUEST_ID_MAX = UINT32_MAX - 1U;

static inline void NvPSDGatewayAddMandatoryEventTypes(std::unordered_set<EventType>& types)
{
    types.insert(SW_FAIL);
    types.insert(PSS_STATUS_NOOP);
    /* Sensor-health evidence must reach valid SDM clients
     * even when their physical-event REGR list omits these health events. */
    types.insert(SENSOR_INVALID);
    types.insert(SENSOR_VALID);
}

static inline bool NvPSDGatewayIsAllowedHealthEventType(uint32_t raw)
{
    return raw == static_cast<uint32_t>(SENSOR_INVALID) ||
           raw == static_cast<uint32_t>(SENSOR_VALID);
}

static inline bool NvPSDGatewayBuildRegisteredEventSet(const uint32_t* rawEventTypes,
                                                       uint8_t count,
                                                       std::unordered_set<EventType>* outTypes)
{
    if (outTypes == nullptr)
        return false;

    outTypes->clear();

    if (rawEventTypes == nullptr || count == 0U ||
        count > NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT)
    {
        return false;
    }

    const uint32_t eventUnknownVal = static_cast<uint32_t>(EVENT_UNKNOWN);
    for (uint8_t i = 0U; i < count; ++i)
    {
        const uint32_t raw = rawEventTypes[i];
        if (raw >= eventUnknownVal && !NvPSDGatewayIsAllowedHealthEventType(raw))
            continue;
        outTypes->insert(static_cast<EventType>(raw));
    }

    if (outTypes->empty())
        return false;

    NvPSDGatewayAddMandatoryEventTypes(*outTypes);
    return true;
}

static inline bool NvPSDGatewayIsStatusNoopOnlyRequest(const DecisionRequest& req)
{
    return req.pssStatus.mode != ERROR &&
           req.sensorDataSummarySize == 1U &&
           req.sensorDataSummary[0].event.type == PSS_STATUS_NOOP;
}

static inline bool NvPSDGatewayShouldSendDecisionRequest(bool sendFullRequest,
                                                         uint8_t eventCount,
                                                         OperationalMode pssMode)
{
    if (sendFullRequest)
    {
        return eventCount > 0U || pssMode == ERROR;
    }
    return eventCount > 0U;
}

enum class NvPSDGatewayPacketSendResult : uint8_t
{
    SENT = 0U,
    FAILED_SYSCALL,
    FAILED_SHORT_SEND
};

static inline NvPSDGatewayPacketSendResult NvPSDGatewayDecisionPacketSendResult(
    ssize_t sentBytes)
{
    if (sentBytes < 0)
        return NvPSDGatewayPacketSendResult::FAILED_SYSCALL;
    if (sentBytes != static_cast<ssize_t>(sizeof(NvPSDGatewayDecisionRequestPacket)))
        return NvPSDGatewayPacketSendResult::FAILED_SHORT_SEND;
    return NvPSDGatewayPacketSendResult::SENT;
}

static inline bool NvPSDGatewayBuildDecisionRequestPacket(
    const DecisionRequest& req,
    uint64_t gatewayEpoch,
    uint32_t gatewayTxSeq,
    NvPSDGatewayDecisionRequestPacket* outPacket)
{
    static_assert(offsetof(NvPSDGatewayDecisionRequestPacket, request) <= UINT16_MAX,
                  "Gateway DecisionRequest packet header size must fit uint16_t");

    if (outPacket == nullptr ||
        !NvPSDGatewayDecisionPacketEpochIsValid(gatewayEpoch) ||
        !NvPSDGatewayDecisionPacketSequenceIsValid(gatewayTxSeq))
    {
        return false;
    }

    std::memset(outPacket, 0, sizeof(*outPacket));
    std::memcpy(outPacket->magic, NVPSD_GATEWAY_DECISION_MAGIC, 4U);
    outPacket->version = NVPSD_GATEWAY_DECISION_PACKET_VERSION;
    outPacket->headerSize = NVPSD_GATEWAY_DECISION_HEADER_SIZE;
    outPacket->gatewayEpoch = gatewayEpoch;
    outPacket->gatewayTxSeq = gatewayTxSeq;
    outPacket->request = req;
    return true;
}

struct NvPSDGatewayPssSequenceState
{
    bool initialized{false};
    uint32_t lastValidRequestId{0U};
};

enum class NvPSDGatewayPssRequestObservation : uint8_t
{
    ACCEPT = 0U,
    GAP,
    DUPLICATE_OR_STALE,
    INVALID
};

static inline bool NvPSDGatewayRequestIdIsSequenceTracked(uint32_t requestId)
{
    return requestId >= NVPSD_GATEWAY_PSS_REQUEST_ID_MIN &&
           requestId <= NVPSD_GATEWAY_PSS_REQUEST_ID_MAX;
}

static inline NvPSDGatewayPssRequestObservation NvPSDGatewayObservePssRequestId(
    NvPSDGatewayPssSequenceState* state,
    uint32_t requestId)
{
    if (state == nullptr || !NvPSDGatewayRequestIdIsSequenceTracked(requestId))
        return NvPSDGatewayPssRequestObservation::INVALID;

    if (!state->initialized)
    {
        state->initialized = true;
        state->lastValidRequestId = requestId;
        return NvPSDGatewayPssRequestObservation::ACCEPT;
    }

    const uint32_t last = state->lastValidRequestId;
    if (requestId == last)
        return NvPSDGatewayPssRequestObservation::DUPLICATE_OR_STALE;

    /* A backward jump can be a PSS daemon restart/reset because PSS starts
     * request ids at 1 on process launch. Treat it as a recoverable gap so
     * Gateway emits fault evidence and still forwards the current request. */
    const uint32_t expected = (last >= NVPSD_GATEWAY_PSS_REQUEST_ID_MAX)
        ? NVPSD_GATEWAY_PSS_REQUEST_ID_MIN
        : (last + 1U);
    const bool gap = requestId != expected;
    state->lastValidRequestId = requestId;
    return gap
        ? NvPSDGatewayPssRequestObservation::GAP
        : NvPSDGatewayPssRequestObservation::ACCEPT;
}

static inline bool NvPSDGatewayShouldForwardObservedPssRequest(
    NvPSDGatewayPssRequestObservation observation)
{
    return observation == NvPSDGatewayPssRequestObservation::ACCEPT ||
           observation == NvPSDGatewayPssRequestObservation::GAP;
}

static inline bool NvPSDGatewayBuildPssSequenceGapSwFailRequest(const DecisionRequest& current,
                                                                DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = current.requestId;
    outRequest->pssStatus = current.pssStatus;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = false;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = SW_FAIL;
    sensorData.event.severity = CRITICAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    (void)std::snprintf(sensorData.event.ruleIdentifier,
                        sizeof(sensorData.event.ruleIdentifier),
                        "%s", "PSS_REQUEST_SEQUENCE_GAP");
    return true;
}

static inline bool NvPSDGatewayBuildInvalidPssRequestIdSwFailRequest(const DecisionRequest& current,
                                                                     DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = NVPSD_GATEWAY_PSS_REQUEST_ID_MIN;
    outRequest->pssStatus = current.pssStatus;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = false;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = SW_FAIL;
    sensorData.event.severity = CRITICAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    (void)std::snprintf(sensorData.event.ruleIdentifier,
                        sizeof(sensorData.event.ruleIdentifier),
                        "%s", "PSS_REQUEST_ID_INVALID");
    return true;
}

static inline bool NvPSDGatewayBuildPssErrorSwFailRequest(const DecisionRequest& current,
                                                          DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = current.requestId;
    outRequest->pssStatus = current.pssStatus;
    outRequest->pssStatus.mode = NORMAL;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = false;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = SW_FAIL;
    sensorData.event.severity = CRITICAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    (void)std::snprintf(sensorData.event.ruleIdentifier,
                        sizeof(sensorData.event.ruleIdentifier),
                        "%s", "PSS_ERROR");
    return true;
}

#endif /* NVPSD_GATEWAY_CONTRACT_HPP */
