/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSS_STATUS_NOOP_HPP
#define NVPSS_STATUS_NOOP_HPP

#include "pss_protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static constexpr uint32_t NVPSS_STATUS_NOOP_INTERVAL_MS_DEFAULT = 5000U;
static constexpr uint32_t NVPSS_STATUS_NOOP_INTERVAL_MS_MIN = 100U;
static constexpr uint32_t NVPSS_STATUS_NOOP_INTERVAL_MS_MAX = 600000U;

static inline bool NvPSSStatusNoopIntervalMsIsValid(uint32_t intervalMs)
{
    return intervalMs >= NVPSS_STATUS_NOOP_INTERVAL_MS_MIN &&
           intervalMs <= NVPSS_STATUS_NOOP_INTERVAL_MS_MAX;
}

static inline void NvPSSBuildStatusNoopDecisionRequest(DecisionRequest* request,
                                                       uint32_t requestId,
                                                       SystemStatus pssStatus,
                                                       uint64_t timestampNs)
{
    if (request == nullptr)
        return;

    std::memset(request, 0, sizeof(*request));
    request->requestId = requestId;
    request->pssStatus = pssStatus;
    request->sensorDataSummarySize = 1U;

    SensorData& sensorData = request->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = true;
    sensorData.isTrustedSource = true;
    sensorData.event.id = requestId;
    sensorData.event.type = PSS_STATUS_NOOP;
    sensorData.event.severity = (pssStatus.mode == ERROR) ? CRITICAL : OPERATIONAL;
    sensorData.event.timestamp = timestampNs;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSS_DAEMON");
    sensorData.event.cause = CAUSE_NONE;
}

/* One presence-report word: a PSS_PRESENCE_* set in bits 0..15 and an
 * input-state set (PSS_INPUT_STATE_MASK layout) in bits 16..31. */
static inline uint32_t NvPSSPackPresenceWord(uint32_t presenceSet, uint32_t inputStateSet)
{
    return (presenceSet & 0x0000FFFFU) |
           ((inputStateSet & PSS_INPUT_STATE_MASK) << PSS_INPUT_STATE_SHIFT);
}

/* Make a PSS no-op the presence report: CAUSE_SOURCE_PRESENCE with
 * the PSS_PRESENCE_* set present now in objectID[0] and the set PSS expects in
 * objectID[1], each with its input-state set packed above it
 * (NvPSSPackPresenceWord: unusable with present, confirmed-usable with expected).
 * All are sampled by the CALLER, and the caller must sample them when
 * the DecisionRequest is BUILT -- not when the no-op was queued -- so a report can
 * never be older than a loss or an escalation PSS has already raised. */
static inline void NvPSSStampSourcePresence(FusedSafetyEvent* event, uint32_t presenceMask,
                                            uint32_t expectedMask)
{
    if (event == nullptr)
        return;
    event->cause = CAUSE_SOURCE_PRESENCE;
    event->fusionMetadata.objectID[0] = presenceMask;
    event->fusionMetadata.objectID[1] = expectedMask;
}

static inline void NvPSSBuildStatusNoopFusedEvent(FusedSafetyEvent* event,
                                                  uint32_t eventId,
                                                  uint64_t timestampNs,
                                                  uint32_t presenceMask,
                                                  uint32_t expectedMask)
{
    if (event == nullptr)
        return;

    std::memset(event, 0, sizeof(*event));
    event->id = eventId;
    event->type = PSS_STATUS_NOOP;
    event->severity = OPERATIONAL;
    event->timestamp = timestampNs;
    event->confidenceLevel = 1.0F;
    event->status = PASSTHROUGH;
    event->fusionMetadata.objectType[0] = OBJECT;
    event->fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(event->sensorIdentifier,
                        sizeof(event->sensorIdentifier),
                        "%s", "PSS_DAEMON");
    NvPSSStampSourcePresence(event, presenceMask, expectedMask);
}

#endif /* NVPSS_STATUS_NOOP_HPP */
