/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <climits>
#include "common.hpp"
#include "MDXClient.hpp"
#include "MDXSharedState.hpp"
#include "NvPSFMsgBus.h"
#include "NvPSFMsgCodec.h"
#include "EventsParser.hpp"
#include "FramesParser.hpp"
#include "SafetyEventReporter.hpp"
#include "NvPSB.h"

namespace MDXClient {

static std::atomic<bool> s_stopRequested{false};

static void mdxSignalHandler(int sig) {
    (void)sig;
    s_stopRequested.store(true);
}

class MDXClientImpl {
public:
    explicit MDXClientImpl(const std::string& configPath, const std::string& sensorConfigPath,
                           bool debugMode = false, const std::string& brokerOverride = "")
        : configPath_(configPath), sensorConfigPath_(sensorConfigPath),
          debugMode_(debugMode),
          eventsParser_([this]() { return nextEventId(); }),
          framesParser_([this]() { return nextEventId(); }, debugMode) {
        if (!brokerOverride.empty())
            brokers_ = brokerOverride;
    }

    bool loadConfig() {
        if (NvPSFMsgCodecDecodeFromFile(configPath_.c_str(), NvPSF_MSG_EVENT_MAPPING, &config_) != NvPSFMSGCODEC_SUCCESS) {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to load config file: " + configPath_, "");
            return false;
        }
        int rulesCount = NvPSFMsgCodecGetRepeatedCount(config_, "rules");
        NvPSBWriteData(NVPSB_LOG_INFO, "Loaded event mapping config, " +
            std::to_string(rulesCount) + " rules", "");
        return true;
    }

    int run() {
        s_stopRequested.store(false);
        if (std::signal(SIGINT, mdxSignalHandler) == SIG_ERR)
            std::cerr << "Failed to register SIGINT handler\n";
        if (std::signal(SIGTERM, mdxSignalHandler) == SIG_ERR)
            std::cerr << "Failed to register SIGTERM handler\n";

        if (NvPSBInitialize("nv_mdx_client", NVPSB_PSS_SOURCE) != NVPSB_SUCCESS) {
            std::cerr << "Failed to initialize PSB" << std::endl;
            return 1;
        }

        if (!loadConfig() || NvPSFMsgCodecGetRepeatedCount(config_, "rules") == 0) {
            NvPSBWriteData(NVPSB_LOG_ERR, "No rules in config or load failed", "");
            NvPSBExit();
            return 1;
        }
        reporter_.setSensorConfigPath(sensorConfigPath_);
        if (!reporter_.init(debugMode_, s_stopRequested)) {
            NvPSBExit();
            return 1;
        }
        if (!connectMsgBus()) {
            cleanup();
            return 1;
        }
        runLoops();
        cleanup();
        return 0;
    }

private:
    std::string configPath_;
    std::string sensorConfigPath_;
    bool debugMode_;
    std::string brokers_ = "localhost:9092";
    NvPSFMsgCodecMsg* config_ = nullptr;
    std::atomic<uint32_t> eventCounter_{0};

    NvPSFMsgBusHandle* eventsConsumer_ = nullptr;
    NvPSFMsgBusHandle* framesConsumer_ = nullptr;
    EventsParser eventsParser_;
    FramesParser framesParser_;
    SafetyEventReporter reporter_;
    SharedState state_;

    uint32_t nextEventId() {
        if (eventCounter_.load(std::memory_order_relaxed) < UINT32_MAX)
            return eventCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
        eventCounter_.store(1, std::memory_order_relaxed);
        return 1;
    }

    bool connectMsgBus() {
        NvPSFMsgBusStatus status = NvPSFMsgBusCreate(brokers_.c_str(), "mdx-events",
            MSGBUS_CONSUMER, "mdx_client_events", &eventsConsumer_);
        if (status.err != NvPSFMSGBUS_SUCCESS || !eventsConsumer_) {
            NvPSBWriteData(NVPSB_LOG_ERR, "MsgBus mdx-events consumer creation failed", "");
            if (eventsConsumer_) {
                NvPSFMsgBusDestroy(eventsConsumer_);
                eventsConsumer_ = nullptr;
            }
            return false;
        }
        NvPSBWriteData(NVPSB_LOG_INFO, "MsgBus consumer initialized for mdx-events", "");

        status = NvPSFMsgBusCreate(brokers_.c_str(), "mdx-frames",
            MSGBUS_CONSUMER, "mdx_client_frames", &framesConsumer_);
        if (status.err != NvPSFMSGBUS_SUCCESS || !framesConsumer_) {
            NvPSBWriteData(NVPSB_LOG_ERR, "MsgBus mdx-frames consumer creation failed", "");
            NvPSFMsgBusDestroy(eventsConsumer_);
            eventsConsumer_ = nullptr;
            return false;
        }
        NvPSBWriteData(NVPSB_LOG_INFO, "MsgBus consumer initialized for mdx-frames", "");
        return true;
    }

    void disconnectMsgBus() {
        if (eventsConsumer_) { NvPSFMsgBusDestroy(eventsConsumer_); eventsConsumer_ = nullptr; }
        if (framesConsumer_) { NvPSFMsgBusDestroy(framesConsumer_); framesConsumer_ = nullptr; }
    }

    void cleanup() {
        reporter_.shutdown();
        disconnectMsgBus();
        if (config_) { NvPSFMsgCodecFreeMsg(config_); config_ = nullptr; }
        NvPSBExit();
        NvPSFMsgCodecShutdown();
    }

    void runLoops() {
        std::thread eventsThread(&MDXClientImpl::eventsLoop, this);
        std::thread framesThread(&MDXClientImpl::framesLoop, this);
        eventsThread.join();
        framesThread.join();
    }

    void eventsLoop() {
        char buf[MSGBUS_MSG_BUFFER_SIZE];
        size_t len = 0;
        std::string msgBuf;
        msgBuf.reserve(MSGBUS_MSG_BUFFER_SIZE);
        while (!s_stopRequested.load()) {
            NvPSFMsgBusStatus status = NvPSFMsgBusReceive(eventsConsumer_, buf, sizeof(buf), &len);
            if (status.err != NvPSFMSGBUS_SUCCESS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (len == 0) continue;
            if (len > sizeof(buf)) {
                NvPSBWriteData(NVPSB_LOG_ERR, "mdx-events message length exceeds buffer size, skipping", "");
                continue;
            }
            msgBuf.assign(buf, len);
            auto alerts = eventsParser_.parseEventsMessage(msgBuf);
            for (const auto& alert : alerts)
                reporter_.reportAlert(alert, config_, state_);
        }
    }

    void framesLoop() {
        char buf[MSGBUS_MSG_BUFFER_SIZE];
        size_t len = 0;
        std::string msgBuf;
        msgBuf.reserve(MSGBUS_MSG_BUFFER_SIZE);
        while (!s_stopRequested.load()) {
            NvPSFMsgBusStatus status = NvPSFMsgBusReceive(framesConsumer_, buf, sizeof(buf), &len);
            if (status.err != NvPSFMSGBUS_SUCCESS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (len == 0) continue;
            if (len > sizeof(buf)) {
                NvPSBWriteData(NVPSB_LOG_ERR, "mdx-frames message length exceeds buffer size, skipping", "");
                continue;
            }
            msgBuf.assign(buf, len);
            auto alerts = framesParser_.parseFramesMessage(msgBuf, config_, state_);
            for (const auto& alert : alerts)
                reporter_.reportAlert(alert, config_, state_);
        }
    }
};

} // namespace MDXClient

int launchMDXClient(const std::string& config_path, const std::string& sensor_config_path,
                    bool debug_mode, const std::string& broker_override) {
    MDXClient::MDXClientImpl client(config_path, sensor_config_path, debug_mode, broker_override);
    return client.run();
}
