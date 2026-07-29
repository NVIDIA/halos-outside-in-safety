/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FramesParser.hpp"
#include "EventMappingValidation.hpp"
#include "FrameViolationUtils.hpp"
#include "NvPSB.h"
#include "ProximityPairUtils.hpp"
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
    if (a.size() != bStr.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(bStr[i]))) {
            return false;
        }
    }
    return true;
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

static bool getOptionalBoolField(const NvPSFMsgCodecMsg* msg, const char* path,
                                 bool* value) {
    if (msg == nullptr || path == nullptr || value == nullptr ||
        !NvPSFMsgCodecGetFieldPresence(msg, path)) {
        return false;
    }
    const NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(msg, path);
    if (result.type != NvPSF_VALUE_BOOL) {
        return false;
    }
    *value = result.data.b;
    return true;
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

static std::string getProximityViolationValueFromFrame(const NvPSFMsgCodecMsg* frameMsg) {
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(
        frameMsg, "socialDistancing.info['proximityViolation']");
    if (result.type != NvPSF_VALUE_MAPVALUE || result.data.mapValue == nullptr) {
        return std::string();
    }
    const std::string value(result.data.mapValue);
    free(const_cast<char*>(result.data.mapValue));
    return value;
}

struct ProximityPairRuleGroup {
    std::string primaryType;
    std::string secondaryType;
    std::vector<double> thresholds;
    bool hasNoViolationRule = false;
};

struct ProximityFrameObject {
    std::string rawId;
    std::string type;
    uint32_t numericId;
    double x;
    double y;
    double z;
};

static std::vector<ProximityPairRuleGroup> collectProximityPairRuleGroups(
        const NvPSFMsgCodecMsg* config) {
    std::vector<ProximityPairRuleGroup> groups;
    if (config == nullptr) {
        return groups;
    }
    std::unordered_map<std::string, size_t> groupIndexes;
    const int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    for (int ruleIndex = 0; ruleIndex < rulesCount; ++ruleIndex) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", ruleIndex);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS ||
            rule == nullptr) {
            continue;
        }
        const std::string messageSource = getStringField(rule, "message_source");
        const std::string alertType = getStringField(rule, "alert_type");
        const std::string ruleId = getStringField(rule, "rule_id");
        const std::string objectType = getStringField(rule, "object_type");
        const std::string configuredPrimaryType = getStringField(rule, "object_type_primary");
        const std::string configuredSecondaryType = getStringField(rule, "object_type_secondary");
        const std::string primaryType = configuredPrimaryType.empty()
            ? objectType : configuredPrimaryType;
        const double distanceThreshold = getDoubleField(rule, "distance_threshold_meters");
        const bool distanceThresholdPresent = NvPSFMsgCodecGetFieldPresence(
            rule, "distance_threshold_meters");
        bool proximityViolation = false;
        const bool proximityViolationPresent = getOptionalBoolField(
            rule, "proximity_violation", &proximityViolation);
        const std::string restrictedFilter = getStringField(rule, "restricted_area_violation");
        const std::string confinedFilter = getStringField(rule, "confined_area_violation");
        const std::string socialFilter = getStringField(rule, "social_distancing_violation");
        const bool hasViolationFilter = !restrictedFilter.empty() ||
                                        !confinedFilter.empty() || !socialFilter.empty();
        const bool isPairRule = isProximityPairRule(
            alertType, configuredPrimaryType, configuredSecondaryType,
            proximityViolationPresent, distanceThresholdPresent);
        if (!isPairRule || !isValidProximityPairRule(
                messageSource, alertType, primaryType, configuredSecondaryType,
                proximityViolationPresent, proximityViolation, distanceThresholdPresent,
                distanceThreshold, hasViolationFilter) ||
            !isValidProximityPairRuleId(ruleId)) {
            NvPSFMsgCodecFreeMsg(rule);
            continue;
        }
        const std::string pairKey = canonicalProximityTypePair(
            primaryType, configuredSecondaryType);
        std::unordered_map<std::string, size_t>::const_iterator groupIndex =
            groupIndexes.find(pairKey);
        if (groupIndex == groupIndexes.end()) {
            ProximityPairRuleGroup group;
            group.primaryType = primaryType;
            group.secondaryType = configuredSecondaryType;
            groups.push_back(group);
            groupIndexes.insert(std::make_pair(pairKey, groups.size() - 1U));
            groupIndex = groupIndexes.find(pairKey);
        }
        if (proximityViolation) {
            groups[groupIndex->second].thresholds.push_back(distanceThreshold);
        } else {
            groups[groupIndex->second].hasNoViolationRule = true;
        }
        NvPSFMsgCodecFreeMsg(rule);
    }
    for (ProximityPairRuleGroup& group : groups) {
        std::sort(group.thresholds.begin(), group.thresholds.end());
    }
    return groups;
}

static bool areProximityThresholdsWithinPipeline(
        const ProximityPairRuleGroup& group, double pipelineThreshold) {
    if (group.thresholds.empty()) {
        return true;
    }
    if (!std::isfinite(pipelineThreshold) || pipelineThreshold <= 0.0) {
        return false;
    }
    for (const double threshold : group.thresholds) {
        if (threshold > pipelineThreshold) {
            return false;
        }
    }
    return true;
}

static std::vector<ProximityFrameObject> collectProximityFrameObjects(
        const NvPSFMsgCodecMsg* frameMsg) {
    std::vector<ProximityFrameObject> allObjects;
    std::unordered_map<std::string, size_t> rawIdCounts;
    std::unordered_map<uint32_t, size_t> numericIdCounts;
    const int objectCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        NvPSFMsgCodecMsg* object = nullptr;
        char objectPath[64];
        snprintf(objectPath, sizeof(objectPath), "objects[%d]", objectIndex);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, objectPath, &object) != NvPSFMSGCODEC_SUCCESS ||
            object == nullptr) {
            continue;
        }
        if (!NvPSFMsgCodecGetFieldPresence(object, "coordinate")) {
            NvPSFMsgCodecFreeMsg(object);
            continue;
        }
        ProximityFrameObject frameObject;
        frameObject.rawId = getStringField(object, "id");
        frameObject.type = getStringField(object, "type");
        frameObject.x = getDoubleField(object, "coordinate.x");
        frameObject.y = getDoubleField(object, "coordinate.y");
        frameObject.z = getDoubleField(object, "coordinate.z");
        NvPSFMsgCodecFreeMsg(object);
        if (frameObject.rawId.empty() || frameObject.type.empty() ||
            !parseStrictObjectId(frameObject.rawId, &frameObject.numericId) ||
            !isRepresentableProximityCoordinate(frameObject.x) ||
            !isRepresentableProximityCoordinate(frameObject.y) ||
            !isRepresentableProximityCoordinate(frameObject.z)) {
            continue;
        }
        ++rawIdCounts[frameObject.rawId];
        ++numericIdCounts[frameObject.numericId];
        allObjects.push_back(frameObject);
    }

    std::vector<ProximityFrameObject> uniqueObjects;
    uniqueObjects.reserve(allObjects.size());
    for (const ProximityFrameObject& frameObject : allObjects) {
        if (rawIdCounts[frameObject.rawId] == 1U &&
            numericIdCounts[frameObject.numericId] == 1U) {
            uniqueObjects.push_back(frameObject);
        }
    }
    return uniqueObjects;
}

static RoiViolationValue getRoiViolationValueFromInfo(const NvPSFMsgCodecMsg* roi,
                                                       const char* key) {
    if (roi == nullptr || key == nullptr || key[0] == '\0') {
        return RoiViolationValue::kInvalid;
    }
    char path[128];
    const int pathLength = snprintf(path, sizeof(path), "info['%s']", key);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(path)) {
        return RoiViolationValue::kInvalid;
    }
    NvPSFMsgCodecFieldResult result = NvPSFMsgCodecGetField(roi, path);
    if (result.type != NvPSF_VALUE_MAPVALUE || result.data.mapValue == nullptr) {
        return RoiViolationValue::kInvalid;
    }
    const std::string value(result.data.mapValue);
    free(const_cast<char*>(result.data.mapValue));
    return parseRoiViolationMapValue(value);
}

static NvPSFMsgCodecMsg* resolveRoiObject(const NvPSFMsgCodecMsg* frameMsg,
                                           const std::string& rawObjectId,
                                           const std::string& roiType,
                                           uint32_t* numericObjectId) {
    if (frameMsg == nullptr || numericObjectId == nullptr ||
        !parseStrictObjectId(rawObjectId, numericObjectId)) {
        return nullptr;
    }
    NvPSFMsgCodecMsg* resolvedObject = nullptr;
    const int objectCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    for (int objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        NvPSFMsgCodecMsg* object = nullptr;
        char objectPath[64];
        snprintf(objectPath, sizeof(objectPath), "objects[%d]", objectIndex);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, objectPath, &object) != NvPSFMSGCODEC_SUCCESS ||
            object == nullptr) {
            continue;
        }
        const std::string candidateId = getStringField(object, "id");
        if (isNumericObjectIdAlias(rawObjectId, *numericObjectId, candidateId)) {
            NvPSFMsgCodecFreeMsg(object);
            if (resolvedObject != nullptr) {
                NvPSFMsgCodecFreeMsg(resolvedObject);
            }
            return nullptr;
        }
        if (candidateId != rawObjectId) {
            NvPSFMsgCodecFreeMsg(object);
            continue;
        }
        if (resolvedObject != nullptr) {
            NvPSFMsgCodecFreeMsg(object);
            NvPSFMsgCodecFreeMsg(resolvedObject);
            return nullptr;
        }
        const std::string candidateType = getStringField(object, "type");
        if (!matchesRoiObjectType(roiType, candidateType)) {
            NvPSFMsgCodecFreeMsg(object);
            return nullptr;
        }
        resolvedObject = object;
    }
    return resolvedObject;
}

// ---------------------------------------------------------------------------
// FramesParser
// ---------------------------------------------------------------------------

FramesParser::FramesParser(NextEventIdFn nextEventId, bool debugMode)
    : nextEventId_(std::move(nextEventId)), debugMode_(debugMode) {}

std::vector<AlertMessage> FramesParser::parseFramesMessage(const std::string& data,
        const NvPSFMsgCodecMsg* config) {
    std::vector<AlertMessage> alerts;
    NvPSFMsgCodecMsg* frameMsg = nullptr;
    if (NvPSFMsgCodecDecode(data.data(), data.size(), NvPSF_MSG_FRAME, &frameMsg) != NvPSFMSGCODEC_SUCCESS) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to decode frames message", "");
        return alerts;
    }
    std::string sensorIdStr = getStringField(frameMsg, "sensorId");
    currentFrameOrdinal_ = frameOrdinalTracker_.next(sensorIdStr);

    int roisCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "rois");
    int objectsCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");

    if (roisCount == 0) {
        emitEmptyRoiClears(frameMsg, config, alerts);
    } else {
        // Track, across all ROI entries of this frame, which restricted areas are
        // present and which (roiId, objectType) combinations actually reported a
        // violation. Used after the loop to synthesize cleared events (EVENT_5)
        // for monitored areas that are present but have no violating object.
        std::set<std::string> presentRestrictedRoiIds;
        std::vector<std::pair<std::string, std::string>> restrictedViolations;
        // (roiId, objectType) pairs for which the per-object loop already emitted a
        // restricted alert this frame -- for ANY value (true=EVENT_4 or false=EVENT_5).
        // If VSS ever starts sending explicit "restrictedAreaViolation=false" Person
        // entries per frame, the per-object path already covers the clear, so the
        // synthesized clear below must be suppressed to avoid a duplicate EVENT_5.
        std::vector<std::pair<std::string, std::string>> restrictedRoiEmitted;
        for (int roiIdx = 0; roiIdx < roisCount; roiIdx++) {
            NvPSFMsgCodecMsg* roi = nullptr;
            char roiPath[64];
            snprintf(roiPath, sizeof(roiPath), "rois[%d]", roiIdx);
            if (NvPSFMsgCodecGetSubMsg(frameMsg, roiPath, &roi) != NvPSFMSGCODEC_SUCCESS) continue;
            const std::string roiId = getStringField(roi, "id");
            const std::string roiType = getStringField(roi, "type");
            if (roiId.empty() || roiType.empty()) {
                NvPSFMsgCodecFreeMsg(roi);
                continue;
            }
            const RoiViolationValue restrictedValue = getRoiViolationValueFromInfo(
                roi, "restrictedAreaViolation");
            const RoiViolationValue confinedValue = getRoiViolationValueFromInfo(
                roi, "confinedAreaViolation");
            if (restrictedValue == RoiViolationValue::kInvalid &&
                confinedValue == RoiViolationValue::kInvalid) {
                NvPSFMsgCodecFreeMsg(roi);
                continue;
            }
            if (restrictedValue != RoiViolationValue::kInvalid) {
                presentRestrictedRoiIds.insert(roiId);
                if (restrictedValue == RoiViolationValue::kTrue) {
                    restrictedViolations.push_back(std::make_pair(roiId, roiType));
                }
            }

            const int roiObjectCount = NvPSFMsgCodecGetRepeatedCount(roi, "objectIds");
            std::set<std::string> uniqueRoiObjectIds;
            std::set<uint32_t> uniqueRoiNumericObjectIds;
            std::set<std::string> duplicateRoiObjectIds;
            std::unordered_map<uint32_t, std::string> rawObjectIdByNumericId;
            std::vector<std::string> rawRoiObjectIds;
            rawRoiObjectIds.reserve(static_cast<size_t>(roiObjectCount));
            for (int objectIndex = 0; objectIndex < roiObjectCount; ++objectIndex) {
                char objectIdPath[64];
                snprintf(objectIdPath, sizeof(objectIdPath), "objectIds[%d]", objectIndex);
                const std::string rawObjectId = getStringField(roi, objectIdPath);
                if (!insertUniqueRoiObjectId(&uniqueRoiObjectIds, rawObjectId)) {
                    if (!rawObjectId.empty()) {
                        duplicateRoiObjectIds.insert(rawObjectId);
                    }
                    continue;
                }
                uint32_t numericObjectId = 0U;
                if (!parseStrictObjectId(rawObjectId, &numericObjectId)) {
                    continue;
                }
                if (!insertUniqueRoiNumericObjectId(&uniqueRoiNumericObjectIds,
                                                    numericObjectId)) {
                    duplicateRoiObjectIds.insert(rawObjectId);
                    const std::unordered_map<uint32_t, std::string>::const_iterator
                        existingObjectId = rawObjectIdByNumericId.find(numericObjectId);
                    if (existingObjectId != rawObjectIdByNumericId.end()) {
                        duplicateRoiObjectIds.insert(existingObjectId->second);
                    }
                    continue;
                }
                rawObjectIdByNumericId.insert(std::make_pair(numericObjectId, rawObjectId));
                rawRoiObjectIds.push_back(rawObjectId);
            }
            for (const std::string& rawObjectId : rawRoiObjectIds) {
                if (duplicateRoiObjectIds.find(rawObjectId) !=
                    duplicateRoiObjectIds.end()) {
                    continue;
                }
                uint32_t numericObjectId = 0U;
                NvPSFMsgCodecMsg* object = resolveRoiObject(
                    frameMsg, rawObjectId, roiType, &numericObjectId);
                if (object == nullptr) {
                    continue;
                }

                if (restrictedValue != RoiViolationValue::kInvalid) {
                    AlertMessage alertMsg = buildAlertFromFrameRoi(
                        frameMsg, roi, object, numericObjectId);
                    alertMsg.restrictedAreaViolation =
                        restrictedValue == RoiViolationValue::kTrue;
                    alertMsg.confinedAreaViolation = false;
                    alertMsg.socialDistancingViolation = false;
                    alertMsg.candidateKind = AlertCandidateKind::kRestrictedRoi;
                    alerts.push_back(alertMsg);
                    restrictedRoiEmitted.push_back(std::make_pair(roiId, roiType));
                }
                if (confinedValue != RoiViolationValue::kInvalid) {
                    AlertMessage alertMsg = buildAlertFromFrameRoi(
                        frameMsg, roi, object, numericObjectId);
                    alertMsg.restrictedAreaViolation = false;
                    alertMsg.confinedAreaViolation =
                        confinedValue == RoiViolationValue::kTrue;
                    alertMsg.socialDistancingViolation = false;
                    alertMsg.candidateKind = AlertCandidateKind::kConfinedRoi;
                    alerts.push_back(alertMsg);
                }
                NvPSFMsgCodecFreeMsg(object);
            }
            NvPSFMsgCodecFreeMsg(roi);
        }
        // A restricted-area violation is only ever reported as a per-object ROI
        // entry (e.g. type=Person, restrictedAreaViolation=true); when the object
        // leaves, that entry is simply omitted (no false entry is sent). Without
        // an explicit clear, the SDM latches restrictedAreaViolationByPerson on.
        // So, per frame, emit a cleared event (EVENT_5) for each monitored
        // restricted area that is present but has no violating object this frame.
        emitRestrictedAreaClears(frameMsg, config, presentRestrictedRoiIds,
                                 restrictedViolations, restrictedRoiEmitted, alerts);
    }

    const bool socialDistancingPresent = NvPSFMsgCodecGetFieldPresence(frameMsg, "socialDistancing");
    const int32_t socialDetections = socialDistancingPresent
        ? getInt32Field(frameMsg, "socialDistancing.proximityDetections")
        : 0;
    if (socialDetections >= 0) {
        AlertMessage sdAlert = buildAlertFromFrameSocialDistancing(frameMsg);
        sdAlert.restrictedAreaViolation = false;
        sdAlert.confinedAreaViolation = false;
        sdAlert.socialDistancingViolation = socialDetections > 0;
        sdAlert.candidateKind = AlertCandidateKind::kFrameSocial;
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
        const NvPSFMsgCodecMsg* roi, const NvPSFMsgCodecMsg* object,
        uint32_t objectId) {
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
    alertMsg.frameOrdinal = currentFrameOrdinal_;
    alertMsg.coordCount = 0;
    alertMsg.objectId = objectId;
    alertMsg.object.confidence = getFloatField(object, "confidence");
    const std::string objectType = getStringField(object, "type");
    strncpy(alertMsg.object.type, objectType.c_str(), sizeof(alertMsg.object.type) - 1);
    alertMsg.speed = getFloatField(object, "speed");
    if (NvPSFMsgCodecGetFieldPresence(object, "coordinate")) {
        alertMsg.coordinates[0].x = static_cast<float>(
            getDoubleField(object, "coordinate.x"));
        alertMsg.coordinates[0].y = static_cast<float>(
            getDoubleField(object, "coordinate.y"));
        alertMsg.coordCount = 1;
    }
    return alertMsg;
}

// Build a ROI-level alert not tied to a specific object, used to report the
// aggregate cleared state of a restricted area on frames where no violating
// object is present. object.type is set to the configured monitored type (e.g.
// "person") so the event-mapping object_type filter still matches EVENT_5.
AlertMessage FramesParser::buildRoiStateAlert(const NvPSFMsgCodecMsg* frameMsg,
        const std::string& roiId, const std::string& objectType) {
    AlertMessage alertMsg = {};
    memset(&alertMsg, 0, sizeof(alertMsg));
    strncpy(alertMsg.messageSource, "mdx-frames", sizeof(alertMsg.messageSource) - 1);
    const std::string sensorId = getStringField(frameMsg, "sensorId");
    strncpy(alertMsg.sensorId, sensorId.c_str(), sizeof(alertMsg.sensorId) - 1);
    strncpy(alertMsg.type, "roi", sizeof(alertMsg.type) - 1);
    strncpy(alertMsg.ruleId, roiId.c_str(), sizeof(alertMsg.ruleId) - 1);
    strncpy(alertMsg.object.type, objectType.c_str(), sizeof(alertMsg.object.type) - 1);
    fillTimestampFromFrame(frameMsg, alertMsg);
    alertMsg.id = nextEventId_();
    alertMsg.frameOrdinal = currentFrameOrdinal_;
    alertMsg.coordCount = 0;
    alertMsg.objectId = 0;
    return alertMsg;
}

// Case-insensitive match of a frame ROI id against a configured rule_id, mirroring
// the prefix rule used by the reporter (exact, or "<rule_id>:<suffix>").
static bool roiIdMatchesRuleId(const std::string& roiId, const std::string& ruleId) {
    if (ruleId.empty()) {
        return false;
    }
    if (roiId == ruleId) {
        return true;
    }
    return roiId.size() > ruleId.size() &&
           roiId.compare(0, ruleId.size(), ruleId) == 0 &&
           roiId[ruleId.size()] == ':';
}

void FramesParser::emitRestrictedAreaClears(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config,
        const std::set<std::string>& presentRestrictedRoiIds,
        const std::vector<std::pair<std::string, std::string>>& restrictedViolations,
        const std::vector<std::pair<std::string, std::string>>& restrictedRoiEmitted,
        std::vector<AlertMessage>& alerts) {
    if (config == nullptr || presentRestrictedRoiIds.empty()) {
        return;
    }
    const int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    std::set<std::string> emittedClears;
    for (int r = 0; r < rulesCount; ++r) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", r);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS) {
            continue;
        }
        const std::string msgSource = getStringField(rule, "message_source");
        const std::string alertType = getStringField(rule, "alert_type");
        const std::string objectType = getStringField(rule, "object_type");
        const std::string ruleId = getStringField(rule, "rule_id");
        const std::string restrictedFilter = getStringField(rule, "restricted_area_violation");
        NvPSFMsgCodecFreeMsg(rule);
        // Only "restricted_area_violation: true" ROI rules for mdx-frames define a
        // monitored area whose clear (EVENT_5) we must synthesize. The cleared
        // object type is taken from the rule config (not hardcoded).
        if (!stringEqualsCaseInsensitive(msgSource, "mdx-frames") ||
            !stringEqualsCaseInsensitive(alertType, "roi") ||
            !stringEqualsCaseInsensitive(restrictedFilter, "true") ||
            objectType.empty() || ruleId.empty()) {
            continue;
        }
        const std::string dedupeKey = ruleId + "\x1f" + objectType;
        if (!emittedClears.insert(dedupeKey).second) {
            continue;
        }
        std::string matchedRoiId;
        for (const std::string& id : presentRestrictedRoiIds) {
            if (roiIdMatchesRuleId(id, ruleId)) {
                matchedRoiId = id;
                break;
            }
        }
        if (matchedRoiId.empty()) {
            continue;  // monitored area not present in this frame -> no assertion
        }
        bool violated = false;
        for (const std::pair<std::string, std::string>& violation : restrictedViolations) {
            if (roiIdMatchesRuleId(violation.first, ruleId) &&
                stringEqualsCaseInsensitive(objectType, violation.second.c_str())) {
                violated = true;
                break;
            }
        }
        if (violated) {
            continue;  // a violating object is present -> EVENT_4 path covers it
        }
        // If the per-object loop already emitted a restricted alert for this monitor
        // (e.g. VSS sent an explicit restrictedAreaViolation=false Person entry), that
        // emit is the clear -- do not synthesize a second one (avoids duplicate EVENT_5).
        bool alreadyEmitted = false;
        for (const std::pair<std::string, std::string>& emitted : restrictedRoiEmitted) {
            if (roiIdMatchesRuleId(emitted.first, ruleId) &&
                stringEqualsCaseInsensitive(objectType, emitted.second.c_str())) {
                alreadyEmitted = true;
                break;
            }
        }
        if (alreadyEmitted) {
            continue;
        }
        AlertMessage alertMsg = buildRoiStateAlert(frameMsg, matchedRoiId, objectType);
        alertMsg.restrictedAreaViolation = false;
        alertMsg.confinedAreaViolation = false;
        alertMsg.socialDistancingViolation = false;
        alertMsg.candidateKind = AlertCandidateKind::kRestrictedRoi;
        alerts.push_back(alertMsg);
    }
}

void FramesParser::emitEmptyRoiClears(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config, std::vector<AlertMessage>& alerts) {
    if (config == nullptr) {
        return;
    }
    const int rulesCount = NvPSFMsgCodecGetRepeatedCount(config, "rules");
    std::set<std::pair<std::string, AlertCandidateKind>> emittedClears;
    for (int ruleIndex = 0; ruleIndex < rulesCount; ++ruleIndex) {
        NvPSFMsgCodecMsg* rule = nullptr;
        char rulePath[64];
        snprintf(rulePath, sizeof(rulePath), "rules[%d]", ruleIndex);
        if (NvPSFMsgCodecGetSubMsg(config, rulePath, &rule) != NvPSFMSGCODEC_SUCCESS ||
            rule == nullptr) {
            continue;
        }
        const std::string messageSource = getStringField(rule, "message_source");
        const std::string alertType = getStringField(rule, "alert_type");
        const std::string objectType = getStringField(rule, "object_type");
        const std::string ruleId = getStringField(rule, "rule_id");
        const std::string restrictedFilter = getStringField(rule, "restricted_area_violation");
        const std::string confinedFilter = getStringField(rule, "confined_area_violation");
        NvPSFMsgCodecFreeMsg(rule);

        if (!stringEqualsCaseInsensitive(messageSource, "mdx-frames") ||
            !stringEqualsCaseInsensitive(alertType, "roi") ||
            objectType.empty() || ruleId.empty()) {
            continue;
        }
        AlertCandidateKind candidateKind = AlertCandidateKind::kNone;
        if (stringEqualsCaseInsensitive(restrictedFilter, "false") &&
            confinedFilter.empty()) {
            candidateKind = AlertCandidateKind::kRestrictedRoi;
        } else if (stringEqualsCaseInsensitive(confinedFilter, "false") &&
                   restrictedFilter.empty()) {
            candidateKind = AlertCandidateKind::kConfinedRoi;
        } else {
            continue;
        }
        const std::pair<std::string, AlertCandidateKind> dedupeKey(
            ruleId + "\x1f" + objectType, candidateKind);
        if (!emittedClears.insert(dedupeKey).second) {
            continue;
        }
        AlertMessage alertMsg = buildRoiStateAlert(frameMsg, ruleId, objectType);
        alertMsg.restrictedAreaViolation = false;
        alertMsg.confinedAreaViolation = false;
        alertMsg.socialDistancingViolation = false;
        alertMsg.candidateKind = candidateKind;
        alerts.push_back(alertMsg);
    }
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
    alertMsg.frameOrdinal = currentFrameOrdinal_;
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
    alertMsg.frameOrdinal = currentFrameOrdinal_;
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
    alertMsg.candidateKind = AlertCandidateKind::kNone;
    return alertMsg;
}

void FramesParser::evaluateProximityRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config, std::vector<AlertMessage>& alerts) {
    const bool socialDistancingPresent = NvPSFMsgCodecGetFieldPresence(
        frameMsg, "socialDistancing");
    const int32_t proximityDetections = socialDistancingPresent
        ? getInt32Field(frameMsg, "socialDistancing.proximityDetections") : 0;
    const std::string proximityViolation = socialDistancingPresent
        ? getProximityViolationValueFromFrame(frameMsg) : std::string();
    const ProximityGate gate = determineProximityGate(
        socialDistancingPresent, proximityDetections, proximityViolation);
    if (gate == ProximityGate::kInvalid) {
        return;
    }

    const std::vector<ProximityPairRuleGroup> groups =
        collectProximityPairRuleGroups(config);
    if (groups.empty()) {
        return;
    }
    const std::vector<ProximityFrameObject> objects =
        collectProximityFrameObjects(frameMsg);
    if (objects.size() < 2U) {
        return;
    }
    const double pipelineThreshold = socialDistancingPresent
        ? getDoubleField(frameMsg, "socialDistancing.threshold") : 0.0;

    for (const ProximityPairRuleGroup& group : groups) {
        if (!shouldEvaluateProximityPairGroup(gate, group.hasNoViolationRule)) {
            continue;
        }
        if (gate == ProximityGate::kTrue &&
            !areProximityThresholdsWithinPipeline(group, pipelineThreshold)) {
            continue;
        }
        for (size_t firstIndex = 0; firstIndex + 1U < objects.size(); ++firstIndex) {
            const ProximityFrameObject& firstObject = objects[firstIndex];
            for (size_t secondIndex = firstIndex + 1U;
                 secondIndex < objects.size(); ++secondIndex) {
                const ProximityFrameObject& secondObject = objects[secondIndex];
                if (!matchesConfiguredProximityPair(group.primaryType, group.secondaryType,
                                                    firstObject.type, secondObject.type)) {
                    continue;
                }
                const double dx = firstObject.x - secondObject.x;
                const double dy = firstObject.y - secondObject.y;
                const double dz = firstObject.z - secondObject.z;
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(distance)) {
                    continue;
                }

                double selectedThreshold = 0.0;
                const bool isViolation = gate == ProximityGate::kTrue &&
                    selectProximityThreshold(group.thresholds, distance, &selectedThreshold);
                if (!shouldEmitProximityPairCandidate(isViolation,
                                                      group.hasNoViolationRule)) {
                    continue;
                }
                AlertMessage alertMsg = buildAlertFromFrameSocialDistancing(frameMsg);
                alertMsg.restrictedAreaViolation = false;
                alertMsg.confinedAreaViolation = false;
                alertMsg.socialDistancingViolation = isViolation;
                alertMsg.candidateKind = AlertCandidateKind::kProximityPair;
                alertMsg.proximitySelection = isViolation
                    ? ProximitySelection::kThresholdViolation
                    : ProximitySelection::kNoViolation;
                alertMsg.proximityThreshold = isViolation ? selectedThreshold : 0.0;
                alertMsg.objectId = firstObject.numericId;
                alertMsg.objectId2 = secondObject.numericId;
                strncpy(alertMsg.object.type, firstObject.type.c_str(),
                        sizeof(alertMsg.object.type) - 1U);
                alertMsg.object.type[sizeof(alertMsg.object.type) - 1U] = '\0';
                strncpy(alertMsg.object2.type, secondObject.type.c_str(),
                        sizeof(alertMsg.object2.type) - 1U);
                alertMsg.object2.type[sizeof(alertMsg.object2.type) - 1U] = '\0';
                alertMsg.coordinates[0].x = static_cast<float>(firstObject.x);
                alertMsg.coordinates[0].y = static_cast<float>(firstObject.y);
                alertMsg.coordinates[1].x = static_cast<float>(secondObject.x);
                alertMsg.coordinates[1].y = static_cast<float>(secondObject.y);
                alertMsg.coordCount = 2;
                strncpy(alertMsg.type, "social_distancing", sizeof(alertMsg.type) - 1U);
                alertMsg.type[sizeof(alertMsg.type) - 1U] = '\0';
                alertMsg.ruleId[0] = '\0';
                alerts.push_back(alertMsg);
            }
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
            alertMsg.candidateKind = AlertCandidateKind::kNone;
            alerts.push_back(alertMsg);
            NvPSFMsgCodecFreeMsg(obj);
        }
    }
}

} // namespace MDXClient
