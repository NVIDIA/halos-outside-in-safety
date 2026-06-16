/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>

#include "SafetyEventReporter.hpp"
#include "MDXSharedState.hpp"
#include "sensor_config_parser.h"
#include "pss_daemon.h"
#include "NvPSB.h"

namespace MDXClient {

/* Helper: get string field from codec sub-message handle. Returns empty string if not present. */
static std::string getRuleStringField(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(rule, field);
    if (r.type == NvPSF_VALUE_STRING && r.data.s) {
        std::string s(r.data.s);
        free((void*)r.data.s);
        return s;
    }
    return std::string();
}

static double getRuleDoubleField(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(rule, field);
    if (r.type == NvPSF_VALUE_DOUBLE) return r.data.d;
    if (r.type == NvPSF_VALUE_FLOAT) return (double)r.data.f;
    return 0.0;
}

static int32_t getRuleInt32Field(const NvPSFMsgCodecMsg* rule, const char* field) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(rule, field);
    if (r.type == NvPSF_VALUE_INT32) return r.data.i32;
    return 0;
}

static bool stringEqualsCaseInsensitive(const std::string& a, const char* b) {
    if (!b) return false;
    std::string bStr(b);
    return std::equal(a.begin(), a.end(), bStr.begin(), bStr.end(),
        [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

static bool parseViolationFilter(const std::string& ruleVal, bool alertVal) {
    if (ruleVal.empty() || ruleVal == "any") return true;
    if (ruleVal == "true")  return alertVal == true;
    if (ruleVal == "false") return alertVal == false;
    return true;
}

static bool ruleMatches(const NvPSFMsgCodecMsg* rule, const AlertMessage& alert) {
    double distThresh = getRuleDoubleField(rule, "distance_threshold_meters");
    if (distThresh > 0.0) {
        if (strcmp(alert.type, "social_distancing") != 0)
            return false;
        char expectedRuleId[64];
        snprintf(expectedRuleId, sizeof(expectedRuleId), "proximity_%.1f", distThresh);
        std::string r(alert.ruleId);
        bool idMatch = stringEqualsCaseInsensitive(r, expectedRuleId)
            || (r.size() > strlen(expectedRuleId) && r.compare(0, strlen(expectedRuleId), expectedRuleId) == 0 && r[strlen(expectedRuleId)] == ':');
        if (!idMatch) return false;
    }
    std::string msgSource = getRuleStringField(rule, "message_source");
    if (!msgSource.empty() && alert.messageSource[0] != '\0') {
        if (!stringEqualsCaseInsensitive(msgSource, alert.messageSource))
            return false;
    }
    std::string alertType = getRuleStringField(rule, "alert_type");
    if (!alertType.empty() && alert.type[0] != '\0') {
        if (!stringEqualsCaseInsensitive(alertType, alert.type))
            return false;
    }
    std::string eventType = getRuleStringField(rule, "event_type");
    if (!eventType.empty() && alert.eventType[0] != '\0') {
        if (!stringEqualsCaseInsensitive(eventType, alert.eventType))
            return false;
    }
    std::string objectType = getRuleStringField(rule, "object_type");
    if (!objectType.empty() && alert.object.type[0] != '\0') {
        if (!stringEqualsCaseInsensitive(objectType, alert.object.type))
            return false;
    }
    std::string ruleId = getRuleStringField(rule, "rule_id");
    if (!ruleId.empty() && alert.ruleId[0] != '\0') {
        std::string alertRuleId(alert.ruleId);
        bool idMatch = stringEqualsCaseInsensitive(ruleId, alertRuleId.c_str())
            || (alertRuleId.size() > ruleId.size() && alertRuleId.compare(0, ruleId.size(), ruleId) == 0 && alertRuleId[ruleId.size()] == ':');
        if (!idMatch) return false;
    }
    std::string restrictedFilter = getRuleStringField(rule, "restricted_area_violation");
    if (!parseViolationFilter(restrictedFilter, alert.restrictedAreaViolation))
        return false;
    std::string confinedFilter = getRuleStringField(rule, "confined_area_violation");
    if (!parseViolationFilter(confinedFilter, alert.confinedAreaViolation))
        return false;
    std::string sdFilter = getRuleStringField(rule, "social_distancing_violation");
    if (!parseViolationFilter(sdFilter, alert.socialDistancingViolation))
        return false;
    return true;
}

static EventType stringToEventType(const std::string& s) {
    static const std::map<std::string, EventType> kEventTypeMap = {
        {"EVENT_0", EVENT_0},   {"EVENT_1", EVENT_1},   {"EVENT_2", EVENT_2},   {"EVENT_3", EVENT_3},
        {"EVENT_4", EVENT_4},   {"EVENT_5", EVENT_5},   {"EVENT_6", EVENT_6},   {"EVENT_7", EVENT_7},
        {"EVENT_8", EVENT_8},   {"EVENT_9", EVENT_9},   {"EVENT_10", EVENT_10}, {"EVENT_11", EVENT_11},
        {"EVENT_12", EVENT_12}, {"EVENT_13", EVENT_13}, {"EVENT_14", EVENT_14}, {"EVENT_15", EVENT_15},
        {"EVENT_16", EVENT_16}, {"EVENT_17", EVENT_17}, {"EVENT_18", EVENT_18}, {"EVENT_19", EVENT_19},
        {"EVENT_20", EVENT_20}, {"EVENT_21", EVENT_21}, {"EVENT_22", EVENT_22},
        {"ROI_ENTRY", ROI_ENTRY}, {"ROI_EXIT", ROI_EXIT},
        {"TW_CROSSING_ENTRY", TW_CROSSING_ENTRY}, {"TW_CROSSING_EXIT", TW_CROSSING_EXIT},
        {"SW_FAIL", SW_FAIL},
    };
    auto it = kEventTypeMap.find(s);
    return (it != kEventTypeMap.end()) ? it->second : EVENT_UNKNOWN;
}

static SeverityLevel stringToSeverity(const std::string& s) {
    if (s == "LOW") return LOW;
    if (s == "MEDIUM") return MEDIUM;
    if (s == "HIGH") return HIGH;
    if (s == "CRITICAL") return CRITICAL;
    return MEDIUM;
}

static ObjectType stringToObjectType(const std::string& s) {
    if (s == "person" || s == "Person") return PERSON;
    if (s == "vehicle" || s == "Vehicle") return VEHICLE;
    return OBJECT;
}

/*
 * Convert a UTC ISO-8601 timestamp ("YYYY-MM-DDTHH:MM:SS.mmmZ") to
 * CLOCK_MONOTONIC nanoseconds. Downstream consumers (isEventStale,
 * ProcessUnmatchedEvents, and the PSS daemon) compare event timestamps
 * against CLOCK_MONOTONIC, so we translate each wall-clock stamp through a
 * one-shot REALTIME→MONOTONIC offset captured on first call.
 *
 * Returns 0 on unrecoverable failure (null/malformed string, impossible date,
 * or a stamp so old it predates this box's monotonic epoch).
 */
static uint64_t stringToNS(const char* ts) {
    if (!ts || ts[0] == '\0') return 0;

    int y, mo, d, h, mi, s, ms = 0;
    if (sscanf(ts, "%d-%d-%dT%d:%d:%d.%dZ",
               &y, &mo, &d, &h, &mi, &s, &ms) < 7) {
        return 0;
    }
    if (ms < 0 || ms > 999) ms = 0;

    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = s;
    const time_t sec = timegm(&t);
    if (sec < 0) return 0;

    /* Overflow guard on sec*1e9 + ms*1e6. Worst-case ms contributes 999e6,
     * so sec*1e9 must fit in UINT64_MAX - 999e6. */
    static constexpr uint64_t kNsPerSec   = 1000000000ULL;
    static constexpr uint64_t kNsPerMs    = 1000000ULL;
    static constexpr uint64_t kMaxSafeSec = (UINT64_MAX - 999U * kNsPerMs) / kNsPerSec;
    if (static_cast<uint64_t>(sec) > kMaxSafeSec) return 0;

    const uint64_t utcNs = static_cast<uint64_t>(sec) * kNsPerSec
                         + static_cast<uint64_t>(ms)  * kNsPerMs;

    /* One-shot REALTIME/MONOTONIC calibration, captured back-to-back so the
     * offset is as close to simultaneous as the two syscalls allow. */
    static std::once_flag calibrationOnce;
    static uint64_t baseUtcNs  = 0;
    static uint64_t baseMonoNs = 0;
    std::call_once(calibrationOnce, []() {
        struct timespec tsReal, tsMono;
        clock_gettime(CLOCK_REALTIME,  &tsReal);
        clock_gettime(CLOCK_MONOTONIC, &tsMono);
        baseUtcNs  = static_cast<uint64_t>(tsReal.tv_sec) * kNsPerSec
                   + static_cast<uint64_t>(tsReal.tv_nsec);
        baseMonoNs = static_cast<uint64_t>(tsMono.tv_sec) * kNsPerSec
                   + static_cast<uint64_t>(tsMono.tv_nsec);
    });

    /* Translate in either direction so backlog events processed at startup —
     * whose wall-clock predates our calibration instant — still map to a real
     * monotonic stamp instead of being dropped. The offset is a linear
     * REALTIME→MONOTONIC shift; going backwards is valid as long as we don't
     * underflow past baseMonoNs (i.e. a stamp from before this box booted,
     * which is not a meaningful input). */
    if (utcNs >= baseUtcNs) {
        const uint64_t delta = utcNs - baseUtcNs;
        if (delta > UINT64_MAX - baseMonoNs) return 0;
        return baseMonoNs + delta;
    }
    const uint64_t delta = baseUtcNs - utcNs;
    if (delta > baseMonoNs) return 0;
    return baseMonoNs - delta;
}

// ---------------------------------------------------------------------------
// SafetyEventReporter
// ---------------------------------------------------------------------------

void SafetyEventReporter::setSensorConfigPath(const std::string& path) {
    sensorConfigPath_ = path;
}

bool SafetyEventReporter::init(bool debugMode, std::atomic<bool>& stopFlag) {
    debugMode_ = debugMode;
    stopFlag_ = &stopFlag;

    if (!sensorConfigPath_.empty()) {
        std::string cfgErr;
        auto entries = sensorConfigLoad(sensorConfigPath_, &cfgErr);
        if (entries.empty()) {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to load sensor config: " + cfgErr, "");
            return false;
        }
        sensorNameToPid_ = sensorConfigNameToIdMap(entries);
        NvPSBWriteData(NVPSB_LOG_INFO, "Loaded sensor config with " +
            std::to_string(sensorNameToPid_.size()) + " sensors", "");
    }

    if (!debugMode_) {
        if (NvPSSRegisterPSSClient(&pssClientId_, CLIENT_MDX) != NVPSSD_SUCCESS) {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register PSS client", "");
            return false;
        }
        pssRegistered_ = true;
        heartbeatRunning_.store(true);
        heartbeatThread_ = std::thread(&SafetyEventReporter::heartbeatLoop, this);
    } else {
        std::cout << "[DEBUG] MDX client running in debug mode: events will be printed, not reported to PSS" << std::endl;
    }
    return true;
}

void SafetyEventReporter::shutdown() {
    heartbeatRunning_.store(false);
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
    if (!debugMode_ && pssRegistered_) NvPSSTerminatePSSClient(pssClientId_);
}

void SafetyEventReporter::heartbeatLoop() {
    uint32_t consecutiveFailures = 0;
    while (heartbeatRunning_.load() && !(stopFlag_ && stopFlag_->load())) {
        if (NvPSSSendHeartbeat(pssClientId_, CLIENT_MDX) != NVPSSD_SUCCESS) {
            if (consecutiveFailures < UINT32_MAX)
                ++consecutiveFailures;
            NvPSBWriteData(NVPSB_LOG_ERR, "Heartbeat failed",
                "miss=" + std::to_string(consecutiveFailures));
            if (consecutiveFailures >= kMaxHbAckFailures) {
                NvPSBWriteData(NVPSB_LOG_ERR,
                    "PSS heartbeat ACK failure limit reached — PSS presumed dead, exiting", "");
                if (stopFlag_)
                    stopFlag_->store(true);
                break;
            }
        } else {
            consecutiveFailures = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(HB_INTERVAL_MS));
    }
}

bool SafetyEventReporter::reportAlert(const AlertMessage& alertMsg,
        const NvPSFMsgCodecMsg* config,
        SharedState& state) {
    int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    const NvPSFMsgCodecMsg* matchedRule = nullptr;
    NvPSFMsgCodecMsg* matchedRuleHandle = nullptr;
    for (int i = 0; i < rulesCount; i++) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS) continue;
        if (ruleMatches(rule, alertMsg)) {
            matchedRuleHandle = rule;
            matchedRule = rule;
            break;
        }
        NvPSFMsgCodecFreeMsg(rule);
    }
    if (!matchedRule) return true;

    std::string sensorIdStr(alertMsg.sensorId);

    std::string matchedMsgSource = getRuleStringField(matchedRule, "message_source");
    int32_t scaleFactor = getRuleInt32Field(matchedRule, "scale_factor");

    if (strcmp(alertMsg.messageSource, "mdx-frames") == 0 &&
        stringEqualsCaseInsensitive(matchedMsgSource, "mdx-frames") &&
        scaleFactor > 1) {
        std::string curSensorId;
        uint64_t curFrameCount = 0;
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            curSensorId = state.currentFrameSensorId;
            curFrameCount = state.currentFrameCount;
        }
        if (curSensorId != sensorIdStr ||
            curFrameCount % static_cast<uint64_t>(scaleFactor) != 0) {
            NvPSFMsgCodecFreeMsg(matchedRuleHandle);
            return true;
        }
    }

    std::string matchedAlertType = getRuleStringField(matchedRule, "alert_type");
    std::string matchedRestrictedFilter = getRuleStringField(matchedRule, "restricted_area_violation");
    std::string matchedConfinedFilter = getRuleStringField(matchedRule, "confined_area_violation");

    bool isRestrictedViolationRule = (matchedMsgSource == "mdx-frames" &&
        matchedAlertType == "roi" && matchedRestrictedFilter == "true");
    bool isRestrictedClearedRule = (matchedMsgSource == "mdx-frames" &&
        matchedAlertType == "restrictedAreaViolationCleared");
    bool isConfinedViolationRule = (matchedMsgSource == "mdx-frames" &&
        matchedAlertType == "roi" && matchedConfinedFilter == "true");
    bool isConfinedClearedRule = (matchedMsgSource == "mdx-frames" &&
        matchedAlertType == "confinedAreaViolationCleared");
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        if (isRestrictedViolationRule) {
            if (state.restrictedViolState[sensorIdStr]) { NvPSFMsgCodecFreeMsg(matchedRuleHandle); return true; }
            state.restrictedViolState[sensorIdStr] = true;
        } else if (isRestrictedClearedRule) {
            if (!state.restrictedViolState[sensorIdStr]) { NvPSFMsgCodecFreeMsg(matchedRuleHandle); return true; }
            state.restrictedViolState[sensorIdStr] = false;
        } else if (isConfinedViolationRule) {
            if (state.confinedViolState[sensorIdStr]) { NvPSFMsgCodecFreeMsg(matchedRuleHandle); return true; }
            state.confinedViolState[sensorIdStr] = true;
        } else if (isConfinedClearedRule) {
            if (!state.confinedViolState[sensorIdStr]) { NvPSFMsgCodecFreeMsg(matchedRuleHandle); return true; }
            state.confinedViolState[sensorIdStr] = false;
        }
    }

    std::string outputEvent = getRuleStringField(matchedRule, "output_event");
    std::string severity = getRuleStringField(matchedRule, "severity");
    std::string ruleName = getRuleStringField(matchedRule, "name");

    SafetyEvent safetyEvent = {};
    /* Pass the producer's endTimestamp through, translated into the
     * CLOCK_MONOTONIC frame the rest of the stack uses. */
    safetyEvent.timestamp = stringToNS(alertMsg.endTimestamp);
    safetyEvent.id = alertMsg.id;
    safetyEvent.confidenceLevel = 0.7f;
    safetyEvent.processed = false;
    safetyEvent.type = stringToEventType(outputEvent);
    safetyEvent.severity = stringToSeverity(severity.empty() ? "MEDIUM" : severity);
    strncpy(safetyEvent.sensorIdentifier, alertMsg.sensorId, MAX_INDENTIFIER_LENGTH - 1);
    safetyEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    strncpy(safetyEvent.ruleIdentifier, alertMsg.ruleId, MAX_INDENTIFIER_LENGTH - 1);
    safetyEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';

    bool isProximityAlert = (strncmp(alertMsg.ruleId, "proximity_", 10) == 0);
    if (isProximityAlert) {
        safetyEvent.fusionMetadata.objectID[0] = alertMsg.objectId;
        safetyEvent.fusionMetadata.objectID[1] = alertMsg.objectId2;
        safetyEvent.fusionMetadata.coordinates[0].x = alertMsg.coordCount > 0 ? alertMsg.coordinates[0].x : 0.f;
        safetyEvent.fusionMetadata.coordinates[0].y = alertMsg.coordCount > 0 ? alertMsg.coordinates[0].y : 0.f;
        safetyEvent.fusionMetadata.coordinates[1].x = alertMsg.coordCount > 1 ? alertMsg.coordinates[1].x : 0.f;
        safetyEvent.fusionMetadata.coordinates[1].y = alertMsg.coordCount > 1 ? alertMsg.coordinates[1].y : 0.f;
        for (int i = 2; i < MAX_TRAJECTORY_COORDINATES; i++) {
            safetyEvent.fusionMetadata.coordinates[i].x = 0.f;
            safetyEvent.fusionMetadata.coordinates[i].y = 0.f;
        }
        safetyEvent.fusionMetadata.objectType[0] = stringToObjectType(alertMsg.object.type);
        safetyEvent.fusionMetadata.objectType[1] = stringToObjectType(alertMsg.object2.type);
    } else {
        safetyEvent.fusionMetadata.objectID[0] = alertMsg.objectId;
        safetyEvent.fusionMetadata.objectID[1] = 0;
        safetyEvent.fusionMetadata.coordinates[0].x = alertMsg.coordCount > 0 ? alertMsg.coordinates[0].x : 0.f;
        safetyEvent.fusionMetadata.coordinates[0].y = alertMsg.coordCount > 0 ? alertMsg.coordinates[0].y : 0.f;
        safetyEvent.fusionMetadata.coordinates[1].x = 0.f;
        safetyEvent.fusionMetadata.coordinates[1].y = 0.f;
        for (int i = 2; i < MAX_TRAJECTORY_COORDINATES; i++) {
            safetyEvent.fusionMetadata.coordinates[i].x = 0.f;
            safetyEvent.fusionMetadata.coordinates[i].y = 0.f;
        }
        std::string ot(alertMsg.object.type);
        ObjectType ot0 = stringToObjectType(ot);
        safetyEvent.fusionMetadata.objectType[0] = ot0;
        safetyEvent.fusionMetadata.objectType[1] = ot0;
    }
    auto pidIt = sensorNameToPid_.find(sensorIdStr);
    if (pidIt != sensorNameToPid_.end()) {
        safetyEvent.fusionMetadata.pipelineID = pidIt->second;
    } else {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "Event from unknown sensor (not in sensor_config): " + sensorIdStr, "");
        safetyEvent.fusionMetadata.pipelineID = 0;
    }
    safetyEvent.fusionMetadata.clientID = static_cast<uint8_t>(pssClientId_);
    safetyEvent.fusionMetadata.speed = alertMsg.speed;

    if (debugMode_) {
        printSafetyEvent(std::cout, safetyEvent, matchedRule);
        NvPSFMsgCodecFreeMsg(matchedRuleHandle);
        return true;
    }
    if (NvPSSReportSafetyEvent(pssClientId_, &safetyEvent) == NVPSSD_SUCCESS) {
        NvPSBWriteData(NVPSB_LOG_INFO, "Safety event reported: " + outputEvent + " (rule: " + ruleName + ")", "");
        NvPSFMsgCodecFreeMsg(matchedRuleHandle);
        return true;
    }
    NvPSBWriteData(NVPSB_LOG_ERR, "Failed to report safety event to PSS", "");
    NvPSFMsgCodecFreeMsg(matchedRuleHandle);
    return false;
}

void SafetyEventReporter::printSafetyEvent(std::ostream& out, const SafetyEvent& e,
        const NvPSFMsgCodecMsg* rule) {
    std::string ruleName = getRuleStringField(rule, "name");
    std::string outputEvent = getRuleStringField(rule, "output_event");
    std::string severity = getRuleStringField(rule, "severity");
    out << "[MDX_DEBUG] Matched rule: \"" << (ruleName.empty() ? "(unnamed)" : ruleName) << "\"\n"
        << "  output_event=" << outputEvent << " severity=" << (severity.empty() ? "MEDIUM" : severity)
        << "\n  SafetyEvent: id=" << e.id << " type=" << e.type << " severity=" << e.severity
        << " sensor=" << e.sensorIdentifier << " rule=" << e.ruleIdentifier
        << " objectID[0]=" << e.fusionMetadata.objectID[0] << " objectID[1]=" << e.fusionMetadata.objectID[1]
        << " timestamp=" << e.timestamp << " confidence=" << e.confidenceLevel << std::endl;
}

} // namespace MDXClient
