/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "EventsParser.hpp"
#include "NvPSB.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <climits>

namespace MDXClient {

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

static NvPSFMsgCodecFieldResult getFieldIfPresent(const NvPSFMsgCodecMsg* msg,
        const char* path, bool* present) {
    NvPSFMsgCodecFieldResult result = {};
    result.type = NvPSF_VALUE_ERROR;
    *present = NvPSFMsgCodecGetFieldPresence(msg, path);
    if (*present)
        result = NvPSFMsgCodecGetField(msg, path);
    return result;
}

// ---------------------------------------------------------------------------
// EventsParser
// ---------------------------------------------------------------------------

EventsParser::EventsParser(NextEventIdFn nextEventId)
    : nextEventId_(std::move(nextEventId)) {}

std::vector<AlertMessage> EventsParser::parseEventsMessage(const std::string& data) {
    std::vector<AlertMessage> alerts;
    NvPSFMsgCodecMsg* msg = nullptr;
    if (NvPSFMsgCodecDecode(data.data(), data.size(), NvPSF_MSG_BEHAVIOR, &msg) != NvPSFMSGCODEC_SUCCESS) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to decode events message", "");
        return alerts;
    }
    AlertMessage alertMsg = {};
    if (!parseBehaviorToAlertMessage(msg, alertMsg)) {
        NvPSFMsgCodecFreeMsg(msg);
        return alerts;
    }
    strncpy(alertMsg.messageSource, "mdx-events", sizeof(alertMsg.messageSource) - 1);
    alertMsg.messageSource[sizeof(alertMsg.messageSource) - 1] = '\0';
    alertMsg.id = nextEventId_();
    alertMsg.restrictedAreaViolation = false;
    alertMsg.confinedAreaViolation = false;
    alertMsg.socialDistancingViolation = false;
    if (strcmp(alertMsg.type, "tripwire") == 0 || strcmp(alertMsg.type, "roi") == 0)
        alerts.push_back(alertMsg);
    NvPSFMsgCodecFreeMsg(msg);
    return alerts;
}

bool EventsParser::parseBehaviorToAlertMessage(const NvPSFMsgCodecMsg* msg, AlertMessage& alertMsg) {
    memset(&alertMsg, 0, sizeof(alertMsg));
    bool sensorIdPresent;
    NvPSFMsgCodecFieldResult sensorId = getFieldIfPresent(msg, "sensor.id", &sensorIdPresent);
    if (sensorIdPresent && sensorId.type == NvPSF_VALUE_STRING) {
        strncpy(alertMsg.sensorId, sensorId.data.s, sizeof(alertMsg.sensorId) - 1);
        alertMsg.sensorId[sizeof(alertMsg.sensorId) - 1] = '\0';
        free((void*)sensorId.data.s);
    }
    bool eventIdPresent;
    NvPSFMsgCodecFieldResult eventId = getFieldIfPresent(msg, "event.id", &eventIdPresent);
    if (eventIdPresent && eventId.type == NvPSF_VALUE_STRING) {
        strncpy(alertMsg.ruleId, eventId.data.s, sizeof(alertMsg.ruleId) - 1);
        alertMsg.ruleId[sizeof(alertMsg.ruleId) - 1] = '\0';
        std::string eventIdStr(eventId.data.s);
        if (eventIdStr.find("tripwire") != std::string::npos)
            strncpy(alertMsg.type, "tripwire", sizeof(alertMsg.type) - 1);
        else if (eventIdStr.find("roi") != std::string::npos)
            strncpy(alertMsg.type, "roi", sizeof(alertMsg.type) - 1);
        free((void*)eventId.data.s);
    }
    bool eventTypePresent;
    NvPSFMsgCodecFieldResult eventType = getFieldIfPresent(msg, "event.type", &eventTypePresent);
    if (eventTypePresent && eventType.type == NvPSF_VALUE_STRING) {
        strncpy(alertMsg.eventType, eventType.data.s, sizeof(alertMsg.eventType) - 1);
        alertMsg.eventType[sizeof(alertMsg.eventType) - 1] = '\0';
        free((void*)eventType.data.s);
    }
    bool endTimestampPresent;
    NvPSFMsgCodecFieldResult endTimestamp = getFieldIfPresent(msg, "end", &endTimestampPresent);
    if (endTimestampPresent && endTimestamp.type == NvPSF_VALUE_TIMESTAMP) {
        time_t seconds = endTimestamp.data.timestamp.seconds;
        int ms = endTimestamp.data.timestamp.nanos / 1000000;
        struct tm tm_buf;
        struct tm* utc = gmtime_r(&seconds, &tm_buf);
        if (utc)
            formatUtcTimestampInto(alertMsg.endTimestamp, sizeof(alertMsg.endTimestamp), utc, ms);
    }
    bool objectIdPresent;
    NvPSFMsgCodecFieldResult objectIdResult = getFieldIfPresent(msg, "object.id", &objectIdPresent);
    if (objectIdPresent && objectIdResult.type == NvPSF_VALUE_STRING) {
        uint32_t objId = static_cast<uint32_t>(strtoul(objectIdResult.data.s, nullptr, 10));
        if (objId > 0) alertMsg.objectId = objId;
        free((void*)objectIdResult.data.s);
    }
    bool objectTypePresent;
    NvPSFMsgCodecFieldResult objectType = getFieldIfPresent(msg, "object.type", &objectTypePresent);
    if (objectTypePresent && objectType.type == NvPSF_VALUE_STRING) {
        strncpy(alertMsg.object.type, objectType.data.s, sizeof(alertMsg.object.type) - 1);
        free((void*)objectType.data.s);
    }
    bool objectConfidencePresent = false;
    NvPSFMsgCodecFieldResult objectConfidence = getFieldIfPresent(msg, "object.confidence", &objectConfidencePresent);
    if (objectConfidencePresent && (objectConfidence.type == NvPSF_VALUE_DOUBLE || objectConfidence.type == NvPSF_VALUE_FLOAT))
        alertMsg.object.confidence = objectConfidence.type == NvPSF_VALUE_DOUBLE ? (float)objectConfidence.data.d : objectConfidence.data.f;
    bool speedPresent;
    NvPSFMsgCodecFieldResult speed = getFieldIfPresent(msg, "behavior.speed", &speedPresent);
    if (speedPresent && (speed.type == NvPSF_VALUE_DOUBLE || speed.type == NvPSF_VALUE_FLOAT))
        alertMsg.speed = speed.type == NvPSF_VALUE_DOUBLE ? (float)speed.data.d : speed.data.f;
    bool lengthPresent;
    NvPSFMsgCodecFieldResult length = getFieldIfPresent(msg, "length", &lengthPresent);
    alertMsg.coordCount = 0;
    if (lengthPresent && length.type == NvPSF_VALUE_INT32) {
        alertMsg.coordCount = length.data.i32;
        if (alertMsg.coordCount > MAX_COORDINATES_COUNT) alertMsg.coordCount = MAX_COORDINATES_COUNT;
    }
    for (int i = 0; i < alertMsg.coordCount && i < MAX_COORDINATES_COUNT; i++) {
        char xPath[256], yPath[256];
        snprintf(xPath, sizeof(xPath), "locations.coordinates[%d].point[0]", i);
        snprintf(yPath, sizeof(yPath), "locations.coordinates[%d].point[1]", i);
        bool xPresent, yPresent;
        NvPSFMsgCodecFieldResult xResult = getFieldIfPresent(msg, xPath, &xPresent);
        NvPSFMsgCodecFieldResult yResult = getFieldIfPresent(msg, yPath, &yPresent);
        if (xPresent && yPresent) {
            if (xResult.type == NvPSF_VALUE_DOUBLE && yResult.type == NvPSF_VALUE_DOUBLE) {
                alertMsg.coordinates[i].x = (float)xResult.data.d;
                alertMsg.coordinates[i].y = (float)yResult.data.d;
            } else if (xResult.type == NvPSF_VALUE_FLOAT && yResult.type == NvPSF_VALUE_FLOAT) {
                alertMsg.coordinates[i].x = xResult.data.f;
                alertMsg.coordinates[i].y = yResult.data.f;
            }
        }
    }
    return true;
}

} // namespace MDXClient
