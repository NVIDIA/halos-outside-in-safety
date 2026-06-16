/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <shared_mutex>
#include "NvPSSSafetyEventFusion.hpp"
#include "NvPSSSpatialCorrelation.hpp"
#include "NvPSB.h"

namespace nvpss {

SafetyEventFusion::SafetyEventFusion(uint64_t timeWindow, float fusionT, float temporalW,
                                     float spatialW, float attributeW, uint64_t temporalT,
                                     uint8_t trajectoryCount, uint8_t maxPipelines,
                                     float earlyTermThreshold, bool enableEarlyTerm)
    : timeWindowSize(timeWindow),
      fusionThreshold(fusionT),
      alpha(temporalW),
      beta(spatialW),
      gamma(attributeW),
      temporalTolerance(temporalT),
      trajectoryCount(trajectoryCount),
      maxSupportedPipelines(maxPipelines),
      earlyTerminationThreshold(earlyTermThreshold),
      enableEarlyTermination(enableEarlyTerm)
{}

SafetyEventFusion::~SafetyEventFusion()
{
    ClearEventQueues();
}

NvPSSDErr SafetyEventFusion::ClearEventQueues()
{
    // Atomically lock all three to avoid deadlock
    std::lock(eventTypeQueuesMutex, fusedEventsMutex, processedEventIdsMutex);

    std::lock_guard<std::mutex> l1(eventTypeQueuesMutex, std::adopt_lock);
    std::lock_guard<std::mutex> l2(fusedEventsMutex, std::adopt_lock);
    std::lock_guard<std::mutex> l3(processedEventIdsMutex, std::adopt_lock);

    eventTypeQueues.clear();
    fusedEvents.clear();
    processedEventIds.clear();

    return NVPSSD_SUCCESS;
}


std::vector<FusedSafetyEvent> SafetyEventFusion::GetFusedEvents() const
{
    std::lock_guard<std::mutex> lock(fusedEventsMutex);
    return fusedEvents;
}

NvPSSDErr SafetyEventFusion::SetFusionThreshold(float threshold)
{
    if(threshold < 0.0f || threshold > 1.0f)
    {
        return NVPSSD_FAIL;
    }

    std::unique_lock<std::mutex> lock(configMutex);
    fusionThreshold = threshold;

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::SetTimeWindowSize(uint64_t windowSize)
{
    if(windowSize == 0)
    {
        return NVPSSD_FAIL;
    }

    std::unique_lock<std::mutex> lock(configMutex);
    timeWindowSize = windowSize;

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::SetTemporalTolerance(uint64_t temporalT)
{
    if(temporalT == 0)
    {
        return NVPSSD_FAIL;
    }

    std::unique_lock<std::mutex> lock(configMutex);
    temporalTolerance = temporalT;

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::SetFusionWeights(float temporalW, float spatialW, float attributeW)
{
    float totalW = temporalW + spatialW + attributeW;

    if(std::abs(totalW - 1.0f) > 0.01f) //Weight sum (alpha + beta + gamma) should be close to 1.0
    {
        return NVPSSD_FAIL;
    }

    std::unique_lock<std::mutex> lock(configMutex);
    alpha = temporalW;
    beta = spatialW;
    gamma = attributeW;

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::SetTrajectoryCount(uint8_t count)
{
    if(count > 10)
    {
        return NVPSSD_FAIL;
    }

    trajectoryCount = count;

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::SetEarlyTerminationThreshold(float threshold) {
    if (threshold < 0.0f || threshold > 1.0f) {
        return NVPSSD_FAIL;
    }
    earlyTerminationThreshold = threshold;
    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::EnableEarlyTermination(bool enable) {
    enableEarlyTermination = enable;
    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::ProcessNewSafetyEvent(const SafetyEvent& event)
{
    // Filter STALE events
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now;
    if (!SafeTimeAdd(static_cast<uint64_t>(ts.tv_sec), 1000000000ULL,
                     static_cast<uint64_t>(ts.tv_nsec), &now)) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Time calculation overflow in ProcessNewSafetyEvent", "");
        return NVPSSD_FAIL;
    }

    uint64_t timeWindowNs;
    if (!SafeTimeMul(timeWindowSize, 1000000ULL, &timeWindowNs)) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Time window multiplication overflow", "");
        return NVPSSD_FAIL;
    }

    uint64_t cutoffTime;
    if (!SafeTimeSub(now, timeWindowNs, &cutoffTime)) {
        // Underflow - means timeWindowSize is very large or now is very small
        // In this case, no events can be stale yet
        cutoffTime = 0;
    }

    if (event.timestamp < cutoffTime)
    {
        // Create a stale event
        std::lock_guard<std::mutex> lock(fusedEventsMutex);
        fusedEvents.push_back(CreatePassThroughEvent(event, true));
        NvPSBWriteData(NVPSB_LOG_INFO, "STALE Safety Events. StaleEventId: " + std::to_string(event.id) +
                       " Source Event Id: " + std::to_string(event.id), "");
        return NVPSSD_SUCCESS;
    }

    // Handle SW_FAIL and other events without pipelineID
    if (event.fusionMetadata.pipelineID == 0)
    {
        std::lock_guard<std::mutex> lock(fusedEventsMutex);
        fusedEvents.push_back(CreatePassThroughEvent(event, false));
        return NVPSSD_SUCCESS;
    }

    // Validate pipeline ID range
    {
        std::lock_guard<std::mutex> configLock(configMutex);
        if (event.fusionMetadata.pipelineID > maxSupportedPipelines)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Invalid pipeline ID: " + std::to_string(event.fusionMetadata.pipelineID), "");
            return NVPSSD_FAIL;
        }
    }

    // Register pipeline if not already registered
    RegisterPipeline(event.fusionMetadata.pipelineID);

    // Add event to the appropriate event type queue
    {
        std::lock_guard<std::mutex> lock(eventTypeQueuesMutex);
        eventTypeQueues[event.type].push_back(event);
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "Added event to type queue. EventType: " + std::to_string(event.type) +
                   " PipelineID: " + std::to_string(event.fusionMetadata.pipelineID) +
                   " EventID: " + std::to_string(event.id), "");

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::PerformSafetyEventFusion()
{
    // Process different event types according to their nature
    for (int i = static_cast<int>(EventType::EVENT_0); i < static_cast<int>(EventType::SW_FAIL); i++)
    {
        ProcessSafetyEvents(static_cast<EventType>(i));
    }

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::ProcessSafetyEvents(EventType type)
{
    // Get events of specified type
    if (eventTypeQueues.find(type) == eventTypeQueues.end() || eventTypeQueues[type].empty())
    {
        return NVPSSD_SUCCESS; // No events of this type
    }

    auto& events = eventTypeQueues[type];

    // Collect unprocessed event indices
    std::vector<size_t> unprocessedEventIndices;
    for (size_t idx = 0; idx < events.size(); ++idx)
    {
        auto& event = events[idx];
        if (!event.processed && processedEventIds.find(event.id) == processedEventIds.end())
        {
            unprocessedEventIndices.push_back(idx);
        }
    }

    if (unprocessedEventIndices.empty() || unprocessedEventIndices.size() == 1)
    {
        return NVPSSD_SUCCESS;
    }

    // Sort indices by timestamp for temporal locality
    std::sort(unprocessedEventIndices.begin(), unprocessedEventIndices.end(),
              [&events](size_t a, size_t b) {
                  return events[a].timestamp < events[b].timestamp;
              });

    // Phase 1 - Initial Pairwise Fusion (Run once)
    std::vector<FusedEventTracker> currentIterationFused;
    std::vector<bool> eventProcessed(unprocessedEventIndices.size(), false);
    bool anyFusionPhase1 = false;

#ifdef NVPSF_DBG
    NvPSBWriteData(NVPSB_LOG_INFO,
                   "Phase 1: Starting pairwise fusion for " +
                   std::to_string(unprocessedEventIndices.size()) + " events", "");
#endif

    // Fuse unfused events with each other
    for (size_t i = 0; i < unprocessedEventIndices.size(); ++i)
    {
        // Already processed in this iteration
        if (eventProcessed[i])
        {
            continue;
        }

        SafetyEvent& baseEvent = events[unprocessedEventIndices[i]];
        MatchResult bestMatch = {nullptr, 0.0f};
        size_t bestMatchIdx = 0;

        // Temporal windowing - only compare events within timeWindowSize
        uint64_t minTime = (baseEvent.timestamp > timeWindowSize * 1000000)
                           ? baseEvent.timestamp - (timeWindowSize * 1000000)
                           : 0;
        uint64_t maxTime = baseEvent.timestamp + (timeWindowSize * 1000000);

        // Compare baseEvent against all other unprocessed events from different pipelines
        // Start search from current position (events are sorted by time)
        for (size_t j = i + 1; j < unprocessedEventIndices.size(); ++j)
        {
            // Already processed
            if (eventProcessed[j])
            {
                continue;
            }

            SafetyEvent& candidateEvent = events[unprocessedEventIndices[j]];

            // Early exit if outside temporal window (sorted order)
            if (candidateEvent.timestamp > maxTime)
            {
                break; // All subsequent events will also be outside window
            }

            // Skip if outside lower temporal bound
            if (candidateEvent.timestamp < minTime)
            {
                continue;
            }

            // Skip events from the same pipeline (enforce different pipeline requirement)
            if (baseEvent.fusionMetadata.pipelineID == candidateEvent.fusionMetadata.pipelineID)
            {
                continue;
            }

            // Skip events with different rule identifiers
            if (strcmp(baseEvent.ruleIdentifier, candidateEvent.ruleIdentifier) != 0)
            {
                continue;
            }

            // Calculate similarities with progressive threshold checking
            SimilarityResult similarity = CalculateSimilarity(&baseEvent, &candidateEvent, false);
            if (similarity.earlyReject)
            {
                continue; // Failed early rejection checks
            }

            float overallSim = similarity.overallSim;

            // Update best match if this is better
            if (overallSim > bestMatch.similarity)
            {
                bestMatch.event = &candidateEvent;
                bestMatch.similarity = overallSim;
                bestMatchIdx = j;

                // Early termination if excellent match found
                if (enableEarlyTermination && overallSim >= earlyTerminationThreshold)
                {
                    break; // Stop searching, found excellent match
                }
            }
        }

        // Check if best match exceeds fusion threshold
        if (bestMatch.event && bestMatch.similarity >= fusionThreshold)
        {
            // Cache values BEFORE creating fused event to avoid any potential issues
            uint8_t matchedPipelineID = bestMatch.event->fusionMetadata.pipelineID;
            uint64_t matchedEventID = bestMatch.event->id;

            // Create fused event and mark source events as processed
            FusedSafetyEvent fusedEvent = CreateFusedSafetyEvent(baseEvent, *bestMatch.event, bestMatch.similarity);

            // Track this fused event with pipeline information
            FusedEventTracker tracker;
            tracker.fusedEvent = fusedEvent;

            // Record contributing pipelines - use cached values
            tracker.contributingPipelines.insert(baseEvent.fusionMetadata.pipelineID);
            tracker.contributingPipelines.insert(matchedPipelineID);

            // Record source event IDs - use cached values
            tracker.sourceEventIds.push_back(baseEvent.id);
            tracker.sourceEventIds.push_back(matchedEventID);

            // Track fusion iteration count
            tracker.fusionIterations = 1;

            // Can this fused event be fused again? Only if we haven't reached maxPipelines
            tracker.canBeFusedAgain = (tracker.contributingPipelines.size() < maxSupportedPipelines);

            currentIterationFused.push_back(tracker);
            eventProcessed[i] = true;
            eventProcessed[bestMatchIdx] = true;
            anyFusionPhase1 = true;

            // Mark both events as processed
            baseEvent.processed = true;
            bestMatch.event->processed = true;

#ifdef NVPSF_DBG
            // Build readable pipeline list for logging
            std::string pipelineList;
            for (uint8_t pId : tracker.contributingPipelines)
            {
                pipelineList += std::to_string(pId) + ",";
            }
            if (!pipelineList.empty()) pipelineList.pop_back();

            NvPSBWriteData(NVPSB_LOG_INFO,
                           "Fused Safety Events (Phase 1). FusedEventId: " +
                           std::to_string(tracker.fusedEvent.id) +
                           " Source Events Ids: E" + std::to_string(baseEvent.id) +
                           " and E" + std::to_string(matchedEventID) +
                           " Pipelines: " + pipelineList +
                           " Score: " + std::to_string(bestMatch.similarity), "");
#endif
        }
    }

#ifdef NVPSF_DBG
    NvPSBWriteData(NVPSB_LOG_INFO,
                   "Phase 1 complete. Fused events: " + std::to_string(currentIterationFused.size()) +
                   " Remaining unfused: " + std::to_string(
                       std::count(eventProcessed.begin(), eventProcessed.end(), false)), "");
#endif

    // Collect remaining unfused event indices
    std::vector<size_t> remainingUnfusedIndices;
    for (size_t i = 0; i < unprocessedEventIndices.size(); ++i)
    {
        if (!eventProcessed[i])
        {
            remainingUnfusedIndices.push_back(unprocessedEventIndices[i]);
        }
    }

    // Check if iterative Fusion is required
    bool iterativeFusionEnabled = false;
    uint8_t maxFusionIterations = 0;
    {
        std::lock_guard<std::mutex> configLock(configMutex);
        iterativeFusionEnabled = (maxSupportedPipelines > 2 ) ? true : false;
        maxFusionIterations = maxSupportedPipelines - 1;
    }

    // If iterative fusion disabled: output Phase 1 results and exit
    if (!iterativeFusionEnabled)
    {
        // Output Phase 1 fused events
        {
            std::lock_guard<std::mutex> fusedLock(fusedEventsMutex);
            std::lock_guard<std::mutex> processedLock(processedEventIdsMutex);

            for (const auto& tracker : currentIterationFused)
            {
                fusedEvents.push_back(tracker.fusedEvent);
                processedEventIds.insert(tracker.sourceEventIds.begin(), tracker.sourceEventIds.end());
            }
        }

        goto done;
    }

    // Iterative Fusion Extension for N > 2 pipelines
    // Update to remaining unfused indices
    unprocessedEventIndices = remainingUnfusedIndices;

    //Starting from 1, as iteration 0 is already performed
    for (uint8_t iteration = 1; iteration < maxFusionIterations; ++iteration)
    {
        if (!anyFusionPhase1)
        {
            break;
        }

        if(unprocessedEventIndices.empty())
        {
            break;
        }

        std::vector<bool> thisIterationProcessed(unprocessedEventIndices.size(), false);
        bool anyFusionThisIteration = false;

#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO,
                       "Phase 2 Iteration " + std::to_string(iteration) +
                       ": Starting with " + std::to_string(currentIterationFused.size()) +
                       " fused and " + std::to_string(unprocessedEventIndices.size()) + " unfused", "");
#endif

        // Extend fused events with unfused from NEW pipelines
        size_t numFusedBefore = currentIterationFused.size();

        for (size_t fusedIdx = 0; fusedIdx < numFusedBefore; ++fusedIdx)
        {
            FusedEventTracker& existingFused = currentIterationFused[fusedIdx];

            if (!existingFused.canBeFusedAgain)
            {
                continue;
            }

            MatchResult bestMatch = {nullptr, 0.0f};
            size_t bestMatchIdx = 0;

            // Temporal windowing
            uint64_t minTime = (existingFused.fusedEvent.timestamp > timeWindowSize * 1000000)
                               ? existingFused.fusedEvent.timestamp - (timeWindowSize * 1000000)
                               : 0;
            uint64_t maxTime = existingFused.fusedEvent.timestamp + (timeWindowSize * 1000000);

            // Find best matching unfused event from a NEW pipeline
            for (size_t i = 0; i < unprocessedEventIndices.size(); ++i)
            {
                if (thisIterationProcessed[i])
                {
                    continue;
                }

                SafetyEvent& candidateEvent = events[unprocessedEventIndices[i]];

                // Skip if outside temporal window
                if (candidateEvent.timestamp > maxTime)
                {
                    continue;
                }

                if (candidateEvent.timestamp < minTime)
                {
                    continue;
                }

                // Skip if pipeline already contributed to this fused event
                if (existingFused.contributingPipelines.count(candidateEvent.fusionMetadata.pipelineID) > 0)
                {
                    continue;
                }

                // Skip events with different rule identifiers
                if (std::strcmp(existingFused.fusedEvent.ruleIdentifier, candidateEvent.ruleIdentifier) != 0)
                {
                    continue;
                }

                // Calculate similarities with progressive threshold checking
                SimilarityResult similarity = CalculateSimilarity(&existingFused.fusedEvent, &candidateEvent, true);
                if (similarity.earlyReject)
				{
                    continue; // Failed early rejection checks
                }

                float overallSim = similarity.overallSim;

                // Update best match if this is better
                if (overallSim > bestMatch.similarity)
                {
                    bestMatch.event = &candidateEvent;
                    bestMatch.similarity = overallSim;
                    bestMatchIdx = i;

                    // Early termination if excellent match found
                    if (enableEarlyTermination && overallSim >= earlyTerminationThreshold)
                    {
                        break; // Stop searching, found excellent match
                    }
                }
            }

            // Fuse existing fused event with new event if match exceeds threshold
            if (bestMatch.event && bestMatch.similarity >= fusionThreshold)
            {
                // Cache values BEFORE creating enhanced fused event
                uint8_t matchedPipelineID = bestMatch.event->fusionMetadata.pipelineID;
                uint64_t matchedEventID = bestMatch.event->id;

                // Create new fused event from existing fused + new candidate
                FusedSafetyEvent enhancedFused = CreateFusedSafetyEvent(
                    existingFused.fusedEvent, *bestMatch.event, bestMatch.similarity);

                // Directly modify existingFused
                existingFused.fusedEvent = enhancedFused;

                // Merge pipeline sets (all pipelines from previous fusions + new pipeline)
                existingFused.contributingPipelines.insert(matchedPipelineID);

                // Merge source event IDs
                existingFused.sourceEventIds.push_back(matchedEventID);

                // Increment fusion iteration count
                existingFused.fusionIterations = existingFused.fusionIterations + 1;

                // Can this be fused again?
                existingFused.canBeFusedAgain = (existingFused.contributingPipelines.size() < maxSupportedPipelines);

                thisIterationProcessed[bestMatchIdx] = true;
                anyFusionThisIteration = true;

                // Mark event as processed
                bestMatch.event->processed = true;

#ifdef NVPSF_DBG
                // Build readable pipeline list for logging
                std::string pipelineList;
                for (uint8_t pId : existingFused.contributingPipelines)
                {
                    pipelineList += std::to_string(pId) + ",";
                }
                if (!pipelineList.empty()) pipelineList.pop_back();

                // Build readable event ID list for logging
                std::string eventIdList;
                for (uint64_t eId : existingFused.sourceEventIds)
                {
                    eventIdList += std::to_string(eId) + ",";
                }
                if (!eventIdList.empty()) eventIdList.pop_back();

                NvPSBWriteData(NVPSB_LOG_INFO,
                               "N-way Fusion (Iteration " + std::to_string(iteration) + "). " +
                               "Fused from " + std::to_string(existingFused.contributingPipelines.size()) +
                               " pipelines: [" + pipelineList + "] Source Event IDs: [" + eventIdList +
                               "] Iterations: " + std::to_string(existingFused.fusionIterations) +
                               " Score: " + std::to_string(bestMatch.similarity), "");
#endif
            }
        }

        // Update remaining unfused indices
        std::vector<size_t> stillUnfusedIndices;
        for (size_t i = 0; i < unprocessedEventIndices.size(); ++i)
        {
            if (!thisIterationProcessed[i])
            {
                stillUnfusedIndices.push_back(unprocessedEventIndices[i]);
            }
        }
        unprocessedEventIndices = stillUnfusedIndices;

        if (!anyFusionThisIteration)
        {
            break;
        }
    }

    // OUTPUT RESULTS
    {
        std::lock_guard<std::mutex> fusedLock(fusedEventsMutex);
        std::lock_guard<std::mutex> processedLock(processedEventIdsMutex);

        // Output all fused events
        for (const auto& tracker : currentIterationFused)
        {
            fusedEvents.push_back(tracker.fusedEvent);
            processedEventIds.insert(tracker.sourceEventIds.begin(), tracker.sourceEventIds.end());
        }
    }

#ifdef NVPSF_DBG
    NvPSBWriteData(NVPSB_LOG_INFO,
                   "ProcessSafetyEvents complete for type " + std::to_string(type) +
                   ". Total fused events: " + std::to_string(currentIterationFused.size()) +
                   " Remaining events: " + std::to_string(unprocessedEventIndices.size()), "");
#endif

done:
    return NVPSSD_SUCCESS;
}

SimilarityResult SafetyEventFusion::CalculateSimilarity(
    const void* fusedOrFirstEvent,
    const SafetyEvent* candidateEvent,
    bool isFused) const
{
    SimilarityResult result;

    if (!candidateEvent)
    {
        result.earlyReject = true;
        return result;
    }

    // Calculate temporal similarity
    if (isFused)
    {
        const FusedSafetyEvent* fusedEvent = static_cast<const FusedSafetyEvent*>(fusedOrFirstEvent);
        result.temporalSim = CalculateTemporalSimilarity(*fusedEvent, *candidateEvent);
    } else
    {
        const SafetyEvent* firstEvent = static_cast<const SafetyEvent*>(fusedOrFirstEvent);
        result.temporalSim = CalculateTemporalSimilarity(*firstEvent, *candidateEvent);
    }

    // Early rejection based on temporal similarity alone
    // Even with perfect spatial/attribute scores (max 1.0), won't reach threshold
    if (result.temporalSim * alpha < (fusionThreshold - (beta + gamma)))
    {
        result.earlyReject = true;
        return result;
    }

    // Calculate spatial similarity
    if (isFused)
    {
        const FusedSafetyEvent* fusedEvent = static_cast<const FusedSafetyEvent*>(fusedOrFirstEvent);
        result.spatialSim = CalculateSpatialSimilarity(*fusedEvent, *candidateEvent);
    } else
    {
        const SafetyEvent* firstEvent = static_cast<const SafetyEvent*>(fusedOrFirstEvent);
        result.spatialSim = CalculateSpatialSimilarity(*firstEvent, *candidateEvent);
    }

    // Progressive threshold check
    float partialScore = alpha * result.temporalSim + beta * result.spatialSim;
    if (partialScore < (fusionThreshold - gamma))
    {
        result.earlyReject = true;
        return result;
    }

    // Calculate attribute similarity
    if (isFused)
    {
        const FusedSafetyEvent* fusedEvent = static_cast<const FusedSafetyEvent*>(fusedOrFirstEvent);
        result.attributeSim = CalculateAttributeSimilarity(*fusedEvent, *candidateEvent);
    } else
    {
        const SafetyEvent* firstEvent = static_cast<const SafetyEvent*>(fusedOrFirstEvent);
        result.attributeSim = CalculateAttributeSimilarity(*firstEvent, *candidateEvent);
    }

    // Calculate composite similarity score
    result.overallSim = CalculateOverallSimilarity(
        result.temporalSim,
        result.spatialSim,
        result.attributeSim);

    result.earlyReject = false;

    return result;
}

float SafetyEventFusion::CalculateTemporalSimilarity(
    const uint64_t timestamp1,
    const uint64_t timestamp2) const
{
    // Handle invalid sigma case to prevent division by zero
    if (temporalTolerance == 0) {
        std::cerr << "Error: temporal tolerance window cannot be zero" << std::endl;
        return 0.0f;
    }

    // Calculate time difference safely
    uint64_t timeDiff;
    if (timestamp1 >= timestamp2) {
        timeDiff = (timestamp1 - timestamp2) / 1000000; // Convert to ms
    } else {
        timeDiff = (timestamp2 - timestamp1) / 1000000;
    }

    // Use double for intermediate calculations to avoid overflow
    double timeDiffDouble = static_cast<double>(timeDiff);
    double toleranceDouble = static_cast<double>(temporalTolerance);

    // Calculate squared difference
    double squaredDiff = timeDiffDouble * timeDiffDouble;
    double sigmaSq = toleranceDouble * toleranceDouble;

    // Compute exponent
    double exponent = -squaredDiff / (2.0 * sigmaSq);

    // Clamp exponent to prevent underflow
    if (exponent < -100.0) {
        return 0.0f; // e^(-100) is effectively 0
    }

    return static_cast<float>(std::exp(exponent));
}

float SafetyEventFusion::CalculateSpatialSimilarity(
    const TrajectoryCoordinates* coordinates1,
    const TrajectoryCoordinates* coordinates2) const
{
    if (!coordinates1 || !coordinates2)
    {
        return 0.0f;
    }

    float trajectory1_x[trajectoryCount];
    float trajectory1_y[trajectoryCount];
    float trajectory2_x[trajectoryCount];
    float trajectory2_y[trajectoryCount];

    for (uint8_t i = 0; i < trajectoryCount; i++)
    {
        trajectory1_x[i] = coordinates1[i].x;
        trajectory1_y[i] = coordinates1[i].y;
        trajectory2_x[i] = coordinates2[i].x;
        trajectory2_y[i] = coordinates2[i].y;
    }

    // Create trajectory objects
    TrajectoryCorrelator correlator;
    auto trajectory1 = correlator.createTrajectory(trajectory1_x, trajectory1_y, trajectoryCount);
    auto trajectory2 = correlator.createTrajectory(trajectory2_x, trajectory2_y, trajectoryCount);

    // Calculate spatial correlation
    float correlation_weight = correlator.calculateSpatialCorrelation(trajectory1, trajectory2);

    return correlation_weight;
}

float SafetyEventFusion::CalculateAttributeSimilarity(
    const SafetyEvent& event1,
    const SafetyEvent& event2) const
{
    // Additional attribute comparison implemented here
    // such as comparing specific fields in the fusion metadata
    uint8_t matchingAttributes = 0;

    if (event1.fusionMetadata.objectType[0] == event2.fusionMetadata.objectType[0])
    {
        matchingAttributes++;
    }

    if(std::fabs(event1.fusionMetadata.speed - event2.fusionMetadata.speed) < speedAttributeTolerance)
    {
        matchingAttributes++;
    }

    return matchingAttributes / attributesCount;
}

float SafetyEventFusion::CalculateAttributeSimilarity(
    const FusedSafetyEvent& existingFused,
    const SafetyEvent& newEvent) const
{
    // Additional attribute comparison implemented here
    // such as comparing specific fields in the fusion metadata
    uint8_t matchingAttributes = 0;

    if (existingFused.fusionMetadata.objectType[0] == newEvent.fusionMetadata.objectType[0])
    {
        matchingAttributes++;
    }

    if(std::fabs(existingFused.fusionMetadata.speed - newEvent.fusionMetadata.speed) < speedAttributeTolerance)
    {
        matchingAttributes++;
    }

    return matchingAttributes / attributesCount;
}

float SafetyEventFusion::CalculateOverallSimilarity(
    float temporalSim,
    float spatialSim,
    float attrSim) const
{
#ifdef NVPSSD_DBG
    std::cout << "Temporal Sim : " << temporalSim << std::endl;
    std::cout << "Spatial Sim : " << spatialSim << std::endl;
    std::cout << "Attribute Sim : " << attrSim << std::endl;
#endif
    // Weighted sum of the three similarity scores
    return alpha * temporalSim + beta * spatialSim + gamma * attrSim;
}

FusedSafetyEvent SafetyEventFusion::CreateFusedSafetyEvent(
    const SafetyEvent& event1,
    const SafetyEvent& event2,
    float similarity) const
{
    FusedSafetyEvent fusedEvent;

    // Use properties from event1 as base
    strncpy(fusedEvent.sensorIdentifier, event1.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    strncpy(fusedEvent.ruleIdentifier, event1.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    fusedEvent.id = event1.id;
    fusedEvent.type = event1.type;
    std::memcpy(&fusedEvent.fusionMetadata, &event1.fusionMetadata, sizeof(EventFusionMetadata));

    // Take the earliest timestamp
    fusedEvent.timestamp = std::min(event1.timestamp, event2.timestamp);

    // Probabilistic fusion of confidence levels using Bayesian approach
    // P(A∪B) = P(A) + P(B) - P(A)P(B)
    fusedEvent.confidenceLevel = event1.confidenceLevel + event2.confidenceLevel -
                                  (event1.confidenceLevel * event2.confidenceLevel);

    // Take the highest severity level
    fusedEvent.severity = static_cast<uint8_t>(event1.severity) > static_cast<uint8_t>(event2.severity) ?
                          event1.severity : event2.severity;

    // Mark as a fused event
    fusedEvent.status = FUSED;

    NvPSBWriteData(NVPSB_LOG_INFO, "FUSED Safety Events. FusedEventId: " + std::to_string(fusedEvent.id) +
                   " Source Event Ids: " + std::to_string(event1.id) + " " + std::to_string(event2.id) +
                   " EventType: " + std::to_string(fusedEvent.type) +
                   " PipelineIDs: " + std::to_string(event1.fusionMetadata.pipelineID) + " "
                   + std::to_string(event2.fusionMetadata.pipelineID), "");

    return fusedEvent;
}

FusedSafetyEvent SafetyEventFusion::CreateFusedSafetyEvent(
    const FusedSafetyEvent& existingFused,
    const SafetyEvent& newEvent,
    float similarity) const
{
    FusedSafetyEvent fusedEvent;

    // Use properties from existingFused as base
    strncpy(fusedEvent.sensorIdentifier, existingFused.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    strncpy(fusedEvent.ruleIdentifier, existingFused.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    fusedEvent.id = existingFused.id;
    fusedEvent.type = existingFused.type;
    std::memcpy(&fusedEvent.fusionMetadata, &existingFused.fusionMetadata, sizeof(EventFusionMetadata));

    // Take the earliest timestamp
    fusedEvent.timestamp = std::min(existingFused.timestamp, newEvent.timestamp);

    // Probabilistic fusion of confidence levels using Bayesian approach
    // P(A∪B) = P(A) + P(B) - P(A)P(B)
    fusedEvent.confidenceLevel = existingFused.confidenceLevel + newEvent.confidenceLevel -
                                  (existingFused.confidenceLevel * newEvent.confidenceLevel);

    // Take the highest severity level
    fusedEvent.severity = static_cast<uint8_t>(existingFused.severity) > static_cast<uint8_t>(newEvent.severity) ?
                          existingFused.severity : newEvent.severity;

    // Mark as a fused event
    fusedEvent.status = FUSED;

    NvPSBWriteData(NVPSB_LOG_INFO, "N-FUSED Safety Events. FusedEventId: " + std::to_string(fusedEvent.id) +
                   " Source Event Ids: " + std::to_string(existingFused.id) + " " + std::to_string(newEvent.id) +
                   " EventType: " + std::to_string(fusedEvent.type) +
                   " PipelineIDs: " + std::to_string(existingFused.fusionMetadata.pipelineID) + " "
                   + std::to_string(newEvent.fusionMetadata.pipelineID), "");

    return fusedEvent;
}

NvPSSDErr SafetyEventFusion::ProcessUnmatchedEvents()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now;
    if (!SafeTimeAdd(static_cast<uint64_t>(ts.tv_sec), 1000000000ULL,
                     static_cast<uint64_t>(ts.tv_nsec), &now)) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Time calculation overflow in ProcessUnmatchedEvents", "");
        return NVPSSD_FAIL;
    }

    uint64_t timeWindowNs;
    if (!SafeTimeMul(timeWindowSize, 1000000ULL, &timeWindowNs)) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Time window multiplication overflow", "");
        return NVPSSD_FAIL;
    }

    uint64_t cutoffTime;
    if (!SafeTimeSub(now, timeWindowNs, &cutoffTime)) {
        cutoffTime = 0;
    }

    std::lock_guard<std::mutex> queueLock(eventTypeQueuesMutex);
    std::lock_guard<std::mutex> fusedLock(fusedEventsMutex);
    std::lock_guard<std::mutex> processedLock(processedEventIdsMutex);

    // Process unmatched events from all event type queues
    for (auto& eventTypePair : eventTypeQueues)
    {
        auto& eventType = eventTypePair.first;
        auto& events = eventTypePair.second;

        for (auto& event : events)
        {
            if (!event.processed && event.timestamp < cutoffTime)
            {
                // Create a pass-through event and mark as processed
                fusedEvents.push_back(CreatePassThroughEvent(event, false));
                event.processed = true;
                processedEventIds.insert(event.id);

                NvPSBWriteData(NVPSB_LOG_INFO, "PASSTHROUGH Safety Events. PassthroughEventId: " + std::to_string(event.id) +
                               " Source Event Id: " + std::to_string(event.id) +
                               " EventType: " + std::to_string(eventType) +
                               " PipelineID: " + std::to_string(event.fusionMetadata.pipelineID), "");
            }
        }
    }

    return NVPSSD_SUCCESS;
}

FusedSafetyEvent SafetyEventFusion::CreatePassThroughEvent(
    const SafetyEvent& event, const bool isStale) const
{
    FusedSafetyEvent fusedEvent;

    // Generate a unique ID
    fusedEvent.id = event.id;

    // Copy properties directly from the source event
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    strncpy(fusedEvent.ruleIdentifier, event.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    fusedEvent.type = event.type;
    fusedEvent.timestamp = event.timestamp;
    fusedEvent.confidenceLevel = event.confidenceLevel;
    fusedEvent.severity = event.severity;
    std::memcpy(&fusedEvent.fusionMetadata, &event.fusionMetadata, sizeof(EventFusionMetadata));

    // Mark as not fused (pass-through) / or stale
    if (isStale)
    {
        fusedEvent.status = STALE;
    }
    else
    {
        fusedEvent.status = PASSTHROUGH;
    }

    return fusedEvent;
}

NvPSSDErr SafetyEventFusion::CleanProcessedEvents()
{
    std::lock_guard<std::mutex> queueLock(eventTypeQueuesMutex);
    std::lock_guard<std::mutex> processedLock(processedEventIdsMutex);

    // Remove processed events from all event type queues
    auto isProcessed = [this](const SafetyEvent& event)
    {
        return event.processed || processedEventIds.find(event.id) != processedEventIds.end();
    };

    for (auto& eventTypePair : eventTypeQueues)
    {
        auto& events = eventTypePair.second;
        events.erase(
            std::remove_if(events.begin(), events.end(), isProcessed),
            events.end()
        );
    }

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::ClearFusedEvents(uint8_t count)
{
    std::lock_guard<std::mutex> lock(fusedEventsMutex);

    if(count == 0 || count > fusedEvents.size())
    {
        return NVPSSD_FAIL;
    }

    fusedEvents.erase(fusedEvents.begin(), fusedEvents.begin() + count);

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::SetMaxPipelines(uint8_t maxPipelines)
{
    if (maxPipelines == 0 || maxPipelines > MAX_SUPPORTED_PIPELINES)
    {
        return NVPSSD_FAIL;
    }

    maxSupportedPipelines = maxPipelines;

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventFusion::RegisterPipeline(uint8_t pipelineId)
{
    std::unique_lock<std::mutex> readLock(configMutex);

    if (pipelineId == 0 || pipelineId > maxSupportedPipelines)
    {
        return NVPSSD_FAIL;
    }

    readLock.unlock();

    std::lock_guard<std::mutex> lock(registeredPipelinesMutex);
    if (registeredPipelines.insert(pipelineId).second)
    {
        NvPSBWriteData(NVPSB_LOG_INFO, "Registered new pipeline: " + std::to_string(pipelineId), "");
    }

    return NVPSSD_SUCCESS;
}

} // namespace nvpss
