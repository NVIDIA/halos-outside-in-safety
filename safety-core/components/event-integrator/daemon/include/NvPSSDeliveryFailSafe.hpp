/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSS_DELIVERY_FAILSAFE_HPP
#define NVPSS_DELIVERY_FAILSAFE_HPP

#include "NvPSD.h"
#include "pss_protocol.h"

#include <atomic>
#include <cstdint>

static constexpr uint32_t NVPSS_PSS_TO_PSD_RETRY_BUDGET_DEFAULT = 3U;
static constexpr uint32_t NVPSS_PSS_TO_PSD_RETRY_BUDGET_MIN = 1U;
static constexpr uint32_t NVPSS_PSS_TO_PSD_RETRY_BUDGET_MAX = 10U;
static constexpr uint32_t NVPSS_PSS_TO_PSD_RETRY_WINDOW_MS_MAX = 6000U;
static constexpr uint32_t NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_DEFAULT =
    NVPSD_DECISION_RESPONSE_TIMEOUT_MS_DEFAULT;
static constexpr uint32_t NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_MIN =
    NVPSD_DECISION_RESPONSE_TIMEOUT_MS_MIN;
static constexpr uint32_t NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_MAX =
    NVPSD_DECISION_RESPONSE_TIMEOUT_MS_MAX;

enum class NvPSSDeliveryState : uint8_t
{
    NORMAL = 0U,
    DELIVERY_RETRYING,
    DELIVERY_ERROR_ACTIVE
};

enum class NvPSSDeliveryFailureReason : uint8_t
{
    NONE = 0U,
    PROCESS_DECISION_REQUEST_FAILED,
    RESPONSE_TIMEOUT
};

enum class NvPSSPsdSendPriority : uint8_t
{
    CRITICAL = 0U,
    OPERATIONAL
};

enum class NvPSSDecisionResponseReceiveAction : uint8_t
{
    ACCEPT = 0U,
    DISCARD_AND_CONTINUE,
    FAIL
};

static constexpr uint64_t NVPSS_PSD_SEND_NS_PER_US = 1000ULL;
static constexpr uint64_t NVPSS_PSD_SEND_NS_PER_MS = 1000000ULL;
static constexpr uint64_t NVPSS_PSD_SEND_OPERATIONAL_RELEASE_PERIOD_US = 2500ULL;
static constexpr uint64_t NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_MS = 100ULL;
static constexpr uint64_t NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_NS =
    NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_MS * NVPSS_PSD_SEND_NS_PER_MS;
static constexpr uint32_t NVPSS_DECISION_REQUEST_ID_MIN = 1U;
static constexpr uint32_t NVPSS_DECISION_REQUEST_ID_MAX = UINT32_MAX - 1U;

struct NvPSSPsdReleaseGate
{
    bool armed;
    uint64_t deadlineNs;
};

struct NvPSSPsdReleaseSnapshot
{
    bool criticalQueued;
    bool operationalQueued;
    NvPSSPsdReleaseGate operationalGate;
    uint64_t nowNs;
};

static inline uint64_t NvPSSPsdSaturatingAddNs(uint64_t baseNs, uint64_t deltaNs)
{
    return (UINT64_MAX - baseNs < deltaNs) ? UINT64_MAX : (baseNs + deltaNs);
}

static inline uint64_t NvPSSPsdSendPriorityReleasePeriodNs(NvPSSPsdSendPriority priority)
{
    switch (priority)
    {
        case NvPSSPsdSendPriority::OPERATIONAL:
            return NVPSS_PSD_SEND_OPERATIONAL_RELEASE_PERIOD_US * NVPSS_PSD_SEND_NS_PER_US;
        default:
            return 0ULL;
    }
}

static inline void NvPSSPsdUpdateReleaseGate(NvPSSPsdReleaseGate* gate,
                                             bool queued,
                                             uint64_t nowNs,
                                             uint64_t periodNs)
{
    if (gate == nullptr)
        return;

    if (!queued)
    {
        gate->armed = false;
        gate->deadlineNs = 0ULL;
        return;
    }

    if (!gate->armed)
    {
        gate->armed = true;
        gate->deadlineNs = NvPSSPsdSaturatingAddNs(nowNs, periodNs);
    }
}

static inline void NvPSSPsdForceReleaseGateDue(NvPSSPsdReleaseGate* gate,
                                               uint64_t nowNs)
{
    if (gate == nullptr)
        return;

    gate->armed = true;
    gate->deadlineNs = nowNs;
}

static inline void NvPSSPsdAdvanceReleaseGateAfterSend(NvPSSPsdReleaseGate* gate,
                                                       bool stillQueued,
                                                       uint64_t nowNs,
                                                       uint64_t periodNs)
{
    if (gate == nullptr)
        return;

    if (!stillQueued)
    {
        gate->armed = false;
        gate->deadlineNs = 0ULL;
        return;
    }

    gate->armed = true;
    gate->deadlineNs = NvPSSPsdSaturatingAddNs(nowNs, periodNs);
}

static inline void NvPSSPsdUpdateStatusNoopDeadline(uint64_t* nextStatusNoopNs,
                                                    bool enqueueSucceeded,
                                                    uint64_t nowNs,
                                                    uint64_t intervalNs)
{
    if (nextStatusNoopNs == nullptr)
        return;

    const uint64_t delayNs = enqueueSucceeded
        ? intervalNs
        : NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_NS;
    *nextStatusNoopNs = NvPSSPsdSaturatingAddNs(nowNs, delayNs);
}

static inline bool NvPSSPsdReleaseGateIsDue(bool queued,
                                            const NvPSSPsdReleaseGate& gate,
                                            uint64_t nowNs)
{
    return queued && gate.armed && nowNs >= gate.deadlineNs;
}

static inline bool NvPSSPsdSelectDuePriority(const NvPSSPsdReleaseSnapshot* snapshot,
                                             NvPSSPsdSendPriority* selected)
{
    if (snapshot == nullptr || selected == nullptr)
        return false;

    if (snapshot->criticalQueued)
    {
        *selected = NvPSSPsdSendPriority::CRITICAL;
        return true;
    }
    if (NvPSSPsdReleaseGateIsDue(snapshot->operationalQueued,
                                 snapshot->operationalGate,
                                 snapshot->nowNs))
    {
        *selected = NvPSSPsdSendPriority::OPERATIONAL;
        return true;
    }

    return false;
}

static inline bool NvPSSDeliveryRetryBudgetIsValid(uint32_t retryBudget)
{
    return retryBudget >= NVPSS_PSS_TO_PSD_RETRY_BUDGET_MIN &&
           retryBudget <= NVPSS_PSS_TO_PSD_RETRY_BUDGET_MAX;
}

static inline bool NvPSSDeliveryResponseTimeoutMsIsValid(uint32_t timeoutMs)
{
    return timeoutMs >= NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_MIN &&
           timeoutMs <= NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_MAX;
}

static inline uint64_t NvPSSDeliveryRetryWindowMs(uint32_t retryBudget,
                                                  uint32_t responseTimeoutMs)
{
    return static_cast<uint64_t>(retryBudget) *
           static_cast<uint64_t>(responseTimeoutMs);
}

static inline bool NvPSSDeliveryRetryWindowMsIsValid(uint32_t retryBudget,
                                                     uint32_t responseTimeoutMs)
{
    return NvPSSDeliveryRetryBudgetIsValid(retryBudget) &&
           NvPSSDeliveryResponseTimeoutMsIsValid(responseTimeoutMs) &&
           NvPSSDeliveryRetryWindowMs(retryBudget, responseTimeoutMs) <=
               NVPSS_PSS_TO_PSD_RETRY_WINDOW_MS_MAX;
}

static inline OperationalMode NvPSSDeliveryOperationalMode(NvPSSDeliveryState state,
                                                           OperationalMode baseMode)
{
    return (state == NvPSSDeliveryState::DELIVERY_ERROR_ACTIVE) ? ERROR : baseMode;
}

static inline NvPSSDeliveryFailureReason NvPSSDeliveryReasonFromNvPSDErr(NvPSDErr err)
{
    return (err == NVPSD_NO_RSP)
        ? NvPSSDeliveryFailureReason::RESPONSE_TIMEOUT
        : NvPSSDeliveryFailureReason::PROCESS_DECISION_REQUEST_FAILED;
}

static inline const char* NvPSSDeliveryFailureReasonName(NvPSSDeliveryFailureReason reason)
{
    switch (reason)
    {
        case NvPSSDeliveryFailureReason::NONE:
            return "none";
        case NvPSSDeliveryFailureReason::PROCESS_DECISION_REQUEST_FAILED:
            return "process_decision_request_failed";
        case NvPSSDeliveryFailureReason::RESPONSE_TIMEOUT:
            return "response_timeout";
        default:
            return "unknown";
    }
}

static inline const char* NvPSSPsdSendPriorityName(NvPSSPsdSendPriority priority)
{
    switch (priority)
    {
        case NvPSSPsdSendPriority::CRITICAL:
            return "critical";
        case NvPSSPsdSendPriority::OPERATIONAL:
            return "operational";
        default:
            return "unknown";
    }
}

static inline bool NvPSSDecisionRequestIsStatusNoopOnly(const DecisionRequest* request)
{
    return request != nullptr &&
           request->sensorDataSummarySize == 1U &&
           request->sensorDataSummary[0].event.type == PSS_STATUS_NOOP;
}

static inline bool NvPSSDecisionRequestIdIsValid(uint32_t requestId)
{
    return requestId >= NVPSS_DECISION_REQUEST_ID_MIN &&
           requestId <= NVPSS_DECISION_REQUEST_ID_MAX;
}

static inline uint32_t NvPSSNextDecisionRequestIdAfter(uint32_t requestId)
{
    return (requestId >= NVPSS_DECISION_REQUEST_ID_MAX)
        ? NVPSS_DECISION_REQUEST_ID_MIN
        : (requestId + 1U);
}

static inline uint32_t NvPSSAllocateSerializedDecisionRequestId(
    std::atomic<uint32_t>* nextRequestId)
{
    if (nextRequestId == nullptr)
        return NVPSS_DECISION_REQUEST_ID_MIN;

    uint32_t observed = nextRequestId->load(std::memory_order_relaxed);
    for (;;)
    {
        const uint32_t allocated = NvPSSDecisionRequestIdIsValid(observed)
            ? observed
            : NVPSS_DECISION_REQUEST_ID_MIN;
        const uint32_t next = NvPSSNextDecisionRequestIdAfter(allocated);
        if (nextRequestId->compare_exchange_weak(observed, next,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed))
        {
            return allocated;
        }
    }
}

static inline bool NvPSSIsCompleteDecisionResponseSize(int receivedBytes)
{
    return receivedBytes == static_cast<int>(sizeof(DecisionResponse));
}

static inline bool NvPSSDecisionResponseMatchesRequest(const DecisionRequest& request,
                                                       const DecisionResponse& response)
{
    return NvPSSDecisionRequestIdIsValid(request.requestId) &&
           response.decisionId == request.requestId;
}

static inline NvPSSDecisionResponseReceiveAction NvPSSDecisionResponseReceiveActionFor(
    const DecisionRequest& request,
    const DecisionResponse& response,
    int receivedBytes)
{
    /* Stale responses may remain after timeout; only a complete response with
     * the active request id is delivery evidence for this DecisionRequest. */
    if (!NvPSSIsCompleteDecisionResponseSize(receivedBytes))
        return NvPSSDecisionResponseReceiveAction::FAIL;

    if (!NvPSSDecisionResponseMatchesRequest(request, response))
        return NvPSSDecisionResponseReceiveAction::DISCARD_AND_CONTINUE;

    return NvPSSDecisionResponseReceiveAction::ACCEPT;
}

static inline void NvPSSAssignSerializedDecisionRequestId(DecisionRequest* request,
                                                          uint32_t requestId)
{
    if (request == nullptr)
        return;

    request->requestId = requestId;
    const uint8_t count = request->sensorDataSummarySize > MAX_SENSORS_DATA_SUMMARY_SIZE
        ? MAX_SENSORS_DATA_SUMMARY_SIZE
        : request->sensorDataSummarySize;
    for (uint8_t i = 0U; i < count; ++i)
    {
        if (request->sensorDataSummary[i].event.type == PSS_STATUS_NOOP)
            request->sensorDataSummary[i].event.id = requestId;
    }
}

#endif /* NVPSS_DELIVERY_FAILSAFE_HPP */
