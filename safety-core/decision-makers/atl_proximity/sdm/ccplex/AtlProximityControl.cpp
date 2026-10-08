/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Forklift-to-person proximity SDM for the combined ATL + proximity deployment.
 *
 * This component owns its decision logic. It started as a copy of
 * decision-makers/proximity/sdm/ccplex/ProximityControl.cpp so that the proven
 * transport and fail-safe machinery (gateway registration and REGR retry,
 * three-tier gateway-heartbeat watchdog, DecisionRequest validation and
 * sequence checking, decision-freshness watchdog, sensor-health tracking,
 * safe-state latching, PLC safe-release handshake, ACK tracking) is carried
 * over intact rather than reimplemented and accidentally weakened.
 *
 * decision-makers/proximity is deployed and field-validated, so it is left
 * untouched by this use case. The two components are independent from here on:
 * a change here must not be assumed to apply there, and vice versa.
 *
 * WHAT DIFFERS FROM THE ORIGINAL
 *
 * Only the batch-to-command step. The original collapses a DecisionRequest by
 * "last qualifying event by index", which lets a safe reading for one object
 * pair silence a still-dangerous reading for a different pair in the same
 * batch. This component instead accumulates observations into explicit
 * per-pair state keyed by (forklift, person) role and derives the command from
 * the worst case across all live pairs. See atl_proximity_pair_fusion.h for the
 * model and for why role resolution has to be backed by the event mapping.
 */

#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <map>
#include <csignal>
#include <condition_variable>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "AtlProximityControl.h"
#include "NvPSDGatewayProtocol.h"
#include "pss_message_validate.h"
#include "sdm_decision_request_state.h"
#include "sdm_decision_freshness.hpp"
#include "sdm_gateway_decision_sequence.hpp"
#include "atl_proximity_pair_fusion.h"

/* Simple SDM logging: console + optional log file (no NvPSB dependency) */
static std::mutex apxLogMtx;
static FILE* apxLogFile = nullptr;

static void apx_log_open(void)
{
    std::lock_guard<std::mutex> lock(apxLogMtx);
    if (!apxLogFile) {
        apxLogFile = fopen("atl_proximity_sdm.log", "a");
        if (apxLogFile)
            setvbuf(apxLogFile, nullptr, _IOLBF, 0);
    }
}

static void apx_log_close(void)
{
    std::lock_guard<std::mutex> lock(apxLogMtx);
    if (apxLogFile) {
        fclose(apxLogFile);
        apxLogFile = nullptr;
    }
}

/* fprintf to stdout/stderr and optional atl_proximity_sdm.log (under apxLogMtx). */
static void apx_log(const char* level, const char* msg)
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000);
    struct tm tm_buf;
    struct tm* lt = localtime_r(&t, &tm_buf);

    char prefix[48];
    if (lt)
        snprintf(prefix, sizeof(prefix), "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
                 lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                 lt->tm_hour, lt->tm_min, lt->tm_sec, ms);
    else
        snprintf(prefix, sizeof(prefix), "[1970-01-01 00:00:00.000]");

    std::lock_guard<std::mutex> lock(apxLogMtx);
    FILE* out = (strcmp(level, "ERR") == 0 || strcmp(level, "WARNING") == 0)
                ? stderr : stdout;
    fprintf(out, "%s [APX][%s] %s\n", prefix, level, msg);
    if (apxLogFile)
        fprintf(apxLogFile, "%s [APX][%s] %s\n", prefix, level, msg);
}

static void apx_log_info(const std::string& msg)    { apx_log("INFO", msg.c_str()); }
static void apx_log_warning(const std::string& msg) { apx_log("WARNING", msg.c_str()); }
static void apx_log_err(const std::string& msg)     { apx_log("ERR", msg.c_str()); }

#define HEARTBEAT_PERIOD_SEC   5
#define ACK_TIMEOUT_SECONDS    3

/* Valid range for launchAtlProximityControlAlgo(decisionRepeatIntervalMs). 0 disables
 * the periodic re-assert; non-zero values are clamped to [100 ms, 36000 ms] so we
 * neither flood the PLC nor let the last decision age past a typical ACK window. */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                      = 100U;
static constexpr std::uint32_t kHbTimingMsMax                      = 600000U;
static constexpr int SAFE_RELEASE_MIN_INTERVAL_MS                  = 1000;
#define ACK_CLEANUP_SECONDS    10
#define ACK_MONITOR_INTERVAL_S 1
#define ACK_RECEIVER_SLEEP_MS  100
#define MAIN_LOOP_POLL_TIMEOUT_MS 50

/* Proximity event types and their severity ordering live in
 * atl_proximity_pair_fusion.h (ATLPXC_EVENT_*), so the mapping from event type
 * to severity tier has one definition. */

/* Heartbeat protocol (NvPSDGatewayProtocol.h) */
/* Re-register with gateway periodically so we recover after gateway restart (in-memory state lost). */
static constexpr int REG_RETRY_INTERVAL_MS = 30000;

/* Event types Proximity subscribes to (must match onAtlProximityEventNotificationReceive) */
static constexpr EventType ATL_PROXIMITY_SUBSCRIBED_EVENTS[] = { EVENT_8, EVENT_9, EVENT_10, SW_FAIL };
static constexpr uint8_t  ATL_PROXIMITY_SUBSCRIBED_COUNT =
    sizeof(ATL_PROXIMITY_SUBSCRIBED_EVENTS) / sizeof(ATL_PROXIMITY_SUBSCRIBED_EVENTS[0]);

struct SafetyRelevantSensor {
    uint8_t pipelineId;
    const char* sensorName;
};

/* Reviewed sensor set from pkg/sensor_config.conf. Gateway auto-subscribes
 * SENSOR_INVALID/SENSOR_VALID; APX uses pipelineID as the safety key. */
static constexpr SafetyRelevantSensor kSafetyRelevantSensors[] = {
    {1U, "Camera"},
    {2U, "Camera_01"},
    {3U, "Camera_02"},
};
static constexpr uint32_t kSafetyRelevantSensorMask =
    (1U << (kSafetyRelevantSensors[0].pipelineId - 1U)) |
    (1U << (kSafetyRelevantSensors[1].pipelineId - 1U)) |
    (1U << (kSafetyRelevantSensors[2].pipelineId - 1U));
static_assert(kSafetyRelevantSensors[0].pipelineId >= 1U &&
              kSafetyRelevantSensors[1].pipelineId >= 1U &&
              kSafetyRelevantSensors[2].pipelineId >= 1U,
              "Pipeline IDs are one-based");
static_assert(kSafetyRelevantSensors[0].pipelineId <= 8U &&
              kSafetyRelevantSensors[1].pipelineId <= 8U &&
              kSafetyRelevantSensors[2].pipelineId <= 8U,
              "Pipeline IDs must stay within sensor_config bounds");
static_assert(kSafetyRelevantSensors[0].pipelineId != kSafetyRelevantSensors[1].pipelineId &&
              kSafetyRelevantSensors[0].pipelineId != kSafetyRelevantSensors[2].pipelineId &&
              kSafetyRelevantSensors[1].pipelineId != kSafetyRelevantSensors[2].pipelineId,
              "Pipeline IDs must be unique");

/* PLC command socket */
static int plcSock = -1;
static struct sockaddr_in plcAddr = {};
/* CmdPacket.seq is uint16_t; use low 16 bits of this monotonic counter for a full 0..65535 wrap. */
static std::atomic<uint32_t> cmdSeqNo{0};
static std::mutex plcSocketMtx;

static uint16_t nextCommandSeq()
{
    const uint32_t n = cmdSeqNo.fetch_add(1U, std::memory_order_relaxed);
    return static_cast<uint16_t>(n & 0xFFFFU);
}

/* ACK tracking */
struct CommandStatus {
    std::chrono::system_clock::time_point sentTime;
    unsigned char command;
    bool acknowledged;
    uint64_t sentTimeSec;
    uint64_t sentTimeMicro;
    CommandStatus()
        : sentTime(std::chrono::system_clock::now()), command(0),
          acknowledged(false), sentTimeSec(0), sentTimeMicro(0) {}
    CommandStatus(unsigned char cmd, uint64_t sec, uint64_t micro)
        : sentTime(std::chrono::system_clock::now()), command(cmd),
          acknowledged(false), sentTimeSec(sec), sentTimeMicro(micro) {}
};
static std::map<uint16_t, CommandStatus> pendingCommands;
static std::mutex commandStatusMtx;

/* threads */
static std::atomic<bool> stopSDMThreads{false};
static std::atomic<bool> signalShutdownRequested{false};
static std::thread       heartbeatThread;
static std::thread       ackHandlerThread;    // receiver + timeout monitor
static std::thread       hbWatchdogThread;
static std::thread       decisionFreshnessWatchdogThread;
static std::thread       periodicDecisionThread;
/* Shutdown-aware sleep for the periodic decision loop: waiting on this CV
 * instead of sleep_for lets shutdownAtlProximityControlAlgo() return in O(ms)
 * rather than waiting up to one full decisionRepeatIntervalMs interval. */
static std::mutex              atlProximityPeriodicDecisionWaitMtx;
static std::condition_variable atlProximityPeriodicDecisionCv;

/* Serializes decision sends (event-driven vs HB watchdog vs periodic repeat).
 * Ordering: always lock decisionSendMtx first, then atlProximityStateMtx. */
static std::mutex                            decisionSendMtx;

/* Retained forklift-to-person proximity state and the command last derived from
 * it. Both are guarded by atlProximityStateMtx.
 *
 * atlProximityPairTable is the authoritative view: one entry per live pair,
 * newest observation per pair. The command is always derived from it by
 * worst-case arbitration, never accumulated incrementally, so the event path
 * and the periodic repeat thread cannot disagree about the current hazard.
 *
 * atlProximityActiveCommand caches that derivation for logging and for the
 * fault paths below, which force CMD_STOP. Those paths also raise a safe-state
 * latch, and a raised latch makes every sender emit the safe hold instead of
 * this value, so the cache is only consulted while no fault is latched.
 *
 * CMD_NORMAL is the safe initial value: before the first event there is no
 * observed hazard, and absent gateway traffic the decision-freshness watchdog
 * is what drives the SDM into safe hold. */
static std::mutex                            atlProximityStateMtx;
static unsigned char                         atlProximityActiveCommand = CMD_NORMAL;
static AtlPxcPairTable                       atlProximityPairTable;
/* Period (ms) for the periodic decision-repeat thread; 0 = disabled.
 * Read in the hot path of the repeat loop, so kept lock-free. */
static std::atomic<std::uint32_t>            g_decisionRepeatIntervalMs{5000U};

/* PSD Gateway socket: bind ephemeral, send REGR to gateway, recv DecisionRequests + HB */
static int psdGatewayListenSock = -1;
static std::mutex gwSockMtx;               // guards psdGatewayListenSock
static struct sockaddr_in gatewayAddr = {}; // gateway address for registration send
static std::chrono::steady_clock::time_point lastRegistrationTime;  // for periodic re-registration

/* Signal-safe flag: set in handler; main/event loop sets stop atomic. */
static volatile sig_atomic_t g_signal_received = 0;

/* Heartbeat watchdog state */
static std::mutex                            hbMtx;
static std::chrono::steady_clock::time_point hbLastRecvTime;
static std::atomic<bool>                     hbGatewayAlive{false};

static std::atomic<uint32_t>                 g_maxHbFailuresCfg{10U};
static std::atomic<uint32_t>                 g_warnThresholdCfg{5U};
static std::atomic<std::uint32_t>            g_hbStaleMs{5000U};
static std::atomic<std::uint32_t>            g_hbPeriodMs{5500U};
static std::atomic<bool>                     hbFaultLatched{false};
/* Tier-2 gateway HB safe-hold: periodic/event send CMD_STOP+CMD_SW_ERROR while true; fusion keeps atlProximityActiveCommand. */
static std::atomic<bool>                     hbTier2SafeHoldActive{false};
/* PSS ERROR/SW_FAIL active flag. The safe-state latch clears only on PLC safe-release. */
static std::atomic<bool>                     pssErrorFusionSuppressLatched{false};
static std::atomic<bool>                     pssFaultActive{false};
static constexpr uint32_t                    kSafeLatchGatewayHb = 1U << 0;
static constexpr uint32_t                    kSafeLatchPss       = 1U << 1;
static constexpr uint32_t                    kSafeLatchDecisionFreshness = 1U << 2;
static constexpr uint32_t                    kSafeLatchGatewayDecisionSequence = 1U << 3;
static constexpr uint32_t                    kSafeLatchAllSafetyRelevantSensorsFailed = 1U << 4;
static std::atomic<uint32_t>                 safeStateLatchCauses{0U};
static std::mutex                            safeStateLatchMtx;
static std::atomic<uint32_t>                 failedSafetyRelevantSensorMask{0U};
static std::atomic<bool>                     allSafetyRelevantSensorsFailedActive{false};
static std::atomic<int>                      regrTier2AttemptsRemaining{0};
static std::atomic<uint32_t>                 g_lastGatewayHbMissCount{0U};
static std::mutex                            safeReleaseRateMtx;
static std::chrono::steady_clock::time_point lastSafeReleaseRequestTime;
static std::mutex                            decisionFreshnessMtx;
static std::chrono::steady_clock::time_point lastValidDecisionRequestTime;
static std::atomic<bool>                     decisionFreshnessFaultActive{false};
static std::atomic<bool>                     gatewayDecisionSequenceFaultActive{false};
static std::atomic<std::uint32_t>            g_decisionFreshnessTimeoutMs{
    SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT};
static SdmGatewayDecisionSequenceState       gatewayDecisionSequenceState = {};

static uint32_t gatewayMissFromElapsedMs(int64_t elapsedMs)
{
    const int64_t staleStartMs = static_cast<int64_t>(
        g_hbStaleMs.load(std::memory_order_relaxed));
    const int64_t periodMs = static_cast<int64_t>(
        g_hbPeriodMs.load(std::memory_order_relaxed));
    if (elapsedMs <= staleStartMs)
        return 0U;
    const uint64_t m = 1U + static_cast<uint64_t>((elapsedMs - staleStartMs) / periodMs);
    const uint32_t maxF = g_maxHbFailuresCfg.load();
    if (m > static_cast<uint64_t>(maxF))
        return maxF;
    return static_cast<uint32_t>(m);
}

static bool safeStateLatched()
{
    return safeStateLatchCauses.load(std::memory_order_acquire) != 0U;
}

static void markPssFaultActive()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    pssFaultActive.store(true, std::memory_order_release);
    pssErrorFusionSuppressLatched.store(true, std::memory_order_release);
    safeStateLatchCauses.fetch_or(kSafeLatchPss, std::memory_order_acq_rel);
}

static void clearPssFaultActive()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    pssFaultActive.store(false, std::memory_order_release);
    pssErrorFusionSuppressLatched.store(false, std::memory_order_release);
}

static uint32_t safetyRelevantSensorBit(uint32_t pipelineId)
{
    if (pipelineId == 0U || pipelineId > 31U)
        return 0U;

    for (const SafetyRelevantSensor& sensor : kSafetyRelevantSensors)
    {
        if (pipelineId == static_cast<uint32_t>(sensor.pipelineId))
            return 1U << (pipelineId - 1U);
    }
    return 0U;
}

static bool markAllSafetyRelevantSensorsFailedLatched()
{
    if (allSafetyRelevantSensorsFailedActive.exchange(true, std::memory_order_acq_rel))
        return false;

    {
        std::lock_guard<std::mutex> lock(safeStateLatchMtx);
        safeStateLatchCauses.fetch_or(kSafeLatchAllSafetyRelevantSensorsFailed,
                                      std::memory_order_acq_rel);
    }
    apx_log_err("APX: All safety-relevant sensors failed; entering safe hold");
    return true;
}

static void clearAllSafetyRelevantSensorsFailedActiveIfRecovered(uint32_t failedMask)
{
    if ((failedMask & kSafetyRelevantSensorMask) == kSafetyRelevantSensorMask)
        return;

    if (allSafetyRelevantSensorsFailedActive.exchange(false, std::memory_order_acq_rel))
    {
        apx_log_info("APX: All safety-relevant sensors failed condition cleared; latch awaits PLC release");
    }
}

static uint32_t updateSafetyRelevantSensorFailedBit(uint32_t bit, bool failed)
{
    if (failed)
        return failedSafetyRelevantSensorMask.fetch_or(bit, std::memory_order_acq_rel) | bit;
    return failedSafetyRelevantSensorMask.fetch_and(~bit, std::memory_order_acq_rel) & ~bit;
}

static bool updateSensorHealthFromDecisionRequest(const DecisionRequest* request)
{
    if (request == nullptr)
        return false;

    bool latchEntered = false;
    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    for (uint8_t i = 0U; i < maxSrc; ++i)
    {
        const SensorData& sd = request->sensorDataSummary[i];
        if (sd.event.status == STALE)
            continue;

        const EventType et = static_cast<EventType>(sd.event.type);
        if (et == PSS_STATUS_NOOP)
            continue;

        const uint32_t pipelineId =
            static_cast<uint32_t>(sd.event.fusionMetadata.pipelineID);
        const uint32_t bit = safetyRelevantSensorBit(pipelineId);
        if (bit == 0U)
            continue;

        bool updateMask = false;
        bool failed = false;
        if (et == SENSOR_INVALID || !sd.isHealthy)
        {
            updateMask = true;
            failed = true;
        }
        else if (et == SENSOR_VALID || (sd.isHealthy && sd.isTrustedSource))
        {
            updateMask = true;
            failed = false;
        }

        if (!updateMask)
            continue;

        const uint32_t previousMask =
            failedSafetyRelevantSensorMask.load(std::memory_order_acquire);
        const uint32_t newMask = updateSafetyRelevantSensorFailedBit(bit, failed);
        if (newMask != previousMask)
        {
            const std::string cause(sd.event.ruleIdentifier,
                strnlen(sd.event.ruleIdentifier, sizeof(sd.event.ruleIdentifier)));
            apx_log_info("APX: safety-sensor health change pipelineID=" +
                std::to_string(pipelineId) +
                (failed ? " state=FAILED" : " state=RECOVERED") +
                " cause=" + (cause.empty() ? std::string("UNKNOWN") : cause));
        }

        if ((newMask & kSafetyRelevantSensorMask) == kSafetyRelevantSensorMask)
            latchEntered = markAllSafetyRelevantSensorsFailedLatched() || latchEntered;
        else
            clearAllSafetyRelevantSensorsFailedActiveIfRecovered(newMask);
    }

    return latchEntered;
}

static bool markGatewayHbFaultLatched()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    if (hbFaultLatched.load(std::memory_order_acquire))
        return false;
    hbFaultLatched.store(true, std::memory_order_release);
    safeStateLatchCauses.fetch_or(kSafeLatchGatewayHb, std::memory_order_acq_rel);
    return true;
}

static bool clearGatewayHbFaultActive()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    return hbFaultLatched.exchange(false, std::memory_order_acq_rel);
}

static void resetSafeStateFaultsForLaunch()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    hbFaultLatched.store(false, std::memory_order_release);
    hbTier2SafeHoldActive.store(false, std::memory_order_release);
    pssErrorFusionSuppressLatched.store(false, std::memory_order_release);
    pssFaultActive.store(false, std::memory_order_release);
    decisionFreshnessFaultActive.store(false, std::memory_order_release);
    gatewayDecisionSequenceFaultActive.store(false, std::memory_order_release);
    failedSafetyRelevantSensorMask.store(0U, std::memory_order_release);
    allSafetyRelevantSensorsFailedActive.store(false, std::memory_order_release);
    sdmGatewayDecisionSequenceReset(&gatewayDecisionSequenceState);
    safeStateLatchCauses.store(0U, std::memory_order_release);
}

/* ====================== helpers ====================== */

/* Monotonic milliseconds for pair-state ageing. Deliberately steady_clock and
 * not the wall clock used for packet timestamps: a wall-clock step must not age
 * out or resurrect a tracked pair. */
static std::uint64_t atlProximityMonotonicMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket()
{
    auto now   = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    uint64_t totalSec  = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    uint64_t totalUsec = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();
    uint64_t microPart = totalUsec - (totalSec * 1000000ULL);
    return { totalSec, microPart };
}

static void copyObjectRecordFromMetadata(ObjectRecord* object,
                                         const EventFusionMetadata& meta,
                                         int index)
{
    if (object == nullptr || index < 0 || index >= COMMAND_NUM_OBJECTS)
        return;

    object->object_id = meta.objectID[index];
    if (index < MAX_TRAJECTORY_COORDINATES) {
        object->x = meta.coordinates[index].x;
        object->y = meta.coordinates[index].y;
    }
    object->z = 0.0f;
    object->metadata = static_cast<uint32_t>(meta.objectType[index]);
}

static void fillRoleNormalizedObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS],
                                            const EventFusionMetadata& meta)
{
    int firstPerson = -1;
    int firstNonPerson = -1;

    for (int i = 0; i < COMMAND_NUM_OBJECTS; i++) {
        if (meta.objectType[i] == PERSON) {
            if (firstPerson < 0)
                firstPerson = i;
        } else if (firstNonPerson < 0) {
            firstNonPerson = i;
        }
    }

    if (firstNonPerson >= 0)
        copyObjectRecordFromMetadata(&objects[0], meta, firstNonPerson);
    if (firstPerson >= 0)
        copyObjectRecordFromMetadata(&objects[1], meta, firstPerson);
}

/* Report the pair that actually drove the command, taken from retained state
 * rather than from a slot in the current batch. The worst-case pair may have
 * been observed in an earlier DecisionRequest and be absent from this one, so
 * there is no batch slot to point at.
 *
 * Slot order matches fillRoleNormalizedObjectRecords(): objects[0] is the
 * non-person endpoint (the forklift), objects[1] is the person. */
static void fillObjectRecordsFromPair(ObjectRecord objects[COMMAND_NUM_OBJECTS],
                                      const AtlPxcPairObjects& pair)
{
    std::memset(objects, 0, sizeof(ObjectRecord) * COMMAND_NUM_OBJECTS);

    objects[0].object_id = pair.forkliftObjectID;
    objects[0].x         = pair.forkliftX;
    objects[0].y         = pair.forkliftY;
    objects[0].z         = 0.0f;
    objects[0].metadata  = static_cast<uint32_t>(pair.forkliftObjectType);

    objects[1].object_id = pair.personObjectID;
    objects[1].x         = pair.personX;
    objects[1].y         = pair.personY;
    objects[1].z         = 0.0f;
    objects[1].metadata  = static_cast<uint32_t>(pair.personObjectType);
}

static bool isUsableObjectMetadataSlot(const DecisionRequest* request,
                                       uint8_t maxSrc,
                                       int slot)
{
    return request != nullptr &&
           slot >= 0 &&
           slot < maxSrc &&
           request->sensorDataSummary[slot].event.status != STALE &&
           request->sensorDataSummary[slot].isHealthy &&
           request->sensorDataSummary[slot].isTrustedSource;
}

/* Populate ObjectRecord from a usable sensor slot. A supplied winningSlot must
 * pass the same freshness, health, and trust checks as fallback selection. */
static void fillObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS],
                               const DecisionRequest* request,
                               int winningSlot = -1)
{
    std::memset(objects, 0, sizeof(ObjectRecord) * COMMAND_NUM_OBJECTS);
    if (!request || request->sensorDataSummarySize == 0)
        return;

    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    int slot = winningSlot;
    if (!isUsableObjectMetadataSlot(request, maxSrc, slot)) {
        slot = -1;
        for (uint8_t i = 0; i < maxSrc; ++i) {
            if (isUsableObjectMetadataSlot(request, maxSrc, static_cast<int>(i))) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return;
    }

    const EventFusionMetadata& meta =
        request->sensorDataSummary[slot].event.fusionMetadata;

    fillRoleNormalizedObjectRecords(objects, meta);
}

bool sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request,
                         int winningSlot,
                         const AtlPxcPairObjects* pairObjects)
{
    const uint16_t seqNo = nextCommandSeq();
    auto timeResult = getCurrentUTCTimeForPacket();
    uint64_t tsSec   = timeResult.first;
    uint64_t tsMicro = timeResult.second;


    CmdPacket pkt;
    std::memset(&pkt, 0, sizeof(pkt));

    pkt.identifier      = PROXIMITY_PACKET_IDENTIFIER;
    pkt.seq             = seqNo;
    pkt.command         = command;
    pkt.ts_seconds      = tsSec;
    pkt.ts_microseconds = tsMicro;

    /* Prefer the fused pair when the caller has one: it identifies the pair the
     * command is actually about, which the current batch may not contain. */
    if (pairObjects != nullptr)
        fillObjectRecordsFromPair(pkt.objects, *pairObjects);
    else
        fillObjectRecords(pkt.objects, request, winningSlot);

    pkt.crc32 = cmdPacketCRC32(&pkt);

    bool shouldPrintAndTrack = false;
    const char* cmdType = commandName(command);
    if (command == CMD_STOP || command == CMD_REDUCE || command == CMD_NORMAL ||
        command == CMD_SW_ERROR ||
        command == CMD_SAFE_RELEASE_ACK || command == CMD_SAFE_RELEASE_DENIED)
        shouldPrintAndTrack = true;

    if (shouldPrintAndTrack) {
        std::ostringstream logMsg;
        logMsg << "Sending decision command: " << cmdType
               << " (0x" << std::hex << std::setfill('0') << std::setw(2)
               << (int)command << std::dec
               << "), SeqNo: " << seqNo
               << ", UTC epoch: " << tsSec << "." << std::setfill('0')
               << std::setw(6) << tsMicro
               << ", Obj0 ID: " << pkt.objects[0].object_id
               << ", Obj1 ID: " << pkt.objects[1].object_id;
        apx_log_info(logMsg.str());
    }

    bool sentOk = false;
    const bool trackPendingAck = trackAck && shouldPrintAndTrack;
    /*
     * Register before sendto(): cmd_rx can return an ACK immediately, and
     * registering after send leaves a window where ackHandlerLoop drops that
     * otherwise valid ACK because no pending entry exists yet.
     */
    if (trackPendingAck) {
        std::lock_guard<std::mutex> cmdLock(commandStatusMtx);
        pendingCommands[seqNo] = CommandStatus(command, tsSec, tsMicro);
    }

    {
        std::lock_guard<std::mutex> sockLock(plcSocketMtx);
        if (plcSock >= 0) {
            const ssize_t sent = sendto(plcSock, &pkt, sizeof(pkt), 0,
                                        (struct sockaddr*)&plcAddr, sizeof(plcAddr));
            if (sent == static_cast<ssize_t>(sizeof(pkt))) {
                sentOk = true;
            } else {
                apx_log_err("Failed to send decision command");
            }
        }
    }

    if (!sentOk && trackPendingAck) {
        std::lock_guard<std::mutex> cmdLock(commandStatusMtx);
        const auto it = pendingCommands.find(seqNo);
        if (it != pendingCommands.end() &&
            it->second.command == command &&
            it->second.sentTimeSec == tsSec &&
            it->second.sentTimeMicro == tsMicro) {
            pendingCommands.erase(it);
        }
    }
    return sentOk;
}

static bool isConfiguredPlcEndpoint(const struct sockaddr_in& sender)
{
    return sender.sin_family == AF_INET &&
           sender.sin_port == plcAddr.sin_port &&
           sender.sin_addr.s_addr == plcAddr.sin_addr.s_addr;
}

static bool isConfiguredGatewayEndpoint(const struct sockaddr_in& sender)
{
    return sender.sin_family == AF_INET &&
           sender.sin_port == gatewayAddr.sin_port &&
           sender.sin_addr.s_addr == gatewayAddr.sin_addr.s_addr;
}

static bool isValidPlcInboundCommand(uint8_t command)
{
    return command == CMD_HEARTBEAT ||
           command == CMD_STOP ||
           command == CMD_REDUCE ||
           command == CMD_NORMAL ||
           command == CMD_HW_ERROR ||
           command == CMD_SW_ERROR ||
           command == CMD_SAFE_RELEASE_REQUEST;
}

static void resetAtlProximityStateFromPlcSafeRelease()
{
    std::lock_guard<std::mutex> stateLock(atlProximityStateMtx);
    atlProximityActiveCommand = CMD_NORMAL;
    /* Drop retained pairs too. Keeping them would let a pair observed before
     * the release immediately re-derive STOP from stale data, so the operator's
     * release would appear to have no effect. Live pairs are re-observed within
     * one perception cycle. */
    AtlPxcPairFusionReset(&atlProximityPairTable);
}

static uint32_t activeSafeLatchCauses()
{
    uint32_t active = 0U;
    if (hbFaultLatched.load(std::memory_order_acquire) ||
        hbTier2SafeHoldActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchGatewayHb;
    }
    if (pssFaultActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchPss;
    }
    if (decisionFreshnessFaultActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchDecisionFreshness;
    }
    if (gatewayDecisionSequenceFaultActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchGatewayDecisionSequence;
    }
    if (allSafetyRelevantSensorsFailedActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchAllSafetyRelevantSensorsFailed;
    }
    return active;
}

static bool safeReleaseNoopResponseInRateWindow(
    const std::chrono::steady_clock::time_point& now)
{
    std::lock_guard<std::mutex> lock(safeReleaseRateMtx);
    if (lastSafeReleaseRequestTime != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastSafeReleaseRequestTime).count() < SAFE_RELEASE_MIN_INTERVAL_MS)
    {
        return true;
    }
    return false;
}

static void markSafeReleaseNoopResponseSuccess(
    const std::chrono::steady_clock::time_point& now)
{
    std::lock_guard<std::mutex> lock(safeReleaseRateMtx);
    lastSafeReleaseRequestTime = now;
}

static void sendAtlProximitySafeHold(const DecisionRequest* request = nullptr,
                                  int winningSlot = -1,
                                  const AtlPxcPairObjects* pairObjects = nullptr)
{
    sendDecisionCommand(CMD_STOP, true, request, winningSlot, pairObjects);
    sendDecisionCommand(CMD_SW_ERROR, true, request, winningSlot, pairObjects);
}

static void markDecisionRequestFresh(uint32_t requestId)
{
    {
        std::lock_guard<std::mutex> lock(decisionFreshnessMtx);
        lastValidDecisionRequestTime = std::chrono::steady_clock::now();
    }

    if (decisionFreshnessFaultActive.exchange(false, std::memory_order_acq_rel))
    {
        apx_log_info("APX: DecisionRequest freshness recovered; requestId=" +
            std::to_string(requestId) + "; latch awaits PLC release");
    }
}

static bool markDecisionFreshnessFaultLatched(std::uint64_t elapsedMs)
{
    (void)elapsedMs;
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    if (decisionFreshnessFaultActive.load(std::memory_order_acquire))
        return false;
    decisionFreshnessFaultActive.store(true, std::memory_order_release);
    safeStateLatchCauses.fetch_or(kSafeLatchDecisionFreshness, std::memory_order_acq_rel);
    return true;
}

static bool markGatewayDecisionSequenceFaultLatched(uint64_t gatewayEpoch, uint32_t gatewayTxSeq)
{
    (void)gatewayEpoch;
    (void)gatewayTxSeq;
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    if (gatewayDecisionSequenceFaultActive.load(std::memory_order_acquire))
        return false;
    gatewayDecisionSequenceFaultActive.store(true, std::memory_order_release);
    safeStateLatchCauses.fetch_or(kSafeLatchGatewayDecisionSequence, std::memory_order_acq_rel);
    return true;
}

static void markGatewayDecisionSequenceRecovered(uint64_t gatewayEpoch, uint32_t gatewayTxSeq)
{
    if (gatewayDecisionSequenceFaultActive.exchange(false, std::memory_order_acq_rel))
    {
        apx_log_info("APX: Gateway DecisionRequest sequence recovered; epoch=" +
            std::to_string(gatewayEpoch) + " txSeq=" +
            std::to_string(gatewayTxSeq) + "; latch awaits PLC release");
    }
}

static void sendLatchedAtlProximitySafeHold()
{
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    sendAtlProximitySafeHold();
}

static void decisionFreshnessWatchdog()
{
    while (!stopSDMThreads.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(SDM_DECISION_FRESHNESS_CHECK_PERIOD_MS));

        std::chrono::steady_clock::time_point lastValid;
        {
            std::lock_guard<std::mutex> lock(decisionFreshnessMtx);
            lastValid = lastValidDecisionRequestTime;
        }
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastValid).count());
        const std::uint32_t timeoutMs =
            g_decisionFreshnessTimeoutMs.load(std::memory_order_relaxed);

        if (sdmDecisionFreshnessExpired(elapsedMs, timeoutMs) &&
            markDecisionFreshnessFaultLatched(elapsedMs))
        {
            apx_log_err("APX: DecisionRequest freshness timeout; entering safe hold, elapsed_ms=" +
                std::to_string(elapsedMs));
            sendLatchedAtlProximitySafeHold();
        }
    }
}

static void handleSafeReleaseRequest()
{
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);

    const uint32_t active = activeSafeLatchCauses();
    if (active != 0U)
    {
        apx_log_warning("APX: PLC safe-release denied; active safe-state fault source remains, active_causes=" +
            std::to_string(active));
        sendDecisionCommand(CMD_SAFE_RELEASE_DENIED, false, nullptr);
        sendAtlProximitySafeHold();
        return;
    }

    uint32_t releaseLatchSnapshot = 0U;
    {
        std::lock_guard<std::mutex> latchLock(safeStateLatchMtx);
        releaseLatchSnapshot = safeStateLatchCauses.load(std::memory_order_acquire);
    }

    if (releaseLatchSnapshot != 0U)
    {
        const auto now = std::chrono::steady_clock::now();
        const bool ackSent = sendDecisionCommand(CMD_SAFE_RELEASE_ACK, false, nullptr);
        const bool normalSent = sendDecisionCommand(CMD_NORMAL, true, nullptr);
        if (ackSent && normalSent) {
            /* Only return local state to normal after the peer can observe the
             * accepted release sequence. Do not clear a new latch cause that
             * arrives while TX is in flight. */
            bool latchCleared = false;
            {
                std::lock_guard<std::mutex> latchLock(safeStateLatchMtx);
                const uint32_t currentLatch =
                    safeStateLatchCauses.load(std::memory_order_acquire);
                if (currentLatch == releaseLatchSnapshot &&
                    activeSafeLatchCauses() == 0U) {
                    safeStateLatchCauses.store(0U, std::memory_order_release);
                    latchCleared = true;
                }
            }
            if (latchCleared) {
                resetAtlProximityStateFromPlcSafeRelease();
                hbTier2SafeHoldActive.store(false, std::memory_order_release);
                apx_log_info("APX: PLC safe-release accepted; returning to normal mode");
                markSafeReleaseNoopResponseSuccess(now);
            } else {
                apx_log_warning("APX: safe-release response sent but latch retained due to concurrent fault");
                sendAtlProximitySafeHold();
            }
        } else {
            apx_log_err("APX: safe-release response TX failed; latch retained and state not reset");
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool rateLimited = safeReleaseNoopResponseInRateWindow(now);
    if (rateLimited)
        apx_log_warning("APX: rate-limited PLC safe-release retry; re-sending current safe-release response");
    else
        apx_log_info("APX: PLC safe-release received while no safe-state latch is active");

    const bool ackSent = sendDecisionCommand(CMD_SAFE_RELEASE_ACK, false, nullptr);
    const bool normalSent = sendDecisionCommand(CMD_NORMAL, true, nullptr);
    if (ackSent && normalSent) {
        resetAtlProximityStateFromPlcSafeRelease();
        markSafeReleaseNoopResponseSuccess(now);
    } else {
        apx_log_err("APX: safe-release no-latch response TX failed; state not reset");
    }
}

/* ====================== ACK handler ====================== */

/* ACK receiver + timeout monitor */
void ackHandlerLoop()
{
    char ackBuf[COMMAND_PACKET_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    struct pollfd pfd;
    auto lastTimeoutScan = std::chrono::steady_clock::now();

    while (!stopSDMThreads.load())
    {
        /* Snapshot the fd under the lock (brief hold) */
        {
            std::lock_guard<std::mutex> lock(plcSocketMtx);
            if (plcSock < 0)
                break;
            pfd.fd = plcSock;
        }
        pfd.events  = POLLIN;
        pfd.revents = 0;

        /* Wait for data up to ACK_RECEIVER_SLEEP_MS -- no mutex held */
        int ready = poll(&pfd, 1, ACK_RECEIVER_SLEEP_MS);

        /* ---- Part 1: receive ACK if data available ---- */
        if (ready > 0)
        {
            ssize_t bytes_received = -1;
            {
                std::lock_guard<std::mutex> lock(plcSocketMtx);
                if (plcSock < 0)
                    break;
                sender_len = sizeof(sender_addr);
                bytes_received = recvfrom(plcSock, ackBuf, COMMAND_PACKET_SIZE,
                                          MSG_DONTWAIT,
                                          (struct sockaddr*)&sender_addr,
                                          &sender_len);
            }
            if (bytes_received == COMMAND_PACKET_SIZE) {
                const CmdPacket* ackPkt =
                    reinterpret_cast<const CmdPacket*>(ackBuf);

                if (!isConfiguredPlcEndpoint(sender_addr))
                {
                    apx_log_warning("SDM: dropping PLC packet from unexpected endpoint");
                    continue;
                }
                if (ackPkt->identifier != PROXIMITY_PACKET_IDENTIFIER) {
                    char hexId[8];
                    snprintf(hexId, sizeof(hexId), "0x%02X", ackPkt->identifier);
                    apx_log_warning(std::string("ACK: invalid identifier ") + hexId +
                                    " (expected 0xA5) — dropped");
                } else if (!cmdPacketValidateCRC(ackPkt)) {
                    apx_log_warning("ACK: CRC-32 mismatch (seq=" +
                                    std::to_string(ackPkt->seq) + ") — dropped");
                } else if (!isValidPlcInboundCommand(ackPkt->command)) {
                    apx_log_warning("SDM: dropping PLC packet with unsupported command");
                } else if (ackPkt->command == CMD_SAFE_RELEASE_REQUEST) {
                    handleSafeReleaseRequest();
                } else {
                    uint16_t seqNo = ackPkt->seq;
                    bool dropAck = false;
                    bool matchedAck = false;
                    unsigned char acknowledgedCommand = 0U;

                    {
                        std::lock_guard<std::mutex> lock(commandStatusMtx);
                        auto it = pendingCommands.find(seqNo);
                        if (it != pendingCommands.end()) {
                            if (ackPkt->command != it->second.command)
                            {
                                dropAck = true;
                            }
                            else
                            {
                                it->second.acknowledged = true;
                                matchedAck = true;
                                acknowledgedCommand = it->second.command;
                            }
                        }
                    }

                    std::string ackLog;
                    if (dropAck)
                    {
                        ackLog = "SDM: dropping ACK with command/sequence mismatch";
                        apx_log_warning(ackLog);
                    }
                    else if (matchedAck)
                    {
                        std::ostringstream ackMsg;
                        ackMsg << "Received acknowledgment for command: "
                               << commandName(acknowledgedCommand)
                               << " (SeqNo: " << seqNo << ")"
                               << ", ACK UTC epoch: " << ackPkt->ts_seconds
                               << "." << std::setfill('0') << std::setw(6)
                               << ackPkt->ts_microseconds;
                        apx_log_info(ackMsg.str());
                    }
                    else
                    {
                        ackLog = "SDM: received valid ACK with no pending command (SeqNo: " +
                                 std::to_string(seqNo) + ", command: " +
                                 std::string(commandName(ackPkt->command)) + ")";
                        apx_log_warning(ackLog);
                    }
                    if (dropAck)
                        continue;
                }
            }
        }

        /* Periodic timeout + cleanup scan */
        auto now_steady = std::chrono::steady_clock::now();
        if (now_steady - lastTimeoutScan >=
            std::chrono::seconds(ACK_MONITOR_INTERVAL_S))
        {
            lastTimeoutScan = now_steady;

            std::lock_guard<std::mutex> lock(commandStatusMtx);
            auto now = std::chrono::system_clock::now();

            for (auto it = pendingCommands.begin();
                 it != pendingCommands.end(); )
            {
                if (it->second.acknowledged)
                {
                    if (now - it->second.sentTime >
                        std::chrono::seconds(ACK_CLEANUP_SECONDS))
                        it = pendingCommands.erase(it);
                    else
                        ++it;
                }
                else
                {
                    if (now - it->second.sentTime >
                        std::chrono::seconds(ACK_TIMEOUT_SECONDS))
                    {
                        std::ostringstream timeoutMsg;
                        timeoutMsg << "WARNING: No acknowledgment received "
                                      "for command: "
                                   << commandName(it->second.command)
                                   << " (SeqNo: " << it->first
                                   << ") after "
                                   << ACK_TIMEOUT_SECONDS << " seconds";
                        apx_log_warning(timeoutMsg.str());
                        it = pendingCommands.erase(it);
                    }
                    else
                        ++it;
                }
            }
        }
    }
}

/* ---- heartbeat to PLC ---- */
static void heartbeatTransmitter()
{
    while (!stopSDMThreads.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_PERIOD_SEC));
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            sendDecisionCommand(CMD_HEARTBEAT, true, nullptr);
        }
    }
}

/*
 * Gateway-heartbeat watchdog (3-tier fail-safe).
 * max_hb_failures N => tier-3 latch on m >= N; tier 2 is warnW < m < N.
 * Tier-2/Tier-3: CMD_STOP+SW_ERROR override; tier-3 remains latched until
 * gateway liveness recovers and PLC sends CMD_SAFE_RELEASE_REQUEST.
 */
static void gatewayHeartbeatWatchdog()
{
    uint32_t prevMiss = 0U;
    while (!stopSDMThreads.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        const uint32_t maxF = g_maxHbFailuresCfg.load();
        const uint32_t warnW = g_warnThresholdCfg.load();

        int64_t elapsed = 0;
        {
            std::lock_guard<std::mutex> lk(hbMtx);
            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - hbLastRecvTime).count();
        }

        const uint32_t m = gatewayMissFromElapsedMs(elapsed);
        if (m == 0U)
        {
            if (clearGatewayHbFaultActive())
                apx_log_info("HB-PSD: gateway HB recovered; safe-state latch awaits PLC release");
        }

        if (m <= warnW)
        {
            const bool hadTier2 = hbTier2SafeHoldActive.exchange(false, std::memory_order_acq_rel);
            if (hadTier2)
            {
                apx_log_info("HB-PSD: tier-2 safe-hold cleared");
                if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                    sendAtlProximitySafeHold();
                }
            }
        }

        if (m > prevMiss && m >= 1U && m <= warnW)
        {
            apx_log_warning(
                "HB-PSD: gateway HB warn tier miss_count=" + std::to_string(m) +
                "/" + std::to_string(maxF) + " elapsed_ms=" + std::to_string(elapsed));
        }
        if (m > warnW && m < maxF && prevMiss <= warnW)
        {
            regrTier2AttemptsRemaining.store(
                (maxF > warnW + 1U) ? static_cast<int>(maxF - warnW - 1U) : 0);
            apx_log_err(
                "HB-PSD: active fault (tier 2) — safe hold + bounded REGR; miss=" + std::to_string(m));
            {
                std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                hbTier2SafeHoldActive.store(true, std::memory_order_release);
                sendAtlProximitySafeHold();
            }
        }

        prevMiss = m;
        g_lastGatewayHbMissCount.store(m);

        if (m >= maxF)
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (markGatewayHbFaultLatched())
            {
                apx_log_err(
                    "HB-PSD: gateway HB fault latched (tier 3) — local fail-safe, no PSS connection");
                sendAtlProximitySafeHold();
            }
        }
    }
}

static unsigned char atlProximityCommandFromTier(AtlPxcSeverityTier tier)
{
    switch (tier)
    {
        case ATLPXC_TIER_CRITICAL: return CMD_STOP;
        case ATLPXC_TIER_WARNING:  return CMD_REDUCE;
        case ATLPXC_TIER_NORMAL:   return CMD_NORMAL;
        default:                   return CMD_NORMAL;
    }
}

static const char* atlProximityTierName(AtlPxcSeverityTier tier)
{
    switch (tier)
    {
        case ATLPXC_TIER_CRITICAL: return "CRITICAL";
        case ATLPXC_TIER_WARNING:  return "WARNING";
        case ATLPXC_TIER_NORMAL:   return "NORMAL";
        default:                   return "NONE";
    }
}

/* Derive the command from retained pair state. Caller must hold
 * atlProximityStateMtx.
 *
 * Every pair still inside the TTL window contributes, and the most severe one
 * wins. When no pair is live the result is CMD_NORMAL: no forklift-to-person
 * hazard is currently observed. That is the correct reading only because loss
 * of gateway traffic is covered separately by the decision-freshness watchdog
 * and the gateway-heartbeat watchdog, both of which latch safe hold; pair
 * expiry with a healthy gateway genuinely means the objects have separated. */
static unsigned char atlProximityDeriveCommandLocked(std::uint64_t nowMs,
                                                     AtlPxcAggregate* outAgg)
{
    AtlPxcAggregate agg;
    AtlPxcPairFusionAggregate(&atlProximityPairTable, nowMs, &agg);

    const unsigned char cmd = atlProximityCommandFromTier(agg.tier);
    atlProximityActiveCommand = cmd;

    if (outAgg != nullptr)
        *outAgg = agg;
    return cmd;
}

/* Fold one DecisionRequest's proximity events into retained per-pair state and
 * derive the resulting command. Caller must hold atlProximityStateMtx.
 *
 * The batch is a partial view: a pair missing from it has not become safe, it
 * simply was not reported this cycle. So events are accumulated into the pair
 * table and the command is then derived from the whole table, rather than the
 * batch being collapsed on its own.
 *
 * Returns false when the batch carried no usable proximity observation, in
 * which case there is nothing new to tell the PLC and the caller sends nothing.
 * Re-asserting the unchanged command is the periodic thread's job. */
static bool foldAtlProximityBatchLocked(const DecisionRequest* request,
                                        std::uint64_t nowMs,
                                        unsigned char* outCmd,
                                        AtlPxcAggregate* outAgg)
{
    if (!request || !outCmd)
        return false;

    bool observedAny = false;

    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    for (uint8_t i = 0; i < maxSrc; ++i)
    {
        const SensorData& sd = request->sensorDataSummary[i];

        if (sd.event.status == STALE)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "Dropping STALE proximity event id=%u type=%d",
                     (unsigned)sd.event.id, (int)sd.event.type);
            apx_log_info(buf);
            continue;
        }

        const EventType et = static_cast<EventType>(sd.event.type);
        if (et == SENSOR_INVALID || et == SENSOR_VALID)
        {
            apx_log_info("APX: Sensor health event consumed by health monitor only");
            continue;
        }

        if (!sd.isHealthy)
        {
            apx_log_info("Sensor unhealthy -- logging only, not processing for decision");
            continue;
        }

        if (!sd.isTrustedSource)
        {
            apx_log_info("AI pipeline untrusted -- logging only, not processing for decision");
            continue;
        }

        const AtlPxcSeverityTier tier = AtlPxcTierFromEventType(et);
        if (tier == ATLPXC_TIER_NONE)
            continue;   /* not a proximity event type this SDM arbitrates */

        AtlPxcPairObjects pair;
        if (!AtlPxcResolvePairRoles(sd.event.fusionMetadata, &pair))
        {
            /* Not a forklift-to-person pair as far as objectType can tell. The
             * observation is still tracked so its severity counts -- see
             * atl_proximity_pair_fusion.h -- but it is worth a warning because
             * the event mapping is supposed to have scoped these rules to
             * forklift-to-person pairs before they reach this SDM. */
            apx_log_warning(
                "APX: proximity event with unresolved forklift/person roles; tracking by object ID"
                " objIDs=" + std::to_string(pair.forkliftObjectID) +
                "/" + std::to_string(pair.personObjectID) +
                " types=" + std::to_string(static_cast<int>(pair.forkliftObjectType)) +
                "/" + std::to_string(static_cast<int>(pair.personObjectType)) +
                " event=" + std::to_string(static_cast<int>(et)));
        }

        if (AtlPxcPairFusionObserve(&atlProximityPairTable, pair, tier, nowMs))
        {
            observedAny = true;
        }
        else
        {
            /* Only reachable when the table is full of pairs at least as severe
             * as this one, so the emitted command is unaffected. */
            apx_log_warning(
                "APX: pair table at capacity; dropped less-severe observation tier=" +
                std::string(atlProximityTierName(tier)) +
                " rejected_total=" +
                std::to_string(atlProximityPairTable.capacityRejectedObservations));
        }
    }

    if (!observedAny)
        return false;

    AtlPxcAggregate agg;
    const unsigned char cmd = atlProximityDeriveCommandLocked(nowMs, &agg);

    apx_log_info(
        "APX: fused proximity worst case tier=" +
        std::string(atlProximityTierName(agg.tier)) +
        " cmd=" + std::string(commandName(cmd)) +
        " live_pairs=" + std::to_string(agg.livePairs) +
        " forklift=" + std::to_string(agg.objects.forkliftObjectID) +
        " person=" + std::to_string(agg.objects.personObjectID));

    *outCmd = cmd;
    if (outAgg != nullptr)
        *outAgg = agg;
    return true;
}

/* Periodic re-assert of the most recent decision, so a single lost UDP
 * datagram does not leave the PLC holding a stale command. Shutdown-aware
 * sleep via atlProximityPeriodicDecisionCv; observes the same safe-hold /
 * ERROR-latch signals as the event path so the periodic emission always
 * reflects the current worst-case fusion view, not a stale "desired". */
static void atlProximityPeriodicDecisionLoop()
{
    while (!stopSDMThreads.load(std::memory_order_relaxed))
    {
        const std::uint32_t intervalMs =
            g_decisionRepeatIntervalMs.load(std::memory_order_relaxed);
        /* Defensive: the start path only launches this thread when the
         * interval is non-zero, but a future runtime-reconfig path could
         * set it to 0 — sleep briefly rather than spinning. */
        const auto sleepMs = (intervalMs == 0U)
            ? std::chrono::milliseconds(100)
            : std::chrono::milliseconds(intervalMs);

        {
            std::unique_lock<std::mutex> lock(atlProximityPeriodicDecisionWaitMtx);
            atlProximityPeriodicDecisionCv.wait_for(lock, sleepMs, [] {
                return stopSDMThreads.load(std::memory_order_relaxed);
            });
        }
        if (stopSDMThreads.load(std::memory_order_relaxed))
            break;

        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (safeStateLatched())
            {
                sendAtlProximitySafeHold();
            }
            else if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
            {
                sendAtlProximitySafeHold();
            }
            else if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
            {
                sendAtlProximitySafeHold();
            }
            else
            {
                /* Re-derive rather than replay the cached command: pairs age
                 * out on a timer, so between event batches the worst case can
                 * legitimately drop (last dangerous pair expired) without any
                 * new event arriving to tell us. Replaying the cache would hold
                 * the PLC in a command the retained state no longer supports. */
                unsigned char cmd = CMD_NORMAL;
                AtlPxcAggregate agg;
                {
                    std::lock_guard<std::mutex> stateLock(atlProximityStateMtx);
                    cmd = atlProximityDeriveCommandLocked(atlProximityMonotonicMs(), &agg);
                }
                sendDecisionCommand(cmd, true, nullptr, -1,
                                    agg.haveObjects ? &agg.objects : nullptr);
            }
        }
    }
}

/* ====================== DECISION LOGIC ========================== */
void onAtlProximityEventNotificationReceive(const DecisionRequest* request)
{
    if (!request)
    {
        apx_log_err("SDM: onAtlProximityEventNotificationReceive called with null DecisionRequest");
        return;
    }

    apx_log_info(
        "SDM: processing DecisionRequest id=" +
        std::to_string(request->requestId));

    /* --- PSS ERROR mode ------------------------------------------------ */
    if (request->pssStatus.mode == ERROR)
    {
        apx_log_warning(
            "PSS is in error mode, sending stop and software error commands");
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            markPssFaultActive();
            {
                /* Publish the safe-state command so the periodic re-assert
                 * thread continues to hold the PLC in STOP after the ERROR
                 * latch is cleared until the next healthy event arrives. */
                std::lock_guard<std::mutex> stateLock(atlProximityStateMtx);
                atlProximityActiveCommand = CMD_STOP;
            }
            sendAtlProximitySafeHold(request);
        }
        return;
    }

    /* --- Normal / Degraded mode ---------------------------------------- */
    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    if (maxSrc == 1U &&
        static_cast<EventType>(request->sensorDataSummary[0].event.type) == PSS_STATUS_NOOP)
    {
        apx_log_info("APX: PSS_STATUS_NOOP received; freshness refreshed without proximity-state update");
        return;
    }

    if (SdmDecisionRequestClearsPssFaultActive(request))
    {
        clearPssFaultActive();
    }

    for (uint8_t i = 0; i < maxSrc; ++i)
    {
        if (static_cast<EventType>(request->sensorDataSummary[i].event.type) == SW_FAIL)
        {
            apx_log_err("Proximity: SW_FAIL received — triggering safe hold");
            {
                std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                markPssFaultActive();
                {
                    std::lock_guard<std::mutex> stateLock(atlProximityStateMtx);
                    atlProximityActiveCommand = CMD_STOP;
                }
                sendAtlProximitySafeHold(request, static_cast<int>(i));
            }
            return;
        }
    }

    if (updateSensorHealthFromDecisionRequest(request))
    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            std::lock_guard<std::mutex> stateLock(atlProximityStateMtx);
            atlProximityActiveCommand = CMD_STOP;
        }
        sendAtlProximitySafeHold(request);
        return;
    }

    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);

        unsigned char desired = CMD_NORMAL;
        AtlPxcAggregate agg;
        bool haveDecision = false;
        {
            /* Fold under the state lock so the batch is absorbed and the
             * worst case is read from one consistent snapshot. This also
             * records the observation even while a tier-2 safe hold overrides
             * what we emit, so the periodic loop derives the right command
             * once that hold clears. */
            std::lock_guard<std::mutex> stateLock(atlProximityStateMtx);
            haveDecision = foldAtlProximityBatchLocked(
                request, atlProximityMonotonicMs(), &desired, &agg);
        }

        if (!haveDecision)
            return;

        const AtlPxcPairObjects* pairObjects =
            agg.haveObjects ? &agg.objects : nullptr;

        if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
        {
            sendAtlProximitySafeHold(request, -1, pairObjects);
        }
        else if (safeStateLatched())
        {
            sendAtlProximitySafeHold(request, -1, pairObjects);
        }
        else
        {
            sendDecisionCommand(desired, true, request, -1, pairObjects);
        }
    }
}

/* Send REGR registration to gateway so it forwards Proximity-subscribed event types. */
static bool sendGatewayRegistration()
{
    constexpr size_t bufSize = 4 + 1 + ATL_PROXIMITY_SUBSCRIBED_COUNT * sizeof(uint32_t);
    char buf[bufSize];
    std::memcpy(buf, NVPSD_GATEWAY_REG_MAGIC, 4);
    buf[4] = static_cast<char>(ATL_PROXIMITY_SUBSCRIBED_COUNT);
    for (uint8_t i = 0; i < ATL_PROXIMITY_SUBSCRIBED_COUNT; ++i)
    {
        uint32_t val = htonl(static_cast<uint32_t>(ATL_PROXIMITY_SUBSCRIBED_EVENTS[i]));
        std::memcpy(buf + 5 + i * sizeof(uint32_t), &val, sizeof(uint32_t));
    }
    std::lock_guard<std::mutex> lk(gwSockMtx);
    if (psdGatewayListenSock < 0)
        return false;
    ssize_t sent = sendto(psdGatewayListenSock, buf, bufSize, 0,
                          reinterpret_cast<struct sockaddr*>(&gatewayAddr),
                          sizeof(gatewayAddr));
    if (sent != static_cast<ssize_t>(bufSize))
    {
        apx_log_err("SDM: failed to send REGR to gateway");
        return false;
    }
    apx_log_info(
        "SDM: sent registration (" + std::to_string(ATL_PROXIMITY_SUBSCRIBED_COUNT) + " event types) to gateway");
    return true;
}

/* EVENT loop: receive DecisionRequest from PSD Gateway */
static void psdGatewayEventListener()
{
    char               rawBuf[sizeof(NvPSDGatewayDecisionRequestPacket)];
    struct sockaddr_in sender;
    socklen_t          slen = sizeof(sender);
    struct pollfd      pfd;

    while (!stopSDMThreads.load() && !signalShutdownRequested.load())
    {
        if (g_signal_received)
            signalShutdownRequested.store(true);
        {
            /* Periodic REGR: keep trying while latched so a restarted gateway can rediscover this SDM. */
            auto now = std::chrono::steady_clock::now();
            const uint32_t m = g_lastGatewayHbMissCount.load();
            const uint32_t maxF = g_maxHbFailuresCfg.load();
            const uint32_t warnW = g_warnThresholdCfg.load();
            const bool due = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRegistrationTime).count() >=
                REG_RETRY_INTERVAL_MS;
            if (due)
            {
                bool allow = false;
                bool tier2Attempt = false;
                if (hbFaultLatched.load(std::memory_order_relaxed) || safeStateLatched())
                {
                    allow = true;
                }
                else if (m <= warnW)
                {
                    allow = true;
                }
                else if (m > warnW && m < maxF && regrTier2AttemptsRemaining.load() > 0)
                {
                    allow = true;
                    tier2Attempt = true;
                }
                if (allow)
                {
                    /* Tier 2: consume one attempt per REGR try (bounded); do not tie budget to send success. */
                    if (tier2Attempt)
                    {
                        const int cur = regrTier2AttemptsRemaining.load(std::memory_order_relaxed);
                        if (cur > 0)
                            regrTier2AttemptsRemaining.fetch_sub(1, std::memory_order_relaxed);
                    }
                    if (sendGatewayRegistration())
                        lastRegistrationTime = now;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(gwSockMtx);
            if (psdGatewayListenSock < 0)
                break;
            pfd.fd = psdGatewayListenSock;
        }
        pfd.events  = POLLIN;
        pfd.revents = 0;
        int ready = poll(&pfd, 1, MAIN_LOOP_POLL_TIMEOUT_MS);

        if (ready <= 0)
            continue;   /* timeout or error -- re-check stop flags */

        /* Lock briefly for recvfrom */
        ssize_t n = -1;
        {
            std::lock_guard<std::mutex> lk(gwSockMtx);
            if (psdGatewayListenSock < 0)
                break;
            slen = sizeof(sender);
            n = recvfrom(psdGatewayListenSock, rawBuf, sizeof(rawBuf),
                         MSG_DONTWAIT,
                         reinterpret_cast<struct sockaddr*>(&sender), &slen);
        }

        if (n > 0 && !isConfiguredGatewayEndpoint(sender))
        {
            apx_log_warning("SDM: dropping PSD Gateway packet from unexpected endpoint");
            continue;
        }


        /* Check if this is a heartbeat from PSDGateway */
        if (n == NVPSD_GATEWAY_HB_MSG_SIZE &&
            std::memcmp(rawBuf, NVPSD_GATEWAY_HB_MAGIC_GATEWAY, 4) == 0)
        {
#ifdef NVPSF_DBG
            /* Extract sequence number */
            uint32_t netSeq;
            std::memcpy(&netSeq, rawBuf + 4, 4);
            uint32_t seq = ntohl(netSeq);

            apx_log_info(
                "HB-PSD: received heartbeat seq=" +
                std::to_string(seq) + " from Gateway");
#endif

            /* Update watchdog timestamp */
            {
                std::lock_guard<std::mutex> lk(hbMtx);
                hbLastRecvTime = std::chrono::steady_clock::now();
                hbGatewayAlive.store(true);
            }

            /* Send ACK: [HBPC][seqNo] */
            char ack[NVPSD_GATEWAY_HB_MSG_SIZE];
            std::memcpy(ack, NVPSD_GATEWAY_HB_MAGIC_CLIENT, 4);
            std::memcpy(ack + 4, rawBuf + 4, 4);  // echo seq in network order
            {
                std::lock_guard<std::mutex> lk(gwSockMtx);
                if (psdGatewayListenSock >= 0)
                {
                    ssize_t sent = sendto(psdGatewayListenSock, ack, NVPSD_GATEWAY_HB_MSG_SIZE, 0,
                                         reinterpret_cast<struct sockaddr*>(&sender), slen);
                    if (sent != static_cast<ssize_t>(NVPSD_GATEWAY_HB_MSG_SIZE))
                        apx_log_err("SDM: heartbeat ACK send failed");
                }
            }
            continue;
        }

        /* --- Gateway-wrapped DecisionRequest --- */
        if (n == static_cast<ssize_t>(sizeof(NvPSDGatewayDecisionRequestPacket)))
        {
            NvPSDGatewayDecisionRequestPacket packet;
            std::memcpy(&packet, rawBuf, sizeof(packet));
            if (!NvPSDGatewayDecisionPacketHeaderIsValid(&packet))
            {
                apx_log_err("SDM: Gateway DecisionRequest packet header validation failed — dropping");
                continue;
            }

            DecisionRequest request;
            std::memcpy(&request, &packet.request, sizeof(DecisionRequest));

            uint32_t vErr = validateDecisionRequest(&request);
            if (vErr != PSS_VALID)
            {
                char vErrHex[12];
                snprintf(vErrHex, sizeof(vErrHex), "%08X", vErr);
                apx_log_err(
                    std::string("SDM: DecisionRequest validation failed (flags=0x") +
                    vErrHex + ") — dropping");
                continue;
            }

            apx_log_info(
                "SDM: received DecisionRequest from psdGateway, reqId=" +
                std::to_string(request.requestId) +
                " gatewayTxSeq=" +
                std::to_string(packet.gatewayTxSeq) +
                " events=" +
                std::to_string(request.sensorDataSummarySize));

            const SdmGatewayDecisionSequenceResult seqResult =
                sdmGatewayDecisionSequenceObserve(&gatewayDecisionSequenceState,
                                                  packet.gatewayEpoch,
                                                  packet.gatewayTxSeq);
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_DUPLICATE_OR_STALE)
            {
                apx_log_warning("APX: dropping duplicate/stale Gateway DecisionRequest packet; epoch=" +
                    std::to_string(packet.gatewayEpoch) + " txSeq=" +
                    std::to_string(packet.gatewayTxSeq));
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_INVALID)
            {
                apx_log_err("APX: invalid Gateway DecisionRequest packet sequence — dropping");
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_GAP)
            {
                if (markGatewayDecisionSequenceFaultLatched(packet.gatewayEpoch, packet.gatewayTxSeq))
                {
                    apx_log_err("APX: Gateway DecisionRequest sequence gap; entering safe hold, epoch=" +
                        std::to_string(packet.gatewayEpoch) + " txSeq=" +
                        std::to_string(packet.gatewayTxSeq));
                    sendLatchedAtlProximitySafeHold();
                }
            }
            else
            {
                markGatewayDecisionSequenceRecovered(packet.gatewayEpoch, packet.gatewayTxSeq);
            }

            markDecisionRequestFresh(request.requestId);
            onAtlProximityEventNotificationReceive(&request);
        }
        else if (n > 0)
        {
            apx_log_warning(
                "SDM: received partial packet (" + std::to_string(n) +
                " bytes, expected " +
                std::to_string(sizeof(NvPSDGatewayDecisionRequestPacket)) + ")");
        }
        /* n < 0 after poll said POLLIN: spurious -- just loop back */
    }
}

/* ====================== LAUNCH / SHUTDOWN ====================== */

/* Async-signal-safe: only set volatile sig_atomic_t. */
static void signalHandler(int /*sig*/)
{
    g_signal_received = 1;
}

int launchAtlProximityControlAlgo(const std::string& gatewayIP,
                               unsigned int gatewayPort,
                               const std::string& plcIP,
                               unsigned int plcPort,
                               std::uint8_t maxHbFailures,
                               std::uint32_t decisionRepeatIntervalMs,
                               std::uint32_t hbStaleMs,
                               std::uint32_t hbPeriodMs,
                               std::uint32_t decisionFreshnessTimeoutMs,
                               std::uint32_t pairTtlMs)
{
    apx_log_open();

    if (gatewayPort < 1U || gatewayPort > 65535U
        || plcPort < 1U || plcPort > 65535U)
    {
        apx_log_err("gateway and PLC ports must be in range 1..65535");
        apx_log_close();
        return -1;
    }

    const uint16_t gatewayPortU16 = static_cast<uint16_t>(gatewayPort);
    const uint16_t plcPortU16     = static_cast<uint16_t>(plcPort);

    if (maxHbFailures == 0)
    {
        apx_log_err("maxHbFailures must be in 1..255");
        apx_log_close();
        return -1;
    }

    /* Validate the same way as the CLI parser so a direct library caller
     * cannot silently install an out-of-range period. 0 = disabled;
     * otherwise must land inside [min, max]. */
    if (decisionRepeatIntervalMs > kDecisionRepeatIntervalMsMax
        || (decisionRepeatIntervalMs != 0U
            && decisionRepeatIntervalMs < kDecisionRepeatIntervalMsMinNonZero))
    {
        apx_log_err("decisionRepeatIntervalMs must be 0 or in " +
                    std::to_string(kDecisionRepeatIntervalMsMinNonZero) + ".." +
                    std::to_string(kDecisionRepeatIntervalMsMax) +
                    " (0 = periodic repeat off)");
        apx_log_close();
        return -1;
    }

    if (hbStaleMs < kHbTimingMsMin || hbStaleMs > kHbTimingMsMax ||
        hbPeriodMs < kHbTimingMsMin || hbPeriodMs > kHbTimingMsMax)
    {
        apx_log_err("hbStaleMs and hbPeriodMs must be in " +
                    std::to_string(kHbTimingMsMin) + ".." +
                    std::to_string(kHbTimingMsMax));
        apx_log_close();
        return -1;
    }

    if (!sdmDecisionFreshnessTimeoutMsIsValid(decisionFreshnessTimeoutMs))
    {
        apx_log_err("decisionFreshnessTimeoutMs must be in " +
                    std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN) + ".." +
                    std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX));
        apx_log_close();
        return -1;
    }

    /* Reject rather than clamp, so a caller asking for an out-of-range pair
     * lifetime is told instead of silently getting a different safety-relevant
     * ageing window. AtlPxcPairFusionInit() clamps as a second line of defence
     * for direct library users. */
    if (pairTtlMs < ATLPXC_PAIR_TTL_MS_MIN || pairTtlMs > ATLPXC_PAIR_TTL_MS_MAX)
    {
        apx_log_err("pairTtlMs must be in " +
                    std::to_string(ATLPXC_PAIR_TTL_MS_MIN) + ".." +
                    std::to_string(ATLPXC_PAIR_TTL_MS_MAX));
        apx_log_close();
        return -1;
    }

    /* Register signal handlers */
    if (std::signal(SIGINT, signalHandler) == SIG_ERR)
        apx_log_err("Failed to register SIGINT handler");
    if (std::signal(SIGTERM, signalHandler) == SIG_ERR)
        apx_log_err("Failed to register SIGTERM handler");

    /* --- PLC command socket (send to PLC, receive ACKs) --- */
    plcSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (plcSock < 0)
    {
        apx_log_err("Failed to create UDP socket");
        apx_log_close();
        return -1;
    }

    int flags = fcntl(plcSock, F_GETFL, 0);
    if (flags == -1 || fcntl(plcSock, F_SETFL, flags | O_NONBLOCK) == -1)
        apx_log_warning("Failed to set plcSock non-blocking");

    struct sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port        = htons(0);
    if (bind(plcSock, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0)
    {
        apx_log_err("Failed to bind UDP socket");
        close(plcSock); plcSock = -1;
        apx_log_close();
        return -1;
    }

    socklen_t addrLen = sizeof(localAddr);
    if (getsockname(plcSock, (struct sockaddr*)&localAddr, &addrLen) < 0)
    {
        apx_log_err("Failed to get socket name");
        close(plcSock); plcSock = -1;
        apx_log_close();
        return -1;
    }

    plcAddr.sin_family = AF_INET;
    plcAddr.sin_port   = htons(plcPortU16);
    if (inet_pton(AF_INET, plcIP.c_str(), &plcAddr.sin_addr) <= 0)
    {
        apx_log_err("Invalid IP address");
        close(plcSock); plcSock = -1;
        apx_log_close();
        return -1;
    }

    /* --- PSD Gateway socket: bind ephemeral, send REGR to gateway, recv DecisionRequests + HB --- */
    psdGatewayListenSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (psdGatewayListenSock < 0)
    {
        apx_log_err("Failed to create psdGateway socket");
        close(plcSock); plcSock = -1;
        apx_log_close();
        return -1;
    }

    int gwFlags = fcntl(psdGatewayListenSock, F_GETFL, 0);
    if (gwFlags == -1 || fcntl(psdGatewayListenSock, F_SETFL, gwFlags | O_NONBLOCK) == -1)
        apx_log_warning("Failed to set psdGatewayListenSock non-blocking");

    struct sockaddr_in bindAddr = {};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port        = htons(0);
    if (bind(psdGatewayListenSock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0)
    {
        apx_log_err("Failed to bind psdGateway socket");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        apx_log_close();
        return -1;
    }

    gatewayAddr.sin_family = AF_INET;
    gatewayAddr.sin_port  = htons(gatewayPortU16);
    if (inet_pton(AF_INET, gatewayIP.c_str(), &gatewayAddr.sin_addr) <= 0)
    {
        apx_log_err("Invalid gateway IP address");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        apx_log_close();
        return -1;
    }
    if (!sendGatewayRegistration())
    {
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        apx_log_close();
        return -1;
    }
    lastRegistrationTime = std::chrono::steady_clock::now();

    /* Reset state */
    stopSDMThreads.store(false);
    signalShutdownRequested.store(false);
    {
        std::lock_guard<std::mutex> lk(hbMtx);
        /* Arm from launch time so a gateway that never starts still trips the cold-start watchdog. */
        hbLastRecvTime = std::chrono::steady_clock::now();
        hbGatewayAlive.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(decisionFreshnessMtx);
        lastValidDecisionRequestTime = std::chrono::steady_clock::now();
    }
    resetSafeStateFaultsForLaunch();
    g_lastGatewayHbMissCount.store(0U);
    {
        std::lock_guard<std::mutex> lock(safeReleaseRateMtx);
        lastSafeReleaseRequestTime = std::chrono::steady_clock::time_point{};
    }
    regrTier2AttemptsRemaining.store(0);
    g_maxHbFailuresCfg.store(static_cast<uint32_t>(maxHbFailures));
    g_warnThresholdCfg.store(g_maxHbFailuresCfg.load() / 2U);
    g_hbStaleMs.store(hbStaleMs, std::memory_order_relaxed);
    g_hbPeriodMs.store(hbPeriodMs, std::memory_order_relaxed);
    g_decisionFreshnessTimeoutMs.store(decisionFreshnessTimeoutMs,
                                       std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(atlProximityStateMtx);
        atlProximityActiveCommand = CMD_NORMAL;
        AtlPxcPairFusionInit(&atlProximityPairTable, pairTtlMs);
    }
    g_decisionRepeatIntervalMs.store(decisionRepeatIntervalMs,
                                     std::memory_order_relaxed);

    /* --- Start threads --- */
    ackHandlerThread = std::thread(ackHandlerLoop);
    heartbeatThread  = std::thread(heartbeatTransmitter);
    hbWatchdogThread = std::thread(gatewayHeartbeatWatchdog);
    decisionFreshnessWatchdogThread = std::thread(decisionFreshnessWatchdog);
    if (decisionRepeatIntervalMs > 0U)
    {
        periodicDecisionThread = std::thread(atlProximityPeriodicDecisionLoop);
        apx_log_info(
            "ATL-Proximity forklift/person SDM initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat every " +
            std::to_string(decisionRepeatIntervalMs) + " ms; gateway HB stale_ms=" +
            std::to_string(hbStaleMs) + " period_ms=" + std::to_string(hbPeriodMs) +
            "; decision_freshness_timeout_ms=" +
            std::to_string(decisionFreshnessTimeoutMs) +
            "; pair_ttl_ms=" + std::to_string(pairTtlMs) +
            "; max_tracked_pairs=" + std::to_string(ATLPXC_MAX_TRACKED_PAIRS) + ")");
    }
    else
    {
        apx_log_info(
            "ATL-Proximity forklift/person SDM initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat disabled; gateway HB stale_ms=" +
            std::to_string(hbStaleMs) + " period_ms=" + std::to_string(hbPeriodMs) +
            "; decision_freshness_timeout_ms=" +
            std::to_string(decisionFreshnessTimeoutMs) +
            "; pair_ttl_ms=" + std::to_string(pairTtlMs) +
            "; max_tracked_pairs=" + std::to_string(ATLPXC_MAX_TRACKED_PAIRS) + ")");
    }

    /* --- Main thread runs the PSD Gateway event loop --- */
    psdGatewayEventListener();

    if (g_signal_received)
        signalShutdownRequested.store(true);
    if (signalShutdownRequested.load())
    {
        apx_log_info("Signal received - initiating graceful shutdown");
        stopSDMThreads.store(true);
    }

    // Perform shutdown
    shutdownAtlProximityControlAlgo();

    return 0;
}

void shutdownAtlProximityControlAlgo()
{
    stopSDMThreads.store(true);
    /* Wake the periodic loop so it observes stopSDMThreads immediately
     * instead of sleeping out the remainder of its current interval.
     * Safe to call even when the thread was never started (interval==0). */
    atlProximityPeriodicDecisionCv.notify_all();

    auto joinThread = [](std::thread& t) {
        if (t.joinable())
            t.join();
    };

    joinThread(heartbeatThread);
    joinThread(ackHandlerThread);
    joinThread(hbWatchdogThread);
    joinThread(decisionFreshnessWatchdogThread);
    joinThread(periodicDecisionThread);

    /* Close sockets */
    {
        std::lock_guard<std::mutex> lock(plcSocketMtx);
        if (plcSock >= 0) { close(plcSock); plcSock = -1; }
    }
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        if (psdGatewayListenSock >= 0)
        {
            close(psdGatewayListenSock);
            psdGatewayListenSock = -1;
        }
    }

    /* Clear pending commands */
    {
        std::lock_guard<std::mutex> lock(commandStatusMtx);
        pendingCommands.clear();
    }

    apx_log_close();
}
