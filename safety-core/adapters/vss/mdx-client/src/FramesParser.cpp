/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FramesParser.hpp"
#include "MDXSharedState.hpp"
#include "NvPSB.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <climits>
#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_map>

namespace MDXClient {

static bool stringEqualsCaseInsensitive(const std::string& a, const char* b) {
    if (!b) return false;
    std::string bStr(b);
    return std::equal(a.begin(), a.end(), bStr.begin(), bStr.end(),
        [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

static void formatUtcTimestampInto(char* dest, size_t destSize, const struct tm* utc, int ms) {
    char buf[80];
    int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                     utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                     utc->tm_hour, utc->tm_min, utc->tm_sec, ms);
    if (n > 0 && destSize > 0) {
        size_t copyLen = static_cast<size_t>(n) < destSize ? static_cast<size_t>(n) + 1 : destSize;
        memcpy(dest, buf, copyLen);
        dest[destSize - 1] = '\0';
    }
}

/* Helper: get string field from a sub-message, return empty if not present. Caller must free. */
static std::string getStringField(const NvPSFMsgCodecMsg* msg, const char* path) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_STRING && r.data.s) {
        std::string s(r.data.s);
        free((void*)r.data.s);
        return s;
    }
    return std::string();
}

/* Helper: get float/double field, return 0.0 if not present */
static double getDoubleField(const NvPSFMsgCodecMsg* msg, const char* path) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_DOUBLE) return r.data.d;
    if (r.type == NvPSF_VALUE_FLOAT) return (double)r.data.f;
    return 0.0;
}

static float getFloatField(const NvPSFMsgCodecMsg* msg, const char* path) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_FLOAT) return r.data.f;
    if (r.type == NvPSF_VALUE_DOUBLE) return (float)r.data.d;
    return 0.0f;
}

static int32_t getInt32Field(const NvPSFMsgCodecMsg* msg, const char* path) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_INT32) return r.data.i32;
    return 0;
}

/* Helper: fill timestamp in AlertMessage from a frame message handle */
static void fillTimestampFromFrame(const NvPSFMsgCodecMsg* frameMsg, AlertMessage& alertMsg) {
    if (!NvPSFMsgCodecGetFieldPresence(frameMsg, "timestamp")) return;
    NvPSFMsgCodecFieldResult ts = NvPSFMsgCodecGetField(frameMsg, "timestamp");
    if (ts.type == NvPSF_VALUE_TIMESTAMP) {
        time_t sec = ts.data.timestamp.seconds;
        int ms = ts.data.timestamp.nanos / 1000000;
        struct tm tm_buf;
        struct tm* utc = gmtime_r(&sec, &tm_buf);
        if (utc)
            formatUtcTimestampInto(alertMsg.endTimestamp, sizeof(alertMsg.endTimestamp), utc, ms);
    }
}

static uint32_t parseObjectIdToU32(const std::string& idStr) {
    if (idStr.empty()) return 0;
    const char* p = idStr.c_str();
    if (strncmp(p, "idx_", 4) == 0) p += 4;
    char* end = nullptr;
    unsigned long v = strtoul(p, &end, 10);
    if (end && *end == '\0' && v <= UINT32_MAX) return static_cast<uint32_t>(v);
    return 0;
}

static std::string resolveObjectIdsToTypes(const NvPSFMsgCodecMsg* frameMsg, const std::string& commaSeparatedIds) {
    /* Build id->type map once up front to avoid nested iteration */
    std::unordered_map<std::string, std::string> idToType;
    int objCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    for (int i = 0; i < objCount; i++) {
        NvPSFMsgCodecMsg* obj = nullptr;
        char objPath[64];
        snprintf(objPath, sizeof(objPath), "objects[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, objPath, &obj) == NvPSFMSGCODEC_SUCCESS) {
            std::string objId = getStringField(obj, "id");
            std::string objType = getStringField(obj, "type");
            NvPSFMsgCodecFreeMsg(obj);
            if (!objId.empty()) idToType[objId] = objType;
        }
    }

    std::ostringstream out;
    std::istringstream in(commaSeparatedIds);
    std::string idStr;
    bool first = true;
    while (std::getline(in, idStr, ',')) {
        while (!idStr.empty() && (idStr.back() == ' ' || idStr.back() == '\t')) idStr.pop_back();
        size_t s = 0;
        while (s < idStr.size() && (idStr[s] == ' ' || idStr[s] == '\t')) s++;
        if (s) idStr = idStr.substr(s);
        if (idStr.empty()) continue;
        auto it = idToType.find(idStr);
        if (!first) out << ',';
        out << (it != idToType.end() ? it->second : "?");
        first = false;
    }
    return out.str();
}

static bool bothObjectTypesPresentInFrame(const NvPSFMsgCodecMsg* frameMsg,
        const std::string& primType, const std::string& secType) {
    bool hasPrimary = false, hasSecondary = false;
    int objCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    for (int i = 0; i < objCount; i++) {
        NvPSFMsgCodecMsg* obj = nullptr;
        char objPath[64];
        snprintf(objPath, sizeof(objPath), "objects[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, objPath, &obj) != NvPSFMSGCODEC_SUCCESS) continue;
        std::string t = getStringField(obj, "type");
        NvPSFMsgCodecFreeMsg(obj);
        if (stringEqualsCaseInsensitive(primType, t.c_str())) hasPrimary = true;
        if (stringEqualsCaseInsensitive(secType, t.c_str())) hasSecondary = true;
        if (hasPrimary && hasSecondary) return true;
    }
    return hasPrimary && hasSecondary;
}

static std::string getProximityViolationObjectIdsFromFrame(const NvPSFMsgCodecMsg* frameMsg) {
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(frameMsg, "socialDistancing.info['proximityViolationObjects']");
    if (r.type == NvPSF_VALUE_MAPVALUE && r.data.mapValue && r.data.mapValue[0] != '\0') {
        std::string s(r.data.mapValue);
        free((void*)r.data.mapValue);
        return s;
    }
    if (r.type == NvPSF_VALUE_MAPVALUE && r.data.mapValue) free((void*)r.data.mapValue);
    return std::string();
}

// ---------------------------------------------------------------------------
// FramesParser
// ---------------------------------------------------------------------------

FramesParser::FramesParser(NextEventIdFn nextEventId, bool debugMode)
    : nextEventId_(std::move(nextEventId)), debugMode_(debugMode) {}

std::vector<AlertMessage> FramesParser::parseFramesMessage(const std::string& data,
        const NvPSFMsgCodecMsg* config, SharedState& state) {
    std::vector<AlertMessage> alerts;
    NvPSFMsgCodecMsg* frameMsg = nullptr;
    if (NvPSFMsgCodecDecode(data.data(), data.size(), NvPSF_MSG_FRAME, &frameMsg) != NvPSFMSGCODEC_SUCCESS) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to decode frames message", "");
        return alerts;
    }
    std::string sensorIdStr = getStringField(frameMsg, "sensorId");
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        state.currentFrameSensorId = sensorIdStr;
        state.currentFrameCount = ++state.frameCountPerSensor[sensorIdStr];
    }

    int roisCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "rois");
    int objectsCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");

    if (roisCount == 0) {
        for (int objIdx = 0; objIdx < objectsCount; objIdx++) {
            NvPSFMsgCodecMsg* obj = nullptr;
            char objPath[64];
            snprintf(objPath, sizeof(objPath), "objects[%d]", objIdx);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, objPath, &obj) != NvPSFMSGCODEC_SUCCESS) continue;
            AlertMessage alertMsg = buildAlertFromFrameObject(frameMsg, obj, "empty_roi", "", 0);
            alertMsg.restrictedAreaViolation = false;
            alertMsg.confinedAreaViolation = false;
            alertMsg.socialDistancingViolation = false;
            alerts.push_back(alertMsg);
            NvPSFMsgCodecFreeMsg(obj);
        }
    } else {
        bool frameRestrictedViol = false;
        bool frameConfinedViol = false;
        int firstRestrictedRoiIdx = -1;
        uint32_t firstRestrictedObjId = 0;
        for (int roiIdx = 0; roiIdx < roisCount; roiIdx++) {
            NvPSFMsgCodecMsg* roi = nullptr;
            char roiPath[64];
            snprintf(roiPath, sizeof(roiPath), "rois[%d]", roiIdx);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, roiPath, &roi) != NvPSFMSGCODEC_SUCCESS) continue;
            char* roiDebugStr = NvPSFMsgCodecGetDebugString(roi);
            std::string roiDebug = roiDebugStr ? roiDebugStr : "";
            free(roiDebugStr);
            std::string roiType = getStringField(roi, "type");
            bool r = (roiDebug.find("restrictedAreaViolation") != std::string::npos &&
                      roiDebug.find("\"true\"") != std::string::npos) ||
                     (roiType.find("restricted") != std::string::npos);
            bool c = (roiDebug.find("confinedAreaViolation") != std::string::npos &&
                      roiDebug.find("\"true\"") != std::string::npos) ||
                     (roiType.find("confined") != std::string::npos);
            if (r) frameRestrictedViol = true;
            if (c) frameConfinedViol = true;
            if (r && firstRestrictedRoiIdx < 0) {
                firstRestrictedRoiIdx = roiIdx;
                std::string objectIdPattern = "5: \"";
                size_t pos = roiDebug.find(objectIdPattern);
                if (pos != std::string::npos) {
                    size_t start = pos + objectIdPattern.length();
                    size_t end = roiDebug.find("\"", start);
                    if (end != std::string::npos)
                        firstRestrictedObjId = static_cast<uint32_t>(strtoul(roiDebug.substr(start, end - start).c_str(), nullptr, 10));
                }
            }
            NvPSFMsgCodecFreeMsg(roi);
        }
        std::string stateKey = sensorIdStr;
        bool wasRestricted = false;
        bool wasConfined = false;
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            wasRestricted = state.restrictedViolState[stateKey];
            wasConfined = state.confinedViolState[stateKey];
        }
        if (wasRestricted && !frameRestrictedViol) {
            int roiIdx = firstRestrictedRoiIdx >= 0 ? firstRestrictedRoiIdx : 0;
            NvPSFMsgCodecMsg* roi = nullptr;
            char roiPath[64];
            snprintf(roiPath, sizeof(roiPath), "rois[%d]", roiIdx);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, roiPath, &roi) == NvPSFMSGCODEC_SUCCESS) {
                AlertMessage cleared = buildAlertFromFrameRoi(frameMsg, roi, firstRestrictedObjId);
                strncpy(cleared.type, "restrictedAreaViolationCleared", sizeof(cleared.type) - 1);
                cleared.type[sizeof(cleared.type) - 1] = '\0';
                cleared.restrictedAreaViolation = false;
                cleared.confinedAreaViolation = false;
                cleared.socialDistancingViolation = false;
                if (cleared.object.type[0] == '\0') {
                    std::lock_guard<std::mutex> lock(state.mtx);
                    auto it = state.lastRestrictedObjectType.find(stateKey);
                    if (it != state.lastRestrictedObjectType.end() && !it->second.empty()) {
                        strncpy(cleared.object.type, it->second.c_str(), sizeof(cleared.object.type) - 1);
                        cleared.object.type[sizeof(cleared.object.type) - 1] = '\0';
                    }
                }
                alerts.push_back(cleared);
                NvPSFMsgCodecFreeMsg(roi);
            }
        }
        if (wasConfined && !frameConfinedViol) {
            NvPSFMsgCodecMsg* roi = nullptr;
            if (NvPSFMsgCodecGetSubMsg(frameMsg, "rois[0]", &roi) == NvPSFMSGCODEC_SUCCESS) {
                AlertMessage cleared = buildAlertFromFrameRoi(frameMsg, roi, 0);
                strncpy(cleared.type, "confinedAreaViolationCleared", sizeof(cleared.type) - 1);
                cleared.type[sizeof(cleared.type) - 1] = '\0';
                cleared.restrictedAreaViolation = false;
                cleared.confinedAreaViolation = false;
                cleared.socialDistancingViolation = false;
                alerts.push_back(cleared);
                NvPSFMsgCodecFreeMsg(roi);
            }
        }
        int alertRoiIdx = firstRestrictedRoiIdx >= 0 ? firstRestrictedRoiIdx : 0;
        NvPSFMsgCodecMsg* alertRoi = nullptr;
        char alertRoiPath[64];
        snprintf(alertRoiPath, sizeof(alertRoiPath), "rois[%d]", alertRoiIdx);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, alertRoiPath, &alertRoi) == NvPSFMSGCODEC_SUCCESS) {
            char* roiDebugStr = NvPSFMsgCodecGetDebugString(alertRoi);
            std::string roiDebug = roiDebugStr ? roiDebugStr : "";
            free(roiDebugStr);
            uint32_t objId = 0;
            std::string objectIdPattern = "5: \"";
            size_t pos = roiDebug.find(objectIdPattern);
            if (pos != std::string::npos) {
                size_t start = pos + objectIdPattern.length();
                size_t end = roiDebug.find("\"", start);
                if (end != std::string::npos)
                    objId = static_cast<uint32_t>(strtoul(roiDebug.substr(start, end - start).c_str(), nullptr, 10));
            }
            AlertMessage alertMsg = buildAlertFromFrameRoi(frameMsg, alertRoi, objId);
            alertMsg.restrictedAreaViolation = frameRestrictedViol;
            alertMsg.confinedAreaViolation = frameConfinedViol;
            alertMsg.socialDistancingViolation = false;
            if (frameRestrictedViol && alertMsg.object.type[0] != '\0') {
                std::lock_guard<std::mutex> lock(state.mtx);
                state.lastRestrictedObjectType[stateKey] = alertMsg.object.type;
            }
            alerts.push_back(alertMsg);
            NvPSFMsgCodecFreeMsg(alertRoi);
        }
    }

    bool sdViol = NvPSFMsgCodecGetFieldPresence(frameMsg, "socialDistancing") &&
        getInt32Field(frameMsg, "socialDistancing.proximityDetections") > 0;
    bool sdViolPast = false;
    {
        std::lock_guard<std::mutex> lock(state.mtx);
        sdViolPast = state.socialDistancingViolState[sensorIdStr];
        state.socialDistancingViolState[sensorIdStr] = sdViol;
    }
    if (sdViolPast && !sdViol) {
        AlertMessage cleared = buildAlertFromFrameSocialDistancing(frameMsg);
        strncpy(cleared.type, "socialDistancingViolationCleared", sizeof(cleared.type) - 1);
        cleared.type[sizeof(cleared.type) - 1] = '\0';
        cleared.restrictedAreaViolation = false;
        cleared.confinedAreaViolation = false;
        cleared.socialDistancingViolation = false;
        alerts.push_back(cleared);
    }
    if (sdViol) {
        AlertMessage sdAlert = buildAlertFromFrameSocialDistancing(frameMsg);
        sdAlert.restrictedAreaViolation = false;
        sdAlert.confinedAreaViolation = false;
        sdAlert.socialDistancingViolation = true;
        alerts.push_back(sdAlert);
    }
    if (objectsCount > 0) {
        evaluateProximityRulesForFrame(frameMsg, config, alerts);
        evaluateObjectPresenceRulesForFrame(frameMsg, config, alerts);
    }
    NvPSFMsgCodecFreeMsg(frameMsg);
    return alerts;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

AlertMessage FramesParser::buildAlertFromFrameRoi(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* roi, uint32_t objectId) {
    AlertMessage alertMsg = {};
    memset(&alertMsg, 0, sizeof(alertMsg));
    strncpy(alertMsg.messageSource, "mdx-frames", sizeof(alertMsg.messageSource) - 1);
    std::string sensorId = getStringField(frameMsg, "sensorId");
    strncpy(alertMsg.sensorId, sensorId.c_str(), sizeof(alertMsg.sensorId) - 1);
    strncpy(alertMsg.type, "roi", sizeof(alertMsg.type) - 1);
    std::string roiId = getStringField(roi, "id");
    strncpy(alertMsg.ruleId, roiId.c_str(), sizeof(alertMsg.ruleId) - 1);
    fillTimestampFromFrame(frameMsg, alertMsg);
    alertMsg.id = nextEventId_();
    alertMsg.objectId = objectId;
    alertMsg.coordCount = 0;
    if (objectId > 0) {
        std::string targetId = std::to_string(objectId);
        int objCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
        for (int i = 0; i < objCount; i++) {
            NvPSFMsgCodecMsg* obj = nullptr;
            char objPath[64];
            snprintf(objPath, sizeof(objPath), "objects[%d]", i);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, objPath, &obj) != NvPSFMSGCODEC_SUCCESS) continue;
            std::string objId = getStringField(obj, "id");
            if (objId == targetId) {
                alertMsg.object.confidence = getFloatField(obj, "confidence");
                std::string objType = getStringField(obj, "type");
                strncpy(alertMsg.object.type, objType.c_str(), sizeof(alertMsg.object.type) - 1);
                alertMsg.speed = getFloatField(obj, "speed");
                if (NvPSFMsgCodecGetFieldPresence(obj, "coordinate")) {
                    alertMsg.coordinates[0].x = (float)getDoubleField(obj, "coordinate.x");
                    alertMsg.coordinates[0].y = (float)getDoubleField(obj, "coordinate.y");
                    alertMsg.coordCount = 1;
                }
                NvPSFMsgCodecFreeMsg(obj);
                break;
            }
            NvPSFMsgCodecFreeMsg(obj);
        }
    }
    return alertMsg;
}

AlertMessage FramesParser::buildAlertFromFrameSocialDistancing(const NvPSFMsgCodecMsg* frameMsg) {
    AlertMessage alertMsg = {};
    memset(&alertMsg, 0, sizeof(alertMsg));
    strncpy(alertMsg.messageSource, "mdx-frames", sizeof(alertMsg.messageSource) - 1);
    std::string sensorId = getStringField(frameMsg, "sensorId");
    strncpy(alertMsg.sensorId, sensorId.c_str(), sizeof(alertMsg.sensorId) - 1);
    strncpy(alertMsg.type, "social_distancing", sizeof(alertMsg.type) - 1);
    std::string idsStr = getProximityViolationObjectIdsFromFrame(frameMsg);
    if (!idsStr.empty()) {
        std::string resolved = resolveObjectIdsToTypes(frameMsg, idsStr);
        if (!resolved.empty()) {
            std::string ruleIdVal = "SD: " + resolved;
            strncpy(alertMsg.ruleId, ruleIdVal.c_str(), sizeof(alertMsg.ruleId) - 1);
            if (debugMode_)
                NvPSBWriteData(NVPSB_LOG_INFO, "Social distance violation: object IDs " + idsStr + " -> " + resolved, "");
        } else {
            strncpy(alertMsg.ruleId, "socialDistancing", sizeof(alertMsg.ruleId) - 1);
        }
    } else {
        strncpy(alertMsg.ruleId, "socialDistancing", sizeof(alertMsg.ruleId) - 1);
    }
    alertMsg.ruleId[sizeof(alertMsg.ruleId) - 1] = '\0';
    strncpy(alertMsg.eventType, "", sizeof(alertMsg.eventType) - 1);
    fillTimestampFromFrame(frameMsg, alertMsg);
    alertMsg.id = nextEventId_();
    alertMsg.objectId = 0;
    alertMsg.coordCount = 0;
    return alertMsg;
}

AlertMessage FramesParser::buildAlertFromFrameObject(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* obj, const char* type, const std::string& ruleId, uint32_t assignId) {
    AlertMessage alertMsg = {};
    memset(&alertMsg, 0, sizeof(alertMsg));
    strncpy(alertMsg.messageSource, "mdx-frames", sizeof(alertMsg.messageSource) - 1);
    std::string sensorId = getStringField(frameMsg, "sensorId");
    strncpy(alertMsg.sensorId, sensorId.c_str(), sizeof(alertMsg.sensorId) - 1);
    strncpy(alertMsg.type, type, sizeof(alertMsg.type) - 1);
    strncpy(alertMsg.ruleId, ruleId.c_str(), sizeof(alertMsg.ruleId) - 1);
    fillTimestampFromFrame(frameMsg, alertMsg);
    alertMsg.id = nextEventId_();
    alertMsg.objectId = assignId;
    alertMsg.object.confidence = getFloatField(obj, "confidence");
    std::string objType = getStringField(obj, "type");
    strncpy(alertMsg.object.type, objType.c_str(), sizeof(alertMsg.object.type) - 1);
    alertMsg.speed = getFloatField(obj, "speed");
    if (NvPSFMsgCodecGetFieldPresence(obj, "coordinate")) {
        alertMsg.coordinates[0].x = (float)getDoubleField(obj, "coordinate.x");
        alertMsg.coordinates[0].y = (float)getDoubleField(obj, "coordinate.y");
        alertMsg.coordCount = 1;
    } else {
        alertMsg.coordCount = 0;
    }
    alertMsg.restrictedAreaViolation = false;
    alertMsg.confinedAreaViolation = false;
    alertMsg.socialDistancingViolation = false;
    return alertMsg;
}

void FramesParser::evaluateProximityRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config, std::vector<AlertMessage>& alerts) {
    std::string sensorIdStr = getStringField(frameMsg, "sensorId");
    if (!NvPSFMsgCodecGetFieldPresence(frameMsg, "socialDistancing")) return;
    double pipelineThreshold = getDoubleField(frameMsg, "socialDistancing.threshold");
    bool sdFlag = getInt32Field(frameMsg, "socialDistancing.proximityDetections") > 0;

    int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    std::vector<std::pair<int, double>> proximityRuleIndices;
    for (int i = 0; i < rulesCount; i++) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS) continue;
        double distThresh = getDoubleField(rule, "distance_threshold_meters");
        if (distThresh <= 0 || distThresh > pipelineThreshold) {
            NvPSFMsgCodecFreeMsg(rule);
            continue;
        }
        std::string alertType = getStringField(rule, "alert_type");
        if (!alertType.empty() && !stringEqualsCaseInsensitive(alertType, "social_distancing")) {
            NvPSFMsgCodecFreeMsg(rule);
            continue;
        }
        proximityRuleIndices.push_back({i, distThresh});
        NvPSFMsgCodecFreeMsg(rule);
    }
    if (proximityRuleIndices.empty()) return;
    std::sort(proximityRuleIndices.begin(), proximityRuleIndices.end(),
        [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
    double threshFar = proximityRuleIndices.front().second;
    double threshNear = proximityRuleIndices.back().second;
    if (threshNear > threshFar) std::swap(threshNear, threshFar);

    std::set<std::pair<std::string, std::string>> typePairs;
    for (int i = 0; i < rulesCount; i++) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS) continue;
        std::string prim = getStringField(rule, "object_type_primary");
        if (prim.empty()) prim = getStringField(rule, "object_type");
        std::string sec = getStringField(rule, "object_type_secondary");
        if (sec.empty()) sec = prim;
        double distThresh = getDoubleField(rule, "distance_threshold_meters");
        std::string alertType = getStringField(rule, "alert_type");
        bool isProximity = distThresh > 0 && distThresh <= pipelineThreshold
            && stringEqualsCaseInsensitive(alertType, "social_distancing");
        if (isProximity && !prim.empty())
            typePairs.insert(std::make_pair(prim, sec));
        NvPSFMsgCodecFreeMsg(rule);
    }

    int objCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    for (const auto& typePair : typePairs) {
        const std::string& primType = typePair.first;
        const std::string& secType = typePair.second;
        if (!bothObjectTypesPresentInFrame(frameMsg, primType, secType)) continue;

        for (int a = 0; a < objCount; a++) {
        NvPSFMsgCodecMsg* objA = nullptr;
        char pathA[64];
        snprintf(pathA, sizeof(pathA), "objects[%d]", a);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, pathA, &objA) != NvPSFMSGCODEC_SUCCESS) continue;
        if (!NvPSFMsgCodecGetFieldPresence(objA, "coordinate")) { NvPSFMsgCodecFreeMsg(objA); continue; }
        std::string aType = getStringField(objA, "type");
        std::string idA = getStringField(objA, "id");
        if (idA.empty()) idA = "idx_" + std::to_string(a);
        double ax = getDoubleField(objA, "coordinate.x");
        double ay = getDoubleField(objA, "coordinate.y");
        double az = getDoubleField(objA, "coordinate.z");

        for (int b = a + 1; b < objCount; b++) {
            NvPSFMsgCodecMsg* objB = nullptr;
            char pathB[64];
            snprintf(pathB, sizeof(pathB), "objects[%d]", b);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, pathB, &objB) != NvPSFMSGCODEC_SUCCESS) continue;
            if (!NvPSFMsgCodecGetFieldPresence(objB, "coordinate")) { NvPSFMsgCodecFreeMsg(objB); continue; }
            std::string bType = getStringField(objB, "type");
            bool pairMatches = (stringEqualsCaseInsensitive(primType, aType.c_str()) && stringEqualsCaseInsensitive(secType, bType.c_str()))
                || (stringEqualsCaseInsensitive(secType, aType.c_str()) && stringEqualsCaseInsensitive(primType, bType.c_str()));
            if (!pairMatches) { NvPSFMsgCodecFreeMsg(objB); continue; }
            std::string idB = getStringField(objB, "id");
            if (idB.empty()) idB = "idx_" + std::to_string(b);

            double bx = getDoubleField(objB, "coordinate.x");
            double by = getDoubleField(objB, "coordinate.y");
            double bz = getDoubleField(objB, "coordinate.z");
            double dx = ax - bx;
            double dy = ay - by;
            double dz = az - bz;
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            int bucket = sdFlag ? ((dist <= threshNear) ? 0 : (dist <= threshFar) ? 1 : 2) : 2;

            AlertMessage alertMsg = buildAlertFromFrameSocialDistancing(frameMsg);
            alertMsg.restrictedAreaViolation = false;
            alertMsg.confinedAreaViolation = false;
            alertMsg.socialDistancingViolation = (bucket != 2);
            alertMsg.objectId = parseObjectIdToU32(idA);
            alertMsg.objectId2 = parseObjectIdToU32(idB);
            strncpy(alertMsg.object.type, aType.c_str(), sizeof(alertMsg.object.type) - 1);
            alertMsg.object.type[sizeof(alertMsg.object.type) - 1] = '\0';
            strncpy(alertMsg.object2.type, bType.c_str(), sizeof(alertMsg.object2.type) - 1);
            alertMsg.object2.type[sizeof(alertMsg.object2.type) - 1] = '\0';
            alertMsg.coordinates[0].x = static_cast<float>(ax);
            alertMsg.coordinates[0].y = static_cast<float>(ay);
            alertMsg.coordinates[1].x = static_cast<float>(bx);
            alertMsg.coordinates[1].y = static_cast<float>(by);
            alertMsg.coordCount = 2;

            char ruleIdWithPair[128];
            if (bucket == 2) {
                snprintf(ruleIdWithPair, sizeof(ruleIdWithPair), "proximity_no_violation:%s,%s", idA.c_str(), idB.c_str());
                strncpy(alertMsg.type, "no_violation", sizeof(alertMsg.type) - 1);
            } else if (bucket == 0) {
                snprintf(ruleIdWithPair, sizeof(ruleIdWithPair), "proximity_%.1f:%s,%s", threshNear, idA.c_str(), idB.c_str());
                strncpy(alertMsg.type, "social_distancing", sizeof(alertMsg.type) - 1);
            } else {
                snprintf(ruleIdWithPair, sizeof(ruleIdWithPair), "proximity_%.1f:%s,%s", threshFar, idA.c_str(), idB.c_str());
                strncpy(alertMsg.type, "social_distancing", sizeof(alertMsg.type) - 1);
            }
            alertMsg.type[sizeof(alertMsg.type) - 1] = '\0';
            strncpy(alertMsg.ruleId, ruleIdWithPair, sizeof(alertMsg.ruleId) - 1);
            alertMsg.ruleId[sizeof(alertMsg.ruleId) - 1] = '\0';

            if (bucket != 2) {
                char logMsg[256];
                snprintf(logMsg, sizeof(logMsg), "Proximity violation: distance=%.2f m, bucket=%d, ruleId=%s, object IDs: %s, %s",
                    dist, bucket, alertMsg.ruleId, idA.c_str(), idB.c_str());
                NvPSBWriteData(NVPSB_LOG_INFO, std::string(logMsg), "");
            }
            alerts.push_back(alertMsg);
            NvPSFMsgCodecFreeMsg(objB);
        }
        NvPSFMsgCodecFreeMsg(objA);
    }
    }
}

void FramesParser::evaluateObjectPresenceRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config, std::vector<AlertMessage>& alerts) {
    int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    int objCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    for (int r = 0; r < rulesCount; r++) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", r);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS) continue;
        std::string alertType = getStringField(rule, "alert_type");
        std::string msgSource = getStringField(rule, "message_source");
        std::string objectType = getStringField(rule, "object_type");
        std::string outputEvent = getStringField(rule, "output_event");
        if (!stringEqualsCaseInsensitive(alertType, "object_presence") ||
            !stringEqualsCaseInsensitive(msgSource, "mdx-frames") ||
            objectType.empty() || outputEvent.empty()) {
            NvPSFMsgCodecFreeMsg(rule);
            continue;
        }
        std::string ruleIdStr = getStringField(rule, "rule_id");
        std::string ruleId = ruleIdStr.empty()
            ? ("object_presence:" + objectType)
            : ruleIdStr;
        NvPSFMsgCodecFreeMsg(rule);

        for (int i = 0; i < objCount; i++) {
            NvPSFMsgCodecMsg* obj = nullptr;
            char objPath[64];
            snprintf(objPath, sizeof(objPath), "objects[%d]", i);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, objPath, &obj) != NvPSFMSGCODEC_SUCCESS) continue;
            std::string objType = getStringField(obj, "type");
            if (!stringEqualsCaseInsensitive(objectType, objType.c_str())) {
                NvPSFMsgCodecFreeMsg(obj);
                continue;
            }
            std::string idStr = getStringField(obj, "id");
            if (idStr.empty()) idStr = "idx_" + std::to_string(i);
            uint32_t assignId = parseObjectIdToU32(idStr);
            AlertMessage alertMsg = buildAlertFromFrameObject(frameMsg, obj, "object_presence", ruleId, assignId);
            alertMsg.restrictedAreaViolation = false;
            alertMsg.confinedAreaViolation = false;
            alertMsg.socialDistancingViolation = false;
            alerts.push_back(alertMsg);
            NvPSFMsgCodecFreeMsg(obj);
        }
    }
}

} // namespace MDXClient
