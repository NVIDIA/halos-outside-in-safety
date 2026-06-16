/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAFETY_EVENT_FUSION_HPP
#define SAFETY_EVENT_FUSION_HPP

#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <set>

#include "pss_protocol.h"
#include "pss_daemon.h"

namespace nvpss {

    // Structure to hold all similarity components and early termination info
    struct SimilarityResult {
        float temporalSim;
        float spatialSim;
        float attributeSim;
        float overallSim;
        bool earlyReject;  // True if should reject early

        SimilarityResult()
            : temporalSim(0.0f), spatialSim(0.0f), attributeSim(0.0f),
            overallSim(0.0f), earlyReject(false) {}
    };

class SafetyEventFusion {
public:
    /**
     * @brief Constructor for SafetyEventFusion
     * @param timeWindowSize Duration of the sliding time window for correlating events (in seconds)
     */
    SafetyEventFusion(uint64_t timeWindow, float fusionT, float temporalW,
                      float spatialW, float attributeW, uint64_t temporalT,
                      uint8_t trajectoryCount, uint8_t maxPipelines,
                      float earlyTermThreshold, bool enableEarlyTerm);

    /**
     * @brief Destructor for SafetyEventFusion
     */
    ~SafetyEventFusion();

    /**
     * @brief Process a new safety event from a pipeline
     * @param event The safety event to process
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr ProcessNewSafetyEvent(const SafetyEvent& event);

    /**
     * @brief Perform fusion of events from both pipelines
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr PerformSafetyEventFusion();

    /**
     * @brief Process events that remain unmatched after timeout
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr ProcessUnmatchedEvents();

    /**
     * @brief Get the list of fused events
     * @return Vector of FusedSafetyEvent objects
     */
    std::vector<FusedSafetyEvent> GetFusedEvents() const;

    /**
     * @brief Clear all event queues
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr ClearEventQueues();

    /**
     * @brief Clear fused events queue from start to endIndex
     * @param index Number of FusedEvents which have been pushed to next pipeline
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr ClearFusedEvents(uint8_t count);

    /**
     * @brief Clean up processed events from the queues
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr CleanProcessedEvents();

    /**
     * @brief Set the fusion similarity threshold
     * @param threshold Threshold value between 0.0 and 1.0
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetFusionThreshold(float threshold);

    /**
     * @brief Set the time window size for event correlation
     * @param windowSize Time window in milliseconds
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetTimeWindowSize(uint64_t windowSize);

    /**
     * @brief Set the time window size for event correlation
     * @param windowSize Time window in milliseconds
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetTemporalTolerance(uint64_t temporalT);

    /**
     * @brief Set the fusion weights
     * @param temporalW Weight value between 0.0 and 1.0
     * @param spatialW Weight value between 0.0 and 1.0
     * @param attributeW Weight value between 0.0 and 1.0
     * Note: Sum of all the weights should be 1.0
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetFusionWeights(float temporalW, float spatialW, float attributeW);

    /**
     * @brief Set the number of trajectory points for statial correlation
     * @param count Number of trajectory points
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetTrajectoryCount(uint8_t count);

    /**
     * @brief Set maximum number of pipelines supported
     * @param maxPipelines Maximum number of pipelines
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetMaxPipelines(uint8_t maxPipelines);

    /**
     * @brief Get current number of active pipelines
     * @return Number of pipelines that have reported events
     */
    uint8_t GetActivePipelineCount() const;

    /**
     * @brief Register a pipeline for health monitoring
     * @param pipelineId Unique pipeline identifier
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr RegisterPipeline(uint8_t pipelineId);

    /**
     * @brief Set early termination threshold for fusion matching
     * @param threshold Similarity score (0.0-1.0) above which search terminates early
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr SetEarlyTerminationThreshold(float threshold);

    /**
     * @brief Enable/disable early termination optimization
     * @param enable True to enable, false to disable
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr EnableEarlyTermination(bool enable);

private:
    /**
     * @brief Process safety events of a specific type
     * @param type The event type to process
     * @return NvPSSDErr indicating success or failure
     */
    NvPSSDErr ProcessSafetyEvents(EventType type);

    /**
     * @struct MatchResult
     * @brief Structure to hold the result of finding the best match
     */
    struct MatchResult {
        SafetyEvent* event;  /* Pointer to the matched event */
        float similarity;    /* Similarity score */
    };

    /**
     * Unified similarity calculation with progressive threshold checking
     * @param fusedOrFirstEvent Either FusedSafetyEvent* or SafetyEvent*
     * @param candidateEvent Candidate SafetyEvent to compare
     * @param isFused true if first param is FusedSafetyEvent, false if SafetyEvent
     * @return SimilarityResult with all metrics and early rejection flag
     */
    SimilarityResult CalculateSimilarity(
        const void* fusedOrFirstEvent,
        const SafetyEvent* candidateEvent,
        bool isFused) const;

    /**
     * @brief Calculate temporal similarity between two events
     * @param timestamp1 First event timestamp
     * @param timestamp2 Second event timestamp
     * @return Similarity score between 0.0 and 1.0
     */
    float CalculateTemporalSimilarity(const uint64_t timestamp1, const uint64_t timestamp2) const;

    /**
     * @brief Calculate spatial similarity between two events
     * @param trajectory1 First event trajectory coordinates
     * @param trajectory2 Second event trajectory coordinates
     * @return Similarity score between 0.0 and 1.0
     */
    float CalculateSpatialSimilarity(const TrajectoryCoordinates* trajectory1,
                                    const TrajectoryCoordinates* trajectory2) const;

    /**
     * @brief Calculate attribute similarity between two events
     * @param event1 First event
     * @param event2 Second event
     * @return Similarity score between 0.0 and 1.0
     */
    float CalculateAttributeSimilarity(const SafetyEvent& event1,
                                       const SafetyEvent& event2) const;

    /**
     * @brief Calculate attribute similarity between two events
     * @param fusedEvent First event
     * @param event Second event
     * @return Similarity score between 0.0 and 1.0
     */
    float CalculateAttributeSimilarity(const FusedSafetyEvent& fusedEvent,
                                       const SafetyEvent& event) const;

    /**
     * @brief Calculate overall similarity from individual scores
     * @param temporalSim Temporal similarity score
     * @param spatialSim Spatial similarity score
     * @param attrSim Attribute similarity score
     * @return Combined similarity score between 0.0 and 1.0
     */
    float CalculateOverallSimilarity(float temporalSim, float spatialSim,
                                     float attrSim) const;

    // Convenience overloads for events
    inline float CalculateTemporalSimilarity(const SafetyEvent& event1,
                                            const SafetyEvent& event2) const
    {
        return CalculateTemporalSimilarity(event1.timestamp, event2.timestamp);
    }

    inline float CalculateTemporalSimilarity(const FusedSafetyEvent& fusedEvent,
                                            const SafetyEvent& event) const
    {
        return CalculateTemporalSimilarity(fusedEvent.timestamp, event.timestamp);
    }

    inline float CalculateSpatialSimilarity(const SafetyEvent& event1,
                                           const SafetyEvent& event2) const
    {
        return CalculateSpatialSimilarity(event1.fusionMetadata.coordinates,
                                         event2.fusionMetadata.coordinates);
    }

    inline float CalculateSpatialSimilarity(const FusedSafetyEvent& fusedEvent,
                                           const SafetyEvent& event) const
    {
        return CalculateSpatialSimilarity(fusedEvent.fusionMetadata.coordinates,
                                         event.fusionMetadata.coordinates);
    }

    /**
     * @brief Create a fused event from two matched events
     * @param event1 First event
     * @param event2 Second event
     * @param similarity Similarity score between the events
     * @return FusedSafetyEvent combining both source events
     */
    FusedSafetyEvent CreateFusedSafetyEvent(const SafetyEvent& event1,
                                            const SafetyEvent& event2,
                                            float similarity) const;

    /**
     * @brief Create a fused event from Fused event and new matched event
     * @param event1 Fised event
     * @param event2 New event
     * @param similarity Similarity score between the events
     * @return FusedSafetyEvent combining both source events
     */
    FusedSafetyEvent CreateFusedSafetyEvent(const FusedSafetyEvent& existingFused,
                                            const SafetyEvent& newEvent,
                                            float similarity) const;

    /**
     * @brief Create a pass-through event for unmatched events
     * @param event Source event
     * @param isStale Flag indicating creatin of pass-through or stale event
     * @return FusedSafetyEvent containing the single source event
     */
    FusedSafetyEvent CreatePassThroughEvent(const SafetyEvent& event, const bool isStale) const;

    /**
     * @brief Safe addition with overflow checking
     * @param a First operand
     * @param multiplier Multiplier for a
     * @param b Second operand to add
     * @param result Output parameter for result
     * @return true if operation succeeded without overflow, false otherwise
     */
    static bool SafeTimeAdd(uint64_t a, uint64_t multiplier, uint64_t b, uint64_t* result) {
        // Check multiplication overflow
        if (a > 0 && multiplier > UINT64_MAX / a) {
            return false;
        }
        uint64_t product = a * multiplier;

        // Check addition overflow
        if (product > UINT64_MAX - b) {
            return false;
        }

        *result = product + b;
        return true;
    }

    /**
     * @brief Safe subtraction with underflow checking
     * @param a Minuend
     * @param b Subtrahend
     * @param result Output parameter for result
     * @return true if operation succeeded without underflow, false otherwise
     */
    static bool SafeTimeSub(uint64_t a, uint64_t b, uint64_t* result) {
        if (a < b) {
            return false;  // Would underflow
        }
        *result = a - b;
        return true;
    }

    /**
     * @brief Safe multiplication with overflow checking
     * @param a First operand
     * @param b Second operand
     * @param result Output parameter for result
     * @return true if operation succeeded without overflow, false otherwise
     */
    static bool SafeTimeMul(uint64_t a, uint64_t b, uint64_t* result) {
        if (a > 0 && b > UINT64_MAX / a) {
            return false;
        }
        *result = a * b;
        return true;
    }

    std::unordered_map<EventType, std::vector<SafetyEvent>> eventTypeQueues; /* Events organized by type */
    std::unordered_set<uint32_t> processedEventIds; /* Track processed events to avoid reprocessing */
    std::unordered_set<uint8_t> registeredPipelines; /* Pipeline IDs registered for fusion */
    std::vector<FusedSafetyEvent> fusedEvents;  /* Resulting fused events */

    // mutexes for thread-safe access
    // Mutable allows these to be changed within const member functions
    mutable std::mutex eventTypeQueuesMutex;
    mutable std::mutex fusedEventsMutex;
    mutable std::mutex processedEventIdsMutex;
    mutable std::mutex registeredPipelinesMutex;
    mutable std::mutex configMutex;  // For configuration parameters

    // Data structure to track fused events across iterations
    struct FusedEventTracker
    {
        FusedSafetyEvent fusedEvent;
        std::set<uint8_t> contributingPipelines;
        std::vector<uint64_t> sourceEventIds;
        uint8_t fusionIterations;
        bool canBeFusedAgain;
        bool superseded;
    };

    uint64_t timeWindowSize;  /* Time window for correlation in milliseonds*/
    float fusionThreshold;  /* Minimum similarity threshold for fusion */
    float alpha; /* Weightage of Temporal Similarity for fusion */
    float beta; /* Weightage of Spatial Similarity for fusion */
    float gamma; /* Weightage of Attribute Similarity for fusion */
    uint64_t temporalTolerance;  /* Time window for tempotal tolerance in milliseconds*/
    uint8_t trajectoryCount; /* Number of co-ordinates for spatial correlation */
    uint8_t maxSupportedPipelines = MAX_SUPPORTED_PIPELINES; /* Maximum number of pipelines supported (configurable) */
    const float speedAttributeTolerance = 1.0f; /* Tolerance for Field 'speed' in Attribute Analysis*/
    const uint8_t attributesCount = 2U; /* Number of attributes that are compared for similarity analysis */

    // Add early termination threshold
    float earlyTerminationThreshold;  // e.g., 0.95 - stop search if match this good is found
    bool enableEarlyTermination;
    // Add temporal bucketing for faster lookup
    static constexpr uint64_t TEMPORAL_BUCKET_SIZE_NS = 100000000; // 100ms buckets

};

} // namespace nvpss

#endif // SAFETY_EVENT_FUSION_HPP
