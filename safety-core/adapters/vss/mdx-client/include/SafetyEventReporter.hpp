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
#include "pss_daemon.h"
#include "NvPSFMsgCodec.h"

namespace MDXClient {

struct SharedState;

class SafetyEventReporter {
    static constexpr uint32_t kMaxHbAckFailures = 10;

public:
    SafetyEventReporter() = default;
    ~SafetyEventReporter() = default;

    SafetyEventReporter(const SafetyEventReporter&) = delete;
    SafetyEventReporter& operator=(const SafetyEventReporter&) = delete;

    void setSensorConfigPath(const std::string& path);
    bool init(bool debugMode, std::atomic<bool>& stopFlag);
    void shutdown();

    bool reportAlert(const AlertMessage& alertMsg,
                     const NvPSFMsgCodecMsg* config,
                     SharedState& state);

private:
    uint32_t pssClientId_ = 0;
    bool pssRegistered_ = false;
    bool debugMode_ = false;
    std::string sensorConfigPath_;
    std::unordered_map<std::string, uint8_t> sensorNameToPid_;
    std::thread heartbeatThread_;
    std::atomic<bool> heartbeatRunning_{false};
    std::atomic<bool>* stopFlag_ = nullptr;

    void heartbeatLoop();
    static void printSafetyEvent(std::ostream& out, const SafetyEvent& e,
                                 const NvPSFMsgCodecMsg* rule);
};

} // namespace MDXClient

#endif
