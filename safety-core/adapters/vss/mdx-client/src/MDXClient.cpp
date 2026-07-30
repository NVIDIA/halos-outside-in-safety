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
#include <cstdlib>
#include <cstdio>
#include <set>
#include "common.hpp"
#include "EventMappingValidation.hpp"
#include "MDXClient.hpp"
#include "NvPSFMsgBus.h"
#include "NvPSFMsgCodec.h"
#include "EventsParser.hpp"
#include "FramesParser.hpp"
#include "SafetyEventReporter.hpp"
#include "NvPSB.h"

namespace MDXClient {

static std::atomic<bool> s_stopRequested{false};

static std::string getConfigRuleStringField(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_STRING && result.data.s != nullptr) {
        std::string value(result.data.s);
        free(const_cast<char*>(result.data.s));
        return value;
    }
    return std::string();
}

static double getConfigRuleDoubleField(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type == NvPSF_VALUE_DOUBLE) {
        return result.data.d;
    }
    if (result.type == NvPSF_VALUE_FLOAT) {
        return static_cast<double>(result.data.f);
    }
    return 0.0;
}

static bool getConfigRuleOptionalBoolField(const NvPSFMsgCodecMsg* rule,
                                           const char* field, bool* value) {
    if (rule == nullptr || field == nullptr || value == nullptr ||
        !NvPSFMsgCodecGetFieldPresence(rule, field)) {
        return false;
    }
    const NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(rule, field);
    if (result.type != NvPSF_VALUE_BOOL) {
        return false;
    }
    *value = result.data.b;
    return true;
}

static bool validateEventMappingConfig(const NvPSFMsgCodecMsg* config) {
    if (config == nullptr) {
        return false;
    }
    const int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    std::set<std::pair<std::string, double>> proximityThresholds;
    std::set<std::string> proximityNoViolationPairs;
    std::set<std::string> proximityRuleIds;
    for (int ruleIndex = 0; ruleIndex < rulesCount; ++ruleIndex) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", ruleIndex);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS ||
            rule == nullptr) {
            if (rule != nullptr) {
                NvPSFMsgCodecFreeMsg(rule);
            }
            NvPSBWriteData(NVPSB_LOG_ERR, "Invalid event mapping rule", "");
            return false;
        }
        const std::string restrictedFilter = getConfigRuleStringField(
            rule, "restricted_area_violation");
        const std::string confinedFilter = getConfigRuleStringField(
            rule, "confined_area_violation");
        const std::string socialFilter = getConfigRuleStringField(
            rule, "social_distancing_violation");
        const std::string messageSource = getConfigRuleStringField(rule, "message_source");
        const std::string alertType = getConfigRuleStringField(rule, "alert_type");
        const std::string objectType = getConfigRuleStringField(rule, "object_type");
        const std::string ruleId = getConfigRuleStringField(rule, "rule_id");
        const std::string configuredPrimaryType = getConfigRuleStringField(
            rule, "object_type_primary");
        const std::string configuredSecondaryType = getConfigRuleStringField(
            rule, "object_type_secondary");
        const std::string primaryType = configuredPrimaryType.empty()
            ? objectType : configuredPrimaryType;
        const double distanceThreshold = getConfigRuleDoubleField(
            rule, "distance_threshold_meters");
        const bool distanceThresholdPresent = NvPSFMsgCodecGetFieldPresence(
            rule, "distance_threshold_meters");
        bool proximityViolation = false;
        const bool proximityViolationPresent = getConfigRuleOptionalBoolField(
            rule, "proximity_violation", &proximityViolation);
        const bool hasViolationFilter = !restrictedFilter.empty() ||
                                        !confinedFilter.empty() ||
                                        !socialFilter.empty();
        const bool isPairRule = isProximityPairRule(
            alertType, configuredPrimaryType, configuredSecondaryType,
            proximityViolationPresent, distanceThresholdPresent);
        const bool hasValidPairRule = !isPairRule ||
            isValidProximityPairRule(messageSource, alertType, primaryType,
                                     configuredSecondaryType, proximityViolationPresent,
                                     proximityViolation, distanceThresholdPresent,
                                     distanceThreshold, hasViolationFilter);
        const bool hasValidPairRuleId = !isPairRule ||
            isValidProximityPairRuleId(ruleId);
        bool hasUniquePairRule = true;
        bool hasUniquePairRuleId = true;
        if (hasValidPairRule && hasValidPairRuleId && isPairRule) {
            hasUniquePairRule = insertUniqueProximityPairRule(
                &proximityThresholds, &proximityNoViolationPairs, primaryType,
                configuredSecondaryType, proximityViolation, distanceThreshold);
            if (hasUniquePairRule) {
                hasUniquePairRuleId = insertUniqueProximityPairRuleId(
                    &proximityRuleIds, ruleId);
            }
        }
        const bool valid = isValidViolationFilter(restrictedFilter) &&
                           isValidViolationFilter(confinedFilter) &&
                           isValidViolationFilter(socialFilter) &&
                           isValidViolationRuleScope(restrictedFilter, confinedFilter,
                                                     socialFilter, messageSource, alertType) &&
                           isValidRoiClearObjectType(restrictedFilter, confinedFilter,
                                                     objectType) &&
                           hasValidPairRule && hasValidPairRuleId &&
                           hasUniquePairRule && hasUniquePairRuleId;
        NvPSFMsgCodecFreeMsg(rule);
        if (!valid) {
            NvPSBWriteData(NVPSB_LOG_ERR, "Rejected unsafe event mapping rule", "");
            return false;
        }
    }
    return true;
}

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
        if (!validateEventMappingConfig(config_)) {
            NvPSFMsgCodecFreeMsg(config_);
            config_ = nullptr;
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
                reporter_.reportAlert(alert, config_);
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
            auto alerts = framesParser_.parseFramesMessage(msgBuf, config_);
            for (const auto& alert : alerts)
                reporter_.reportAlert(alert, config_);
        }
    }
};

} // namespace MDXClient

int launchMDXClient(const std::string& config_path, const std::string& sensor_config_path,
                    bool debug_mode, const std::string& broker_override) {
    MDXClient::MDXClientImpl client(config_path, sensor_config_path, debug_mode, broker_override);
    return client.run();
}
