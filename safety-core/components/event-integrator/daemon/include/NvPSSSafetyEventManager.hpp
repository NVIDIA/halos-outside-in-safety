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
#include "NvPSD.h"
#include "NvPSSDToPSD.hpp"

#define MAX_EVENTS_PER_QUE 8
#define SEC_TO_NANO_SEC 1000000000L

typedef enum NvPSDChannelBackend_t
{
    POSIX_MSG_QUE=0,
    POSIX_SOCKET
}NvPSDChannelBackend;

static inline bool NvPSSStatusNoopEnabledForBackend(NvPSDChannelBackend backend)
{
    return backend == POSIX_MSG_QUE || backend == POSIX_SOCKET;
}

namespace nvpss
{
class NvPSSDRPC;

class SafetyEventManager
{
public:
    SafetyEventManager(uint64_t inputSafetyEventQuePeriod, uint64_t fusionEventPeriod,
                       NvPSDChannelBackend PSSDToPSDComBackend,
                       uint32_t pssToPsdRetryBudget,
                       uint32_t pssToPsdResponseTimeoutMs,
                       uint32_t statusNoopIntervalMs);
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
     * @brief Handle trust-report event (SENSOR_INVALID, SENSOR_VALID, AI_PIPELINE_INVALID, AI_PIPELINE_VALID).
     * Only Safety Monitor may send these events; others are rejected at RPC and here. Returns true if accepted.
     */
    bool OnTrustReport(uint32_t rpcClientId, uint8_t reporterClientType, const SafetyEvent& event);

    /** Non-owning pointer; guarded by rpcOperationalModeMutex_. Call SetRpcForOperationalMode(nullptr) before destroying the RPC (msgListener) so in-flight DecisionRequest mode queries finish first. */
    void SetRpcForOperationalMode(NvPSSDRPC* rpc);

    /*Get references of the queues*/
    std::deque<std::pair<int, FusedSafetyEvent>>& getCriticalPrioQueRef();
    std::deque<std::pair<int, FusedSafetyEvent>>& getOperationalPrioQueRef();
    std::deque<std::pair<int, SafetyEvent>>& getInputSafetyEventQueRef();

    /* Mutex accessors for shared queues */
    std::mutex& getInputSafetyEventQueMutexRef();

    /*PSD Socket communication from PSS Daemon*/
    NvPSSDErr initializePSSDServer();
    NvPSSDErr startPSSDServer();
    NvPSSDErr stopPSSDServer();

private:
    NvPSSDErr managePsdSender();
    NvPSSDErr manageInputSafetyEventQue();
    bool buildDecisionRequestFromQueuedEvents(NvPSSPsdSendPriority priority,
                                              DecisionRequest* request);
    bool hasQueuedEvents(NvPSSPsdSendPriority priority);
    bool enqueueStatusNoopEvent(uint64_t timestampNs);
    bool enqueuePsdFusedEvent(NvPSSPsdSendPriority priority,
                              int clientId,
                              const FusedSafetyEvent& event);
    void notifyPsdSenderForQueuedEvent();

    /**
     * @brief Handle fused events by adding them to the appropriate priority queue
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr HandleFusedEvents();
    void fusionProcessingLoop();

    FusedSafetyEvent CreateBypassEvent(const SafetyEvent& event) const;
    FusedSafetyEvent CreateInvalidSourceEvent(const SafetyEvent& event) const;

    /** True when the event's monotonic timestamp is older than stalenessThresholdMs_. */
    bool isEventStale(uint64_t eventTimestampNs) const;
    /** Overload accepting a pre-computed monotonic now (milliseconds) to
     *  avoid repeated clock_gettime calls in batch loops. */
    bool isEventStale(uint64_t eventTimestampNs, uint64_t nowMs) const;

    /* pipelineID = sensor producing data; clientID = AI inference pipeline (see pss_protocol.h).
     * Single lock for both checks to avoid TOCTOU race with OnTrustReport. */
    struct TrustState { bool sensorInvalid; bool aiPipelineInvalid; };
    TrustState QueryTrustState(uint8_t pipelineId, uint8_t clientId) const;

    SystemStatus makePssStatusForDecisionRequest() const;
    OperationalMode decisionRequestOperationalMode() const;
    NvPSSDErr sendDecisionRequestWithRetry(DecisionRequest* request,
                                           DecisionResponse* response,
                                           const char* requestClass);
    void markDeliveryFailure(const DecisionRequest& request,
                             const char* requestClass,
                             NvPSSDeliveryFailureReason reason,
                             uint32_t attemptCount,
                             uint32_t responseTimeoutMs);
    void markDeliveryRecovered(const DecisionRequest& request, const char* requestClass);

    mutable std::mutex rpcOperationalModeMutex_;
    NvPSSDRPC*         rpcForOperationalMode_{nullptr};

    std::deque<std::pair<int, FusedSafetyEvent>> criticalPrioQue;
    std::deque<std::pair<int,FusedSafetyEvent>> operationalPrioQue;
    std::deque<std::pair<int,SafetyEvent>> inputSafetyEventQue;

    // Mutexes for thread-safe queue access
    std::mutex criticalPrioQueMutex;
    std::mutex operationalPrioQueMutex;
    std::mutex inputSafetyEventQueMutex;

    const std::chrono::microseconds inputSafetyEventQuePeriod;
    const std::chrono::microseconds fusionEventPeriod;

    NvPSDChannelBackend PSSDToPSDComBackend;

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
    bool sensorConfigLoaded_{false};

    /* Staleness threshold derived from timeWindowSize (milliseconds, CLOCK_MONOTONIC).
     * Events older than this are marked STALE rather than UNKNOWN/PASSTHROUGH. */
    uint64_t stalenessThresholdMs_{UINT64_MAX};

    /* Trust state: invalid sources excluded from fusion; events sent to PSD as UNKNOWN critical evidence.
     * invalidSensors is keyed by pipelineID (sensor). AI-pipeline trust is pipeline-wide
     * (one PCM verdict for the whole pipeline), so it is a single latch, not per-client. */
    mutable std::mutex trustStateMutex;
    std::unordered_set<uint8_t> invalidSensors;      /* pipelineID = sensor producing data */
    /* Pipeline-wide latch of AI-pipeline-invalid: set on AI_PIPELINE_INVALID, cleared on
     * AI_PIPELINE_VALID. Every MDX client's isTrustedSource derives from this flag, so a
     * late-connecting or reconnecting client always sees the current state (no stale ids). */
    bool aiPipelineInvalidGlobal{false};

    /* Monotonic id for DecisionRequest. Safety-supported PSD backends assign under
     * psdSendMtx_ immediately before the first send attempt so Gateway-visible ids
     * follow send order and retries keep the same logical id. */
    std::atomic<uint32_t> psdRequestId;
    NvPSDCtx* psdCtx;
    std::atomic<NvPSSDeliveryState> psdDeliveryState_{NvPSSDeliveryState::NORMAL};
    const uint32_t pssToPsdRetryBudget_;
    const uint32_t pssToPsdResponseTimeoutMs_;
    const uint32_t statusNoopIntervalMs_;
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
