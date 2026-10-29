/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSS_DELIVERY_FAILSAFE_HPP
#define NVPSS_DELIVERY_FAILSAFE_HPP

#include "pss_protocol.h"
#include "NvPSD.h"   /* NVPSD_DECISION_RESPONSE_TIMEOUT_MS_DEFAULT/_MIN/_MAX */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <mutex>

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

/*
 * Depth of the PSS -> PSD OPERATIONAL staging queue
 * (nvpss.conf psd_operational_queue_depth).
 *
 * SIZED AGAINST FRAME FAN-OUT, NOT AGAINST EVENT RATE. Perception reports per
 * frame, so events do not arrive as a flow: they arrive as a clump sharing one
 * timestamp, then nothing until the next frame. Measured on IGX against live
 * VSS over 600 s: 32.1 frames/s, mean 4.66 events per frame, peak 19.
 *
 * A depth equal to the wire batch therefore overflows inside a SINGLE frame's
 * flush, which is what made the loss look rate-induced when it was not. The
 * drain was never the constraint: 19 events cost three DecisionRequests at the
 * 2.5 ms operational release period, so 7.5 ms against a 31 ms frame period.
 * That is also why the loss did not improve with a faster drain but did
 * disappear once the depth exceeded one frame's fan-out.
 *
 * The floor is one wire batch -- below MAX_SENSORS_DATA_SUMMARY_SIZE the queue
 * cannot stage even a single full request. The default carries the measured
 * peak with room for a busier scene, since fan-out scales with objects in view
 * rather than with anything this daemon controls. The maximum bounds the heap
 * this key can claim at roughly 261 bytes per slot.
 *
 * Cost is PSS-internal heap only. It is NOT a wire cost: DecisionRequest still
 * carries MAX_SENSORS_DATA_SUMMARY_SIZE slots, so sizeof(DecisionRequest),
 * PSS_SCHEMA_VERSION and the wire layout are untouched, and no
 * coordinated PSS/gateway/SDM redeploy is implied.
 */
static constexpr uint32_t NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MIN =
    MAX_SENSORS_DATA_SUMMARY_SIZE;
static constexpr uint32_t NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MAX = 512U;
static constexpr uint32_t NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_DEFAULT = 32U;

/*
 * Depth of the PSS -> PSD CRITICAL staging queue
 * (nvpss.conf psd_critical_queue_depth).
 *
 * THE CRITICAL LANE TAKES THE SAME FAN-OUT, so it needs the same depth. This
 * was previously fixed at the wire batch on the grounds that critical
 * delivery is a rarely exercised path. It is not: NvPSSClassifyPsdSeverity()
 * routes an event to this lane when the event itself is critical OR the mode
 * is ERROR OR its sensor is invalid OR its AI pipeline is untrusted. The last
 * three are properties of the SOURCE, not of the event, so when any of them
 * holds, an ENTIRE frame's fan-out lands here -- the same clump of up to 19
 * measured above, against a depth of 8.
 *
 * That is the worst possible moment to shed evidence. The lane carries
 * SENSOR_INVALID, AI_PIPELINE_INVALID and SW_FAIL, which are exactly the
 * reports a degraded deployment is entering ERROR mode on, and a refused
 * enqueue there loses the fault report that explains the degradation.
 *
 * Bounds and default match the operational lane; the two are sized by the same
 * frame fan-out and there is no reason for them to differ.
 */
static constexpr uint32_t NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MIN =
    MAX_SENSORS_DATA_SUMMARY_SIZE;
static constexpr uint32_t NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MAX = 512U;
static constexpr uint32_t NVPSS_PSD_CRITICAL_QUEUE_DEPTH_DEFAULT = 32U;

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

/* Number of send lanes, so per-lane state can be indexed by the enum. */
static constexpr size_t NVPSS_PSD_SEND_PRIORITY_COUNT = 2U;

static inline size_t NvPSSPsdSendPriorityIndex(NvPSSPsdSendPriority priority)
{
    const size_t index = static_cast<size_t>(priority);
    return (index < NVPSS_PSD_SEND_PRIORITY_COUNT) ? index : 0U;
}

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
/* Spacing between retries of a DecisionRequest whose send failed.
 *
 * Required because the batch is no longer destroyed by a failed send: it stays
 * queued so it can be retried (see buildDecisionRequestFromQueuedEvents).
 * Without a backoff the sender would rebuild and resend it immediately, and
 * sendDecisionRequestWithRetry() does not sleep between its own attempts -- on
 * a link that fails fast, such as pssdServer being absent, the whole retry
 * budget returns at once and the loop would spin at 100% on the safety path.
 * The critical lane needs this most: it has no release gate to space it. */
static constexpr uint64_t NVPSS_PSD_SEND_FAILURE_RETRY_BACKOFF_MS = 100ULL;
/*
 * How many times one staged batch may be rebuilt and resent before it is
 * abandoned.
 *
 * RETENTION MUST BE BOUNDED, or the fix for a lost batch becomes a stuck lane.
 * Retention exists for a TRANSIENT failure: the batch survives the hiccup and
 * goes out on the next pass. A batch that fails DETERMINISTICALLY -- PSD
 * rejecting this particular request, say -- is retried forever instead, and
 * because it is never removed the lane it sits in never empties. For the
 * critical lane that is fatal twice over: NvPSSPsdSelectDuePriority() sees
 * criticalQueued stay true and never reaches the operational branch, so the
 * operational queue fills and sheds every subsequent event -- the exact failure
 * this depth work exists to remove -- while the critical queue, also full,
 * refuses the NEW fault reports arriving behind the stuck one.
 *
 * So the batch is dropped once the bound is reached, loudly and counted, which
 * is the same contract the enqueue path already honours: evidence may be shed
 * only where the shedding is reported. Losing one undeliverable batch is
 * strictly better than losing every event behind it.
 *
 * Sized as an outer multiplier on sendDecisionRequestWithRetry()'s own budget,
 * not as a standalone number: each attempt here is already a full
 * pss_to_psd_retry_budget x pss_to_psd_response_timeout_ms window, bounded by
 * NVPSS_PSS_TO_PSD_RETRY_WINDOW_MS_MAX. At the shipped 3 x 2000 ms that is 6 s
 * per attempt, so three attempts hold an undeliverable batch for about 18 s
 * before abandoning it -- 9 transport attempts, against the 3 a single
 * sendDecisionRequestWithRetry() call made before retention existed.
 *
 * That 18 s is a delivery stall, not an outage: the lane alternation in
 * NvPSSPsdSelectDuePriority() keeps the operational lane sending throughout,
 * and the single-in-flight invariant means no arrangement here could overlap a
 * critical attempt with anything else anyway.
 */
static constexpr uint32_t NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS = 3U;
/* Minimum spacing between staging-queue overflow warnings. Overflow is bursty
 * by construction -- it happens when one frame's fan-out exceeds the depth, so
 * every remaining event in that same flush is refused too -- and an unthrottled
 * log on the safety path would be its own fault. The counters stay exact. */
static constexpr uint64_t NVPSS_PSD_ENQUEUE_DROP_WARN_INTERVAL_MS = 1000ULL;
static constexpr uint64_t NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_NS =
    NVPSS_PSD_STATUS_NOOP_ENQUEUE_RETRY_BACKOFF_MS * NVPSS_PSD_SEND_NS_PER_MS;
static constexpr uint64_t NVPSS_PSD_ENQUEUE_DROP_WARN_INTERVAL_NS =
    NVPSS_PSD_ENQUEUE_DROP_WARN_INTERVAL_MS * NVPSS_PSD_SEND_NS_PER_MS;
static constexpr uint64_t NVPSS_PSD_SEND_FAILURE_RETRY_BACKOFF_NS =
    NVPSS_PSD_SEND_FAILURE_RETRY_BACKOFF_MS * NVPSS_PSD_SEND_NS_PER_MS;
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
    /* Set when the critical lane owes the operational lane one send slot,
     * i.e. a critical batch has just failed and is being retried. ONE slot,
     * consumed by the next operational send: a flag that stayed set for the
     * whole retry sequence would invert the starvation instead of fixing it,
     * because operational is due every release period on a live deployment and
     * the critical retries would then never run. See
     * NvPSSPsdSelectDuePriority(). */
    bool criticalYieldPending;
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

/*
 * Which lane, if any, may send this pass.
 *
 * CRITICAL WINS, BUT NOT WHILE IT IS FAILING. Unconditional critical
 * precedence is correct as long as critical batches leave: they are the
 * fault reports, and delaying them behind routine proximity evidence would
 * invert the priority the lanes exist to express.
 *
 * It stops being correct the moment a critical batch is retained across
 * retries, because "critical first" then means "critical only" -- the queue
 * cannot drain, criticalQueued cannot go false, and the operational lane is
 * never selected again.
 *
 * criticalYieldPending breaks that by ALTERNATING rather than reordering. The
 * first critical attempt keeps absolute precedence; each failure then hands
 * the operational lane exactly one slot before the next retry. So a stuck
 * batch costs operational one slot per retry instead of every slot, and the
 * retries still happen -- which matters, because they are what carries the
 * batch to NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS and out of the lane.
 *
 * Critical is chosen last as well, so a lull in operational traffic does not
 * leave the link idle with a batch waiting.
 */
static inline bool NvPSSPsdSelectDuePriority(const NvPSSPsdReleaseSnapshot* snapshot,
                                             NvPSSPsdSendPriority* selected)
{
    if (snapshot == nullptr || selected == nullptr)
        return false;

    if (snapshot->criticalQueued && !snapshot->criticalYieldPending)
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
    if (snapshot->criticalQueued)
    {
        *selected = NvPSSPsdSendPriority::CRITICAL;
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

/* NvPSSDeliveryReasonFromNvPSDErr() has been removed: its only caller was
 * the POSIX_MSG_QUE arm of SafetyEventManager, and the socket path reports its
 * reason directly.
 *
 * NvPSD.h is still included above, and a first pass at this deletion wrongly
 * removed it -- the header also takes NVPSD_DECISION_RESPONSE_TIMEOUT_MS_DEFAULT
 * /_MIN/_MAX from it, which has nothing to do with the transport. The build
 * failed with 13 errors and named all three. */

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

/*
 * ============================ CRITICAL-LANE ADMISSION ============================
 *
 * The critical lane carries two kinds of entry:
 *   - MUST-DELIVER reports: SW_FAIL and the trust reports (SENSOR_INVALID/VALID,
 *     AI_PIPELINE_INVALID/VALID). Each is the only statement of a fault or of
 *     its recovery; several are sent once, on change.
 *   - ordinary events routed here only because their SOURCE was distrusted, or
 *     because the mode was ERROR.
 * An ordinary event from a distrusted source, or a STALE one, is skipped by
 * every SDM, so evicting it loses nothing a decision uses. Its producer marks
 * it EVICTABLE at enqueue. A trusted ordinary event, or one queued only because
 * the mode was ERROR, is not evictable: it may still be acted on.
 */
static inline bool NvPSSPsdIsMustDeliverReport(EventType type)
{
    return type == SW_FAIL ||
           type == SENSOR_INVALID ||
           type == SENSOR_VALID ||
           type == AI_PIPELINE_INVALID ||
           type == AI_PIPELINE_VALID;
}

/*
 * The entry to evict so a must-deliver report can be admitted to a full
 * critical lane: the newest evictable entry outside the first
 * protectedFront positions, or -1 if there is none. The front is protected
 * because a batch is copied from it, and a retry is matched against that copy
 * by its entries; evicting from it would turn a retry into a new request.
 * Entry is any type with a bool member `evictable`.
 *
 * `evictable` is decided once, when the entry is queued, and is not re-asked
 * here. It must be set only for an entry that every SDM will skip WHATEVER
 * trust does before it is sent: the staging queue keeps the trust an entry was
 * queued with, and the build can only lower a stamp, never raise it. Re-asking
 * the current trust instead would refuse exactly the report that restores it:
 * a trust recovery is applied before the report itself is queued, so at that
 * moment every queued event looks trusted.
 */
template <typename Entry>
static inline long NvPSSPsdCriticalLaneVictim(const std::deque<Entry>& lane, size_t protectedFront)
{
    for (size_t i = lane.size(); i > protectedFront; --i)
    {
        if (lane[i - 1U].evictable)
            return static_cast<long>(i - 1U);
    }
    return -1;
}

/*
 * Critical-lane overflow: a must-deliver report was refused because the lane
 * held nothing evictable. While raised, PSS stamps every DecisionRequest ERROR,
 * so the loss is SDM-visible instead of silent.
 *
 * Cleared only when BOTH hold:
 *   - the lane has drained to half its depth (a hysteresis, so a lane hovering
 *     at full does not flap the mode), and
 *   - a request stamped ERROR for the LATEST refusal has been acknowledged by
 *     PSD. Every refusal advances a generation; a send attempt carries the
 *     generation it was stamped with (stampGeneration), and only a successful
 *     send reports it (noteStampDelivered). A batch built before the refusal
 *     carries 0, and an abandoned batch is never reported, so neither can clear
 *     a refusal no SDM has been told about.
 */
class NvPSSPsdCriticalOverflowLatch
{
public:
    /* Every refusal; true on the rising edge only. */
    bool raise()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        return !active_.exchange(true, std::memory_order_acq_rel);
    }

    /* The generation to stamp into a request sent now; 0 when not raised. */
    uint64_t stampGeneration() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_.load(std::memory_order_acquire) ? generation_ : 0U;
    }

    /* PSD acknowledged a request stamped with this generation. */
    void noteStampDelivered(uint64_t generation)
    {
        if (generation == 0U)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation > deliveredGeneration_)
            deliveredGeneration_ = generation;
    }

    /* True on the falling edge only. refusedOutstanding: a refused report is
     * still pending re-admission, or re-admitted but not yet delivered -- the
     * refusal is not over until the report itself has gone out. */
    bool clearIfDrained(size_t depth, size_t capacity, bool refusedOutstanding = false)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.load(std::memory_order_acquire) || depth * 2U > capacity ||
            deliveredGeneration_ < generation_ || refusedOutstanding)
            return false;
        active_.store(false, std::memory_order_release);
        return true;
    }

    bool active() const { return active_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mutex_;
    std::atomic<bool> active_{false};
    uint64_t generation_{0U};
    uint64_t deliveredGeneration_{0U};
};

/*
 * Refused must-deliver reports are kept, not lost: retained in a bounded list,
 * re-admitted to the critical lane as it drains, and the overflow holds until
 * every one has been delivered. A trust verdict states the CURRENT state of one
 * slot -- a sensor (pipelineID) or an AI pipeline -- so a newer verdict for the
 * same slot supersedes a pending older one: delivering the older one after it
 * would roll the SDMs back. A SW_FAIL is a fault in its own right and is never
 * coalesced.
 *
 * The slot is what the verdict is ABOUT, as PSS's trust state and every SDM key
 * it: a sensor verdict by its pipelineID, an AI-pipeline verdict by class alone
 * (pipeline-wide, one latch). Not by who sent it: fusionMetadata.clientID is the
 * reporting connection's slot, which changes when a monitor reconnects into
 * another one, and is 0 on the daemon's own re-assertions.
 */
static constexpr size_t NVPSS_PSD_REFUSED_PENDING_MAX = 32U;

static inline int NvPSSPsdVerdictSlotClass(EventType type)
{
    if (type == SENSOR_INVALID || type == SENSOR_VALID)
        return 1;
    if (type == AI_PIPELINE_INVALID || type == AI_PIPELINE_VALID)
        return 2;
    return 0;
}

static inline bool NvPSSPsdSameVerdictSlot(const FusedSafetyEvent& a, const FusedSafetyEvent& b)
{
    const int cls = NvPSSPsdVerdictSlotClass(a.type);
    if (cls == 0 || cls != NvPSSPsdVerdictSlotClass(b.type))
        return false;
    if (cls == 2)
        return true; /* the AI pipeline: one slot */
    return a.fusionMetadata.pipelineID == b.fusionMetadata.pipelineID;
}

/* Drop pending entries a newer verdict supersedes. Entry has a member `event`. */
template <typename Entry>
static inline size_t NvPSSPsdDropSupersededRefused(std::deque<Entry>& pending, const FusedSafetyEvent& newer)
{
    size_t dropped = 0U;
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (NvPSSPsdSameVerdictSlot(it->event, newer))
        {
            it = pending.erase(it);
            ++dropped;
        }
        else
        {
            ++it;
        }
    }
    return dropped;
}

/* Retain a refused report: it supersedes a pending one in its slot, then joins
 * the back. False if the list is full: that report is lost -- the overflow stays
 * raised, so the SDMs hold, and the caller counts and records it. */
template <typename Entry>
static inline bool NvPSSPsdRetainRefused(std::deque<Entry>& pending, const Entry& refused, size_t max)
{
    (void)NvPSSPsdDropSupersededRefused(pending, refused.event);
    if (pending.size() >= max)
        return false;
    pending.push_back(refused);
    return true;
}

/*
 * An abandoned batch's events, for its audit record:
 * "[id:type:pipelineID:clientID,...]". Which events no decision will ever see.
 */
static inline std::string NvPSSPsdDescribeEvents(const FusedSafetyEvent* events, size_t count)
{
    std::string s = "[";
    for (size_t i = 0; events != nullptr && i < count; ++i)
    {
        if (i != 0U)
            s += ",";
        s += std::to_string(events[i].id) + ":" + std::to_string(static_cast<int>(events[i].type)) + ":" +
             std::to_string(static_cast<unsigned>(events[i].fusionMetadata.pipelineID)) + ":" +
             std::to_string(static_cast<unsigned>(events[i].fusionMetadata.clientID));
    }
    return s + "]";
}

/*
 * The abandoned batches since PSD last acknowledged a request.
 *
 * Every SDM logs the id of each request it processes, and the gateway passes
 * PSS's id through. So the first request PSD acknowledges after one or more
 * batches were abandoned is the join key: its record names that id, and what
 * was lost before it. An SDM log is then joined to the loss by request id, not
 * by timestamp. Sender thread only.
 */
class NvPSSPsdUndeliverableLedger
{
public:
    static constexpr size_t kListedRequestIds = 8U;

    void noteAbandoned(uint32_t requestId, size_t eventCount)
    {
        ++batches_;
        events_ += eventCount;
        requestIds_.push_back(requestId);
        if (requestIds_.size() > kListedRequestIds)
            requestIds_.pop_front();
    }

    bool pending() const { return batches_ != 0U; }

    /* The detail of PSD_STAGING_DELIVERY_RESUMED for the request PSD has just
     * acknowledged, and the ledger cleared; empty if nothing was abandoned. */
    std::string takeResumed(uint32_t deliveredRequestId)
    {
        if (batches_ == 0U)
            return std::string();
        std::string ids = "[";
        for (size_t i = 0; i < requestIds_.size(); ++i)
            ids += (i != 0U ? "," : "") + std::to_string(requestIds_[i]);
        ids += "]";
        std::string s = "requestId=" + std::to_string(deliveredRequestId) +
                        ", abandonedBatches=" + std::to_string(batches_) +
                        ", abandonedEvents=" + std::to_string(events_) +
                        ", abandonedRequestIds=" + ids;
        if (batches_ > requestIds_.size())
            s += " (the last " + std::to_string(requestIds_.size()) + ")";
        batches_ = 0U;
        events_ = 0U;
        requestIds_.clear();
        return s;
    }

private:
    uint64_t batches_{0U};
    uint64_t events_{0U};
    std::deque<uint32_t> requestIds_;
};

#endif /* NVPSS_DELIVERY_FAILSAFE_HPP */
