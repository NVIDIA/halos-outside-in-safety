/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAFETY_EVENT_MANAGER_HPP
#define SAFETY_EVENT_MANAGER_HPP

#include <deque>
#include <utility>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <unordered_map>

#include "NvPSSSafetyEventFusion.hpp"
#include "NvPSSDeliveryFailSafe.hpp"
#include "NvPSSStatusNoop.hpp"
#include "pss_daemon.h"
#include "nvpsf_transport.h"
/* NvPSD.h is not needed here: the daemon's only use of it was the mqueue arm and
 * its NvPSDCtx member. The PSS -> PSD link is NvPSSDToPSD now, end to end. */
#include "NvPSSDToPSD.hpp"

/**
 * @brief Placeholder entry count inputSafetyEventQue is constructed with.
 *
 * NOT a staging depth. Both PSS -> PSD staging queues are configurable now --
 * NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_DEFAULT and
 * NVPSS_PSD_CRITICAL_QUEUE_DEPTH_DEFAULT -- and neither is sized from this.
 * The constructor clears the prefill before use.
 */
#define MAX_EVENTS_PER_QUE 8

/**
 * @brief Events the non-fusing path drains per event-processor tick.
 *
 * Bounds the bypass drain so one tick cannot run unboundedly under sustained
 * arrival. Sized well above the observed steady-state rate, so the cap is a
 * backstop rather than a throttle: in normal operation the queue empties and
 * the loop exits early. Anything left over is taken on the next tick, in order.
 */
#define kBypassDrainPerTick 64U
#define SEC_TO_NANO_SEC 1000000000L

/* Kinds selectable on the PSS -> PSD link. UDP_SOCKET is not one of them: that
 * kind belongs to the gateway -> SDM link only, and POSIX_MSG_QUE was deleted
 * from the enum -- a stale 0 fails this check rather than enabling
 * status no-ops on a transport that does not exist. */
static inline bool NvPSSStatusNoopEnabledForBackend(NvPSFTransportKind backend)
{
    return backend == NVPSF_TRANSPORT_UNIX_SOCKET;
}

/*
 * How long a fusion stage must fail WITHOUT INTERRUPTION before PSS declares
 * ERROR (nvpss.conf fusion_failure_error_ms).
 *
 * A DURATION, NOT AN ITERATION COUNT, and the distinction is the whole point of
 * this key. The five instrumented stages run on two different loops --
 * INPUT_SAFETYEVENT_Q_PERIOD_US (100 us) carries ProcessSafetyEventForFusion,
 * SAFETYEVENT_FUSION_PERIOD_US (2000 us) carries the other four -- so one
 * iteration count would mean two different wall-clock durations twenty times
 * apart. An earlier draft of this feature specified "10 consecutive failures",
 * which is 1 ms on the first loop and 20 ms on the second: both transients, and
 * escalating on either would fan a synthetic SW_FAIL to every SDM and demand a
 * manual release for a hiccup. Each loop converts this duration against its own
 * period instead (NvPSSFusionFailureStreakThreshold).
 *
 * The default is one second of uninterrupted failure. That is unambiguously
 * sustained on a loop iterating every 0.1-2 ms, and it still escalates well
 * inside the 5000 ms status/no-op interval and the 7000 ms SDM freshness
 * timeout -- so PSS names the cause before an SDM would otherwise infer one
 * from silence. Expressing it as time also survives a change to either period
 * constant, which an iteration count would silently invalidate.
 */
static constexpr uint32_t NVPSS_FUSION_FAILURE_ERROR_MS_MIN = 100U;
static constexpr uint32_t NVPSS_FUSION_FAILURE_ERROR_MS_MAX = 60000U;
static constexpr uint32_t NVPSS_FUSION_FAILURE_ERROR_MS_DEFAULT = 1000U;

/* Never escalate on a single failure, whatever the arithmetic below produces.
 * The acceptance criterion "a transient does not escalate" is then a property of
 * the code rather than of the configured value. */
static constexpr uint32_t NVPSS_FUSION_FAILURE_STREAK_MIN = 2U;

/*
 * Consecutive failures on a loop of `periodUs` that amount to `errorMs` of
 * uninterrupted failure. Saturates rather than wrapping, and never returns less
 * than NVPSS_FUSION_FAILURE_STREAK_MIN.
 */
static inline uint32_t NvPSSFusionFailureStreakThreshold(uint32_t errorMs, uint64_t periodUs)
{
    if (periodUs == 0ULL)
        return NVPSS_FUSION_FAILURE_STREAK_MIN;

    const uint64_t iterations = (static_cast<uint64_t>(errorMs) * 1000ULL) / periodUs;
    if (iterations < static_cast<uint64_t>(NVPSS_FUSION_FAILURE_STREAK_MIN))
        return NVPSS_FUSION_FAILURE_STREAK_MIN;
    if (iterations > static_cast<uint64_t>(UINT32_MAX))
        return UINT32_MAX;
    return static_cast<uint32_t>(iterations);
}

static inline bool NvPSSFusionFailureErrorMsIsValid(uint32_t errorMs)
{
    return errorMs >= NVPSS_FUSION_FAILURE_ERROR_MS_MIN &&
           errorMs <= NVPSS_FUSION_FAILURE_ERROR_MS_MAX;
}

/*
 * The fusion-health fault: raised when any stage has failed without
 * interruption for its threshold, cleared only when EVERY stage is healthy.
 *
 * The stages run on two threads, the input loop and the fusion loop. The
 * per-iteration streak counters stay lock-free. The two EDGE decisions do not:
 * "raise" and "every stage is healthy, so clear" are taken under one mutex.
 * Without it, the fusion thread could pass its all-healthy check, stall, and
 * then clear a flag that the input thread had meanwhile found already set -- and
 * had therefore not raised again. The fault would be gone while a stage was past
 * its threshold. Both edges are rare, so the mutex costs nothing on the
 * per-iteration path.
 */
template <size_t N>
class NvPSSFusionHealthLatch
{
public:
    /* One failure of `stage`. Returns true on the rising edge only; *outStreak
     * receives the stage's consecutive-failure count. */
    bool noteFailure(size_t stage, uint32_t threshold, uint32_t* outStreak = nullptr)
    {
        const uint32_t streak = streak_[stage].fetch_add(1U, std::memory_order_relaxed) + 1U;
        if (outStreak != nullptr)
            *outStreak = streak;
        if (streak < threshold)
            return false;
        std::lock_guard<std::mutex> lock(edgeMutex_);
        return !error_.exchange(true, std::memory_order_acq_rel);
    }

    /* One success of `stage`. Returns true when this success cleared the fault.
     * `betweenCheckAndClear` runs inside the edge lock, after the all-healthy
     * check and before the clear; it exists so a unit test can pause exactly
     * there and prove the other thread cannot interleave. */
    template <typename Hook>
    bool noteSuccess(size_t stage, Hook&& betweenCheckAndClear)
    {
        /* Cheap common path: a stage that was already healthy costs one load. */
        if (streak_[stage].load(std::memory_order_relaxed) == 0U)
            return false;
        streak_[stage].store(0U, std::memory_order_relaxed);
        if (!error_.load(std::memory_order_acquire))
            return false;

        std::lock_guard<std::mutex> lock(edgeMutex_);
        /* EVERY stage must be healthy, not just this one: one stage recovering
         * while another is still failing is not a recovered pipeline. */
        for (size_t i = 0U; i < N; ++i)
        {
            if (streak_[i].load(std::memory_order_relaxed) != 0U)
                return false;
        }
        betweenCheckAndClear();
        return error_.exchange(false, std::memory_order_acq_rel);
    }

    bool noteSuccess(size_t stage) { return noteSuccess(stage, [] {}); }

    bool active() const { return error_.load(std::memory_order_acquire); }
    uint32_t streak(size_t stage) const { return streak_[stage].load(std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> streak_[N]{};
    std::atomic<bool>     error_{false};
    std::mutex            edgeMutex_;
};

/*
 * AUDIT RECORD for a failing fusion stage -- bounded, and separate from the
 * escalation streak.
 *
 * Recording every failure put ~500 records/s into syslog on the 2 ms fusion
 * loop. journald's default per-service budget (10000 per 30 s) is gone in ~20 s,
 * after which journald drops EVERY record from the daemon for the rest of the
 * window -- including unrelated audit records. The record meant to make a
 * failure visible would have hidden the others.
 *
 * "One record per failure episode" is not enough on its own: a stage that
 * alternates fail/succeed every iteration opens a new episode every iteration
 * and floods just the same. So the episode has HYSTERESIS: it opens on the
 * first failure and closes only after NVPSS_FUSION_STAGE_RECORD_INTERVAL_MS
 * with no failure at all. While it is open, at most one progress record is
 * due per interval. Per stage that is at most one record per interval plus the
 * open and close records, whatever the failure pattern, and the close record
 * carries the exact failure count, so nothing is lost.
 *
 * This governs ONLY what is written to the log. The escalation still counts
 * consecutive failures per iteration (NvPSSFusionFailureStreakThreshold) and is
 * unaffected: PSS_FUSION_HEALTH_ERROR / _RECOVERED keep their own edges, and
 * HEALTH_RECOVERED normally precedes the stage's close record by one interval.
 *
 * Owned by the thread that runs the stage; not shared, hence not atomic.
 */
static constexpr uint32_t NVPSS_FUSION_STAGE_RECORD_INTERVAL_MS = 1000U;

enum class NvPSSFusionStageRecord : uint8_t
{
    NONE = 0U,
    STARTED,   /* first failure of an episode: record now */
    ONGOING,   /* episode still failing, one interval since the last record */
    ENDED      /* a full interval with no failure: record the episode's total */
};

struct NvPSSFusionStageEpisode
{
    bool     open{false};
    uint64_t openedMs{0U};
    uint64_t lastFailureMs{0U};
    uint64_t lastRecordMs{0U};
    uint32_t failures{0U};   /* in the current (or just-ended) episode; saturates */
};

/* Call on every failure of the stage. */
static inline NvPSSFusionStageRecord NvPSSFusionStageNoteFailure(NvPSSFusionStageEpisode* e,
                                                                 uint64_t nowMs,
                                                                 uint32_t intervalMs)
{
    e->lastFailureMs = nowMs;
    if (!e->open)
    {
        e->open = true;
        e->openedMs = nowMs;
        e->lastRecordMs = nowMs;
        e->failures = 1U;
        return NvPSSFusionStageRecord::STARTED;
    }
    if (e->failures < UINT32_MAX)
        e->failures++;
    if (nowMs - e->lastRecordMs >= intervalMs)
    {
        e->lastRecordMs = nowMs;
        return NvPSSFusionStageRecord::ONGOING;
    }
    return NvPSSFusionStageRecord::NONE;
}

/* Call on every success of the stage. Cheap when no episode is open. */
static inline NvPSSFusionStageRecord NvPSSFusionStageNoteSuccess(NvPSSFusionStageEpisode* e,
                                                                 uint64_t nowMs,
                                                                 uint32_t intervalMs)
{
    if (!e->open || nowMs - e->lastFailureMs < intervalMs)
        return NvPSSFusionStageRecord::NONE;
    e->open = false;   /* failures/openedMs/lastFailureMs stay readable for the close record */
    return NvPSSFusionStageRecord::ENDED;
}

namespace nvpss
{
class NvPSSDRPC;

class SafetyEventManager
{
public:
    SafetyEventManager(uint64_t inputSafetyEventQuePeriod, uint64_t fusionEventPeriod,
                       NvPSFTransportKind PSSDToPSDComBackend,
                       uint32_t pssToPsdRetryBudget,
                       uint32_t pssToPsdResponseTimeoutMs,
                       uint32_t statusNoopIntervalMs,
                       uint32_t psdOperationalQueueDepth =
                           NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_DEFAULT,
                       uint32_t psdCriticalQueueDepth =
                           NVPSS_PSD_CRITICAL_QUEUE_DEPTH_DEFAULT,
                       uint32_t fusionFailureErrorMs =
                           NVPSS_FUSION_FAILURE_ERROR_MS_DEFAULT);
    ~SafetyEventManager();

    NvPSSDErr StartSafetyEventManager();
    NvPSSDErr StopSafetyEventManager();

    /**
     * @brief Enable or disable the fusion functionality
     * @param enable True to enable fusion, false to disable
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr EnableFusion(bool enable);

    /**
     * @brief Configure multi-camera fusion parameters
     * @param maxSensors Maximum number of sensors to support
     * @param timeWindowSize Time window for event correlation
     * @param fusionThreshold Minimum similarity threshold for fusion
     * @param temporalW Weightage of Temporal Similarity for fusion
     * @param spatialW Weightage of Spatial Similarity for fusion
     * @param attributeW Weightage of Attribute Similarity for fusion
     * @param temporalT Temporal tolerance in milliseconds
     * @param trajectoryCount Number of trajectory points for spatial correlation
     * @param earlyTermThreshold Confidence threshold for events accepted as bestMatch
     * @param enableEarlyTerm Flasg to enable/disable skipping futher fusion matches if current match crosses threshold
     * @return NvPSSDErr indicating success or failure
     */
    /**
     * @brief Configure the event stream: pipeline bound and staleness threshold.
     *
     * Independent of fusion and required on every startup path. Must be called
     * before ConfigureMultiCameraFusion().
     */
    NvPSSDErr ConfigureEventStream(std::chrono::milliseconds timeWindowSize,
                                   uint8_t maxPipelines);

    NvPSSDErr ConfigureMultiCameraFusion(uint8_t maxSensors,
                                        std::chrono::milliseconds timeWindowSize,
                                        float fusionThreshold,
                                        float temporalW,
                                        float spatialW,
                                        float attributeW,
                                        std::chrono::milliseconds temporalT,
                                        uint8_t trajectoryCount,
                                        float earlyTermThreshold,
                                        bool enableEarlyTerm);

    /**
     * @brief Process a safety event through the fusion algorithm
     * @param event The safety event to process
     * @return NvPSSDErr indicating success or failure
     */
    /**
     * @brief Validate producer identity for a TRUSTED event, in either mode.
     *
     * Callers must invoke this AFTER the trust check and BEFORE the
     * fusion/bypass branch. Untrusted events bypass it deliberately: their
     * delivery is the fault report.
     */
    bool ValidateEventSource(const SafetyEvent& event) const;

    NvPSSDErr ProcessSafetyEventForFusion(const SafetyEvent& event);

    /**
     * @brief Get the list of fused events
     * @return Vector of FusedSafetyEvent objects
     */
    std::vector<FusedSafetyEvent> GetFusedEvents() const;

    /**
     * @brief Check if fusion is enabled
     * @return True if fusion is enabled, false otherwise
     */
    bool IsFusionEnabled() const;

    /**
     * @brief Start Fusion Process
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr StartFusionProcessing();

    /**
     * @brief Stop Fusion process
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr StopFusionProcessing();

    /**
     *  @brief Create list of SafetyEVents that bypass Fusion
     */
    void SetBypassFusionEvents(const std::unordered_set<EventType>& set);

    /**
     * @brief Load sensor configuration for pipelineID validation.
     * Events with a pipelineID not in this map (and pipelineID != 0) are rejected.
     */
    void SetSensorConfig(const std::unordered_map<uint8_t, std::string>& pipelineIdToName);
    /**
     * @brief Is @a pipelineId a source whose loss the SDMs latch on?
     *
     * PSS DOES NOT HOLD THE REVIEWED SENSOR SET. That set lives in
     * sensor_pipelines_config.conf, which the SDMs read; the PSS daemon has
     * never read it and giving it a second
     * consumer would move an SDM-owned ownership boundary. This reconstructs the
     * same membership from configuration PSS does hold:
     *
     *   2D  the reviewed set is the cameras, which is exactly sensor_config,
     *       so any configured pipelineId qualifies.
     *   3D  the reviewed set is the BEV group ALONE. sensor_config additionally
     *       carries the member cameras, so membership there is NOT sufficient --
     *       only the group id qualifies.
     *
     * The 3D branch is the conservative reading and is deliberate: a member
     * camera degrading is resolved into the group's three-valued verdict before
     * the SDM sees anything, so escalating separately for a camera would bypass
     * the routing 3D is built around and latch every SDM on evidence the design
     * intends to deliver as a group verdict.
     */
    bool IsRequiredSourcePipeline(uint8_t pipelineId) const;

    /*
     * Declare the BEV sensor group and its member cameras (3D only).
     *
     * Membership is kept SEPARATE from sensorConfigIdToName_ on purpose. That
     * map answers "is this a configured identity?" for ValidateEventSource, and
     * a group is a valid identity like any other. This answers "which cameras
     * does this group's health derive from?", which only group evaluation asks.
     * Merging the two would make a group indistinguishable from a camera at the
     * point where that distinction decides how health is computed.
     */
    void SetSensorGroup(uint8_t groupPipelineId, const std::vector<uint8_t>& memberPipelineIds);

    /*
     * A BEV group's health has THREE states, mirroring what the SDMs have always
     * done per sensor (ProximityControl.cpp:423-440 and its three peers):
     *
     *   Trusted    at least one member is genuinely healthy   -> SDM clears the bit
     *   Untrusted  all members down, none CAUSE_SAIM_INIT     -> SDM sets the bit, latches
     *   Pending    all members down, at least one SAIM_INIT   -> SDM bit UNCHANGED
     *
     * Pending is not a refinement -- it is the state that keeps the design safe.
     * A binary trusted/untrusted verdict has nowhere to put it, so it collapses
     * into Trusted, which emits SENSOR_VALID, which CLEARS the failed bit. A SAIM
     * restart while every camera is genuinely down would then unlatch a safe
     * state and permit a validated release with no camera delivering frames.
     * That is gap GC-B1, and Pending is its fix.
     */
    enum class GroupVerdict { Trusted, Untrusted, Pending };

    /**
     * @brief Handle trust-report event (SENSOR_INVALID, SENSOR_VALID, AI_PIPELINE_INVALID, AI_PIPELINE_VALID).
     * Only Safety Monitor may send these events; others are rejected at RPC and here. Returns true if accepted.
     */
    bool OnTrustReport(uint32_t rpcClientId, uint8_t reporterClientType, const SafetyEvent& event);

    /** Non-owning pointer; guarded by rpcOperationalModeMutex_. Call SetRpcForOperationalMode(nullptr) before destroying the RPC (msgListener) so in-flight DecisionRequest mode queries finish first. */
    void SetRpcForOperationalMode(NvPSSDRPC* rpc);

    /* pipelineID = sensor producing data; clientID = AI inference pipeline (see pss_protocol.h).
     * Single lock for both checks to avoid TOCTOU race with OnTrustReport. */
    struct TrustState { bool sensorInvalid; bool aiPipelineInvalid; };

    /**
     * @brief One entry in a PSS -> PSD staging queue.
     *
     * Carries an enqueue sequence number alongside the event because the
     * operational queue is kept ordered by event timestamp, and timestamp alone
     * is not a total order here: perception reports per frame, so every event
     * from one frame carries the SAME timestamp -- measured on IGX, a mean of
     * 4.66 events and up to 19 per frame instant. seq breaks those ties so the
     * ordering is stable and a re-run over the same input drains in the same
     * order. Their relative order carries no meaning (they are simultaneous
     * observations); reproducibility is the point.
     */
    struct PsdQueuedEvent
    {
        int clientId{-1};
        uint64_t seq{0U};
        FusedSafetyEvent event{};
#ifdef NVPSF_DBG
        /* CLOCK_MONOTONIC ns at admission, used only to report how long this
         * entry waited behind the release gate. Distinct from event.timestamp,
         * which is the PRODUCER's stamp and so already carries upstream age:
         * only the difference between admission and release attributes latency
         * to this queue. Debug-only; nothing in the send path reads it. */
        uint64_t enqueueNs{0U};
#endif
        /* The trust the producer routed this event by. A distrust recorded here
         * is kept: the build stamps isHealthy / isTrustedSource from the trust
         * state at build AND this, so it can lower a stamp but never raise it.
         * An event observed while its source was distrusted is therefore
         * delivered distrusted even if the source recovers before it is sent --
         * it was produced while the source was declared invalid. */
        TrustState queuedTrust{false, false};
        /* Critical lane only: may be evicted to admit a must-deliver report
         * (NvPSSPsdCriticalLaneVictim). Set by the producer from the same trust
         * snapshot as queuedTrust, so an evictable entry is one every SDM will
         * skip whatever trust does afterwards. */
        bool evictable{false};
    };

    /* getCriticalPrioQueRef()/getOperationalPrioQueRef() are gone: they had no
     * caller in the tree and handed out unsynchronised references to deques
     * that are only safe to touch under criticalPrioQueMutex /
     * operationalPrioQueMutex. Anything needing staging state should go through
     * hasQueuedEvents() or the counters, which take the locks. */
    std::deque<std::pair<int, SafetyEvent>>& getInputSafetyEventQueRef();

    /* Mutex accessors for shared queues */
    std::mutex& getInputSafetyEventQueMutexRef();

    /*PSD Socket communication from PSS Daemon*/
    NvPSSDErr initializePSSDServer();
    NvPSSDErr startPSSDServer();
    NvPSSDErr stopPSSDServer();

private:
    /**
     * @brief Identifies the entries copied into one in-flight DecisionRequest.
     *
     * Positions would not do. The operational queue is ordered by event
     * timestamp, and fusion can insert an event that is up to timeWindowSize
     * old (see insertOperationalOrdered), so an entry can appear IN FRONT of a
     * batch while that batch is on the wire. Committing "the first N" would
     * then erase the newcomer, which was never sent, and leave behind one that
     * was -- losing an event and duplicating another. Sequence numbers name the
     * exact entries instead, and they are unique and never reused.
     */
    struct PsdBatchTicket
    {
        uint64_t seq[MAX_SENSORS_DATA_SUMMARY_SIZE]{};
        uint8_t  count{0U};
        /*
         * The DecisionRequest id this batch has already been sent under, or 0
         * if it has not been sent yet.
         *
         * Carried on the ticket so a RETRY reuses it. Without this the outer
         * retry allocates a fresh id every pass: buildDecisionRequestFromQueuedEvents()
         * starts from *request = {}, which zeroes requestId, and
         * sendDecisionRequestWithRetry() then treats the batch as new and
         * assigns the next id -- it keeps one id stable across its OWN
         * attempts, but cannot see across a rebuild.
         *
         * That defeats deduplication at PSD for the one case retention adds:
         * a request that ARRIVED and was acted on, whose response was then
         * lost. Retried under a new id, the same safety events are
         * indistinguishable from a genuine second observation. Reusing the id
         * makes the retry recognisable as the same request.
         */
        uint32_t requestId{0U};
        /* Sends already attempted for this batch, bounded by
         * NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS. */
        uint32_t attempts{0U};
        /*
         * The pssStatus this batch's request id was FIRST sent with, and the
         * overflow generation that stamp carried. Every retry reuses both, so
         * every transmission of one request id is the same request.
         *
         * PSD answers a request id it has already seen as a duplicate --
         * acknowledged, not forwarded. A retry re-stamped ERROR whose NORMAL
         * first transmission had been forwarded would be acknowledged with no
         * SDM having seen ERROR, and PSS would credit it. With the stamp frozen,
         * whichever copy PSD forwarded carried this stamp, so an acknowledgement
         * proves it reached the SDMs. An ERROR raised meanwhile goes out on the
         * next new request id.
         */
        bool         stamped{false};
        SystemStatus stamp{};
        uint64_t     stampGeneration{0U};
    };

    /** True when both tickets name the same staged entries, i.e. the batch just
     *  built is a retry of the one that failed rather than a new batch. */
    static bool psdBatchIdentityMatches(const PsdBatchTicket& a, const PsdBatchTicket& b);

    NvPSSDErr managePsdSender();
    NvPSSDErr manageInputSafetyEventQue();
    bool buildDecisionRequestFromQueuedEvents(NvPSSPsdSendPriority priority,
                                              DecisionRequest* request,
                                              PsdBatchTicket* ticket);
    bool hasQueuedEvents(NvPSSPsdSendPriority priority);
    bool enqueueStatusNoopEvent(uint64_t timestampNs);
    /* queuedTrust: the trust snapshot the caller routed the event by, and from
     * which it derived evictable (see PsdQueuedEvent). */
    bool enqueuePsdFusedEvent(NvPSSPsdSendPriority priority,
                              int clientId,
                              const FusedSafetyEvent& event,
                              bool evictable = false,
                              TrustState queuedTrust = TrustState{false, false});
    void notifyPsdSenderForQueuedEvent();

    /**
     * @brief Handle fused events by adding them to the appropriate priority queue
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr HandleFusedEvents();
    void fusionProcessingLoop();

    FusedSafetyEvent CreateBypassEvent(const SafetyEvent& event) const;
    FusedSafetyEvent CreateInvalidSourceEvent(const SafetyEvent& event) const;

    /** TWO-SIDED freshness: true when the event's CLOCK_MONOTONIC timestamp is
     *  older than stalenessThresholdMs_ (timeWindowSize) OR more than that far
     *  in the FUTURE. Applies in both fusion modes. Reads the clock, and logs a
     *  warning when the timestamp is in the future. */
    bool isEventStale(uint64_t eventTimestampNs) const;
    /** The same two-sided rule against a pre-computed monotonic now
     *  (milliseconds). Pure: no clock read and no logging. */
    bool isEventStale(uint64_t eventTimestampNs, uint64_t nowMs) const;

    /* TrustState is declared with PsdQueuedEvent, which carries one. */
    TrustState QueryTrustState(uint8_t pipelineId, uint8_t clientId) const;

    /* overflowGeneration, if given, receives the critical-lane overflow
     * generation the stamp was decided from (0: not raised). */
    SystemStatus makePssStatusForDecisionRequest(uint64_t* overflowGeneration = nullptr) const;
    /* Clear the critical-lane overflow once it has been delivered and drained. */
    void maybeClearCriticalLaneOverflow();
    /* Overflow generation stamped into the last send attempt. Sender thread only. */
    uint64_t lastSendOverflowGeneration_{0U};
    OperationalMode decisionRequestOperationalMode() const;
    /* The two presence-report words carried on every status no-op, sampled when
     * a request is BUILT: present now with the input-unusable set above it, and
     * expected with the input-confirmed set above it (NvPSSPackPresenceWord). */
    void sourcePresenceSnapshot(uint32_t* presentMask, uint32_t* expectedMask) const;
    /* ticket, if given, carries the request id's first-transmission stamp: set on
     * the first attempt, reused by every later one (see PsdBatchTicket::stamp). */
    NvPSSDErr sendDecisionRequestWithRetry(DecisionRequest* request,
                                           DecisionResponse* response,
                                           const char* requestClass,
                                           PsdBatchTicket* ticket = nullptr);
    void markDeliveryFailure(const DecisionRequest& request,
                             const char* requestClass,
                             NvPSSDeliveryFailureReason reason,
                             uint32_t attemptCount,
                             uint32_t responseTimeoutMs);
    void markDeliveryRecovered(const DecisionRequest& request, const char* requestClass);

    /**
     * @brief Insert into the operational queue at its timestamp-ordered position.
     *
     * Called with operationalPrioQueMutex held. Scans back-to-front, which is
     * O(1) in the ordinary case: events reach this point in roughly
     * non-decreasing timestamp order, so the insertion point is almost always
     * the back. The scan exists for the case that is NOT ordinary and is the
     * reason the queue is sorted at all -- fusion releases an unmatched event
     * as a passthrough only once it is older than the correlation window, and
     * CreatePassThroughEvent keeps the ORIGINAL timestamp, so such an event
     * arrives up to timeWindowSize (400 ms as shipped) behind its neighbours.
     */
    void insertOperationalOrdered(const PsdQueuedEvent& entry);

    /**
     * @brief Remove exactly the entries named by @p ticket.
     *
     * The commit half of the peek-then-commit hand-off in
     * buildDecisionRequestFromQueuedEvents(): called only once PSD has
     * acknowledged the request, so a failed send leaves the batch queued for
     * the next attempt instead of destroying it.
     */
    void commitQueuedEvents(NvPSSPsdSendPriority priority, const PsdBatchTicket& ticket);

    /**
     * @brief Abandon a batch that has exhausted NVPSS_PSD_BATCH_MAX_DELIVERY_ATTEMPTS.
     *
     * The bound on retention. Removes the entries and reports the loss at
     * ERR with the same exactness as the enqueue-drop path, because it is the
     * same kind of event: evidence that reached PSS and never became evidence
     * for a decision. Called only from the sender, so the entries are not
     * in flight anywhere else.
     */
    void dropUndeliverableBatch(NvPSSPsdSendPriority priority, const PsdBatchTicket& ticket);
    /** PSD acknowledged deliveredRequestId: if batches were abandoned since the
     *  last acknowledgement, write PSD_STAGING_DELIVERY_RESUMED naming it and
     *  what was lost. Returns the record's detail, empty if nothing was. */
    std::string reportDeliveryResumed(uint32_t deliveredRequestId);
    /** PSD acknowledged the batch sent as requestId: credit its first
     *  transmission's overflow stamp, record a resumed delivery, commit it. */
    void onPsdRequestAcknowledged(NvPSSPsdSendPriority priority, const PsdBatchTicket& ticket,
                                  uint32_t requestId);

    /** Report a refused enqueue: exact counters, rate-limited warning. */
    void recordCriticalLaneEviction(const FusedSafetyEvent& evicted, const FusedSafetyEvent& admitted);
    void recordPsdEnqueueDrop(NvPSSPsdSendPriority priority,
                              const FusedSafetyEvent& event,
                              size_t depth,
                              size_t capacity);

    mutable std::mutex rpcOperationalModeMutex_;
    NvPSSDRPC*         rpcForOperationalMode_{nullptr};

    /*
     * Fusion-stage health, the third source of PSS ERROR alongside PSD delivery
     * state and the SAIM heartbeat.
     *
     * One streak per instrumented stage, reset by that stage's next success, so
     * the counts mean CONSECUTIVE failures. A stage that fails intermittently
     * never accumulates, which is deliberate: the fault this escalates is a
     * stage that has stopped working, not one that is occasionally unlucky.
     */
    enum FusionStage : size_t
    {
        FUSION_STAGE_PROCESS_EVENT = 0U,  /* ProcessSafetyEventForFusion, input-queue loop */
        FUSION_STAGE_PERFORM_FUSION,      /* PerformSafetyEventFusion,    fusion loop */
        FUSION_STAGE_PROCESS_UNMATCHED,   /* ProcessUnmatchedEvents,      fusion loop */
        FUSION_STAGE_CLEAN_PROCESSED,     /* CleanProcessedEvents,        fusion loop */
        FUSION_STAGE_HANDLE_FUSED,        /* HandleFusedEvents,           fusion loop */
        FUSION_STAGE_COUNT
    };

    static const char* fusionStageName(FusionStage stage);
    /* Per-stage threshold: the two loops have different periods, so the same
     * configured duration is a different number of iterations on each. */
    uint32_t fusionStageStreakThreshold(FusionStage stage) const;
    /* Returns true on the rising edge only, so the caller logs once. */
    bool bumpFusionStageFailure(FusionStage stage);
    void clearFusionStageFailure(FusionStage stage);

    /* Per-stage failure streaks and the fusion-health fault they raise. */
    NvPSSFusionHealthLatch<FUSION_STAGE_COUNT> fusionHealth_;

    /* Audit-record episodes, one per stage (see NvPSSFusionStageEpisode). Each
     * is touched only by the loop that runs its stage. */
    NvPSSFusionStageEpisode fusionStageEpisode_[FUSION_STAGE_COUNT]{};
    /* Record the failure if its episode says a record is due. `detail` is built
     * by the caller only when this returns true, so the common suppressed path
     * does no string work. */
    bool fusionStageFailureRecordDue(FusionStage stage, NvPSSFusionStageRecord* outKind);
    void writeFusionStageFailureRecord(FusionStage stage, NvPSSFusionStageRecord kind,
                                       const std::string& detail);
    /* fusionFailureErrorMs_ is declared with the other constructor-configured
     * members further down, not here, so that declaration order matches the
     * constructor's initializer list. Members initialise in DECLARATION order
     * whatever the list says, and -Wreorder rightly flags a list that implies
     * otherwise. */

    std::deque<PsdQueuedEvent> criticalPrioQue;
    std::deque<PsdQueuedEvent> operationalPrioQue;
    std::deque<std::pair<int,SafetyEvent>> inputSafetyEventQue;

    /* Configured depths of the two staging lanes. Both are sized against frame
     * fan-out; see NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_DEFAULT and
     * NVPSS_PSD_CRITICAL_QUEUE_DEPTH_DEFAULT. */
    const uint32_t psdOperationalQueueDepth_;
    const uint32_t psdCriticalQueueDepth_;
    /* Monotonic enqueue counter, the tie-break for equal timestamps. Shared by
     * both lanes so an event's admission order is unambiguous regardless of
     * which lane it took. */
    std::atomic<uint64_t> psdEnqueueSeq_{0U};

    /* Overflow accounting for the staging queues.
     *
     * enqueuePsdFusedEvent() is the only point at which a fused event can be
     * discarded on its way to PSD, and every call site casts its return value
     * away. That made the loss unmeasurable from outside: nothing counted it
     * and nothing logged it, so the rate had to be inferred by correlating PSS
     * logs against emitted commands -- which cannot separate a drop from
     * ordinary fusion coalescing. Kept in release builds: a full staging queue
     * means the safety pipeline is shedding observations. */
    std::atomic<uint64_t> psdEnqueueAcceptedCount_{0U};
    std::atomic<uint64_t> psdEnqueueDroppedCount_{0U};
    /* Monotonic ns of the last emitted overflow warning; rate-limits the log. */
    std::atomic<uint64_t> psdEnqueueDropWarnNs_{0U};
    /* High-water marks of the two staging lanes. The number that says whether
     * the configured depth is right for the deployment's scene: it is the
     * observed frame fan-out, which is the quantity the depth has to cover.
     * Tracked per lane because the critical lane's fan-out appears only while
     * a source is untrusted or the mode is ERROR, so it cannot be inferred
     * from the operational mark. */
    std::atomic<uint64_t> psdOperationalHighWater_{0U};
    std::atomic<uint64_t> psdCriticalHighWater_{0U};
    /* Events abandoned by dropUndeliverableBatch(). Distinct from
     * psdEnqueueDroppedCount_: these were accepted into a staging queue and
     * lost on the way OUT, which points at the link rather than at depth. */
    std::atomic<uint64_t> psdUndeliverableDroppedCount_{0U};
    /* Abandoned batches since PSD last acknowledged a request. Sender thread only. */
    NvPSSPsdUndeliverableLedger psdUndeliverableLedger_;
    /* Evictable critical-lane entries removed to admit a must-deliver report,
     * and the monotonic ns of the last eviction record (rate-limited). */
    std::atomic<uint64_t> psdCriticalEvictedCount_{0U};
    std::atomic<uint64_t> psdCriticalEvictWarnNs_{0U};
    /* Raised when a must-deliver report is refused because the critical lane
     * holds nothing evictable; every DecisionRequest is stamped ERROR while it
     * stands. Cleared when the lane drains to half its depth. */
    NvPSSPsdCriticalOverflowLatch criticalLaneOverflow_;
    /* Refused must-deliver reports awaiting re-admission to the critical lane,
     * and the highest sequence re-admitted while the overflow stands. Both under
     * criticalPrioQueMutex. See NvPSSPsdRetainRefused(). */
    std::deque<PsdQueuedEvent> psdRefusedPending_;
    uint64_t psdRefusedReadmittedSeqMax_{0U};
    /* Refused reports the full pending list could not keep. */
    std::atomic<uint64_t> psdRefusedLostCount_{0U};

    // Mutexes for thread-safe queue access
    std::mutex criticalPrioQueMutex;
    std::mutex operationalPrioQueMutex;
    std::mutex inputSafetyEventQueMutex;

    const std::chrono::microseconds inputSafetyEventQuePeriod;
    const std::chrono::microseconds fusionEventPeriod;

    NvPSFTransportKind PSSDToPSDComBackend;

    std::thread psdSenderMonitor;
    std::thread inputSafetyEventQueMonitor;
    std::atomic<bool> queMonitorsRunning;

    std::unique_ptr<SafetyEventFusion> eventFusion;  /* Fusion algorithm implementation */
    bool fusionEnabled;  /* Flag indicating if fusion is enabled */
    std::thread fusionProcessorThread;
    std::atomic<bool> fusionProcessorRunning;

    /*  Store bypass Fusion Event List in SafetyEventManager */
    std::unordered_set<EventType> bypassFusionEvents;

    /* Multi-camera fusion configuration */
    uint8_t maxPipelinesSupported;
    std::unordered_map<uint8_t, bool> registeredPipelines;

    /* Sensor config: pipelineId -> sensorName for event validation.
     * Empty means no sensor config was loaded (validation skipped). */
    std::unordered_map<uint8_t, std::string> sensorConfigIdToName_;
    /* 3D only. Empty in 2D, which is what makes every group-aware path inert
     * there rather than conditional on a mode flag. */
    uint8_t              sensorGroupPipelineId_{0U};
    std::vector<uint8_t> sensorGroupMembers_{};
    /* Last verdict EMITTED downstream. The verdict itself is recomputed from
     * member state on every query (level-triggered), so nothing here can go
     * stale; this records only what the SDMs were last told, so a transition is
     * emitted once rather than per report.
     *
     * NEVER set to Pending. Pending emits nothing, so the SDMs' view does not
     * change when the group enters it -- and on leaving it, the right question
     * is whether the new verdict differs from what they were last TOLD, not from
     * Pending. Untrusted -> Pending -> Untrusted must stay silent; Untrusted ->
     * Pending -> Trusted must emit.
     *
     * Initialised to Trusted because that is the initial DERIVED state: with an
     * empty trust map no member is invalid, so the first evaluation yields
     * Trusted and must not read as a transition. An earlier version initialised
     * this to Pending and emitted a spurious SENSOR_VALID on the first member
     * report, before anything was known about the group. */
    GroupVerdict         lastEmittedGroupVerdict_{GroupVerdict::Trusted};
    /* Last verdict COMPUTED, for detecting entry to and exit from Pending so the
     * warning fires on the edges rather than on every report. */
    GroupVerdict         lastGroupVerdict_{GroupVerdict::Trusted};

    /* Require trustStateMutex held. */
    GroupVerdict EvaluateGroupVerdictLocked() const;
    bool IsSensorGroupLocked(uint8_t pipelineId) const;

    /*
     * Same question as IsSensorGroupLocked, asked from a context that does NOT
     * hold trustStateMutex.
     *
     * Safe without the lock because the group configuration is startup-only: it
     * is written once by SetSensorGroup during daemon startup, before any
     * listener, fusion or RPC thread exists, and never again. That is the same
     * basis on which ValidateEventSource already reads sensorConfigIdToName_
     * lock-free.
     *
     * It exists as a separate name rather than by calling the *Locked variant
     * from an unlocked caller, so neither contract has to be read as advisory.
     */
    bool IsSensorGroupId(uint8_t pipelineId) const;


    /* Synthesise the group's own SENSOR_INVALID / SENSOR_VALID. Must be called
     * with trustStateMutex RELEASED: it takes the input-queue mutex, and taking
     * the two in this order in one place and the reverse elsewhere is how a
     * deadlock is built. */
    void EmitGroupTrustEvent(EventType type, uint8_t groupId, const std::string& groupName);

    /*
     * Re-send every INVALID verdict that stands, as daemon-originated trust
     * reports: AI_PIPELINE_INVALID; in 2D each invalid camera's SENSOR_INVALID with
     * the cause last recorded for it; in 3D the group's SENSOR_INVALID if the group
     * was last reported Untrusted. Called once per status interval by the sender.
     *
     * A monitor reports on change, and an SDM that restarts after the change
     * would otherwise never hear it. Only INVALID is re-sent: a restarted SDM
     * starts with nothing failed. Changes no trust state. Same lock discipline as
     * EmitGroupTrustEvent: snapshot under trustStateMutex, release, then enqueue.
     */
    void ReassertStandingVerdicts();
    /* What the last re-assertion covered, so its record is written on change
     * only. Sender thread only. */
    std::string lastReassertSummary_;
    bool sensorConfigLoaded_{false};
    /* Set by ConfigureEventStream(). ConfigureMultiCameraFusion() requires it.
     * A dedicated flag, not a sentinel value in maxPipelinesSupported: the
     * constructor initialises that member to 2, so a `== 0` test could never
     * detect a missing call -- it read as protection and provided none. */
    bool eventStreamConfigured_{false};

    /* Staleness threshold derived from timeWindowSize (milliseconds, CLOCK_MONOTONIC).
     * Events older than this are marked STALE rather than UNKNOWN/PASSTHROUGH. */
    uint64_t stalenessThresholdMs_{UINT64_MAX};

    /* Trust state: invalid sources excluded from fusion; events sent to PSD as UNKNOWN critical evidence.
     * invalidSensors is keyed by pipelineID (sensor). AI-pipeline trust is pipeline-wide
     * (one Perception Monitor verdict for the whole pipeline), so it is a single latch, not per-client.
     *
     * The VALUE is the cause that last reported the sensor invalid, and it exists
     * for BEV group evaluation only (3D). Per-camera trust is still pure
     * membership: QueryTrustState returns count(id) != 0 exactly as before, so a
     * camera reporting SENSOR_INVALID with CAUSE_SAIM_INIT remains invalid at the
     * daemon just as it is today.
     *
     * That asymmetry is deliberate and is worth stating, because the daemon and
     * the SDMs now answer "is this sensor failed?" differently on purpose:
     *   - the SDMs suppress CAUSE_SAIM_INIT when updating the failed-sensor mask
     *     (AVOID_SAFETY_LATCH_FOR_SAIM_INIT), so a boot-time seed does not latch;
     *   - the daemon records it as invalid regardless, because its job here is to
     *     keep an untrusted source out of fusion, which a pre-frame camera is.
     * Collapsing the two would change 2D behaviour, which INV-6 forbids. */
    mutable std::mutex trustStateMutex;
    std::unordered_map<uint8_t, EventCause> invalidSensors;  /* pipelineID -> cause that invalidated it */
    /* Pipeline-wide latch of AI-pipeline-invalid: set on AI_PIPELINE_INVALID, cleared on
     * AI_PIPELINE_VALID. Every MDX client's isTrustedSource derives from this flag, so a
     * late-connecting or reconnecting client always sees the current state (no stale ids). */
    bool aiPipelineInvalidGlobal{false};

    /* Monotonic id for DecisionRequest. Safety-supported PSD backends assign under
     * psdSendMtx_ immediately before the first send attempt so Gateway-visible ids
     * follow send order and retries keep the same logical id. */
    std::atomic<uint32_t> psdRequestId;
    std::atomic<NvPSSDeliveryState> psdDeliveryState_{NvPSSDeliveryState::NORMAL};
    const uint32_t pssToPsdRetryBudget_;
    const uint32_t pssToPsdResponseTimeoutMs_;
    const uint32_t statusNoopIntervalMs_;
    /* Grouped here with the other constructor-configured values; used by
     * fusionStageStreakThreshold() alongside the two loop periods. */
    const uint32_t fusionFailureErrorMs_;
    std::mutex psdSendMtx_;
    std::mutex psdSenderWakeMtx_;
    std::condition_variable psdSenderWakeCv_;
    std::atomic<uint32_t> psdSenderWakeSeq_{0U};
    std::mutex psdDeliveryAuditMtx_;
    NvPSSDeliveryFailureReason lastPsdDeliveryFailureReason_{NvPSSDeliveryFailureReason::NONE};

    std::unique_ptr<NvPSSDToPSDClient> pssdServer;
};
}
#endif // SAFETY_EVENT_MANAGER_HPP
