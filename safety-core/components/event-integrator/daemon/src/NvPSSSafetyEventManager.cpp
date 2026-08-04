/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <vector>
#include <iostream>
#include <cstring>
#include <ctime>

#include "NvPSSSafetyEventManager.hpp"
#include "NvPSSDRPC.hpp"
#include "NvPSSDeliveryFailSafe.hpp"
#include "NvPSSStatusNoop.hpp"
#include "NvPSB.h"
#include "pss_protocol.h"
#include "pss_message_validate.h"

extern std::atomic<uint32_t> g_pssMaxHbFailures;
extern std::atomic<uint32_t> g_pssWarnThreshold;

namespace nvpss
{

static const char* NvPSSChannelBackendName(NvPSDChannelBackend backend)
{
    switch (backend)
    {
        case NvPSDChannelBackend::POSIX_MSG_QUE:
            return "POSIX_MSG_QUE";
        case NvPSDChannelBackend::POSIX_SOCKET:
            return "POSIX_SOCKET";
        default:
            return "UNKNOWN";
    }
}

static bool NvPSSEventTypeRequiresCriticalDelivery(EventType type)
{
    return type == SW_FAIL ||
           type == SENSOR_INVALID ||
           type == SENSOR_VALID ||
           type == AI_PIPELINE_INVALID ||
           type == AI_PIPELINE_VALID;
}

static SeverityLevel NvPSSClassifyPsdSeverity(const FusedSafetyEvent& event,
                                              bool isHealthy,
                                              bool isTrustedSource,
                                              OperationalMode mode)
{
    if (mode == ERROR ||
        event.severity == CRITICAL ||
        NvPSSEventTypeRequiresCriticalDelivery(event.type) ||
        !isHealthy ||
        !isTrustedSource)
    {
        return CRITICAL;
    }

    return OPERATIONAL;
}

static NvPSSPsdSendPriority NvPSSPsdSendPriorityFromSeverity(SeverityLevel severity)
{
    return (severity == CRITICAL)
        ? NvPSSPsdSendPriority::CRITICAL
        : NvPSSPsdSendPriority::OPERATIONAL;
}

static SeverityLevel NvPSSDecisionRequestSeverity(const DecisionRequest* request)
{
    if (request == nullptr)
        return OPERATIONAL;

    const uint8_t count = request->sensorDataSummarySize > MAX_SENSORS_DATA_SUMMARY_SIZE
        ? MAX_SENSORS_DATA_SUMMARY_SIZE
        : request->sensorDataSummarySize;

    for (uint8_t i = 0U; i < count; ++i)
    {
        const SensorData& sensorData = request->sensorDataSummary[i];
        if (NvPSSClassifyPsdSeverity(
            sensorData.event,
            sensorData.isHealthy,
            sensorData.isTrustedSource,
            request->pssStatus.mode) == CRITICAL)
        {
            return CRITICAL;
        }
    }

    return OPERATIONAL;
}

static void NvPSSFinalizeDecisionRequestSeverity(DecisionRequest* request)
{
    if (request == nullptr)
        return;

    const uint8_t count = request->sensorDataSummarySize > MAX_SENSORS_DATA_SUMMARY_SIZE
        ? MAX_SENSORS_DATA_SUMMARY_SIZE
        : request->sensorDataSummarySize;
    const SeverityLevel requestSeverity = NvPSSDecisionRequestSeverity(request);

    for (uint8_t i = 0U; i < count; ++i)
    {
        request->sensorDataSummary[i].event.severity = requestSeverity;
    }
}

SafetyEventManager::SafetyEventManager(uint64_t inputSafetyEventQuePeriod, uint64_t fusionEventPeriod,
                                       NvPSDChannelBackend PSSDToPSDComBackend,
                                       uint32_t pssToPsdRetryBudget,
                                       uint32_t pssToPsdResponseTimeoutMs,
                                       uint32_t statusNoopIntervalMs)
    :criticalPrioQue(MAX_EVENTS_PER_QUE, std::make_pair(-1, FusedSafetyEvent())),
    operationalPrioQue(MAX_EVENTS_PER_QUE, std::make_pair(-1,FusedSafetyEvent())),
    inputSafetyEventQue(MAX_EVENTS_PER_QUE, std::make_pair(-1, SafetyEvent())),
    inputSafetyEventQuePeriod(inputSafetyEventQuePeriod), fusionEventPeriod(fusionEventPeriod), PSSDToPSDComBackend(PSSDToPSDComBackend),
    queMonitorsRunning(false), maxPipelinesSupported(2), registeredPipelines(),
    psdRequestId(1), psdCtx(nullptr),
    pssToPsdRetryBudget_(NvPSSDeliveryRetryWindowMsIsValid(
                             pssToPsdRetryBudget, pssToPsdResponseTimeoutMs)
                             ? pssToPsdRetryBudget
                             : NVPSS_PSS_TO_PSD_RETRY_BUDGET_DEFAULT),
    pssToPsdResponseTimeoutMs_(NvPSSDeliveryRetryWindowMsIsValid(
                                  pssToPsdRetryBudget, pssToPsdResponseTimeoutMs)
                                  ? pssToPsdResponseTimeoutMs
                                  : NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_DEFAULT),
    statusNoopIntervalMs_(NvPSSStatusNoopIntervalMsIsValid(statusNoopIntervalMs)
                              ? statusNoopIntervalMs
                              : NVPSS_STATUS_NOOP_INTERVAL_MS_DEFAULT)
{
    // Clear all the deques before start
    criticalPrioQue.clear();
    operationalPrioQue.clear();
    inputSafetyEventQue.clear();

    NvPSBWriteData(NVPSB_LOG_INFO, "Instance of NvPSSDaemon-Event Manager is created", "");
}

SafetyEventManager::~SafetyEventManager()
{
    StopSafetyEventManager();
}

void SafetyEventManager::SetRpcForOperationalMode(NvPSSDRPC* rpc)
{
    std::lock_guard<std::mutex> lock(rpcOperationalModeMutex_);
    rpcForOperationalMode_ = rpc;
}

OperationalMode SafetyEventManager::decisionRequestOperationalMode() const
{
    const NvPSSDeliveryState deliveryState = psdDeliveryState_.load(std::memory_order_relaxed);
    if (deliveryState == NvPSSDeliveryState::DELIVERY_ERROR_ACTIVE)
        return ERROR;

    std::lock_guard<std::mutex> lock(rpcOperationalModeMutex_);
    NvPSSDRPC* const rpc = rpcForOperationalMode_;
    if (!rpc)
        return NvPSSDeliveryOperationalMode(deliveryState, NORMAL);
    return NvPSSDeliveryOperationalMode(deliveryState,
        rpc->getSafetyMonitorOperationalMode(
        g_pssMaxHbFailures.load(std::memory_order_relaxed),
        g_pssWarnThreshold.load(std::memory_order_relaxed)));
}

SystemStatus SafetyEventManager::makePssStatusForDecisionRequest() const
{
    return {false, false, decisionRequestOperationalMode()};
}

void SafetyEventManager::markDeliveryFailure(const DecisionRequest& request,
                                             const char* requestClass,
                                             NvPSSDeliveryFailureReason reason,
                                             uint32_t attemptCount,
                                             uint32_t responseTimeoutMs)
{
    {
        std::lock_guard<std::mutex> lock(psdDeliveryAuditMtx_);
        lastPsdDeliveryFailureReason_ = reason;
    }
    psdDeliveryState_.store(NvPSSDeliveryState::DELIVERY_ERROR_ACTIVE, std::memory_order_relaxed);

    NvPSBWriteData(NVPSB_LOG_ERR,
        "PSS_TO_PSD_DELIVERY_FAILURE",
        "backend=" + std::string(NvPSSChannelBackendName(PSSDToPSDComBackend)) +
            ", requestId=" + std::to_string(request.requestId) +
            ", class=" + std::string(requestClass ? requestClass : "unknown") +
            ", reason=" + NvPSSDeliveryFailureReasonName(reason) +
            ", attempts=" + std::to_string(attemptCount) +
            ", timeoutMs=" + std::to_string(responseTimeoutMs) +
            ", resultingMode=ERROR"
            ", auditPath=PSB_LOG");
}

void SafetyEventManager::markDeliveryRecovered(const DecisionRequest& request, const char* requestClass)
{
    const NvPSSDeliveryState previous =
        psdDeliveryState_.exchange(NvPSSDeliveryState::NORMAL, std::memory_order_relaxed);
    if (previous == NvPSSDeliveryState::DELIVERY_RETRYING)
        return;
    if (previous != NvPSSDeliveryState::DELIVERY_ERROR_ACTIVE)
    {
        return;
    }

    NvPSSDeliveryFailureReason previousReason = NvPSSDeliveryFailureReason::NONE;
    {
        std::lock_guard<std::mutex> lock(psdDeliveryAuditMtx_);
        previousReason = lastPsdDeliveryFailureReason_;
        lastPsdDeliveryFailureReason_ = NvPSSDeliveryFailureReason::NONE;
    }

    NvPSBWriteData(NVPSB_LOG_INFO,
        "PSS_TO_PSD_DELIVERY_RECOVERED",
        "backend=" + std::string(NvPSSChannelBackendName(PSSDToPSDComBackend)) +
            ", requestId=" + std::to_string(request.requestId) +
            ", class=" + std::string(requestClass ? requestClass : "unknown") +
            ", previousReason=" + NvPSSDeliveryFailureReasonName(previousReason) +
            ", auditPath=PSB_LOG");
}

NvPSSDErr SafetyEventManager::sendDecisionRequestWithRetry(DecisionRequest* request,
                                                           DecisionResponse* response,
                                                           const char* requestClass)
{
    if (request == nullptr || response == nullptr)
        return NVPSSD_FAIL;

    const uint32_t retryBudget = pssToPsdRetryBudget_;
    const uint32_t responseTimeoutMs = pssToPsdResponseTimeoutMs_;
    NvPSSDeliveryFailureReason lastReason = NvPSSDeliveryFailureReason::NONE;
    bool requestIdAssigned = request->requestId != 0U;
    const char* const backendName = NvPSSChannelBackendName(PSSDToPSDComBackend);

    for (uint32_t attempt = 1U; attempt <= retryBudget; ++attempt)
    {
        NvPSSDErr sendErr = NVPSSD_FAIL;
        {
            std::lock_guard<std::mutex> lock(psdSendMtx_);
            if (!requestIdAssigned)
            {
                NvPSSAssignSerializedDecisionRequestId(
                    request, NvPSSAllocateSerializedDecisionRequestId(&psdRequestId));
                requestIdAssigned = true;
            }
            request->pssStatus = makePssStatusForDecisionRequest();
            NvPSSFinalizeDecisionRequestSeverity(request);
            pssDecisionRequestSetCRC(request);
            if (PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
            {
                const NvPSDErr psdErr = (psdCtx != nullptr)
                    ? NvPSDProcessDecisionRequest(psdCtx, request, response)
                    : NVPSD_FAIL;
                sendErr = (psdErr == NVPSD_SUCCESS) ? NVPSSD_SUCCESS : NVPSSD_FAIL;
                lastReason = NvPSSDeliveryReasonFromNvPSDErr(psdErr);
            }
            else if (PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
            {
                sendErr = (pssdServer != nullptr)
                    ? pssdServer->sendDecisionRequestToPSD(*request, response, responseTimeoutMs)
                    : NVPSSD_FAIL;
                lastReason = NvPSSDeliveryFailureReason::PROCESS_DECISION_REQUEST_FAILED;
            }
            else
            {
                lastReason = NvPSSDeliveryFailureReason::PROCESS_DECISION_REQUEST_FAILED;
            }
        }

        if (sendErr == NVPSSD_SUCCESS)
        {
            markDeliveryRecovered(*request, requestClass);
            return NVPSSD_SUCCESS;
        }

        if (attempt < retryBudget)
        {
            psdDeliveryState_.store(NvPSSDeliveryState::DELIVERY_RETRYING, std::memory_order_relaxed);
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "PSS-to-PSD delivery attempt failed",
                "backend=" + std::string(backendName) +
                    ", requestId=" + std::to_string(request->requestId) +
                    ", class=" + std::string(requestClass ? requestClass : "unknown") +
                    ", reason=" + NvPSSDeliveryFailureReasonName(lastReason) +
                    ", attempt=" + std::to_string(attempt) +
                    "/" + std::to_string(retryBudget));
        }
    }

    markDeliveryFailure(*request, requestClass, lastReason, retryBudget, responseTimeoutMs);
    return NVPSSD_FAIL;
}

static uint64_t monotonicNowNs()
{
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 1ULL;
    return (static_cast<uint64_t>(ts.tv_sec) * SEC_TO_NANO_SEC) +
           static_cast<uint64_t>(ts.tv_nsec);
}

bool SafetyEventManager::buildDecisionRequestFromQueuedEvents(NvPSSPsdSendPriority priority,
                                                              DecisionRequest* request)
{
    if (request == nullptr)
        return false;

    std::deque<std::pair<int, FusedSafetyEvent>>* queue = nullptr;
    std::mutex* queueMutex = nullptr;
    FusedSafetyEvent events[MAX_SENSORS_DATA_SUMMARY_SIZE] = {};
    uint8_t eventCount = 0U;

    switch (priority)
    {
        case NvPSSPsdSendPriority::CRITICAL:
            queue = &criticalPrioQue;
            queueMutex = &criticalPrioQueMutex;
            break;
        case NvPSSPsdSendPriority::OPERATIONAL:
            queue = &operationalPrioQue;
            queueMutex = &operationalPrioQueMutex;
            break;
        default:
            return false;
    }

    {
        std::unique_lock<std::mutex> lock(*queueMutex);
        if (queue->empty())
            return false;

        size_t queueEventCount = queue->size();
        if (queueEventCount > MAX_SENSORS_DATA_SUMMARY_SIZE)
            queueEventCount = MAX_SENSORS_DATA_SUMMARY_SIZE;
        eventCount = static_cast<uint8_t>(queueEventCount);
        for (uint8_t i = 0U; i < eventCount; ++i)
        {
            events[i] = std::get<1>(queue->front());
            queue->pop_front();
        }
    }

    *request = {};
    request->pssStatus = makePssStatusForDecisionRequest();
    request->sensorDataSummarySize = eventCount;

    for (uint8_t i = 0U; i < request->sensorDataSummarySize; ++i)
    {
        FusedSafetyEvent ev = events[i];
        request->sensorDataSummary[i].clientID =
            static_cast<uint32_t>(ev.fusionMetadata.clientID);
        if (ev.type == PSS_STATUS_NOOP)
        {
            request->sensorDataSummary[i].clientID = 0U;
            request->sensorDataSummary[i].isHealthy = true;
            request->sensorDataSummary[i].isTrustedSource = true;
        }
        else
        {
            const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                            ev.fusionMetadata.clientID);
            request->sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
            request->sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
        }
        request->sensorDataSummary[i].event = ev;
    }

    NvPSSFinalizeDecisionRequestSeverity(request);

    const char* const priorityName = NvPSSPsdSendPriorityName(priority);
    NvPSBWriteData(NVPSB_LOG_INFO,
                   "EXIT POINT: Sending queued priority events to PSD Gateway",
                   "priority=" + std::string(priorityName) +
                       ", eventCount=" + std::to_string(eventCount));

    return true;
}

bool SafetyEventManager::hasQueuedEvents(NvPSSPsdSendPriority priority)
{
    std::deque<std::pair<int, FusedSafetyEvent>>* queue = nullptr;
    std::mutex* queueMutex = nullptr;

    switch (priority)
    {
        case NvPSSPsdSendPriority::CRITICAL:
            queue = &criticalPrioQue;
            queueMutex = &criticalPrioQueMutex;
            break;
        case NvPSSPsdSendPriority::OPERATIONAL:
            queue = &operationalPrioQue;
            queueMutex = &operationalPrioQueMutex;
            break;
        default:
            return false;
    }

    std::lock_guard<std::mutex> lock(*queueMutex);
    return !queue->empty();
}

bool SafetyEventManager::enqueueStatusNoopEvent(uint64_t timestampNs)
{
    FusedSafetyEvent event = {};
    NvPSSBuildStatusNoopFusedEvent(&event, 0U, timestampNs);

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(operationalPrioQueMutex);
        if (operationalPrioQue.size() < MAX_EVENTS_PER_QUE)
        {
            operationalPrioQue.push_front(std::make_pair(0, event));
            queued = true;
        }
        else if (!operationalPrioQue.empty() &&
                 std::get<1>(operationalPrioQue.back()).type != PSS_STATUS_NOOP)
        {
            operationalPrioQue.pop_back();
            operationalPrioQue.push_front(std::make_pair(0, event));
            queued = true;
        }
    }

    if (!queued)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "status/no-op operational queue insertion skipped; queue full", "");
        return false;
    }

    notifyPsdSenderForQueuedEvent();
    NvPSBWriteData(NVPSB_LOG_INFO,
        "queued status/no-op event in operational queue", "");
    return true;
}

bool SafetyEventManager::enqueuePsdFusedEvent(NvPSSPsdSendPriority priority,
                                              int clientId,
                                              const FusedSafetyEvent& event)
{
    std::deque<std::pair<int, FusedSafetyEvent>>* queue = nullptr;
    std::mutex* queueMutex = nullptr;

    switch (priority)
    {
        case NvPSSPsdSendPriority::CRITICAL:
            queue = &criticalPrioQue;
            queueMutex = &criticalPrioQueMutex;
            break;
        case NvPSSPsdSendPriority::OPERATIONAL:
            queue = &operationalPrioQue;
            queueMutex = &operationalPrioQueMutex;
            break;
        default:
            return false;
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(*queueMutex);
        if (queue->size() < MAX_EVENTS_PER_QUE)
        {
            queue->push_back(std::make_pair(clientId, event));
            queued = true;
        }
    }

    if (queued)
        notifyPsdSenderForQueuedEvent();

    return queued;
}

void SafetyEventManager::notifyPsdSenderForQueuedEvent()
{
    psdSenderWakeSeq_.fetch_add(1U, std::memory_order_release);
    psdSenderWakeCv_.notify_one();
}

std::deque<std::pair<int, FusedSafetyEvent>>& SafetyEventManager::getCriticalPrioQueRef()
{
    return criticalPrioQue;
}

std::deque<std::pair<int, FusedSafetyEvent>>& SafetyEventManager::getOperationalPrioQueRef()
{
    return operationalPrioQue;
}

std::deque<std::pair<int, SafetyEvent>>& SafetyEventManager::getInputSafetyEventQueRef()
{
    return inputSafetyEventQue;
}

std::mutex& SafetyEventManager::getInputSafetyEventQueMutexRef()
{
    return inputSafetyEventQueMutex;
}

NvPSSDErr SafetyEventManager::StartSafetyEventManager()
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
    {
        /*Initialize the communication channel with PSD here*/
        psdCtx = NvPSDCreateContext();
        if (!psdCtx)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to create NvPSD context", "");
            err = NVPSSD_FAIL;
            goto exit;
        }

        if(NvPSDInitialize(psdCtx, "/pss_to_client", "/client_to_pss", "/pss_to_client_critical",
                           "/client_to_pss_critical", NVPSD_PSS) != NVPSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to initialize NvPSD", "");
            NvPSDDestroyContext(psdCtx);
            psdCtx = nullptr;
            err = NVPSSD_FAIL;
            goto exit;
        }

        if (NvPSDSetDecisionResponseTimeoutMs(psdCtx, pssToPsdResponseTimeoutMs_) != NVPSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to configure NvPSD DecisionResponse timeout", "");
            NvPSDExit(psdCtx);
            NvPSDDestroyContext(psdCtx);
            psdCtx = nullptr;
            err = NVPSSD_FAIL;
            goto exit;
        }

        if(NvPSDStart(psdCtx) != NVPSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to start NvPSD", "");
            NvPSDExit(psdCtx);
            NvPSDDestroyContext(psdCtx);
            psdCtx = nullptr;
            err = NVPSSD_FAIL;
            goto exit;
        }
    }
    else if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
    {
        // Initialize and start PSS Daemon Socket
        err = initializePSSDServer();
        if(err != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to initialize PSD server", "");
            goto exit;
        }

        err = startPSSDServer();
        if(err != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to start PSD server", "");
            goto exit;
        }
    }
    else
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Unsupported PSS-to-PSD backend for SafetyEventManager", "");
        err = NVPSSD_FAIL;
        goto exit;
    }

    queMonitorsRunning = true;
    psdSenderMonitor = std::thread(&SafetyEventManager::managePsdSender, this);
    inputSafetyEventQueMonitor = std::thread(&SafetyEventManager::manageInputSafetyEventQue, this);

exit:
    return err;
}

NvPSSDErr SafetyEventManager::StopSafetyEventManager()
{
    queMonitorsRunning = false;
    psdSenderWakeSeq_.fetch_add(1U, std::memory_order_release);
    psdSenderWakeCv_.notify_all();

    if(PSSDToPSDComBackend == POSIX_SOCKET)
    {
        stopPSSDServer();
    }

    if (psdSenderMonitor.joinable()) psdSenderMonitor.join();
    if (inputSafetyEventQueMonitor.joinable()) inputSafetyEventQueMonitor.join();

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::managePsdSender()
{
    DecisionRequest psdDecisionRequest{};
    DecisionResponse psdDecisionResponse{};
    const bool statusNoopEnabled =
        NvPSSStatusNoopEnabledForBackend(PSSDToPSDComBackend);
    const uint64_t statusNoopIntervalNs =
        static_cast<uint64_t>(statusNoopIntervalMs_) * 1000000ULL;
    const uint64_t operationalReleasePeriodNs =
        NvPSSPsdSendPriorityReleasePeriodNs(NvPSSPsdSendPriority::OPERATIONAL);
    uint64_t nextStatusNoopNs = NvPSSPsdSaturatingAddNs(monotonicNowNs(), statusNoopIntervalNs);
    NvPSSPsdReleaseGate operationalGate{};
    uint32_t observedWakeSeq = psdSenderWakeSeq_.load(std::memory_order_acquire);

    while (queMonitorsRunning)
    {
        const uint64_t nowNs = monotonicNowNs();
        if (statusNoopEnabled && nowNs >= nextStatusNoopNs)
        {
            const bool noopQueued = enqueueStatusNoopEvent(nowNs);
            NvPSSPsdForceReleaseGateDue(&operationalGate, nowNs);
            NvPSSPsdUpdateStatusNoopDeadline(
                &nextStatusNoopNs, noopQueued, nowNs, statusNoopIntervalNs);
        }

        const bool criticalQueued = hasQueuedEvents(NvPSSPsdSendPriority::CRITICAL);
        const bool operationalQueued = hasQueuedEvents(NvPSSPsdSendPriority::OPERATIONAL);

        NvPSSPsdUpdateReleaseGate(
            &operationalGate, operationalQueued, nowNs, operationalReleasePeriodNs);

        NvPSSPsdReleaseSnapshot releaseSnapshot{};
        releaseSnapshot.criticalQueued = criticalQueued;
        releaseSnapshot.operationalQueued = operationalQueued;
        releaseSnapshot.operationalGate = operationalGate;
        releaseSnapshot.nowNs = nowNs;

        NvPSSPsdSendPriority duePriority = NvPSSPsdSendPriority::OPERATIONAL;
        if (NvPSSPsdSelectDuePriority(&releaseSnapshot, &duePriority))
        {
            const char* requestClass = nullptr;
            if (!buildDecisionRequestFromQueuedEvents(duePriority, &psdDecisionRequest))
                continue;

            requestClass = NvPSSPsdSendPriorityName(duePriority);

            psdDecisionResponse = {};
            if(sendDecisionRequestWithRetry(&psdDecisionRequest, &psdDecisionResponse, requestClass)
                != NVPSSD_SUCCESS)
            {
                if (NvPSSDecisionRequestIsStatusNoopOnly(&psdDecisionRequest))
                {
                    NvPSBWriteData(NVPSB_LOG_ERR,
                        "failed to send status/no-op DecisionRequest", "");
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_ERR,
                        "Failed to report queued priority events to PSD",
                        "priority=" + std::string(requestClass ? requestClass : "unknown"));
                }
            }
            else
            {
                NvPSBWriteData(NVPSB_LOG_INFO,
                    "Reported queued priority events to PSD",
                    "backend=" + std::string(NvPSSChannelBackendName(PSSDToPSDComBackend)) +
                        ", priority=" + std::string(requestClass ? requestClass : "unknown"));
            }

            const uint64_t postSendNs = monotonicNowNs();
            switch (duePriority)
            {
                case NvPSSPsdSendPriority::OPERATIONAL:
                    NvPSSPsdAdvanceReleaseGateAfterSend(
                        &operationalGate,
                        hasQueuedEvents(NvPSSPsdSendPriority::OPERATIONAL),
                        postSendNs,
                        operationalReleasePeriodNs);
                    break;
                default:
                    break;
            }
            continue;
        }

        uint64_t nextWakeNs = UINT64_MAX;
        if (statusNoopEnabled && nextStatusNoopNs < nextWakeNs)
            nextWakeNs = nextStatusNoopNs;
        if (operationalQueued && operationalGate.armed &&
            operationalGate.deadlineNs < nextWakeNs)
        {
            nextWakeNs = operationalGate.deadlineNs;
        }

        std::unique_lock<std::mutex> waitLock(psdSenderWakeMtx_);
        if (nextWakeNs != UINT64_MAX)
        {
            const uint64_t waitStartNs = monotonicNowNs();
            if (waitStartNs >= nextWakeNs)
                continue;
            const uint64_t waitNs = nextWakeNs - waitStartNs;
            psdSenderWakeCv_.wait_for(waitLock, std::chrono::nanoseconds(waitNs),
                [this, observedWakeSeq] {
                    return !queMonitorsRunning.load(std::memory_order_acquire) ||
                        psdSenderWakeSeq_.load(std::memory_order_acquire) != observedWakeSeq;
                });
        }
        else
        {
            psdSenderWakeCv_.wait(waitLock,
                [this, observedWakeSeq] {
                    return !queMonitorsRunning.load(std::memory_order_acquire) ||
                        psdSenderWakeSeq_.load(std::memory_order_acquire) != observedWakeSeq;
                });
        }
        observedWakeSeq = psdSenderWakeSeq_.load(std::memory_order_acquire);
    }

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::manageInputSafetyEventQue()
{
#ifdef NVPSF_DBG
    struct timespec ts = {};
    uint64_t timestamp_ns;
#endif
    /*Busy wait for input que*/
    while (queMonitorsRunning)
    {

        std::this_thread::sleep_for(std::chrono::microseconds(inputSafetyEventQuePeriod));
        //Lock before checking and accessing queue
        std::unique_lock<std::mutex> lock(inputSafetyEventQueMutex);

        if(inputSafetyEventQue.empty())
        {
            lock.unlock();
            continue;
        }
        else
        {
            if(fusionEnabled)
            {
#ifdef NVPSF_DBG
                clock_gettime(CLOCK_MONOTONIC, &ts);
                timestamp_ns = (ts.tv_nsec + ts.tv_sec*SEC_TO_NANO_SEC);
                std::cout << "Input event timestamp in ns: " << timestamp_ns << std::endl;
#endif
                SafetyEvent eventToProcess = std::get<1>(inputSafetyEventQue.front());
                inputSafetyEventQue.pop_front();

                // Release lock before expensive fusion processing
                lock.unlock();

                /* Events from invalid sensors (pipelineID) or invalid AI pipelines (clientID) are not fused; send to PSD as UNKNOWN critical evidence. */
                const auto trust = QueryTrustState(eventToProcess.fusionMetadata.pipelineID,
                                                   eventToProcess.fusionMetadata.clientID);
                if (trust.sensorInvalid || trust.aiPipelineInvalid)
                {
                    FusedSafetyEvent invalidEvent = CreateInvalidSourceEvent(eventToProcess);
                    invalidEvent.severity = NvPSSClassifyPsdSeverity(
                        invalidEvent,
                        !trust.sensorInvalid,
                        !trust.aiPipelineInvalid,
                        decisionRequestOperationalMode());
                    /* Use semantic AI pipeline id for PSD attribution, not RPC slot. */
                    const int semanticClientId = static_cast<int>(invalidEvent.fusionMetadata.clientID);
                    (void)enqueuePsdFusedEvent(
                        NvPSSPsdSendPriorityFromSeverity(invalidEvent.severity),
                        semanticClientId,
                        invalidEvent);
                }
                else if(ProcessSafetyEventForFusion(eventToProcess) != NVPSSD_SUCCESS)
                {
                    std::cerr<<"Failed to process reported SafetyEvent\n" << std::endl;
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_ERR,"Failed to process reported SafetyEvent","");
#endif
                    /*TODO : This is a serious failure. Devise a strategy to handle this kind of failure*/
                }
                else
                {
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO, "processed reported SafetyEvent","");
#endif
                }
            } else
            {
                lock.unlock();
                // No-op. Operational/critical output queues are filled by input safety events.
            }
        }
    }
    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::EnableFusion(bool enable)
{
    fusionEnabled = enable;

    return NVPSSD_SUCCESS;
}

bool SafetyEventManager::IsFusionEnabled() const
{
    return fusionEnabled;
}

NvPSSDErr SafetyEventManager::ConfigureMultiCameraFusion(uint8_t maxPipelines,
                                                        std::chrono::milliseconds timeWindowSize,
                                                        float fusionThreshold,
                                                        float temporalW,
                                                        float spatialW,
                                                        float attributeW,
                                                        std::chrono::milliseconds temporalT,
                                                        uint8_t trajectoryCount,
                                                        float earlyTermThreshold,
                                                        bool enableEarlyTerm)
{
    NvPSSDErr result = NVPSSD_SUCCESS;
    float weightSum = temporalW + spatialW + attributeW;

    if (!fusionEnabled) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Fusion has not been enabled", "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }

    // Validate maximum pipelines
    if (maxPipelines == 0 || maxPipelines > MAX_SUPPORTED_PIPELINES) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid maxPipelines value: " + std::to_string(maxPipelines), "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }
    maxPipelinesSupported = maxPipelines;
    {
        const auto ms = timeWindowSize.count();
        if (ms < 0) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "Invalid negative timeWindowSize: " + std::to_string(ms) + " ms", "");
            result = NVPSSD_FAIL;
            goto done;
        }
        stalenessThresholdMs_ = static_cast<uint64_t>(ms);
    }

    // Validate fusion parameters
    if (fusionThreshold < 0.0f || fusionThreshold > 1.0f) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid fusionThreshold: " + std::to_string(fusionThreshold), "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }


    if (std::abs(weightSum - 1.0f) > 0.01f) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Weight sum should be close to 1.0. Current sum: " + std::to_string(weightSum), "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }

    if (!eventFusion)
    {
        eventFusion = std::make_unique<SafetyEventFusion>(
            std::chrono::milliseconds(timeWindowSize).count(),
            fusionThreshold,
            temporalW,
            spatialW,
            attributeW,
            std::chrono::milliseconds(temporalT).count(),
            trajectoryCount,
            maxPipelinesSupported,
            earlyTermThreshold,
            enableEarlyTerm);

        NvPSBWriteData(NVPSB_LOG_INFO, "Multi-camera fusion configured with " + std::to_string(maxPipelines) + " pipelines", "");
    }
    else
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "Fusion module already configured. Updating parameters for multi-camera support.", "");
#endif

        eventFusion->SetMaxPipelines(maxPipelines);
        eventFusion->SetTimeWindowSize(std::chrono::milliseconds(timeWindowSize).count());
        eventFusion->SetFusionThreshold(fusionThreshold);
        eventFusion->SetTemporalTolerance(std::chrono::milliseconds(temporalT).count());
        eventFusion->SetFusionWeights(temporalW, spatialW, attributeW);
        eventFusion->SetTrajectoryCount(trajectoryCount);
        eventFusion->EnableEarlyTermination(enableEarlyTerm);
        eventFusion->SetEarlyTerminationThreshold(earlyTermThreshold);
    }

done:
    return result;
}

NvPSSDErr SafetyEventManager::ProcessSafetyEventForFusion(const SafetyEvent& event)
{
    NvPSSDErr result = NVPSSD_SUCCESS;
    uint8_t pipelineId = 0;

    if (!fusionEnabled || !eventFusion)
    {
        result = NVPSSD_FAIL;
        goto done;
    }

    pipelineId = event.fusionMetadata.pipelineID;
    if (pipelineId > maxPipelinesSupported)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Event from unregistered sensor: " + std::to_string(pipelineId), "");
        result = NVPSSD_FAIL;
        goto done;
    }

    if (sensorConfigLoaded_ && pipelineId != 0)
    {
        auto cfgIt = sensorConfigIdToName_.find(pipelineId);
        if (cfgIt == sensorConfigIdToName_.end())
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: pipelineID " + std::to_string(pipelineId) + " not in sensor_config", "");
            result = NVPSSD_FAIL;
            goto done;
        }
        /* Treat inbound buffer as untrusted: bound the length to the fixed-size
         * field so we never read past event.sensorIdentifier even if the sender
         * omits the NUL terminator. */
        const size_t sensorIdLen = strnlen(event.sensorIdentifier, MAX_INDENTIFIER_LENGTH);
        std::string sensorId(event.sensorIdentifier, sensorIdLen);
        if (!sensorId.empty() && sensorId != cfgIt->second)
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: sensorIdentifier '" + sensorId +
                "' does not match sensor_config entry '" + cfgIt->second +
                "' for pipelineID " + std::to_string(pipelineId), "");
            result = NVPSSD_FAIL;
            goto done;
        }
    }

    /* pipelineId == 0 is the well-known "unknown sensor" passthrough channel
     * (MDX emits it for events that cannot be attributed to a configured pipeline).
     * SafetyEventFusion::RegisterPipeline rejects 0, and
     * SafetyEventFusion::ProcessNewSafetyEvent has explicit passthrough handling
     * for 0 — so skip auto-registration here and let the downstream path run. */
    if (pipelineId != 0 &&
        registeredPipelines.find(pipelineId) == registeredPipelines.end())
    {
        NvPSSDErr regRes = eventFusion->RegisterPipeline(pipelineId);
        if (regRes != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "Failed to auto-register pipeline: " + std::to_string(pipelineId), "");
            result = NVPSSD_FAIL;
            goto done;
        }
        registeredPipelines[pipelineId] = true;
        NvPSBWriteData(NVPSB_LOG_INFO,
            "Auto-registered pipeline: " + std::to_string(pipelineId), "");
    }

    if (bypassFusionEvents.count(event.type))
    {
        // Directly create a FusedSafetyEvent of status PASSTHROUGH and route to decision
        FusedSafetyEvent fusedEvent = CreateBypassEvent(event);
        fusedEvent.severity = NvPSSClassifyPsdSeverity(
            fusedEvent,
            true,
            true,
            decisionRequestOperationalMode());

        (void)enqueuePsdFusedEvent(
            NvPSSPsdSendPriorityFromSeverity(fusedEvent.severity),
            fusedEvent.fusionMetadata.clientID,
            fusedEvent);

        return NVPSSD_SUCCESS;
    }

    result = eventFusion->ProcessNewSafetyEvent(event);

done:
    return result;
}

NvPSSDErr SafetyEventManager::StartFusionProcessing()
{
    fusionProcessorRunning = true;
    fusionProcessorThread = std::thread(&SafetyEventManager::fusionProcessingLoop, this);

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::StopFusionProcessing()
{
    fusionProcessorRunning = false;

    if (fusionProcessorThread.joinable())
    {
        fusionProcessorThread.join();
    }

    return NVPSSD_SUCCESS;
}

std::vector<FusedSafetyEvent> SafetyEventManager::GetFusedEvents() const
{
    if (!fusionEnabled || !eventFusion)
    {
        return {};
    }

    return eventFusion->GetFusedEvents();
}

NvPSSDErr SafetyEventManager::HandleFusedEvents()
{
#ifdef NVPSF_DBG
    struct timespec ts = {};
    uint64_t timestamp_ns;
#endif
    if (!fusionEnabled)
    {
        while (true)
        {
            SafetyEvent event = {};
            {
                std::lock_guard<std::mutex> lock(inputSafetyEventQueMutex);
                if (inputSafetyEventQue.empty())
                    break;
                event = std::get<1>(inputSafetyEventQue.front());
                inputSafetyEventQue.pop_front();
            }

            /* Apply trust enforcement when fusion is disabled: events from invalid sources get UNKNOWN status. */
            const auto trustBypass = QueryTrustState(event.fusionMetadata.pipelineID,
                                                     event.fusionMetadata.clientID);
            FusedSafetyEvent fusedEvent = (trustBypass.sensorInvalid || trustBypass.aiPipelineInvalid)
                ? CreateInvalidSourceEvent(event)
                : CreateBypassEvent(event);
            fusedEvent.severity = NvPSSClassifyPsdSeverity(
                fusedEvent,
                !trustBypass.sensorInvalid,
                !trustBypass.aiPipelineInvalid,
                decisionRequestOperationalMode());

            /* Use semantic client ID (fusionMetadata.clientID) for queue key, consistent with invalid-source and fusion-enabled paths. */
            const int semanticClientId = static_cast<int>(fusedEvent.fusionMetadata.clientID);

            (void)enqueuePsdFusedEvent(
                NvPSSPsdSendPriorityFromSeverity(fusedEvent.severity),
                semanticClientId,
                fusedEvent);
        }


    } else
    {
        // Get fused events
        auto fusedEvents = eventFusion->GetFusedEvents();

        // Process each fused event and add to appropriate priority queue
        for (auto fusedEvent : fusedEvents)
        {
#ifdef NVPSF_DBG
            std::cout << "Number of Event Fused/Pssthrough: " << fusedEvents.size() << std::endl;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            timestamp_ns = (ts.tv_nsec + ts.tv_sec*SEC_TO_NANO_SEC);
            std::cout << "Event Severity queue timestamp in ns: " << timestamp_ns << std::endl;
#endif

            const auto trust = QueryTrustState(fusedEvent.fusionMetadata.pipelineID,
                                               fusedEvent.fusionMetadata.clientID);
            fusedEvent.severity = NvPSSClassifyPsdSeverity(
                fusedEvent,
                !trust.sensorInvalid,
                !trust.aiPipelineInvalid,
                decisionRequestOperationalMode());
            (void)enqueuePsdFusedEvent(
                NvPSSPsdSendPriorityFromSeverity(fusedEvent.severity),
                fusedEvent.fusionMetadata.clientID,
                fusedEvent);
        }

        eventFusion->ClearFusedEvents(fusedEvents.size());
    }

    return NVPSSD_SUCCESS;
}

void SafetyEventManager::fusionProcessingLoop()
{
    while(fusionProcessorRunning)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(fusionEventPeriod));

        if(eventFusion->PerformSafetyEventFusion() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to perform safety event fusion","");
#endif
        }

        if(eventFusion->ProcessUnmatchedEvents() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to process unmatched safety events","");
#endif
        }

        if(eventFusion->CleanProcessedEvents() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to clean processed safety events","");
#endif
        }

        if(HandleFusedEvents() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to process fused safety events","");
#endif
        }
    }
}

bool SafetyEventManager::isEventStale(uint64_t eventTimestampNs,
                                      uint64_t nowMs) const
{
    const uint64_t eventMs = eventTimestampNs / 1000000ULL;

    /* Reject timestamps that are unreasonably far in the future.
     * A corrupted or malicious value could sit in the queue
     * indefinitely; treat it as stale so it gets discarded. */
    if (eventMs > nowMs && (eventMs - nowMs > stalenessThresholdMs_))
        return true;

    return (nowMs > eventMs) &&
           (nowMs - eventMs > stalenessThresholdMs_);
}

bool SafetyEventManager::isEventStale(uint64_t eventTimestampNs) const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t nowMs = static_cast<uint64_t>(ts.tv_sec) * 1000ULL
                         + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    return isEventStale(eventTimestampNs, nowMs);
}

FusedSafetyEvent SafetyEventManager::CreateBypassEvent(const SafetyEvent& event) const
{
    //Pass through SafetyEvent to Fused SafetyEvent
    FusedSafetyEvent fusedEvent;

    // Generate a unique ID
    fusedEvent.id = static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count());

    // Copy properties directly from the source event
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    strncpy(fusedEvent.ruleIdentifier, event.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    fusedEvent.type = event.type;
    fusedEvent.timestamp = event.timestamp;
    fusedEvent.confidenceLevel = event.confidenceLevel;
    fusedEvent.severity = OPERATIONAL;
    std::memcpy(&fusedEvent.fusionMetadata, &event.fusionMetadata, sizeof(EventFusionMetadata));

    fusedEvent.status = isEventStale(event.timestamp) ? STALE : UNKNOWN;

    return fusedEvent;
}

void SafetyEventManager::SetBypassFusionEvents(const std::unordered_set<EventType>& set)
{
    bypassFusionEvents = set;
}

void SafetyEventManager::SetSensorConfig(const std::unordered_map<uint8_t, std::string>& pipelineIdToName)
{
    sensorConfigIdToName_ = pipelineIdToName;
    sensorConfigLoaded_ = !sensorConfigIdToName_.empty();
}

bool SafetyEventManager::OnTrustReport(uint32_t rpcClientId, uint8_t reporterClientType, const SafetyEvent& event)
{
    /* Defense-in-depth per-event-type authorization. The RPC layer enforces
     * the same rule when accepting REPORT_SAFETY_EVENT (NvPSSDRPC.cpp); rejecting
     * here too so malformed in-process call sequences cannot bypass it. */
    const bool isSensorTrustReport     = (event.type == SENSOR_INVALID      || event.type == SENSOR_VALID);
    const bool isAIPipelineTrustReport = (event.type == AI_PIPELINE_INVALID || event.type == AI_PIPELINE_VALID);
    if (!isSensorTrustReport && !isAIPipelineTrustReport)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "Trust report rejected: event type is not a trust-report type",
                       "eventType: " + std::to_string(event.type));
        return false;
    }
    const bool sensorReporterOk     = (reporterClientType == CLIENT_SAFETY_MONITOR);
    const bool aiPipelineReporterOk = (reporterClientType == CLIENT_PERCEPTION_MONITOR);
    if ((isSensorTrustReport && !sensorReporterOk) ||
        (isAIPipelineTrustReport && !aiPipelineReporterOk))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "Trust report rejected: client type not authorized for this event type",
                       "reporterType: " + std::to_string(reporterClientType) +
                       ", eventType: " + std::to_string(event.type));
        return false;
    }

    (void)rpcClientId;

    /* AI-pipeline trust is pipeline-wide: PCM emits one verdict for the whole AI
     * pipeline, not per MDX client (fusionMetadata.clientID is not used here), so it
     * is tracked as a single global latch. Every MDX client -- already connected,
     * late-connecting, or reconnecting into a reused slot -- consistently reads the
     * current state via QueryTrustState, with no per-client bookkeeping to go stale. */
    std::lock_guard<std::mutex> lock(trustStateMutex);
    switch (event.type)
    {
        case SENSOR_INVALID:
            invalidSensors.insert(event.fusionMetadata.pipelineID);  /* pipelineID = sensor */
            break;
        case SENSOR_VALID:
            invalidSensors.erase(event.fusionMetadata.pipelineID);
            break;
        case AI_PIPELINE_INVALID:
            aiPipelineInvalidGlobal = true;
            break;
        case AI_PIPELINE_VALID:
            aiPipelineInvalidGlobal = false;
            break;
        default:
            break;
    }
    return true;
}

SafetyEventManager::TrustState SafetyEventManager::QueryTrustState(
    uint8_t pipelineId, uint8_t clientId) const
{
    /* AI-pipeline trust is a single pipeline-wide latch, so clientId no longer selects
     * per-client state; it is retained in the signature for callers and for the
     * sensor-trust lookup keyed by pipelineId. */
    (void)clientId;
    std::lock_guard<std::mutex> lock(trustStateMutex);
    return { invalidSensors.count(pipelineId) != 0,
             aiPipelineInvalidGlobal };
}

FusedSafetyEvent SafetyEventManager::CreateInvalidSourceEvent(const SafetyEvent& event) const
{
    FusedSafetyEvent fusedEvent;
    fusedEvent.id = static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count());
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    strncpy(fusedEvent.ruleIdentifier, event.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    fusedEvent.type = event.type;
    fusedEvent.timestamp = event.timestamp;
    fusedEvent.confidenceLevel = event.confidenceLevel;
    fusedEvent.severity = CRITICAL;
    fusedEvent.status = isEventStale(event.timestamp) ? STALE : UNKNOWN;
    std::memcpy(&fusedEvent.fusionMetadata, &event.fusionMetadata, sizeof(EventFusionMetadata));
    return fusedEvent;
}
NvPSSDErr SafetyEventManager::initializePSSDServer()
{
    pssdServer = std::make_unique<NvPSSDToPSDClient>();
    return pssdServer->initializePSSDServer();
}

NvPSSDErr SafetyEventManager::startPSSDServer()
{
    if(pssdServer)
    {
        return pssdServer->startPSSDServer();
    }
    return NVPSSD_FAIL;
}

NvPSSDErr SafetyEventManager::stopPSSDServer()
{
    if(pssdServer)
    {
        return pssdServer->stopPSSDServer();
    }
    return NVPSSD_SUCCESS;
}

}
