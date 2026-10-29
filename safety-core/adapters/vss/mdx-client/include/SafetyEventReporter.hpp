/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_SAFETY_EVENT_REPORTER_HPP
#define MDX_CLIENT_SAFETY_EVENT_REPORTER_HPP

#include <string>
#include <atomic>
#include <thread>
#include <iostream>
#include <unordered_map>
#include "common.hpp"
#include "EventMapping.hpp"
#include "pss_daemon.h"

namespace MDXClient {

inline Coordinate selectCommandLocationCoordinate(const AlertMessage& alert)
{
    Coordinate coordinate = {};
    if (alert.coordCount <= 0) {
        return coordinate;
    }
    int boundedCount = alert.coordCount;
    if (boundedCount > MAX_COORDINATES_COUNT) {
        boundedCount = MAX_COORDINATES_COUNT;
    }
    return alert.coordinates[boundedCount - 1];
}

class SafetyEventReporter {
    static constexpr uint32_t kMaxHbAckFailures = 10;

public:
    SafetyEventReporter() = default;
    ~SafetyEventReporter() = default;

    SafetyEventReporter(const SafetyEventReporter&) = delete;
    SafetyEventReporter& operator=(const SafetyEventReporter&) = delete;

    /*
     * Which file the Kafka sensor id -> pipelineID map is built from. Exactly
     * one, never both, and the exclusivity is a safety property rather than a
     * convenience.
     *
     * Holding both maps at once would let a stray per-camera event in a 3D
     * deployment resolve against a camera's pipelineID and be reported as
     * though it were a decision-driving identity -- gated against ONE camera's
     * health instead of the group's. With one map per mode a topology mismatch
     * is simply unresolvable, and the drop in reportAlert (no pipelineID, no
     * event) is what makes it safe.
     *
     *   CameraConfig    sensor_config.conf           2D: the cameras ARE the
     *                   (pipelineId, name, rtspUrl)      event sources.
     *   PipelinesConfig sensor_pipelines_config.conf 3D: only the BEV group
     *                   (pipelineId, name)               appears, because that
     *                                                    is what Kafka reports.
     */
    enum class IdentitySource { CameraConfig, PipelinesConfig };

    void setSensorIdentityConfig(const std::string& path, IdentitySource source);
    /* PSS instance id. 0 registers anonymously; 1..PSS_MDX_INSTANCE_MAX
     * makes this client identifiable, so PSS reports its loss and only its return
     * clears it. Must be set before init(). */
    void setPssInstanceId(uint8_t instanceId) { pssInstanceId_ = instanceId; }
    bool init(bool debugMode, std::atomic<bool>& stopFlag);
    void shutdown();

    bool reportAlert(const AlertMessage& alertMsg, const EventMapping& mapping);

private:
    uint32_t pssClientId_ = 0;
    uint8_t pssInstanceId_ = 0;
    bool pssRegistered_ = false;
    bool debugMode_ = false;
    std::string sensorConfigPath_;
    IdentitySource identitySource_ = IdentitySource::CameraConfig;
    /* Kafka sensor id -> pipelineID. Built from exactly one file; a name absent
     * from it has no pipelineID and its events are dropped rather than being
     * given a default. */
    std::unordered_map<std::string, uint8_t> sensorNameToPid_;
    std::thread heartbeatThread_;
    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool>* stopFlag_ = nullptr;

    void heartbeatLoop();
    static void printSafetyEvent(std::ostream& out, const SafetyEvent& e,
                                 const EventMappingRule& rule);
};

} // namespace MDXClient

#endif
