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
    (void)std::snprintf(sensorData.event.ruleIdentifier,
                        sizeof(sensorData.event.ruleIdentifier),
                        "%s", "PSS_STATUS_NOOP");
}

static inline void NvPSSBuildStatusNoopFusedEvent(FusedSafetyEvent* event,
                                                  uint32_t eventId,
                                                  uint64_t timestampNs)
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
    (void)std::snprintf(event->ruleIdentifier,
                        sizeof(event->ruleIdentifier),
                        "%s", "PSS_STATUS_NOOP");
}

#endif /* NVPSS_STATUS_NOOP_HPP */
