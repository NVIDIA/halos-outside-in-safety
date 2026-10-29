/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <vector>
#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>
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

/*
 * Ids for daemon-synthesised events.
 *
 * The SDMs deduplicate strictly on event.id: ATLControl.cpp keeps a per-slot
 * lastEventId and skips an event whose id equals it. A reused id is therefore
 * not a cosmetic collision -- it silently drops the event, and the events
 * synthesised here are group trust transitions and invalid-source reports, both
 * of which move a latch.
 *
 * This replaces three copies of
 *     static_cast<uint32_t>(system_clock::now().time_since_epoch().count())
 * which had no uniqueness guarantee at all. system_clock is nanosecond-period,
 * so that value wraps every 4.295 s; two consecutive events in one slot that
 * happen to be an exact multiple of 2^32 ns apart collide. Measured at roughly
 * 2.3e-10 per pair -- unlikely, unbounded, and undetectable when it happens.
 *
 * A counter makes it impossible for 2^32 events instead of merely improbable.
 *
 * SEEDED FROM THE CLOCK, not from 1. The SDM's lastEventId is a process-lifetime
 * static that is never reset, so it OUTLIVES a daemon restart. A counter
 * restarting at 1 would hand the restarted daemon an id the SDM may still be
 * holding from the previous instance, turning a 1-in-4-billion coincidence into
 * a systematic first-event drop. Seeding from the clock keeps successive daemon
 * instances far apart while the increment guarantees uniqueness within one.
 *
 * Skips 0xFFFFFFFF: that is the SDMs' EVENT_ID_UNSEEN sentinel, and an event
 * carrying it is discarded as "already seen" on a slot's very first event. 0 is
 * skipped too -- it is not reserved today, but it is the value a zeroed struct
 * carries, and an id that cannot arise by accident is worth the one comparison.
 */
static uint32_t nextDaemonEventId()
{
    static std::atomic<uint32_t> counter(
        static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count()));
    uint32_t id;
    do {
        id = counter.fetch_add(1U, std::memory_order_relaxed);
    } while (id == 0U || id == 0xFFFFFFFFU);
    return id;
}

static bool NvPSSEventTypeRequiresCriticalDelivery(EventType type)
{
    return NvPSSPsdIsMustDeliverReport(type);
}

/* Whether a critical-lane entry may be evicted to admit a must-deliver report:
 * not a report itself, and either from a source distrusted when it was observed
 * or STALE -- an event every SDM skips. */
static bool NvPSSPsdEvictable(const FusedSafetyEvent& event, bool sourceDistrusted)
{
    return !NvPSSPsdIsMustDeliverReport(event.type) && (sourceDistrusted || event.status == STALE);
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
                                       NvPSFTransportKind PSSDToPSDComBackend,
                                       uint32_t pssToPsdRetryBudget,
                                       uint32_t pssToPsdResponseTimeoutMs,
                                       uint32_t statusNoopIntervalMs,
                                       uint32_t psdOperationalQueueDepth,
                                       uint32_t psdCriticalQueueDepth,
                                       uint32_t fusionFailureErrorMs)
    :inputSafetyEventQue(MAX_EVENTS_PER_QUE, std::make_pair(-1, SafetyEvent())),
    /* Clamped rather than trusted: the daemon validates the configured value,
     * but this constructor is also reachable from tests and from the defaulted
     * argument, and a depth below one wire batch cannot stage a full
     * DecisionRequest. */
    psdOperationalQueueDepth_(
        (psdOperationalQueueDepth < NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MIN)
            ? NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MIN
            : ((psdOperationalQueueDepth > NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MAX)
                   ? NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MAX
                   : psdOperationalQueueDepth)),
    psdCriticalQueueDepth_(
        (psdCriticalQueueDepth < NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MIN)
            ? NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MIN
            : ((psdCriticalQueueDepth > NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MAX)
                   ? NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MAX
                   : psdCriticalQueueDepth)),
    inputSafetyEventQuePeriod(inputSafetyEventQuePeriod), fusionEventPeriod(fusionEventPeriod), PSSDToPSDComBackend(PSSDToPSDComBackend),
    queMonitorsRunning(false), maxPipelinesSupported(2), registeredPipelines(),
    psdRequestId(1),
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
                              : NVPSS_STATUS_NOOP_INTERVAL_MS_DEFAULT),
    /* Same reasoning as the queue depths above: the daemon validates it, but
     * this constructor is also reachable from tests and from the defaulted
     * argument. */
    fusionFailureErrorMs_(NvPSSFusionFailureErrorMsIsValid(fusionFailureErrorMs)
                              ? fusionFailureErrorMs
                              : NVPSS_FUSION_FAILURE_ERROR_MS_DEFAULT)
{
    /* inputSafetyEventQue is constructed prefilled with placeholder entries, so
     * it has to be emptied before use. The staging queues are not prefilled and
     * start empty. */
    inputSafetyEventQue.clear();

    NvPSBWriteData(NVPSB_LOG_INFO, "Instance of NvPSSDaemon-Event Manager is created",
        "psdOperationalQueueDepth=" + std::to_string(psdOperationalQueueDepth_) +
            ", psdCriticalQueueDepth=" + std::to_string(psdCriticalQueueDepth_) +
            ", wireBatch=" + std::to_string(static_cast<unsigned>(MAX_SENSORS_DATA_SUMMARY_SIZE)) +
            ", batchMaxDeliveryAttempts=" +
            std::to_string(static_cast<unsigned>(NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS)) +
            ", fusionFailureErrorMs=" + std::to_string(fusionFailureErrorMs_) +
            /* Both thresholds, because the same duration is a different number
             * of iterations on each loop and an operator reading the log should
             * not have to do that division. */
            ", fusionFailStreakInputLoop=" +
            std::to_string(fusionStageStreakThreshold(FUSION_STAGE_PROCESS_EVENT)) +
            ", fusionFailStreakFusionLoop=" +
            std::to_string(fusionStageStreakThreshold(FUSION_STAGE_PERFORM_FUSION)));
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

const char* SafetyEventManager::fusionStageName(FusionStage stage)
{
    switch (stage)
    {
        case FUSION_STAGE_PROCESS_EVENT:     return "process_safety_event_for_fusion";
        case FUSION_STAGE_PERFORM_FUSION:    return "perform_safety_event_fusion";
        case FUSION_STAGE_PROCESS_UNMATCHED: return "process_unmatched_events";
        case FUSION_STAGE_CLEAN_PROCESSED:   return "clean_processed_events";
        case FUSION_STAGE_HANDLE_FUSED:      return "handle_fused_events";
        default:                             return "unknown";
    }
}

uint32_t SafetyEventManager::fusionStageStreakThreshold(FusionStage stage) const
{
    /* Which loop this stage runs on decides the conversion. Only the first
     * stage is driven by manageInputSafetyEventQue(); the rest are driven by
     * fusionProcessingLoop(), which iterates twenty times more slowly. */
    const std::chrono::microseconds& period =
        (stage == FUSION_STAGE_PROCESS_EVENT) ? inputSafetyEventQuePeriod : fusionEventPeriod;
    return NvPSSFusionFailureStreakThreshold(
        fusionFailureErrorMs_, static_cast<uint64_t>(period.count()));
}

bool SafetyEventManager::bumpFusionStageFailure(FusionStage stage)
{
    if (stage >= FUSION_STAGE_COUNT)
        return false;

    const uint32_t threshold = fusionStageStreakThreshold(stage);
    uint32_t streak = 0U;
    /* Rising edge only. Once the fault is set the streak keeps climbing, but the
     * fault is already declared and repeating the audit record every iteration
     * would bury the log on a safety path. */
    if (!fusionHealth_.noteFailure(stage, threshold, &streak))
        return false;

    NvPSBWriteData(NVPSB_LOG_ERR,
        "PSS_FUSION_HEALTH_ERROR",
        "stage=" + std::string(fusionStageName(stage)) +
            ", consecutiveFailures=" + std::to_string(streak) +
            ", threshold=" + std::to_string(threshold) +
            ", fusionFailureErrorMs=" + std::to_string(fusionFailureErrorMs_) +
            ", resultingMode=ERROR"
            ", auditPath=PSB_LOG");
    return true;
}

/* Monotonic milliseconds for the fusion-stage audit record only. */
static uint64_t fusionAuditNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool SafetyEventManager::fusionStageFailureRecordDue(FusionStage stage,
                                                     NvPSSFusionStageRecord* outKind)
{
    if (stage >= FUSION_STAGE_COUNT || outKind == nullptr)
        return false;
    *outKind = NvPSSFusionStageNoteFailure(&fusionStageEpisode_[stage], fusionAuditNowMs(),
                                           NVPSS_FUSION_STAGE_RECORD_INTERVAL_MS);
    return *outKind != NvPSSFusionStageRecord::NONE;
}

void SafetyEventManager::writeFusionStageFailureRecord(FusionStage stage,
                                                       NvPSSFusionStageRecord kind,
                                                       const std::string& detail)
{
    const NvPSSFusionStageEpisode& episode = fusionStageEpisode_[stage];
    std::string body = "stage=" + std::string(fusionStageName(stage));
    if (kind == NvPSSFusionStageRecord::STARTED)
    {
        body += ", episode=started";
    }
    else
    {
        body += ", episode=ongoing"
                ", failuresInEpisode=" + std::to_string(episode.failures) +
                ", consecutiveFailures=" +
                    std::to_string(fusionHealth_.streak(stage)) +
                ", episodeMs=" + std::to_string(episode.lastFailureMs - episode.openedMs);
    }
    NvPSBWriteData(NVPSB_LOG_ERR, "PSS_FUSION_STAGE_FAILURE", body + detail);
}

void SafetyEventManager::clearFusionStageFailure(FusionStage stage)
{
    if (stage >= FUSION_STAGE_COUNT)
        return;

    /* The audit episode can still be open after the streak is back to 0: it
     * closes only after a full quiet interval. The clock is read only while one
     * is open, so the healthy path stays a bool test plus the load below. */
    NvPSSFusionStageEpisode& episode = fusionStageEpisode_[stage];
    if (episode.open &&
        NvPSSFusionStageNoteSuccess(&episode, fusionAuditNowMs(),
                                    NVPSS_FUSION_STAGE_RECORD_INTERVAL_MS) ==
            NvPSSFusionStageRecord::ENDED)
    {
        NvPSBWriteData(NVPSB_LOG_INFO,
            "PSS_FUSION_STAGE_RECOVERED",
            "stage=" + std::string(fusionStageName(stage)) +
                ", failuresInEpisode=" + std::to_string(episode.failures) +
                ", episodeMs=" + std::to_string(episode.lastFailureMs - episode.openedMs) +
                ", quietMs=" + std::to_string(NVPSS_FUSION_STAGE_RECORD_INTERVAL_MS));
    }

    /* The fault clears only when EVERY stage is healthy, not just this one, and
     * the check and the clear are one step against a concurrent raise
     * (NvPSSFusionHealthLatch). A healthy stage costs one load here, and
     * successes are the normal case on this loop. */
    if (!fusionHealth_.noteSuccess(stage))
        return;

    NvPSBWriteData(NVPSB_LOG_INFO,
        "PSS_FUSION_HEALTH_RECOVERED",
        "recoveredStage=" + std::string(fusionStageName(stage)) +
            ", auditPath=PSB_LOG");
}

void SafetyEventManager::sourcePresenceSnapshot(uint32_t* presentMask, uint32_t* expectedMask) const
{
    /* Only what is true right now: a PSS that has just restarted, or a monitor
     * that is registered but has not yet heartbeated, reports it absent. */
    std::lock_guard<std::mutex> lock(rpcOperationalModeMutex_);
    NvPSSDRPC* const rpc = rpcForOperationalMode_;
    *presentMask  = (rpc != nullptr)
        ? NvPSSPackPresenceWord(rpc->sourcePresenceMask(), rpc->sourceInputUnusableMask()) : 0U;
    *expectedMask = (rpc != nullptr)
        ? NvPSSPackPresenceWord(rpc->sourceExpectedMask(), rpc->sourceInputConfirmedMask()) : 0U;
}

OperationalMode SafetyEventManager::decisionRequestOperationalMode() const
{
    const NvPSSDeliveryState deliveryState = psdDeliveryState_.load(std::memory_order_relaxed);
    if (deliveryState == NvPSSDeliveryState::DELIVERY_ERROR_ACTIVE)
        return ERROR;

    /* Third ERROR source, alongside PSD delivery state above and the SAIM
     * heartbeat below: safety-input processing has been failing without
     * interruption for fusion_failure_error_ms. Reusing mode=ERROR rather than
     * inventing a path means the Gateway's existing CAUSE_PSS_ERROR fanout
     * carries it to every SDM with no new wire value and no new SDM logic. */
    if (fusionHealth_.active())
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

SystemStatus SafetyEventManager::makePssStatusForDecisionRequest(uint64_t* overflowGeneration) const
{
    /* A refused must-deliver report makes the request ERROR, so every SDM
     * latches on the loss. Only the STAMP: routing keeps using
     * decisionRequestOperationalMode(), or the overflow would route every event
     * onto the lane it is waiting to drain. The generation is read ONCE and
     * decides the stamp, so the two cannot disagree. */
    const uint64_t generation = criticalLaneOverflow_.stampGeneration();
    if (overflowGeneration != nullptr)
        *overflowGeneration = generation;
    if (generation != 0U)
        return {false, false, ERROR};
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
        "backend=" + std::string(NvPSFTransportKindName(PSSDToPSDComBackend)) +
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
    /* The delivery failure is over for the SDMs only once they have been TOLD:
     * an acknowledged request whose id first went out stamped ERROR. A retry of
     * an id first sent NORMAL may have been answered as a duplicate without
     * being forwarded, so it proves nothing; ERROR stays active and the next new
     * request carries it. Sender thread only, like every writer of this state. */
    if (request.pssStatus.mode != ERROR &&
        psdDeliveryState_.load(std::memory_order_relaxed) == NvPSSDeliveryState::DELIVERY_ERROR_ACTIVE)
        return;

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
        "backend=" + std::string(NvPSFTransportKindName(PSSDToPSDComBackend)) +
            ", requestId=" + std::to_string(request.requestId) +
            ", class=" + std::string(requestClass ? requestClass : "unknown") +
            ", previousReason=" + NvPSSDeliveryFailureReasonName(previousReason) +
            ", auditPath=PSB_LOG");
}

NvPSSDErr SafetyEventManager::sendDecisionRequestWithRetry(DecisionRequest* request,
                                                           DecisionResponse* response,
                                                           const char* requestClass,
                                                           PsdBatchTicket* ticket)
{
    if (request == nullptr || response == nullptr)
        return NVPSSD_FAIL;

    const uint32_t retryBudget = pssToPsdRetryBudget_;
    const uint32_t responseTimeoutMs = pssToPsdResponseTimeoutMs_;
    NvPSSDeliveryFailureReason lastReason = NvPSSDeliveryFailureReason::NONE;
    bool requestIdAssigned = request->requestId != 0U;
    const char* const backendName = NvPSFTransportKindName(PSSDToPSDComBackend);

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
            /* The stamp this request id FIRST went out with, on every attempt:
             * PSD may have forwarded an earlier copy and will answer this one as
             * a duplicate, so only that stamp can be credited (see
             * PsdBatchTicket::stamp). The overflow generation it carries is
             * credited if PSD acknowledges. */
            if (ticket != nullptr && ticket->stamped)
            {
                request->pssStatus = ticket->stamp;
                lastSendOverflowGeneration_ = ticket->stampGeneration;
            }
            else
            {
                request->pssStatus = makePssStatusForDecisionRequest(&lastSendOverflowGeneration_);
                if (ticket != nullptr)
                {
                    ticket->stamped = true;
                    ticket->stamp = request->pssStatus;
                    ticket->stampGeneration = lastSendOverflowGeneration_;
                }
            }
            NvPSSFinalizeDecisionRequestSeverity(request);
            pssDecisionRequestSetCRC(request);
            /* The else below rejects anything that is not UNIX_SOCKET, so an
             * unexpected kind fails loudly. */
            if (PSSDToPSDComBackend == NVPSF_TRANSPORT_UNIX_SOCKET)
            {
                /* The reason comes back from the sender rather than being
                 * assumed here: only it can tell a response timeout apart from
                 * a refusal to send, and both return NVPSSD_FAIL. */
                lastReason = NvPSSDeliveryFailureReason::PROCESS_DECISION_REQUEST_FAILED;
                sendErr = (pssdServer != nullptr)
                    ? pssdServer->sendDecisionRequestToPSD(*request, response,
                                                           responseTimeoutMs, &lastReason)
                    : NVPSSD_FAIL;
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
                                                              DecisionRequest* request,
                                                              PsdBatchTicket* ticket)
{
    if (request == nullptr || ticket == nullptr)
        return false;

    *ticket = PsdBatchTicket{};

    std::deque<PsdQueuedEvent>* queue = nullptr;
    std::mutex* queueMutex = nullptr;
    FusedSafetyEvent events[MAX_SENSORS_DATA_SUMMARY_SIZE] = {};
    TrustState queuedTrust[MAX_SENSORS_DATA_SUMMARY_SIZE] = {};
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

    /*
     * COPY, DO NOT POP. The queue is not modified here; commitQueuedEvents()
     * removes these entries only after the send is acknowledged.
     *
     * This used to pop_front() the batch before handing it to
     * sendDecisionRequestWithRetry(), and the failure path in managePsdSender()
     * only logs. So a send that exhausted its retry budget destroyed the batch:
     * the events were already out of the queue, nothing held another copy, and
     * the log line named a count rather than the events themselves. Peeking
     * instead means a failed send costs a retry, not evidence.
     *
     * Head-of-line blocking on a persistently failing link is the deliberate
     * trade, but only for a BOUNDED number of attempts. A link that cannot
     * deliver should back up and be visible as a full queue plus counted
     * enqueue refusals, which the delivery fail-safe state machine already acts
     * on, rather than quietly draining events into a dead socket.
     *
     * Unbounded retention would be a different thing entirely, and worse than
     * what it replaced. A batch that fails deterministically is never removed,
     * so its lane never empties; on the critical lane that also stops the
     * OPERATIONAL lane, because NvPSSPsdSelectDuePriority() cannot get past a
     * non-empty critical queue. The result is every operational event shed at
     * enqueue -- the failure this depth work exists to remove -- alongside a
     * critical queue too full to accept the new fault reports arriving behind
     * the stuck one. So the sender counts attempts per batch and
     * dropUndeliverableBatch() abandons it at
     * NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS, and criticalRetrying lets
     * operational interleave in the meantime.
     */
#ifdef NVPSF_DBG
    size_t dbgOccupancy = 0U;
    uint64_t dbgMinResidencyNs = UINT64_MAX;
    uint64_t dbgMaxResidencyNs = 0U;
#endif

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
            events[i] = (*queue)[i].event;
            queuedTrust[i] = (*queue)[i].queuedTrust;
            ticket->seq[i] = (*queue)[i].seq;
        }
        ticket->count = eventCount;
#ifdef NVPSF_DBG
        /* Occupancy BEFORE this batch leaves, which is the number the release
         * gate is actually working against and the one that pairs with the
         * high-water mark. Residency is gathered under the lock but reported
         * outside it -- logging here would hold the queue mutex across a write
         * that the enqueue path also contends for. */
        dbgOccupancy = queue->size();
        const uint64_t dbgNow = monotonicNowNs();
        for (uint8_t i = 0U; i < eventCount; ++i)
        {
            const uint64_t enq = (*queue)[i].enqueueNs;
            if (enq == 0U || dbgNow < enq)
                continue;
            const uint64_t res = dbgNow - enq;
            if (res < dbgMinResidencyNs) dbgMinResidencyNs = res;
            if (res > dbgMaxResidencyNs) dbgMaxResidencyNs = res;
        }
#endif
    }

#ifdef NVPSF_DBG
    if (dbgMinResidencyNs != UINT64_MAX)
    {
        NvPSBWriteData(
            NVPSB_LOG_INFO, "PSD_STAGING_RESIDENCY",
            "occupancy=" + std::to_string(dbgOccupancy) + "/" +
                std::to_string(psdOperationalQueueDepth_) +
                ", batch=" + std::to_string(static_cast<unsigned>(eventCount)) +
                ", minUs=" + std::to_string(dbgMinResidencyNs / 1000ULL) +
                ", maxUs=" + std::to_string(dbgMaxResidencyNs / 1000ULL));
    }
#endif

    *request = {};
    request->pssStatus = makePssStatusForDecisionRequest();
    request->sensorDataSummarySize = eventCount;

    /*
     * Presence is sampled HERE, at build, and not when the no-op was queued. A
     * no-op can wait in the operational lane -- or be retained and rebuilt for a
     * retry -- while a monitor is lost; stamped at enqueue it would then say
     * "present" AFTER the *_LOST SW_FAIL had gone out on the critical lane, and
     * clear what that had just raised. Every loss path updates the set before it
     * enqueues the SW_FAIL, and this one thread builds and sends in order, so a
     * report built after the loss says absent and one built before it is
     * delivered before it.
     */
    uint32_t presenceMask = 0U;
    uint32_t expectedMask = 0U;
    sourcePresenceSnapshot(&presenceMask, &expectedMask);

    for (uint8_t i = 0U; i < request->sensorDataSummarySize; ++i)
    {
        FusedSafetyEvent ev = events[i];
        request->sensorDataSummary[i].clientID =
            static_cast<uint32_t>(ev.fusionMetadata.clientID);
        if (ev.type == PSS_STATUS_NOOP)
        {
            NvPSSStampSourcePresence(&ev, presenceMask, expectedMask);
            request->sensorDataSummary[i].clientID = 0U;
            request->sensorDataSummary[i].isHealthy = true;
            request->sensorDataSummary[i].isTrustedSource = true;
        }
        else
        {
            /* Trusted only if trusted now AND when it was queued: a recovery
             * since then does not vouch for an event observed before it. */
            const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                            ev.fusionMetadata.clientID);
            request->sensorDataSummary[i].isHealthy =
                !ts.sensorInvalid && !queuedTrust[i].sensorInvalid;
            request->sensorDataSummary[i].isTrustedSource =
                !ts.aiPipelineInvalid && !queuedTrust[i].aiPipelineInvalid;
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

void SafetyEventManager::commitQueuedEvents(NvPSSPsdSendPriority priority,
                                            const PsdBatchTicket& ticket)
{
    if (ticket.count == 0U)
        return;

    std::deque<PsdQueuedEvent>* queue = nullptr;
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
            return;
    }

    {
        std::lock_guard<std::mutex> lock(*queueMutex);
        /* Remove by sequence number, not by position: producers ran while the lock
         * was dropped for the send, and a timestamp-ordered insert can have placed
         * a new entry ahead of this batch. The scan is bounded by the queue depth
         * and runs once per DecisionRequest; the erased entries are at or near the
         * front in every ordinary case. */
        for (uint8_t i = 0U; i < ticket.count; ++i)
        {
            const uint64_t wanted = ticket.seq[i];
            for (auto it = queue->begin(); it != queue->end(); ++it)
            {
                if (it->seq == wanted)
                {
                    queue->erase(it);
                    break;
                }
            }
        }
    }

    /* After every commit, on either lane: an operational request (a no-op) can
     * carry the overflow ERROR stamp just as well, and the critical lane may
     * already be drained when it is acknowledged. */
    maybeClearCriticalLaneOverflow();
}

void SafetyEventManager::maybeClearCriticalLaneOverflow()
{
    if (!criticalLaneOverflow_.active())
        return;
    size_t depth = 0U;
    size_t readmitted = 0U;
    size_t stillPending = 0U;
    bool refusedOutstanding = false;
    {
        std::lock_guard<std::mutex> lock(criticalPrioQueMutex);
        /* Re-admit refused reports, oldest first, while the lane has room. They
         * are must-deliver reports, so never evictable. */
        const size_t capacity = static_cast<size_t>(psdCriticalQueueDepth_);
        while (!psdRefusedPending_.empty() && criticalPrioQue.size() < capacity)
        {
            PsdQueuedEvent entry = psdRefusedPending_.front();
            psdRefusedPending_.pop_front();
            entry.seq = psdEnqueueSeq_.fetch_add(1U, std::memory_order_relaxed);
            entry.evictable = false;
            criticalPrioQue.push_back(entry);
            psdRefusedReadmittedSeqMax_ = entry.seq;
            ++readmitted;
        }
        stillPending = psdRefusedPending_.size();
        /* Outstanding until every re-admitted report has left the lane, i.e. been
         * delivered: commit removes an entry only on acknowledgement. */
        bool readmittedInLane = false;
        if (psdRefusedReadmittedSeqMax_ != 0U)
        {
            for (const PsdQueuedEvent& e : criticalPrioQue)
            {
                if (e.seq <= psdRefusedReadmittedSeqMax_)
                {
                    readmittedInLane = true;
                    break;
                }
            }
        }
        refusedOutstanding = stillPending != 0U || readmittedInLane;
        depth = criticalPrioQue.size();
    }
    if (readmitted != 0U)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING, "PSS_CRITICAL_LANE_READMITTED",
                       "readmitted=" + std::to_string(readmitted) +
                           ", stillPending=" + std::to_string(stillPending) +
                           ", reason=refused must-deliver reports re-delivered as the lane drains"
                           ", auditPath=PSB_LOG");
        notifyPsdSenderForQueuedEvent();
    }
    if (criticalLaneOverflow_.clearIfDrained(depth, static_cast<size_t>(psdCriticalQueueDepth_),
                                             refusedOutstanding))
    {
        NvPSBWriteData(NVPSB_LOG_INFO,
            "PSS_CRITICAL_LANE_RECOVERED",
            "depth=" + std::to_string(depth) + "/" + std::to_string(psdCriticalQueueDepth_) +
                ", errorStampDelivered=yes, refusedReportsDelivered=yes, auditPath=PSB_LOG");
    }
}

bool SafetyEventManager::psdBatchIdentityMatches(const PsdBatchTicket& a,
                                                 const PsdBatchTicket& b)
{
    if (a.count == 0U || a.count != b.count)
        return false;

    /* Sequence numbers are unique and never reused, so equal seq sets mean the
     * same staged entries. Order is part of the identity: the batch is copied
     * from the front of the queue in queue order, and a rebuild that produced
     * the same entries in a different order would be a different request. */
    for (uint8_t i = 0U; i < a.count; ++i)
    {
        if (a.seq[i] != b.seq[i])
            return false;
    }

    return true;
}

void SafetyEventManager::dropUndeliverableBatch(NvPSSPsdSendPriority priority,
                                                const PsdBatchTicket& ticket)
{
    if (ticket.count == 0U)
        return;

    psdUndeliverableDroppedCount_.fetch_add(ticket.count, std::memory_order_relaxed);

    /*
     * Not rate-limited, unlike the enqueue-drop warning. Reaching the attempt
     * bound is not bursty: it costs several full retry windows per occurrence,
     * so it cannot flood, and each line stands for a batch of safety events
     * that no decision will ever see.
     */
    /* Which events: still in the lane, matched by sequence as the commit below
     * matches them. */
    FusedSafetyEvent abandoned[MAX_SENSORS_DATA_SUMMARY_SIZE] = {};
    size_t abandonedCount = 0U;
    {
        const bool operational = (priority == NvPSSPsdSendPriority::OPERATIONAL);
        std::lock_guard<std::mutex> lock(operational ? operationalPrioQueMutex : criticalPrioQueMutex);
        const std::deque<PsdQueuedEvent>& queue = operational ? operationalPrioQue : criticalPrioQue;
        for (uint8_t i = 0U; i < ticket.count && abandonedCount < MAX_SENSORS_DATA_SUMMARY_SIZE; ++i)
        {
            for (const PsdQueuedEvent& entry : queue)
            {
                if (entry.seq == ticket.seq[i])
                {
                    abandoned[abandonedCount++] = entry.event;
                    break;
                }
            }
        }
    }

    NvPSBWriteData(NVPSB_LOG_ERR,
        "PSD_STAGING_BATCH_UNDELIVERABLE",
        "priority=" + std::string(NvPSSPsdSendPriorityName(priority)) +
            ", eventCount=" + std::to_string(static_cast<unsigned>(ticket.count)) +
            ", requestId=" + std::to_string(ticket.requestId) +
            ", attempts=" + std::to_string(ticket.attempts) +
            "/" + std::to_string(static_cast<unsigned>(NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS)) +
            ", undeliverableTotal=" +
            std::to_string(psdUndeliverableDroppedCount_.load(std::memory_order_relaxed)) +
            ", events(id:type:pipelineID:clientID)=" + NvPSSPsdDescribeEvents(abandoned, abandonedCount) +
            ", auditPath=PSB_LOG");
    psdUndeliverableLedger_.noteAbandoned(ticket.requestId, ticket.count);

    /* Same removal-by-sequence as the acknowledged path: producers ran while
     * the send was in progress, so position is not a safe handle. */
    commitQueuedEvents(priority, ticket);
}

void SafetyEventManager::onPsdRequestAcknowledged(NvPSSPsdSendPriority priority,
                                                  const PsdBatchTicket& ticket,
                                                  uint32_t requestId)
{
    /* If this request id FIRST went out stamped ERROR for an overflow, the SDMs
     * have now been told about the refusal. Only the first transmission's stamp
     * counts: PSD may have answered this copy as a duplicate of an earlier one
     * (see PsdBatchTicket::stamp). */
    criticalLaneOverflow_.noteStampDelivered(ticket.stamped ? ticket.stampGeneration : 0U);
    (void)reportDeliveryResumed(requestId);
    commitQueuedEvents(priority, ticket);
}

std::string SafetyEventManager::reportDeliveryResumed(uint32_t deliveredRequestId)
{
    if (!psdUndeliverableLedger_.pending())
        return std::string();
    const std::string detail = psdUndeliverableLedger_.takeResumed(deliveredRequestId);
    /* Once per recovery from abandonment: the join key between this log and
     * every SDM's, which logs the id of each request it processes. */
    NvPSBWriteData(NVPSB_LOG_WARNING, "PSD_STAGING_DELIVERY_RESUMED",
                   detail + ", joinKey=requestId (every SDM logs the requestIds it processes)"
                            ", auditPath=PSB_LOG");
    return detail;
}

bool SafetyEventManager::hasQueuedEvents(NvPSSPsdSendPriority priority)
{
    std::deque<PsdQueuedEvent>* queue = nullptr;
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
    /* The presence set is re-sampled when the request is built; see there. */
    NvPSSBuildStatusNoopFusedEvent(&event, 0U, timestampNs, 0U, 0U);

    /*
     * The no-op is an ordinary queue entry and gets no privileges.
     *
     * It used to push_front(), and on a full queue it would pop_back() a real
     * event to make room for itself. Both were wrong. The pop_back() made the
     * liveness probe a third, uncounted way to lose a genuine safety
     * observation; the push_front() let it overtake every event already
     * staged. Now it is inserted by its own timestamp like anything else --
     * that timestamp is monotonicNowNs() at enqueue, so it naturally lands at
     * the back -- and a full queue simply refuses it.
     *
     * Refusing it is harmless: managePsdSender() re-arms on the returned false
     * via NvPSSPsdUpdateStatusNoopDeadline(), retrying after
     * NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_MS. A queue full of real
     * events is itself proof of liveness, which is all the probe asserts.
     */
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(operationalPrioQueMutex);
        if (operationalPrioQue.size() < psdOperationalQueueDepth_)
        {
            insertOperationalOrdered(
                PsdQueuedEvent{0, psdEnqueueSeq_.fetch_add(1U, std::memory_order_relaxed), event});
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

void SafetyEventManager::insertOperationalOrdered(const PsdQueuedEvent& entry)
{
    auto pos = operationalPrioQue.end();
    while (pos != operationalPrioQue.begin())
    {
        const PsdQueuedEvent& prev = *std::prev(pos);
        const bool prevIsEarlier = (prev.event.timestamp < entry.event.timestamp) ||
                                   (prev.event.timestamp == entry.event.timestamp &&
                                    prev.seq < entry.seq);
        if (prevIsEarlier)
            break;
        --pos;
    }
    operationalPrioQue.insert(pos, entry);
}

bool SafetyEventManager::enqueuePsdFusedEvent(NvPSSPsdSendPriority priority,
                                              int clientId,
                                              const FusedSafetyEvent& event,
                                              bool evictable,
                                              TrustState queuedTrust)
{
    const bool operational = (priority == NvPSSPsdSendPriority::OPERATIONAL);
    if (!operational && priority != NvPSSPsdSendPriority::CRITICAL)
        return false;

    const size_t capacity = operational ? static_cast<size_t>(psdOperationalQueueDepth_)
                                        : static_cast<size_t>(psdCriticalQueueDepth_);
    std::mutex& queueMutex = operational ? operationalPrioQueMutex : criticalPrioQueMutex;

    bool queued = false;
    bool evicted = false;
    bool overflowRaised = false;
    bool refusedRetained = false;
    bool refusedLost = false;
    size_t refusedPending = 0U;
    FusedSafetyEvent evictedEvent{};
    size_t depth = 0U;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        std::deque<PsdQueuedEvent>& queue = operational ? operationalPrioQue : criticalPrioQue;
        PsdQueuedEvent entry{
            clientId, 0U, event};
        entry.queuedTrust = queuedTrust;
        entry.evictable = (!operational && evictable);
#ifdef NVPSF_DBG
        entry.enqueueNs = monotonicNowNs();
#endif
        if (queue.size() < capacity)
        {
            entry.seq = psdEnqueueSeq_.fetch_add(1U, std::memory_order_relaxed);
            /* The critical lane keeps plain FIFO append: it is not
             * timestamp-ordered because it is not staged behind a release gate,
             * so there is nothing for a sort to reorder against. */
            if (operational)
                insertOperationalOrdered(entry);
            else
                queue.push_back(entry);
            queued = true;
            /* A verdict admitted now supersedes a refused older one for its slot,
             * which must not be delivered after it. */
            if (!operational)
                (void)NvPSSPsdDropSupersededRefused(psdRefusedPending_, event);
        }
        else if (!operational && NvPSSPsdIsMustDeliverReport(event.type))
        {
            /* A full critical lane must not refuse the one report that explains
             * the degradation in favour of an event every SDM will skip. An
             * evictable entry is skipped whatever trust does before it is sent
             * (see PsdQueuedEvent::queuedTrust), so it is not re-asked here --
             * which would refuse the very report that restores trust. */
            const long victim = NvPSSPsdCriticalLaneVictim(
                queue, static_cast<size_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));
            if (victim >= 0)
            {
                evictedEvent = queue[static_cast<size_t>(victim)].event;
                queue.erase(queue.begin() + victim);
                entry.seq = psdEnqueueSeq_.fetch_add(1U, std::memory_order_relaxed);
                queue.push_back(entry);
                queued = true;
                evicted = true;
                (void)NvPSSPsdDropSupersededRefused(psdRefusedPending_, event);
            }
            else
            {
                /* Refused, so every request is stamped ERROR -- and the report is
                 * kept, to be re-admitted as the lane drains: the overflow holds
                 * until it has been delivered (maybeClearCriticalLaneOverflow). */
                overflowRaised = criticalLaneOverflow_.raise();
                refusedRetained = NvPSSPsdRetainRefused(psdRefusedPending_, entry,
                                                        NVPSS_PSD_REFUSED_PENDING_MAX);
                refusedLost = !refusedRetained;
                refusedPending = psdRefusedPending_.size();
            }
        }
        depth = queue.size();
    }

    if (evicted)
        recordCriticalLaneEviction(evictedEvent, event);

    if (!queued)
    {
        recordPsdEnqueueDrop(priority, event, depth, capacity);
        if (overflowRaised)
        {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "PSS_CRITICAL_LANE_OVERFLOW",
                "refusedEventType=" + std::to_string(static_cast<unsigned>(event.type)) +
                    ", pipelineID=" + std::to_string(static_cast<unsigned>(event.fusionMetadata.pipelineID)) +
                    ", depth=" + std::to_string(depth) + "/" + std::to_string(capacity) +
                    ", reason=no evictable entry"
                    ", action=PSS_ERROR, refused report retained for re-delivery"
                    ", clears=when the lane has drained to half, every refused report has been delivered"
                    " and an ERROR-stamped request has been delivered"
                    ", auditPath=PSB_LOG");
        }
        if (refusedLost)
        {
            const uint64_t lost = psdRefusedLostCount_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            NvPSBWriteData(NVPSB_LOG_ERR,
                "PSS_CRITICAL_LANE_REFUSED_LOST",
                "refusedEventType=" + std::to_string(static_cast<unsigned>(event.type)) +
                    ", pipelineID=" + std::to_string(static_cast<unsigned>(event.fusionMetadata.pipelineID)) +
                    ", pending=" + std::to_string(refusedPending) + "/" +
                    std::to_string(NVPSS_PSD_REFUSED_PENDING_MAX) +
                    ", lostTotal=" + std::to_string(lost) +
                    ", effect=PSS stays ERROR, so every SDM holds, until the pending reports are delivered"
                    ", auditPath=PSB_LOG");
        }
        return false;
    }

    psdEnqueueAcceptedCount_.fetch_add(1U, std::memory_order_relaxed);
    {
        /* High-water mark of this lane. This is the observed frame fan-out, and
         * therefore the evidence for whether the configured depth suits the
         * deployment's scene -- object count per frame is what drives it, and
         * that is a property of the site, not of this daemon.
         *
         * Tracked on the critical lane too, now that its depth is configurable:
         * its fan-out only shows up while a source is untrusted or the mode is
         * ERROR, so it is precisely the mark an operator has no other way to
         * observe. */
        std::atomic<uint64_t>& highWater =
            operational ? psdOperationalHighWater_ : psdCriticalHighWater_;
        uint64_t observed = highWater.load(std::memory_order_relaxed);
        while (depth > observed &&
               !highWater.compare_exchange_weak(observed, depth,
                                                std::memory_order_relaxed))
        {
        }

        /*
         * Report each NEW high-water mark, not just the one that overflowed.
         *
         * The depth keys are meant to be sized against observed
         * fan-out, but until this existed the mark was only ever printed inside
         * the PSD_STAGING_QUEUE_OVERFLOW warning -- so the number an operator
         * needs in order NOT to drop events was only available after events had
         * already been dropped, and a healthy deployment reported nothing at
         * all. A validation run at depth 32 that shed nothing could not show
         * how much of the 32 it had actually used, which is exactly the margin
         * question.
         *
         * Bounded by construction, so it cannot flood the log on a safety path:
         * it fires only on a strict increase, and the depth it reports is
         * capped by the configured capacity, giving at most `capacity` lines
         * for the lifetime of the process.
         */
        if (depth > observed)
        {
            NvPSBWriteData(
                NVPSB_LOG_INFO,
                "PSD_STAGING_QUEUE_HIGH_WATER",
                "priority=" + std::string(NvPSSPsdSendPriorityName(priority)) +
                    ", highWater=" + std::to_string(depth) +
                    "/" + std::to_string(capacity) +
                    ", acceptedTotal=" +
                    std::to_string(psdEnqueueAcceptedCount_.load(std::memory_order_relaxed)));
        }
    }

    notifyPsdSenderForQueuedEvent();
    return true;
}

void SafetyEventManager::recordCriticalLaneEviction(const FusedSafetyEvent& evicted,
                                                    const FusedSafetyEvent& admitted)
{
    /* Counted exactly, recorded at the overflow warning's bounded rate: a burst
     * of distrusted events can evict at frame rate. */
    const uint64_t total = psdCriticalEvictedCount_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const uint64_t nowNs = monotonicNowNs();
    uint64_t lastNs = psdCriticalEvictWarnNs_.load(std::memory_order_relaxed);
    if ((nowNs - lastNs) < NVPSS_PSD_ENQUEUE_DROP_WARN_INTERVAL_NS ||
        !psdCriticalEvictWarnNs_.compare_exchange_strong(lastNs, nowNs, std::memory_order_relaxed))
    {
        return;
    }
    NvPSBWriteData(NVPSB_LOG_WARNING,
        "PSD_CRITICAL_LANE_EVICTION",
        "evictedEventType=" + std::to_string(static_cast<unsigned>(evicted.type)) +
            ", evictedPipelineID=" + std::to_string(static_cast<unsigned>(evicted.fusionMetadata.pipelineID)) +
            ", admittedEventType=" + std::to_string(static_cast<unsigned>(admitted.type)) +
            ", evictedTotal=" + std::to_string(total) +
            ", reason=a must-deliver report displaced an event from a distrusted source");
}

void SafetyEventManager::recordPsdEnqueueDrop(NvPSSPsdSendPriority priority,
                                              const FusedSafetyEvent& event,
                                              size_t depth,
                                              size_t capacity)
{
    /*
     * THE staging-queue drop point. Reaching here means a fused event that
     * survived the whole PSS pipeline never became evidence for any decision,
     * and the caller cannot recover it: every call site casts the return value
     * away, and HandleFusedEvents() clears the fusion output whether or not the
     * enqueue succeeded, so this copy was the last one.
     *
     * Counted exactly and warned about at a bounded rate, because before this
     * the loss was invisible from outside the process -- it had to be inferred
     * by comparing event counts against emitted commands, which cannot
     * distinguish a drop from ordinary fusion coalescing.
     */
    const uint64_t dropped = psdEnqueueDroppedCount_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const uint64_t nowNs = monotonicNowNs();
    uint64_t lastWarnNs = psdEnqueueDropWarnNs_.load(std::memory_order_relaxed);

    if ((nowNs - lastWarnNs) < NVPSS_PSD_ENQUEUE_DROP_WARN_INTERVAL_NS ||
        !psdEnqueueDropWarnNs_.compare_exchange_strong(lastWarnNs, nowNs,
                                                       std::memory_order_relaxed))
    {
        return;
    }

    NvPSBWriteData(NVPSB_LOG_WARNING,
        "PSD_STAGING_QUEUE_OVERFLOW",
        "priority=" + std::string(NvPSSPsdSendPriorityName(priority)) +
            ", depth=" + std::to_string(depth) + "/" + std::to_string(capacity) +
            ", eventType=" + std::to_string(static_cast<unsigned>(event.type)) +
            ", pipelineID=" + std::to_string(static_cast<unsigned>(event.fusionMetadata.pipelineID)) +
            ", droppedTotal=" + std::to_string(dropped) +
            ", acceptedTotal=" +
            std::to_string(psdEnqueueAcceptedCount_.load(std::memory_order_relaxed)) +
            ", operationalHighWater=" +
            std::to_string(psdOperationalHighWater_.load(std::memory_order_relaxed)));
}

void SafetyEventManager::notifyPsdSenderForQueuedEvent()
{
    psdSenderWakeSeq_.fetch_add(1U, std::memory_order_release);
    psdSenderWakeCv_.notify_one();
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

    /*
     * The else below rejects anything that is not UNIX_SOCKET, so an
     * unexpected transport kind fails loudly rather than starting a
     * daemon with no PSD server behind it.
     */
    if(PSSDToPSDComBackend == NVPSF_TRANSPORT_UNIX_SOCKET)
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

    /*
     * ORDER IS LOAD-BEARING. psdSenderMonitor runs sendDecisionRequestToPSD,
     * which calls NvPSFTransportSend on the transport that stopPSSDServer()
     * closes -- and NvPSFTransportClose frees the handle. Closing first left a
     * window where the sender had already resolved its peer and armed its
     * mailbox, then dereferenced a freed object and locked a mutex inside it.
     *
     * A bare close(fd) here would give the sender a benign EBADF. It is
     * now a use-after-free on the safety daemon's shutdown path, so the sender
     * must be joined first. Waking the parked senders inside stopPSSDServer()
     * does NOT cover this: it releases senders already waiting, not one in the
     * window between arming and Send.
     */
    if (psdSenderMonitor.joinable()) psdSenderMonitor.join();

    if(PSSDToPSDComBackend == NVPSF_TRANSPORT_UNIX_SOCKET)
    {
        stopPSSDServer();
    }

    if (inputSafetyEventQueMonitor.joinable()) inputSafetyEventQueMonitor.join();

    return NVPSSD_SUCCESS;
}

/*
 * SINGLE REQUEST IN FLIGHT ON PSS -> PSD. READ BEFORE ADDING CONCURRENCY HERE.
 *
 * This loop is the only producer on the PSS -> PSD link, and every send goes out
 * under psdSendMtx_ held across the whole send-and-await-response cycle. That
 * single-in-flight property is a REQUIREMENT, not an accident of the
 * current shape, and two design decisions rest on it:
 *
 *   - Severity lanes are removed. One socket carries CRITICAL and OPERATIONAL
 *     both. Valid only because a CRITICAL request can queue behind at most ONE
 *     outstanding request.
 *   - minQueueDepth on the gateway -> SDM link is FINITE. The formula divides
 *     by T_emit_min, which is bounded by one DecisionRequest per completed
 *     PSS -> PSD cycle. Concurrency here makes the emission rate unbounded and
 *     the depth with it.
 *
 * Adding a second concurrent sender -- or releasing psdSendMtx_ before the
 * response arrives -- invalidates BOTH, and there is no alternative
 * configuration to fall back on: the only remedy is a code change on a safety
 * path.
 *
 * MEASURED, not assumed: under a
 * saturated OPERATIONAL rate the worst CRITICAL round trip was 559.7 ms against
 * a 2000 ms budget -- 28% used. Of that, the transport itself contributes
 * p50 0.076-0.090 ms; essentially all the rest is psdSendMtx_ queueing. A
 * second socket for CRITICAL would therefore buy nothing unless it also
 * bypassed this lock, which is precisely what the invariant forbids.
 *
 * NvPSSDToPSD enforces the invariant rather than trusting it: arming a mailbox
 * that is already armed is refused with "REQUEST ALREADY IN FLIGHT" instead of
 * being
 * overwritten.
 */
NvPSSDErr SafetyEventManager::managePsdSender()
{
    DecisionRequest psdDecisionRequest{};
    DecisionResponse psdDecisionResponse{};
    PsdBatchTicket psdBatchTicket{};
    const bool statusNoopEnabled =
        NvPSSStatusNoopEnabledForBackend(PSSDToPSDComBackend);
    const uint64_t statusNoopIntervalNs =
        static_cast<uint64_t>(statusNoopIntervalMs_) * 1000000ULL;
    const uint64_t operationalReleasePeriodNs =
        NvPSSPsdSendPriorityReleasePeriodNs(NvPSSPsdSendPriority::OPERATIONAL);
    uint64_t nextStatusNoopNs = NvPSSPsdSaturatingAddNs(monotonicNowNs(), statusNoopIntervalNs);
    NvPSSPsdReleaseGate operationalGate{};
    /* Earliest time the next send may be attempted after a failure. Zero means
     * no backoff is in effect. */
    uint64_t sendRetryNotBeforeNs = 0ULL;
    /*
     * Per lane, the batch whose last send failed and which is therefore still
     * queued. count == 0 means that lane is not retrying anything.
     *
     * Held across iterations so the next pass can tell a RETRY from a new
     * batch: that is what makes the request id stable (PSD can recognise the
     * duplicate) and what makes the attempt bound countable (retention cannot
     * become permanent). Safe as a plain local -- this loop is the only
     * sender, by the invariant above.
     *
     * PER LANE, not one slot, because the lanes now interleave. A single slot
     * is cleared by whichever send last succeeded, so an operational batch
     * going out would reset the failing CRITICAL batch's attempt count and
     * request id -- the count would never reach the bound and retention would
     * be unbounded again, which is the whole thing being fixed.
     */
    PsdBatchTicket retainedTickets[NVPSS_PSD_SEND_PRIORITY_COUNT]{};
    /* One send slot owed to the operational lane, set by a critical failure
     * and consumed by the next operational send. See the selector. */
    bool criticalYieldPending = false;
    uint32_t observedWakeSeq = psdSenderWakeSeq_.load(std::memory_order_acquire);

    while (queMonitorsRunning)
    {
        const uint64_t nowNs = monotonicNowNs();
        if (statusNoopEnabled && nowNs >= nextStatusNoopNs)
        {
            const bool noopQueued = enqueueStatusNoopEvent(nowNs);
            /* Once per interval, not per backoff retry: the verdicts a restarted
             * SDM would otherwise never hear again. */
            if (noopQueued)
                ReassertStandingVerdicts();
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
        releaseSnapshot.criticalYieldPending = criticalYieldPending;
        releaseSnapshot.operationalGate = operationalGate;
        releaseSnapshot.nowNs = nowNs;

        const bool sendBackoffActive = (nowNs < sendRetryNotBeforeNs);

        NvPSSPsdSendPriority duePriority = NvPSSPsdSendPriority::OPERATIONAL;
        if (!sendBackoffActive && NvPSSPsdSelectDuePriority(&releaseSnapshot, &duePriority))
        {
            const char* requestClass = nullptr;
            if (!buildDecisionRequestFromQueuedEvents(duePriority, &psdDecisionRequest,
                                                      &psdBatchTicket))
                continue;

            /*
             * Is this the batch that just failed, or a new one?
             *
             * The rebuild is what loses the answer: build starts from
             * *request = {}, so nothing in the request survives to say. Only
             * the ticket does, and only because it names entries by sequence
             * number. A match carries the previous request id forward -- so
             * PSD sees the same id and can recognise a retry of a request it
             * may already have acted on -- and counts the attempt against
             * NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS. A mismatch means the
             * queue moved on, so the retry state belongs to nothing and is
             * discarded with it.
             */
            PsdBatchTicket& retainedTicket =
                retainedTickets[NvPSSPsdSendPriorityIndex(duePriority)];
            if (psdBatchIdentityMatches(retainedTicket, psdBatchTicket))
            {
                psdBatchTicket.requestId = retainedTicket.requestId;
                psdBatchTicket.attempts = retainedTicket.attempts + 1U;
                /* Same request id, same request: the first transmission's stamp. */
                psdBatchTicket.stamped = retainedTicket.stamped;
                psdBatchTicket.stamp = retainedTicket.stamp;
                psdBatchTicket.stampGeneration = retainedTicket.stampGeneration;
            }
            else
            {
                psdBatchTicket.requestId = 0U;
                psdBatchTicket.attempts = 1U;
            }

            /* Zero leaves sendDecisionRequestWithRetry() to allocate; non-zero
             * makes it reuse, since it only allocates when the field is 0. */
            if (psdBatchTicket.requestId != 0U)
            {
                NvPSSAssignSerializedDecisionRequestId(&psdDecisionRequest,
                                                       psdBatchTicket.requestId);
            }

            requestClass = NvPSSPsdSendPriorityName(duePriority);

            psdDecisionResponse = {};
            if(sendDecisionRequestWithRetry(&psdDecisionRequest, &psdDecisionResponse, requestClass,
                                            &psdBatchTicket)
                != NVPSSD_SUCCESS)
            {
                /* Batch stays queued. buildDecisionRequestFromQueuedEvents()
                 * only copied it, so the next pass retries the same events
                 * rather than skipping them -- hence the backoff, which is what
                 * keeps that retry from becoming a spin. */
                sendRetryNotBeforeNs = NvPSSPsdSaturatingAddNs(
                    monotonicNowNs(), NVPSS_PSD_SEND_FAILURE_RETRY_BACKOFF_NS);

                /* Whatever id the send used, so the retry can reuse it. */
                psdBatchTicket.requestId = psdDecisionRequest.requestId;

                if (psdBatchTicket.attempts >= NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS)
                {
                    /* Retention is over for this batch: it has had every
                     * attempt it is going to get, and holding it any longer
                     * blocks the lane rather than protecting the events. */
                    dropUndeliverableBatch(duePriority, psdBatchTicket);
                    retainedTicket = PsdBatchTicket{};   /* this lane only */
                }
                else if (NvPSSDecisionRequestIsStatusNoopOnly(&psdDecisionRequest))
                {
                    retainedTicket = psdBatchTicket;
                    NvPSBWriteData(NVPSB_LOG_ERR,
                        "failed to send status/no-op DecisionRequest", "");
                }
                else
                {
                    retainedTicket = psdBatchTicket;
                    NvPSBWriteData(NVPSB_LOG_ERR,
                        "Failed to report queued priority events to PSD; batch retained for retry",
                        "priority=" + std::string(requestClass ? requestClass : "unknown") +
                            ", eventCount=" +
                            std::to_string(static_cast<unsigned>(
                                psdDecisionRequest.sensorDataSummarySize)) +
                            ", requestId=" + std::to_string(psdBatchTicket.requestId) +
                            ", attempt=" + std::to_string(psdBatchTicket.attempts) +
                            "/" + std::to_string(static_cast<unsigned>(
                                NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS)));
                }
            }
            else
            {
                onPsdRequestAcknowledged(duePriority, psdBatchTicket,
                                         psdDecisionRequest.requestId);
                sendRetryNotBeforeNs = 0ULL;
                retainedTicket = PsdBatchTicket{};   /* this lane only */
                NvPSBWriteData(NVPSB_LOG_INFO,
                    "Reported queued priority events to PSD",
                    "backend=" + std::string(NvPSFTransportKindName(PSSDToPSDComBackend)) +
                        ", priority=" + std::string(requestClass ? requestClass : "unknown"));
            }

            /*
             * Hand the operational lane one slot after a critical failure, and
             * take it back once operational has used it.
             *
             * retainedTicket is this lane's slot, so for a critical send a
             * non-zero count means "failed and still queued" -- exactly when
             * the yield is owed. A critical send that succeeded or whose batch
             * was abandoned leaves count at zero and owes nothing.
             */
            if (duePriority == NvPSSPsdSendPriority::CRITICAL)
                criticalYieldPending = (retainedTicket.count != 0U);
            else
                criticalYieldPending = false;

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
        /*
         * THE GATE DEADLINE IS IGNORED WHILE THE BACKOFF IS ACTIVE, or this
         * loop spins.
         *
         * An armed gate whose deadline has passed is normally impossible to
         * observe here: the selector would have returned OPERATIONAL and the
         * send branch above would have taken the iteration. The backoff is the
         * one thing that reaches this point with the gate still due, because it
         * suppresses the send without touching the gate.
         *
         * Including that stale deadline in the minimum then puts nextWakeNs in
         * the PAST, so the wait below takes its `waitStartNs >= nextWakeNs`
         * exit and continues immediately -- at 100% of a core until the backoff
         * expires. Both lanes reach it: a failed critical send never advances
         * the gate, and a failed operational one advances it by only 2.5 ms
         * against a 100 ms backoff.
         *
         * Dropping the gate here costs nothing, because a gate that is already
         * due needs no timer: the moment the backoff expires the selector fires
         * on it. Only the two deadlines that can still be in the future are
         * left, so the wait is always a real wait.
         */
        if (!sendBackoffActive && operationalQueued && operationalGate.armed &&
            operationalGate.deadlineNs < nextWakeNs)
        {
            nextWakeNs = operationalGate.deadlineNs;
        }
        /* Wake when the send backoff expires, otherwise a retained batch on an
         * otherwise idle queue would wait for the next unrelated notification. */
        if (sendBackoffActive && sendRetryNotBeforeNs < nextWakeNs)
            nextWakeNs = sendRetryNotBeforeNs;

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
                        invalidEvent,
                        NvPSSPsdEvictable(invalidEvent, true),
                        trust);
                }
                else if(!ValidateEventSource(eventToProcess))
                {
                    /* Trusted source, but its identity does not match the
                     * configured topology. Dropped, not forwarded: unlike an
                     * untrusted-source event, this carries no fault report. */
                }
                else if(ProcessSafetyEventForFusion(eventToProcess) != NVPSSD_SUCCESS)
                {
                    /*
                     * COUNT ONLY IF THE ENGINE EXISTS.
                     *
                     * ProcessSafetyEventForFusion() returns NVPSSD_FAIL for
                     * `!fusionEnabled || !eventFusion` before doing any work.
                     * That is a CONFIGURATION STATE, not a processing failure,
                     * and counting it would drive a correctly configured
                     * non-fusing deployment straight to PSS ERROR -- latching
                     * every SDM on a system that is behaving exactly as
                     * configured. This branch is already inside
                     * `if(fusionEnabled)`, so testing eventFusion here excludes
                     * precisely the remaining half of that guard.
                     */
                    if (eventFusion)
                        (void)bumpFusionStageFailure(FUSION_STAGE_PROCESS_EVENT);
                    {
                        /* UNCONDITIONAL, not #ifdef NVPSF_DBG -- a release build
                         * used to report nothing here, so a safety event that
                         * failed to be processed left no trace while the periodic
                         * no-op kept transport liveness looking healthy. But
                         * BOUNDED (NvPSSFusionStageEpisode): one record per
                         * failure was itself a way to lose the audit trail.
                         * Recorded even when not counted (no engine), and after
                         * the bump so an ongoing record reports the current
                         * streak. The detail is built only when a record is due. */
                        NvPSSFusionStageRecord kind = NvPSSFusionStageRecord::NONE;
                        if (fusionStageFailureRecordDue(FUSION_STAGE_PROCESS_EVENT, &kind))
                            writeFusionStageFailureRecord(FUSION_STAGE_PROCESS_EVENT, kind,
                                ", pipelineID=" +
                                    std::to_string(static_cast<unsigned>(
                                        eventToProcess.fusionMetadata.pipelineID)) +
                                ", eventType=" +
                                    std::to_string(static_cast<unsigned>(eventToProcess.type)));
                    }
                }
                else
                {
                    clearFusionStageFailure(FUSION_STAGE_PROCESS_EVENT);
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

/*
 * Event-stream configuration that applies whether or not events are fused.
 *
 * Both fields were previously set inside ConfigureMultiCameraFusion, which is
 * skipped when fusion is disabled. That left stalenessThresholdMs_ at its
 * UINT64_MAX default -- isEventStale() could never return true, so no event was
 * ever marked STALE and both SDMs would have consumed arbitrarily old
 * detections as fresh -- and maxPipelinesSupported at the constructor default,
 * silently narrowing the accepted pipeline range.
 */
NvPSSDErr SafetyEventManager::ConfigureEventStream(std::chrono::milliseconds timeWindowSize,
                                                   uint8_t maxPipelines)
{
    if (maxPipelines == 0U || maxPipelines > MAX_SUPPORTED_PIPELINES) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Invalid maxPipelines value: " + std::to_string(maxPipelines), "");
        return NVPSSD_FAIL;
    }

    const auto ms = timeWindowSize.count();
    if (ms < 0) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Invalid negative timeWindowSize: " + std::to_string(ms) + " ms", "");
        return NVPSSD_FAIL;
    }

    maxPipelinesSupported = maxPipelines;
    stalenessThresholdMs_ = static_cast<uint64_t>(ms);
    eventStreamConfigured_ = true;

    return NVPSSD_SUCCESS;
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

    /* maxPipelinesSupported and stalenessThresholdMs_ are NOT set here. They
     * describe the event stream, not the fusion algorithm, and are needed
     * whether or not events are fused: the pipeline bound rejects out-of-range
     * sensors and the staleness threshold is what marks an event STALE for the
     * SDMs. ConfigureEventStream() owns them and runs on every startup path.
     * Callers must invoke it before this function. */
    if (!eventStreamConfigured_) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "ConfigureMultiCameraFusion called before ConfigureEventStream", "");
        result = NVPSSD_FAIL;
        goto done;
    }

    /*
     * The two rejections below are UNCONDITIONAL, not #ifdef NVPSF_DBG.
     *
     * Both refuse to start the daemon, and in a release build they used to do so
     * in silence: the operator saw a startup failure with nothing naming the key
     * to fix. That was drift rather than policy -- the first rejection in this
     * function logs unconditionally, and so do both of ConfigureEventStream()'s.
     * These two had fallen out of the convention the surrounding code sets.
     */

    // Validate fusion parameters
    if (fusionThreshold < 0.0f || fusionThreshold > 1.0f) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Invalid fusionThreshold value; refusing to start",
            "fusionThreshold=" + std::to_string(fusionThreshold) +
                ", acceptedRange=0.0..1.0");
        result = NVPSSD_FAIL;
        goto done;
    }


    if (std::abs(weightSum - 1.0f) > 0.01f) {
        /* Named by their nvpss.conf keys, not by the parameter names they arrive
         * under. An operator reading "weight sum" has no way to know that
         * temporalW, spatialW and attributeW are alpha, beta and gamma in the
         * file they have to edit -- which is the whole point of reporting it. */
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Fusion weights alpha + beta + gamma must sum to 1.0; refusing to start",
            "alpha=" + std::to_string(temporalW) +
                ", beta=" + std::to_string(spatialW) +
                ", gamma=" + std::to_string(attributeW) +
                ", sum=" + std::to_string(weightSum) +
                ", tolerance=0.01");
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

/*
 * Producer-identity validation, applied to every TRUSTED event in both modes.
 *
 * Ordering matters and is deliberate: callers run this AFTER the trust check,
 * never before. An untrusted event's delivery IS the fault report -- it is
 * forwarded as CreateInvalidSourceEvent critical evidence -- so rejecting it
 * here for a sensor_config mismatch would suppress the very report the SDMs
 * need. Validation gates events that will drive a decision; it does not gate
 * events that exist to say a source cannot be trusted.
 *
 * These three checks previously lived at the head of ProcessSafetyEventForFusion,
 * which is only reachable when fusion is enabled. That left the non-fusing path
 * accepting events the fusing path rejects -- an unconfigured pipeline, or a
 * sensorIdentifier contradicting the configured topology -- purely as a side
 * effect of where the code sat.
 */
bool SafetyEventManager::ValidateEventSource(const SafetyEvent& event) const
{
    const uint8_t pipelineId = event.fusionMetadata.pipelineID;

    if (pipelineId > maxPipelinesSupported)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Event from unregistered sensor: " + std::to_string(pipelineId), "");
        return false;
    }

    /* pipelineId == 0 is the well-known "unknown sensor" passthrough channel.
     * Daemon-generated events (heartbeat SW_FAIL, status no-op) also use it and
     * carry a sensorIdentifier that is not in any sensor_config, so they must
     * not be subjected to the membership check. */
    if (sensorConfigLoaded_ && pipelineId != 0)
    {
        auto cfgIt = sensorConfigIdToName_.find(pipelineId);
        if (cfgIt == sensorConfigIdToName_.end())
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: pipelineID " + std::to_string(pipelineId) + " not in sensor_config", "");
            return false;
        }
        /* Treat inbound buffer as untrusted: bound the length to the fixed-size
         * field so we never read past event.sensorIdentifier even if the sender
         * omits the NUL terminator. */
        const size_t sensorIdLen = strnlen(event.sensorIdentifier, MAX_INDENTIFIER_LENGTH);
        std::string sensorId(event.sensorIdentifier, sensorIdLen);

        /*
         * AN OMITTED IDENTIFIER.
         *
         * The name is what makes the pipelineID checkable: a producer configured
         * with the wrong pipelineID is caught only because its name then
         * contradicts sensor_config. Omitting the name switches that check off,
         * and the event is stamped with whichever sensor owns the id it claims.
         *
         * Groups (3D): always refused. A group and a camera are the same shape
         * on the wire, so a claim on the group's id must be explicit.
         *
         * Cameras (2D, and 3D members): refused for an event that drives a
         * decision -- the same rule the docs state, and the one MDX already
         * applies by resolving every name against sensor_config.
         *
         * Must-deliver reports on a camera -- SW_FAIL and the trust reports --
         * are NOT refused here, deliberately; each is the only statement of a
         * fault or of its recovery:
         *  - SW_FAIL: refusing an unverifiable one could miss a latch, while
         *    accepting it can at worst latch spuriously. Fail safe, not open.
         *  - Trust reports: the verdict is applied in OnTrustReport BEFORE the
         *    event is queued, so this check cannot protect the trust map. An
         *    unnamed SENSOR_INVALID never gets here (its sensor is already
         *    untrusted, so the caller forwards it as fault evidence); an
         *    unnamed SENSOR_VALID would, and dropping it would leave the SDMs
         *    holding a failed bit the daemon has already cleared.
         */
        if (sensorId.empty())
        {
            if (IsSensorGroupId(pipelineId))
            {
                NvPSBWriteData(NVPSB_LOG_WARNING,
                    "Rejecting event: empty sensorIdentifier for sensor group pipelineID " +
                    std::to_string(pipelineId) + "; a group must be named explicitly, expected '" +
                    cfgIt->second + "'", "");
                return false;
            }
            if (!NvPSSPsdIsMustDeliverReport(event.type))
            {
                NvPSBWriteData(NVPSB_LOG_WARNING,
                    "Rejecting event: empty sensorIdentifier for pipelineID " +
                    std::to_string(pipelineId) + "; an event that drives a decision must name its sensor, expected '" +
                    cfgIt->second + "'", "");
                return false;
            }
        }
        else if (sensorId != cfgIt->second)
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: sensorIdentifier '" + sensorId +
                "' does not match sensor_config entry '" + cfgIt->second +
                "' for pipelineID " + std::to_string(pipelineId), "");
            return false;
        }
    }

    return true;
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

    /* Source identity is validated by the caller, before the fusion/bypass
     * branch, so both modes get it. Only fusion-specific work remains here. */
    pipelineId = event.fusionMetadata.pipelineID;

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

    /*
     * FRESHNESS BEFORE FUSION -- the two-sided rule of isEventStale(): STALE
     * when OLDER than timeWindowSize or more than timeWindowSize in the
     * FUTURE. Applied here exactly as with fusion off, so staleness does not
     * depend on enableFusion.
     *
     * The engine's own check bounds only the past. An event dated more than
     * timeWindowSize in the FUTURE used to enter its queue, wait until real
     * time caught up, and leave as PASSTHROUGH: a decision input delivered
     * late and never marked STALE -- or, for a grossly wrong timestamp, never
     * evicted at all. Stamped STALE here, every SDM drops it as it would with
     * fusion disabled.
     *
     * At ingestion on purpose: fusion holds an unmatched event for the full
     * window, so the same check at emission would mark every single-camera
     * event STALE.
     *
     * Evaluated ONCE, through CreateBypassEvent -- the same call, and so the
     * same future-timestamp warning, as fusion off. A fresh event's bypass
     * copy is discarded and the source event goes to the engine. Its daemon
     * event id goes unused, which is harmless: ids are unique, not
     * contiguous, and the SDMs dedup by equality.
     *
     * Enqueued exactly as the engine's output is (trust, severity, evictable),
     * not through the bypass block above: that block's defaults make an entry
     * non-evictable, and a STALE entry that cannot be evicted can keep a
     * must-deliver report out of the critical lane.
     */
    {
        FusedSafetyEvent fusedEvent = CreateBypassEvent(event);
        if (fusedEvent.status == STALE)
        {
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
                fusedEvent,
                NvPSSPsdEvictable(fusedEvent, trust.sensorInvalid || trust.aiPipelineInvalid),
                trust);
            NvPSBWriteData(NVPSB_LOG_INFO,
                "STALE Safety Event before fusion (older than timeWindowSize, or more than "
                "timeWindowSize in the future). Source Event Id: " +
                std::to_string(event.id) + " EventType: " + std::to_string(event.type) +
                " PipelineID: " + std::to_string(event.fusionMetadata.pipelineID), "");
            return NVPSSD_SUCCESS;
        }
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
        /*
         * Bounded drain, and the bound is deliberate.
         *
         * The two modes drain at different rates by construction: with fusion
         * enabled manageInputSafetyEventQue() pops ONE event per tick, while
         * this branch used to drain the queue to empty in a single pass. An
         * unbounded `while (true)` inside a periodic thread means a sustained
         * arrival rate can keep one iteration running indefinitely, delaying
         * everything else the event processor does on that tick. Draining
         * faster than events arrive is the normal case, so this is latency
         * shaping rather than a livelock fix -- but an uncapped loop in a
         * periodic safety thread should not be left implicit.
         *
         * Nothing is dropped: whatever remains stays at the head of the queue
         * and is taken on the next tick, in order.
         */
        for (uint32_t drained = 0U; drained < kBypassDrainPerTick; ++drained)
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
            const bool untrusted = trustBypass.sensorInvalid || trustBypass.aiPipelineInvalid;

            /* Same order as the fusion path: trust first, then identity. An
             * untrusted event is still forwarded as evidence; a trusted event
             * whose identity does not match sensor_config is dropped. Without
             * this the non-fusing mode would accept events the fusing mode
             * rejects. */
            if (!untrusted && !ValidateEventSource(event))
            {
                continue;
            }

            FusedSafetyEvent fusedEvent = untrusted
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
                fusedEvent,
                NvPSSPsdEvictable(fusedEvent, untrusted),
                trustBypass);
        }


    } else
    {
        // Get fused events
        auto fusedEvents = eventFusion->GetFusedEvents();

        /*
         * Emit in timestamp order. Fusion does not produce one.
         *
         * PerformSafetyEventFusion() walks the EventType enum from EVENT_0
         * upward and ProcessUnmatchedEvents() iterates eventTypeQueues, an
         * std::unordered_map -- so the output order is event-type ordinal and
         * hash-bucket order, neither of which has anything to do with when the
         * events were observed. Enqueue order is what assigns the tie-break
         * sequence numbers, so sorting here is what makes the staging queue's
         * ordering reproducible rather than dependent on the map's layout.
         *
         * stable_sort: events sharing a timestamp are one frame's simultaneous
         * observations, and their relative order is meaningless but should not
         * vary between runs over identical input.
         */
        std::stable_sort(fusedEvents.begin(), fusedEvents.end(),
                         [](const FusedSafetyEvent& a, const FusedSafetyEvent& b) {
                             return a.timestamp < b.timestamp;
                         });

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
                fusedEvent,
                NvPSSPsdEvictable(fusedEvent, trust.sensorInvalid || trust.aiPipelineInvalid),
                trust);
        }

        eventFusion->ClearFusedEvents(fusedEvents.size());
    }

    return NVPSSD_SUCCESS;
}

/*
 * The event processor. Fusion is one OPTIONAL stage of it, not its purpose.
 *
 * The distinction is load-bearing. HandleFusedEvents() below is what drains
 * inputSafetyEventQue and forwards to PSD when fusion is disabled, so this loop
 * must keep running in both modes -- skipping it would strand every event. But
 * eventFusion is constructed only by ConfigureMultiCameraFusion(), which is not
 * called when fusion is disabled, so the three fusion stages must be guarded or
 * they dereference a null unique_ptr on the first iteration.
 */
void SafetyEventManager::fusionProcessingLoop()
{
    while(fusionProcessorRunning)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(fusionEventPeriod));

        /* Both conditions, not just fusionEnabled: the flag and the engine are
         * set by different functions, so a partially initialised manager must
         * not be able to reach the dereference. */
        /*
         * The four failure reports below are UNCONDITIONAL, not #ifdef NVPSF_DBG.
         *
         * They were debug-only, which meant a release build reported nothing at
         * all when a fusion stage failed: safety-input processing could degrade
         * indefinitely while the periodic status/no-op kept transport liveness
         * looking healthy, and there was not even a log line to correlate
         * against afterwards.
         *
         * Each record names its stage because the sustained-failure escalation
         * counts and reports per stage. Escalation itself is not
         * in this slice -- these are still reports, not faults.
         */
        if(fusionEnabled && eventFusion)
        {
            /* The guard above is the configuration check, so every failure
             * reached from here is a genuine processing failure and counts. */
            if(eventFusion->PerformSafetyEventFusion() != NVPSSD_SUCCESS)
            {
                (void)bumpFusionStageFailure(FUSION_STAGE_PERFORM_FUSION);
                {
                    /* After the bump, so an ongoing record reports the current streak. */
                    NvPSSFusionStageRecord kind = NvPSSFusionStageRecord::NONE;
                    if (fusionStageFailureRecordDue(FUSION_STAGE_PERFORM_FUSION, &kind))
                        writeFusionStageFailureRecord(FUSION_STAGE_PERFORM_FUSION, kind, std::string());
                }
            }
            else
            {
                clearFusionStageFailure(FUSION_STAGE_PERFORM_FUSION);
            }

            if(eventFusion->ProcessUnmatchedEvents() != NVPSSD_SUCCESS)
            {
                (void)bumpFusionStageFailure(FUSION_STAGE_PROCESS_UNMATCHED);
                {
                    /* After the bump, so an ongoing record reports the current streak. */
                    NvPSSFusionStageRecord kind = NvPSSFusionStageRecord::NONE;
                    if (fusionStageFailureRecordDue(FUSION_STAGE_PROCESS_UNMATCHED, &kind))
                        writeFusionStageFailureRecord(FUSION_STAGE_PROCESS_UNMATCHED, kind, std::string());
                }
            }
            else
            {
                clearFusionStageFailure(FUSION_STAGE_PROCESS_UNMATCHED);
            }

            if(eventFusion->CleanProcessedEvents() != NVPSSD_SUCCESS)
            {
                (void)bumpFusionStageFailure(FUSION_STAGE_CLEAN_PROCESSED);
                {
                    /* After the bump, so an ongoing record reports the current streak. */
                    NvPSSFusionStageRecord kind = NvPSSFusionStageRecord::NONE;
                    if (fusionStageFailureRecordDue(FUSION_STAGE_CLEAN_PROCESSED, &kind))
                        writeFusionStageFailureRecord(FUSION_STAGE_CLEAN_PROCESSED, kind, std::string());
                }
            }
            else
            {
                clearFusionStageFailure(FUSION_STAGE_CLEAN_PROCESSED);
            }
        }

        /* Unconditional: this is the delivery path in BOTH modes.
         *
         * HandleFusedEvents() has exactly one return and it is NVPSSD_SUCCESS,
         * so this failure branch is unreachable as the code stands. It is wired
         * to the streak anyway rather than left as a bare log, so that a future
         * failure return is escalated by construction instead of depending on
         * whoever adds it also remembering to instrument it. */
        if(HandleFusedEvents() != NVPSSD_SUCCESS)
        {
            (void)bumpFusionStageFailure(FUSION_STAGE_HANDLE_FUSED);
            {
                /* After the bump, so an ongoing record reports the current streak. */
                NvPSSFusionStageRecord kind = NvPSSFusionStageRecord::NONE;
                if (fusionStageFailureRecordDue(FUSION_STAGE_HANDLE_FUSED, &kind))
                    writeFusionStageFailureRecord(FUSION_STAGE_HANDLE_FUSED, kind, std::string());
            }
        }
        else
        {
            clearFusionStageFailure(FUSION_STAGE_HANDLE_FUSED);
        }
    }
}

/*
 * THE FRESHNESS RULE -- TWO-SIDED, and the same in both fusion modes.
 *
 * An event is STALE when its CLOCK_MONOTONIC timestamp is
 *   - OLDER than timeWindowSize (stalenessThresholdMs_), or
 *   - more than timeWindowSize in the FUTURE.
 * Within the window, in either direction, it is fresh.
 *
 * Where it is applied, so that enableFusion does not change the answer:
 *   - fusion off: every event, through CreateBypassEvent;
 *   - fusion on:  every event, BEFORE the fusion engine, in
 *                 ProcessSafetyEventForFusion (bypass types through
 *                 CreateBypassEvent as well);
 *   - untrusted evidence, in either mode: CreateInvalidSourceEvent.
 * The fusion engine's own check in SafetyEventFusion::ProcessNewSafetyEvent
 * bounds only the past and is a backstop, not this rule.
 *
 * The future bound matters as much as the past one. A corrupted, malicious or
 * clock-skewed timestamp ahead of now would otherwise wait in a queue until
 * real time caught up with it, and then be delivered as if it were fresh.
 *
 * Pure: no clock read and no logging. The overload below reads the clock and
 * reports a future timestamp.
 */
bool SafetyEventManager::isEventStale(uint64_t eventTimestampNs,
                                      uint64_t nowMs) const
{
    const uint64_t eventMs = eventTimestampNs / 1000000ULL;

    /* Future side: dated more than the window AHEAD of now. */
    if (eventMs > nowMs && (eventMs - nowMs > stalenessThresholdMs_))
        return true;

    /* Past side: older than the window. */
    return (nowMs > eventMs) &&
           (nowMs - eventMs > stalenessThresholdMs_);
}

/*
 * The two-sided rule above, against the clock now -- and the one place a
 * FUTURE timestamp is reported. Every event's staleness is evaluated here
 * exactly once (CreateBypassEvent or CreateInvalidSourceEvent), in either
 * fusion mode, so the warning appears once per event. An old event is routine
 * (late delivery) and is not reported here; a future one means the producer's
 * clock or its timestamping is wrong, so it is a warning.
 */
bool SafetyEventManager::isEventStale(uint64_t eventTimestampNs) const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t nowMs = static_cast<uint64_t>(ts.tv_sec) * 1000ULL
                         + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    const bool stale = isEventStale(eventTimestampNs, nowMs);

    const uint64_t eventMs = eventTimestampNs / 1000000ULL;
    if (stale && eventMs > nowMs)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "STALE: event timestamp is " + std::to_string(eventMs - nowMs) +
            " ms in the FUTURE, beyond timeWindowSize (" +
            std::to_string(stalenessThresholdMs_) + " ms); check the producer's CLOCK_MONOTONIC", "");
    }
    return stale;
}

FusedSafetyEvent SafetyEventManager::CreateBypassEvent(const SafetyEvent& event) const
{
    //Pass through SafetyEvent to Fused SafetyEvent
    FusedSafetyEvent fusedEvent;

    // Generate a unique ID
    fusedEvent.id = nextDaemonEventId();

    // Copy properties directly from the source event
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    fusedEvent.cause = event.cause;
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

void SafetyEventManager::SetSensorGroup(uint8_t groupPipelineId,
                                        const std::vector<uint8_t>& memberPipelineIds)
{
    /* Startup-only, before the event threads run; no lock needed here, and the
     * values are read-only afterwards. Validation (members exist, no collision
     * with a camera, within maxPipelines) happens at load in NvPSSDaemon.cpp so
     * that a bad configuration is fatal rather than silently degrading here. */
    sensorGroupPipelineId_ = groupPipelineId;
    sensorGroupMembers_    = memberPipelineIds;
}

void SafetyEventManager::SetSensorConfig(const std::unordered_map<uint8_t, std::string>& pipelineIdToName)
{
    sensorConfigIdToName_ = pipelineIdToName;
    sensorConfigLoaded_ = !sensorConfigIdToName_.empty();
}

bool SafetyEventManager::IsRequiredSourcePipeline(uint8_t pipelineId) const
{
    /* pipelineId 0 is the daemon-internal passthrough channel, never a sensor. */
    if (pipelineId == 0U)
        return false;

    /* 3D: the group alone. Member cameras are in sensor_config but are NOT in
     * the SDMs' reviewed set, and their health reaches the latch through the
     * group verdict rather than directly. */
    if (!sensorGroupMembers_.empty())
        return pipelineId == sensorGroupPipelineId_;

    /* 2D: the reviewed set is the cameras, which is sensor_config exactly. An
     * unloaded config cannot establish that any source is required, so nothing
     * qualifies and escalation stays off rather than firing on everything. */
    if (!sensorConfigLoaded_)
        return false;

    return sensorConfigIdToName_.find(pipelineId) != sensorConfigIdToName_.end();
}

bool SafetyEventManager::IsSensorGroupId(uint8_t pipelineId) const
{
    /* Empty membership means no group is configured -- the 2D case -- so this is
     * false for every pipelineId and every group path below stays inert without
     * needing a mode flag to test. */
    return !sensorGroupMembers_.empty() && (pipelineId == sensorGroupPipelineId_);
}

bool SafetyEventManager::IsSensorGroupLocked(uint8_t pipelineId) const
{
    return IsSensorGroupId(pipelineId);
}

SafetyEventManager::GroupVerdict SafetyEventManager::EvaluateGroupVerdictLocked() const
{
    /* Level-triggered: derived from member state on every call, never stored.
     * There is therefore no group state that can go stale against its members
     * (see design B6 -- the "hold until release" that INV-4 asks for is provided
     * by the SDM's own safe-state latch, not by latching here). */
    bool anyHealthy = false;
    bool anyInit    = false;

    for (const uint8_t member : sensorGroupMembers_)
    {
        const auto it = invalidSensors.find(member);
        if (it == invalidSensors.end())
        {
            anyHealthy = true;
            break;                      /* one healthy member is enough */
        }
        if (it->second == CAUSE_SAIM_INIT)
        {
            anyInit = true;
        }
    }

    if (anyHealthy)
    {
        return GroupVerdict::Trusted;
    }
    /* Every member is down. B-strict (requester decision, OQ-B5): the group is
     * untrusted only when NONE of them is merely initialising. If any member is
     * still SAIM_INIT the group is Pending -- not trusted, so nothing clears,
     * and not untrusted, so nothing latches. */
    return anyInit ? GroupVerdict::Pending : GroupVerdict::Untrusted;
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

    /*
     * A configured group's health is DERIVED from its members and must never be
     * ASSERTED by a client.
     *
     * The daemon synthesises the group's own SENSOR_INVALID / SENSOR_VALID in
     * EmitGroupTrustEvent, which pushes straight onto the input queue and does
     * not pass through here -- so this rejection cannot silence the daemon's own
     * verdict, only a client's attempt to override it.
     *
     * Why it matters more than it looks: a client SENSOR_VALID naming the group
     * would, without this, land in invalidSensors' erase path for the group id
     * and then be forwarded downstream, telling the SDM to CLEAR the failed bit
     * for a group whose members are all still down. That is the latch-clear
     * path -- the direction where being wrong releases a safe state rather than
     * entering one.
     *
     * Checked before the lock is taken: the group configuration is startup-only,
     * and a rejected report must not touch trust state at all.
     */
    if (isSensorTrustReport && IsSensorGroupId(event.fusionMetadata.pipelineID))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "Trust report rejected: pipelineID names a sensor group, whose health is "
                       "derived from its members and cannot be reported directly",
                       "pipelineID: " + std::to_string(event.fusionMetadata.pipelineID) +
                       ", eventType: " + std::to_string(event.type) +
                       ", reporterType: " + std::to_string(reporterClientType));
        return false;
    }

    /* AI-pipeline trust is pipeline-wide: the Perception Monitor emits one verdict for the whole AI
     * pipeline, not per MDX client (fusionMetadata.clientID is not used here), so it
     * is tracked as a single global latch. Every MDX client -- already connected,
     * late-connecting, or reconnecting into a reused slot -- consistently reads the
     * current state via QueryTrustState, with no per-client bookkeeping to go stale. */
    std::unique_lock<std::mutex> lock(trustStateMutex);
    switch (event.type)
    {
        case SENSOR_INVALID:
            /* Record the cause alongside membership. Overwrites any previous
             * cause for this sensor: the latest report is the current state,
             * and a stale cause would make a group verdict answer for a
             * condition that has already been superseded. */
            invalidSensors[event.fusionMetadata.pipelineID] = event.cause;  /* pipelineID = sensor */
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

    /* Group verdict transition, computed while the trust state is still held. */
    bool         emit          = false;
    bool         enteredPending = false;
    bool         leftPending    = false;
    GroupVerdict verdict        = GroupVerdict::Trusted;
    if (isSensorTrustReport && !sensorGroupMembers_.empty())
    {
        verdict = EvaluateGroupVerdictLocked();

        /* Pending edges, for the warning. Logged on entry and exit only: a
         * misconfigured or slow-starting system can sit in Pending for many
         * reports, and one line per report would bury an unrelated fault. */
        enteredPending = (verdict == GroupVerdict::Pending) &&
                         (lastGroupVerdict_ != GroupVerdict::Pending);
        leftPending    = (verdict != GroupVerdict::Pending) &&
                         (lastGroupVerdict_ == GroupVerdict::Pending);
        lastGroupVerdict_ = verdict;

        /* Pending emits NOTHING -- leaving the SDM's failed bit unchanged is the
         * whole point: it must neither latch (the members may only be
         * initialising) nor clear (they may be genuinely failed).
         *
         * Compared against what the SDMs were last TOLD, not against the
         * previous computed verdict, so Untrusted -> Pending -> Untrusted stays
         * silent while Untrusted -> Pending -> Trusted still emits. */
        if (verdict != GroupVerdict::Pending && verdict != lastEmittedGroupVerdict_)
        {
            emit = true;
            lastEmittedGroupVerdict_ = verdict;
        }
    }
    /* Snapshot everything needed after the unlock. Group configuration is
     * startup-only, but reading it after releasing the lock should not depend on
     * knowing that. */
    const bool        groupConfigured = !sensorGroupMembers_.empty();
    const uint8_t     groupId         = sensorGroupPipelineId_;
    const std::string groupName       =
        (sensorConfigIdToName_.count(groupId) != 0U) ? sensorConfigIdToName_.at(groupId)
                                                     : std::string();

    lock.unlock();   /* leaf-lock discipline: never enqueue while holding trust state */

    if (leftPending && groupConfigured)
    {
        NvPSBWriteData(NVPSB_LOG_INFO,
            "BEV group left the initialising state",
            "group=" + groupName + ", pipelineID=" + std::to_string(groupId));
    }

    if (enteredPending && groupConfigured)
    {
        /* The window B-strict accepts, made visible. It is bounded by SAIM's
         * connect-retry budget (MAX_RTSP_CONNECT_RETRIES x RETRY_DELAY_SEC),
         * after which a stuck camera reports a genuine cause and the verdict
         * becomes Untrusted. Nothing in PSS enforces that bound -- it is a
         * property of SAIM's retry loop -- so a window that outlives it must be
         * visible in the record rather than only in an incident. */
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "BEV group has no healthy member but is not untrusted (initialisation in progress)",
            "group=" + groupName + ", pipelineID=" + std::to_string(groupId));
    }

    if (emit)
    {
        EmitGroupTrustEvent(verdict == GroupVerdict::Untrusted ? SENSOR_INVALID : SENSOR_VALID,
                            groupId, groupName);
    }
    return true;
}

/*
 * Synthesise the group's own trust report so the SDM sees it as it sees any
 * sensor's.
 *
 * This must be daemon-originated rather than stamped onto a passing group event:
 * BEV may stop emitting precisely when its cameras fail, and a verdict that
 * depends on downstream traffic still arriving would never be delivered in the
 * one case that matters.
 */
void SafetyEventManager::EmitGroupTrustEvent(EventType type, uint8_t groupId,
                                             const std::string& groupName)
{
    /*
     * The name is now load-bearing, not cosmetic. ValidateEventSource requires a
     * group event to name its group (GC-B3), so an unnamed one would be dropped
     * downstream -- and the thing dropped would be a group trust verdict, with
     * nothing in the log tying the drop back to here.
     *
     * Unreachable: sensorGroupConfigParse refuses an empty group name and daemon
     * startup inserts it into sensorConfigIdToName_ before any of this runs. It
     * is stated rather than assumed because the cost of the assumption silently
     * breaking changed when the name became mandatory.
     */
    if (groupName.empty())
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Not emitting group trust verdict: the group has no configured name, so the event "
            "would be rejected by source validation and the verdict lost",
            "pipelineID: " + std::to_string(groupId) +
            ", eventType: " + std::to_string(type));
        return;
    }

    SafetyEvent ev{};
    /* Unique id: ATLControl.cpp:1354 deduplicates on event.id, so a repeated
     * transition carrying a reused id would be silently dropped (gap GC-B8).
     * Same clock-derived scheme as CreateInvalidSourceEvent. */
    ev.id              = nextDaemonEventId();
    ev.type            = type;
    ev.timestamp       = monotonicNowNs();
    ev.confidenceLevel = 1.0F;
    ev.processed       = false;
    ev.cause           = (type == SENSOR_INVALID) ? CAUSE_SAIM_UNKNOWN
                                                  : CAUSE_SAIM_SENSOR_HEALTHY;
    ev.fusionMetadata.pipelineID = groupId;
    ev.fusionMetadata.clientID   = 0U;
    /* Unconditional: the empty case returned above. */
    std::snprintf(ev.sensorIdentifier, MAX_INDENTIFIER_LENGTH, "%s", groupName.c_str());
    pssSafetyEventSetCRC(&ev);

    NvPSBWriteData(NVPSB_LOG_INFO,
        std::string("BEV group trust transition -> ") +
            (type == SENSOR_INVALID ? "SENSOR_INVALID" : "SENSOR_VALID"),
        "group=" + groupName + ", pipelineID=" + std::to_string(groupId));

    {
        std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
        inputSafetyEventQue.push_back(std::make_pair(0, ev));
    }
}

void SafetyEventManager::ReassertStandingVerdicts()
{
    bool aiInvalid = false;
    bool groupUntrusted = false;
    uint8_t groupId = 0U;
    std::vector<std::pair<uint8_t, EventCause>> cameras;
    {
        std::lock_guard<std::mutex> lock(trustStateMutex);
        aiInvalid = aiPipelineInvalidGlobal;
        if (!sensorGroupMembers_.empty())
        {
            /* 3D: the reviewed set is the group alone, and what the SDMs hold is
             * the verdict they were last TOLD. Members are not re-sent. */
            groupUntrusted = (lastEmittedGroupVerdict_ == GroupVerdict::Untrusted);
            groupId = sensorGroupPipelineId_;
        }
        else
        {
            cameras.assign(invalidSensors.begin(), invalidSensors.end());
        }
    }
    std::sort(cameras.begin(), cameras.end());

    std::vector<SafetyEvent> events;
    std::string summary;
    auto make = [&](EventType type, uint8_t pipelineId, EventCause cause, const std::string& name) {
        SafetyEvent ev{};
        ev.id              = nextDaemonEventId();
        ev.type            = type;
        ev.timestamp       = monotonicNowNs();
        ev.confidenceLevel = 1.0F;
        ev.processed       = false;
        ev.cause           = cause;
        ev.fusionMetadata.pipelineID = pipelineId;
        ev.fusionMetadata.clientID   = 0U;
        std::snprintf(ev.sensorIdentifier, MAX_INDENTIFIER_LENGTH, "%s", name.c_str());
        pssSafetyEventSetCRC(&ev);
        events.push_back(ev);
    };
    /* Names are startup-only configuration, read lock-free as ValidateEventSource
     * does. An id with no configured name is skipped: source validation would
     * drop the event, and the verdict with it. */
    auto nameOf = [&](uint8_t id) -> std::string {
        const auto it = sensorConfigIdToName_.find(id);
        return (it != sensorConfigIdToName_.end()) ? it->second : std::string();
    };

    if (aiInvalid)
    {
        make(AI_PIPELINE_INVALID, 0U, CAUSE_NONE, std::string());
        summary += "aiPipeline=INVALID";
    }
    if (groupUntrusted && !nameOf(groupId).empty())
    {
        make(SENSOR_INVALID, groupId, CAUSE_SAIM_UNKNOWN, nameOf(groupId));
        summary += (summary.empty() ? "" : ", ") + std::string("group=") + std::to_string(groupId);
    }
    for (const auto& cam : cameras)
    {
        const std::string name = nameOf(cam.first);
        if (name.empty())
            continue;
        make(SENSOR_INVALID, cam.first, cam.second, name);
        summary += (summary.empty() ? "" : ", ") + std::string("pipelineID=") +
                   std::to_string(cam.first) + "(cause " +
                   std::to_string(static_cast<unsigned>(cam.second)) + ")";
    }

    if (summary != lastReassertSummary_)
    {
        NvPSBWriteData(NVPSB_LOG_INFO,
            "PSS_TRUST_VERDICTS_REASSERTED",
            (summary.empty() ? std::string("none: no INVALID verdict stands") : summary) +
                ", every=status interval, reason=a restarted SDM must relearn verdicts sent on change");
        lastReassertSummary_ = summary;
    }
    if (events.empty())
        return;

    std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
    for (const auto& ev : events)
        inputSafetyEventQue.push_back(std::make_pair(0, ev));
}

SafetyEventManager::TrustState SafetyEventManager::QueryTrustState(
    uint8_t pipelineId, uint8_t clientId) const
{
    /* AI-pipeline trust is a single pipeline-wide latch, so clientId no longer selects
     * per-client state; it is retained in the signature for callers and for the
     * sensor-trust lookup keyed by pipelineId. */
    (void)clientId;
    std::lock_guard<std::mutex> lock(trustStateMutex);
    /* A configured group's health is DERIVED from its members and is never
     * asserted directly, so a report naming the group cannot move it. Pending
     * reads as trusted here: it must not gate decisions (the members may only be
     * initialising) and it must not latch. */
    if (IsSensorGroupLocked(pipelineId))
    {
        return { EvaluateGroupVerdictLocked() == GroupVerdict::Untrusted,
                 aiPipelineInvalidGlobal };
    }

    /* Per-camera trust is membership only -- unchanged. The recorded cause is
     * read by group evaluation (3D), never here. */
    return { invalidSensors.count(pipelineId) != 0,
             aiPipelineInvalidGlobal };
}

FusedSafetyEvent SafetyEventManager::CreateInvalidSourceEvent(const SafetyEvent& event) const
{
    FusedSafetyEvent fusedEvent;
    fusedEvent.id = nextDaemonEventId();
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    fusedEvent.cause = event.cause;
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
