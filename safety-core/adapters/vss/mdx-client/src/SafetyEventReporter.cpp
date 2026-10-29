/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>

#include "SafetyEventReporter.hpp"
#include "EventMappingValidation.hpp"
#include "FrameReportingUtils.hpp"
#include "sensor_config_parser.h"
#include "pss_daemon.h"
#include "NvPSB.h"

namespace MDXClient {

static bool stringEqualsCaseInsensitive(const std::string& a, const char* b) {
    if (!b) return false;
    return matchesConfiguredStringCondition(a, b);
}

static bool parseViolationFilter(const std::string& ruleVal, bool alertVal) {
    if (!isValidViolationFilter(ruleVal)) return false;
    if (ruleVal.empty() || ruleVal == "any") return true;
    if (ruleVal == "true")  return alertVal == true;
    if (ruleVal == "false") return alertVal == false;
    return false;
}

static bool ruleMatches(const EventMappingRule& rule, const AlertMessage& alert) {
    const std::string& ruleId = rule.ruleId;
    if (rule.isCoMovementRule) {
        // Re-validated here for the same reason the pair branch is: a rule that
        // did not survive compilation must not match anything, since matching it
        // would suppress a stop.
        if (!isValidCoMovementRule(rule.messageSource, rule.alertType, rule.primaryType,
                                   rule.configuredSecondaryType, rule.proximityViolationPresent,
                                   rule.distanceThresholdPresent, rule.hasViolationFilter,
                                   rule.outputEvent) ||
            !isValidProximityPairRuleId(ruleId) ||
            !isValidCoMovementParams(rule.coMovement)) {
            return false;
        }
        if (alert.candidateKind != AlertCandidateKind::kCoMovementPair) {
            return false;
        }
        // The candidate always carries the carrier first, so the roles are matched
        // in order rather than unordered.
        if (!matchesConfiguredStringCondition(rule.primaryType, alert.object.type) ||
            !matchesConfiguredStringCondition(rule.configuredSecondaryType,
                                              alert.object2.type)) {
            return false;
        }
    } else if (alert.candidateKind == AlertCandidateKind::kCoMovementPair) {
        // No other rule category may consume a co-movement candidate.
        return false;
    } else if (rule.isPairRule) {
        if (!isValidProximityPairRule(rule.messageSource, rule.alertType, rule.primaryType,
                                      rule.configuredSecondaryType,
                                      rule.proximityViolationPresent, rule.proximityViolation,
                                      rule.distanceThresholdPresent, rule.distanceThreshold,
                                      rule.hasViolationFilter) ||
            !isValidProximityPairRuleId(ruleId)) {
            return false;
        }
        if (alert.candidateKind != AlertCandidateKind::kProximityPair) {
            return false;
        }
        if (!matchesConfiguredProximityPair(rule.primaryType, rule.configuredSecondaryType,
                                            alert.object.type, alert.object2.type)) {
            return false;
        }
        if (rule.proximityViolation) {
            if (alert.proximitySelection != ProximitySelection::kThresholdViolation ||
                alert.proximityThreshold != rule.distanceThreshold) {
                return false;
            }
        } else if (alert.proximitySelection != ProximitySelection::kNoViolation) {
            return false;
        }
    } else if (alert.candidateKind == AlertCandidateKind::kProximityPair) {
        // A generic frame-social rule cannot consume a pair-specific candidate.
        return false;
    }
    if (!violationRuleMatchesCandidateScope(rule.restrictedFilter, rule.confinedFilter,
                                            rule.socialFilter, rule.messageSource,
                                            rule.alertType, alert.messageSource,
                                            alert.type)) {
        return false;
    }
    if (!violationRuleMatchesCandidateKind(rule.restrictedFilter, rule.confinedFilter,
                                           rule.socialFilter, alert.candidateKind)) {
        return false;
    }
    if (!matchesConfiguredStringCondition(rule.messageSource, alert.messageSource)) return false;
    if (!matchesConfiguredStringCondition(rule.alertType, alert.type)) return false;
    if (!matchesConfiguredStringCondition(rule.eventType, alert.eventType)) return false;
    // Both pair-shaped categories matched their object types by role above, and
    // neither candidate carries a rule identifier of its own.
    const bool isPairShapedRule = rule.isPairRule || rule.isCoMovementRule;
    if (!isPairShapedRule &&
        !matchesConfiguredStringCondition(rule.objectType, alert.object.type)) return false;
    if (!isPairShapedRule && !ruleId.empty()) {
        if (alert.ruleId[0] == '\0') return false;
        std::string alertRuleId(alert.ruleId);
        bool idMatch = stringEqualsCaseInsensitive(ruleId, alertRuleId.c_str())
            || (alertRuleId.size() > ruleId.size() && alertRuleId.compare(0, ruleId.size(), ruleId) == 0 && alertRuleId[ruleId.size()] == ':');
        if (!idMatch) return false;
    }
    if (!parseViolationFilter(rule.restrictedFilter, alert.restrictedAreaViolation))
        return false;
    if (!parseViolationFilter(rule.confinedFilter, alert.confinedAreaViolation))
        return false;
    if (!parseViolationFilter(rule.socialFilter, alert.socialDistancingViolation))
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
        {"EVENT_23", EVENT_23},
        {"SW_FAIL", SW_FAIL},
    };
    auto it = kEventTypeMap.find(s);
    return (it != kEventTypeMap.end()) ? it->second : EVENT_UNKNOWN;
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

void SafetyEventReporter::setSensorIdentityConfig(const std::string& path,
                                                 IdentitySource source) {
    sensorConfigPath_ = path;
    identitySource_   = source;
}

bool SafetyEventReporter::init(bool debugMode, std::atomic<bool>& stopFlag) {
    debugMode_ = debugMode;
    stopFlag_ = &stopFlag;

    if (!sensorConfigPath_.empty()) {
        if (identitySource_ == IdentitySource::CameraConfig) {
            std::string cfgErr;
            auto entries = sensorConfigLoad(sensorConfigPath_, &cfgErr);
            if (entries.empty()) {
                NvPSBWriteData(NVPSB_LOG_ERR, "Failed to load sensor config: " + cfgErr, "");
                return false;
            }
            sensorNameToPid_ = sensorConfigNameToIdMap(entries);
        } else {
            /* Two columns, no rtspUrl -- a BEV group has no stream, which is
             * exactly why it cannot be expressed in sensor_config.conf and why
             * these are separate files rather than one with an extra column.
             * sensorPipelinesParse refuses a third field, so a sensor_config
             * handed in here is reported as the wrong file rather than being
             * silently accepted with the URL ignored. */
            SensorPipelines pipelines{};
            const SensorConfigErr rc =
                sensorPipelinesParse(sensorConfigPath_.c_str(), &pipelines);
            if (rc != SENSOR_CONFIG_OK) {
                NvPSBWriteData(NVPSB_LOG_ERR,
                    "Failed to load sensor pipelines config: " +
                    std::string(sensorConfigErrStr(rc)), "file: " + sensorConfigPath_);
                return false;
            }
            sensorNameToPid_.clear();
            for (size_t i = 0U; i < pipelines.count; ++i) {
                sensorNameToPid_[pipelines.entries[i].sensorName] =
                    pipelines.entries[i].pipelineId;
            }
        }

        /* Name the FILE and the identities it resolved, not just a count. In 3D
         * this list is expected to hold the group alone; a reader who sees
         * cameras here is looking at a 2D file in a 3D deployment, where every
         * group-addressed event would be dropped as unresolvable. */
        std::string names;
        for (const auto& kv : sensorNameToPid_) {
            if (!names.empty()) names += ", ";
            names += kv.first + ":" + std::to_string(kv.second);
        }
        NvPSBWriteData(NVPSB_LOG_INFO,
            std::string("Sensor identity map loaded from ") +
            (identitySource_ == IdentitySource::CameraConfig
                 ? "sensor_config" : "sensor_pipelines_config"),
            "file: " + sensorConfigPath_ + ", count: " +
            std::to_string(sensorNameToPid_.size()) + ", {" + names + "}");
    }

    if (!debugMode_) {
        if (NvPSSRegisterPSSClientInstance(&pssClientId_, CLIENT_MDX, pssInstanceId_) !=
            NVPSSD_SUCCESS) {
            /* A refused numbered instance is usually a second mdx_client started
             * with the same --instance-id: PSS allows one client per instance. */
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register PSS client",
                "instanceId: " + std::to_string(pssInstanceId_));
            return false;
        }
        NvPSBWriteData(NVPSB_LOG_INFO, "Registered with PSS",
            pssInstanceId_ == 0U
                ? std::string("instanceId: 0 (anonymous -- PSS cannot report this client's loss)")
                : "instanceId: " + std::to_string(pssInstanceId_));
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
        const EventMapping& mapping) {
    const EventMappingRule* matchedRule = nullptr;
    for (size_t i = 0; i < mapping.rules.size(); i++) {
        if (ruleMatches(mapping.rules[i], alertMsg)) {
            matchedRule = &mapping.rules[i];
            break;
        }
    }
    if (matchedRule == nullptr) return true;

    std::string sensorIdStr(alertMsg.sensorId);

    if (!shouldReportFrameCandidate(alertMsg.messageSource, matchedRule->messageSource,
                                    matchedRule->scaleFactor, alertMsg.frameOrdinal)) {
        return true;
    }

    /* Validity gate, not a payload. Wire schema 4 removed
     * SafetyEvent.ruleIdentifier, so this identifier is no longer sent
     * anywhere -- only whether it could be formed at all is consumed here. An
     * empty result means a proximity-pair rule carries an unusable rule_id,
     * which rejects the alert. Named for what it decides, because a
     * std::string holding a value nothing reads invites the next reader to
     * assume it still reaches PSS. */
    const bool ruleIdentityValid = !selectOutputRuleIdentifier(
        alertMsg.candidateKind, alertMsg.ruleId, matchedRule->ruleId).empty();
    if (!ruleIdentityValid) {
        return false;
    }

    SafetyEvent safetyEvent = {};
    /* Pass the producer's endTimestamp through, translated into the
     * CLOCK_MONOTONIC frame the rest of the stack uses. */
    safetyEvent.timestamp = stringToNS(alertMsg.endTimestamp);

#ifdef NVPSF_DBG
    /* Ingest age: how stale the producer's stamp already is by the time this
     * event is minted here. Everything it covers is UPSTREAM of PSF -- the VSS
     * perception/analytics pipeline plus the Kafka hop -- so subtracting it
     * from the age the SDM measures isolates what PSF itself contributes.
     *
     * clock_gettime(CLOCK_MONOTONIC) to match the epoch stringToNS() just
     * mapped into. Debug-only: nothing in the reporting path reads this. */
    {
        struct timespec ingestTs = {};
        if (safetyEvent.timestamp != 0U &&
            clock_gettime(CLOCK_MONOTONIC, &ingestTs) == 0) {
            const uint64_t nowNs =
                static_cast<uint64_t>(ingestTs.tv_sec) * 1000000000ULL +
                static_cast<uint64_t>(ingestTs.tv_nsec);
            if (nowNs >= safetyEvent.timestamp) {
                std::cout << "MDX_INGEST_AGE_US "
                          << ((nowNs - safetyEvent.timestamp) / 1000ULL)
                          << " src=" << alertMsg.messageSource
                          << " ts=" << alertMsg.endTimestamp << std::endl;
            }
        }
    }
#endif

    safetyEvent.id = alertMsg.id;
    safetyEvent.confidenceLevel = 0.7f;
    safetyEvent.processed = false;
    safetyEvent.type = stringToEventType(matchedRule->outputEvent);
    strncpy(safetyEvent.sensorIdentifier, alertMsg.sensorId, MAX_INDENTIFIER_LENGTH - 1);
    safetyEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    /* Rule identity is the EVENT_* value; the configured rule_id stays client-side. */
    safetyEvent.cause = CAUSE_NONE;

    const bool isProximityAlert = shouldPopulatePairFusionMetadata(
        alertMsg.candidateKind);
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
        const Coordinate commandLocation = selectCommandLocationCoordinate(alertMsg);
        safetyEvent.fusionMetadata.coordinates[0].x = commandLocation.x;
        safetyEvent.fusionMetadata.coordinates[0].y = commandLocation.y;
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
    if (pidIt == sensorNameToPid_.end()) {
        /* Drop, do not fall back to pipelineID 0.
         *
         * 0 is the daemon's "unknown sensor" passthrough, reserved for events
         * that name no sensor at all (heartbeat SW_FAIL, status no-op). An
         * application event sent with it skips the sensor_config membership
         * check in the daemon and is stamped isHealthy=true permanently,
         * because QueryTrustState(0) can never report a sensor invalid --
         * so it would drive an ATL or PXC decision with no perception-health
         * gating whatsoever.
         *
         * The daemon refuses such an event too (REPORT_REJECTED_UNRESOLVED_SENSOR).
         * Dropping here as well is not redundant: this is the only place that
         * knows the offending sensor NAME, which is what an operator needs to
         * fix the configuration. */
        /* Name the file the map was ACTUALLY built from. The identity source is
         * mode-dependent (sensor_config.conf in 2D, sensor_pipelines_config.conf
         * in 3D), so a fixed "add it to sensor_config" is not merely imprecise
         * in 3D -- it is wrong. Adding a camera to sensor_config changes nothing
         * there, because the map is not built from that file, and the operator
         * would edit the wrong thing while the events keep dropping. */
        const bool fromCameras = (identitySource_ == IdentitySource::CameraConfig);
        const std::string sourceName = fromCameras ? "sensor_config"
                                                   : "sensor_pipelines_config";
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Dropping event: sensor not in " + sourceName +
                ", so it has no pipelineID: " + sensorIdStr,
            "output_event: " + matchedRule->outputEvent +
                ", identity file: " + sensorConfigPath_ +
                ". Add the sensor to " + sourceName +
                ", or correct the sensor name the producer emits." +
                (fromCameras
                     ? ""
                     : " In 3D this file names the BEV group, not the cameras, so a"
                       " per-camera name arriving here means the producer and the"
                       " deployment topology disagree."));
        return false;
    }
    safetyEvent.fusionMetadata.pipelineID = pidIt->second;
    safetyEvent.fusionMetadata.clientID = static_cast<uint8_t>(pssClientId_);
    safetyEvent.fusionMetadata.speed = alertMsg.speed;

    if (debugMode_) {
        printSafetyEvent(std::cout, safetyEvent, *matchedRule);
        return true;
    }
    if (NvPSSReportSafetyEvent(pssClientId_, &safetyEvent) == NVPSSD_SUCCESS) {
        std::string detail;
        if (alertMsg.candidateKind == AlertCandidateKind::kCoMovementPair) {
            // The evidence is logged with the event because this is the only
            // record of why a stop was later suppressed for this pair.
            char evidence[192];
            snprintf(evidence, sizeof(evidence),
                     " carrier=%u rider=%u offsetSd=%.3fm travel=%.2fm containment=%.2f%s",
                     alertMsg.objectId, alertMsg.objectId2,
                     static_cast<double>(alertMsg.coMovement.offsetSd),
                     static_cast<double>(alertMsg.coMovement.carrierTravel),
                     static_cast<double>(alertMsg.coMovement.boxContainment),
                     alertMsg.coMovement.travelWaivedByBox ? " (travel waived by box)" : "");
            detail = evidence;
        }
        NvPSBWriteData(NVPSB_LOG_INFO, "Safety event reported: " + matchedRule->outputEvent +
            " (rule: " + matchedRule->name + ")" + detail, "");
        return true;
    }
    NvPSBWriteData(NVPSB_LOG_ERR, "Failed to report safety event to PSS", "");
    return false;
}

void SafetyEventReporter::printSafetyEvent(std::ostream& out, const SafetyEvent& e,
        const EventMappingRule& rule) {
    const std::string& ruleName = rule.name;
    out << "[MDX_DEBUG] Matched rule: \"" << (ruleName.empty() ? "(unnamed)" : ruleName) << "\"\n"
        << "  output_event=" << rule.outputEvent
        << "\n  SafetyEvent: id=" << e.id << " type=" << e.type
        /* Numeric: see pss_protocol.h -- no enum-to-string helper exists. */
        << " sensor=" << e.sensorIdentifier
        << " cause=" << static_cast<unsigned>(e.cause)
        << " objectID[0]=" << e.fusionMetadata.objectID[0] << " objectID[1]=" << e.fusionMetadata.objectID[1]
        << " timestamp=" << e.timestamp << " confidence=" << e.confidenceLevel << std::endl;
}

} // namespace MDXClient
