/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "ProximityControl.h"
#include "NvPSDGatewayProtocol.h"
#include "pss_message_validate.h"

/* Simple SDM logging: console + optional log file (no NvPSB dependency) */
static std::mutex pxcLogMtx;
static FILE* pxcLogFile = nullptr;

static void pxc_log_open(void)
{
    std::lock_guard<std::mutex> lock(pxcLogMtx);
    if (!pxcLogFile) {
        pxcLogFile = fopen("pxc_sdm.log", "a");
        if (pxcLogFile)
            setvbuf(pxcLogFile, nullptr, _IOLBF, 0);
    }
}

static void pxc_log_close(void)
{
    std::lock_guard<std::mutex> lock(pxcLogMtx);
    if (pxcLogFile) {
        fclose(pxcLogFile);
        pxcLogFile = nullptr;
    }
}

/* fprintf to stdout/stderr and optional pxc_sdm.log (under pxcLogMtx). */
static void pxc_log(const char* level, const char* msg)
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

    std::lock_guard<std::mutex> lock(pxcLogMtx);
    FILE* out = (strcmp(level, "ERR") == 0 || strcmp(level, "WARNING") == 0)
                ? stderr : stdout;
    fprintf(out, "%s [PXC][%s] %s\n", prefix, level, msg);
    if (pxcLogFile)
        fprintf(pxcLogFile, "%s [PXC][%s] %s\n", prefix, level, msg);
}

static void pxc_log_info(const std::string& msg)    { pxc_log("INFO", msg.c_str()); }
static void pxc_log_warning(const std::string& msg) { pxc_log("WARNING", msg.c_str()); }
static void pxc_log_err(const std::string& msg)     { pxc_log("ERR", msg.c_str()); }

#define MAX_SEQ_NUMBER         65535
#define HEARTBEAT_PERIOD_SEC   5
#define ACK_TIMEOUT_SECONDS    3

/* Valid range for launchProximityControlAlgo(decisionRepeatIntervalMs). 0 disables
 * the periodic re-assert; non-zero values are clamped to [100 ms, 36000 ms] so we
 * neither flood the PLC nor let the last decision age past a typical ACK window. */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
#define ACK_CLEANUP_SECONDS    10
#define ACK_MONITOR_INTERVAL_S 1
#define ACK_RECEIVER_SLEEP_MS  100
#define MAIN_LOOP_POLL_TIMEOUT_MS 50

#define EVENT_0_PROXIMITY_NO_VIOLATION       0   // No violation (safe distance)
#define EVENT_1_PROXIMITY_VIOLATION_WARNING  1   // Proximity 2m > distance > 1m
#define EVENT_2_PROXIMITY_VIOLATION_CRITICAL 2   // Proximity distance < 1m

/* Heartbeat protocol (NvPSDGatewayProtocol.h) */
static constexpr int HB_WATCHDOG_TIMEOUT_MS = 8000;  // no HB for 8 s -> alarm
/* Re-register with gateway periodically so we recover after gateway restart (in-memory state lost). */
static constexpr int REG_RETRY_INTERVAL_MS = 30000;

/* Event types Proximity subscribes to (must match onEventNotificationReceive) */
static constexpr EventType PROXIMITY_SUBSCRIBED_EVENTS[] = { EVENT_0, EVENT_1, EVENT_2 };
static constexpr uint8_t  PROXIMITY_SUBSCRIBED_COUNT =
    sizeof(PROXIMITY_SUBSCRIBED_EVENTS) / sizeof(PROXIMITY_SUBSCRIBED_EVENTS[0]);

/* PLC command socket */
static int plcSock = -1;
static struct sockaddr_in plcAddr = {};
static std::atomic<uint16_t> cmdSeqNo{0};
static std::mutex plcSocketMtx;

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
static std::thread       periodicDecisionThread;
/* Shutdown-aware sleep for the periodic decision loop: waiting on this CV
 * instead of sleep_for lets shutdownProximityControlAlgo() return in O(ms)
 * rather than waiting up to one full decisionRepeatIntervalMs interval. */
static std::mutex              proximityPeriodicDecisionWaitMtx;
static std::condition_variable proximityPeriodicDecisionCv;

/* Serializes decision sends (event-driven vs HB watchdog vs periodic repeat).
 * Ordering: always lock decisionSendMtx first, then proximityStateMtx. */
static std::mutex                            decisionSendMtx;

/* Last committed decision command, published by onEventNotificationReceive()
 * and replayed by the periodic repeat thread. Guarded by proximityStateMtx so
 * the periodic loop observes a consistent value while the event path is
 * mid-update. CMD_NORMAL is the safe initial value before the first event. */
static std::mutex                            proximityStateMtx;
static unsigned char                         proximityActiveCommand = CMD_NORMAL;
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
static std::atomic<bool>                     hbFaultLatched{false};
/* Tier-2 gateway HB safe-hold: periodic/event send CMD_STOP+CMD_SW_ERROR while true; fusion keeps proximityActiveCommand. */
static std::atomic<bool>                     hbTier2SafeHoldActive{false};
/* PSS ERROR latched: repeat STOP+SW_ERROR on periodic loop until a non-ERROR request clears it. */
static std::atomic<bool>                     pssErrorFusionSuppressLatched{false};
static std::atomic<int>                      regrTier2AttemptsRemaining{0};
static std::atomic<uint32_t>                 g_lastGatewayHbMissCount{0U};

static uint32_t gatewayMissFromElapsedMs(int64_t elapsedMs)
{
    const int64_t kStaleStartMs = 5000;
    const int64_t kPeriodMs     = 5500;
    if (elapsedMs <= kStaleStartMs)
        return 0U;
    const uint64_t m = 1U + static_cast<uint64_t>((elapsedMs - kStaleStartMs) / kPeriodMs);
    const uint32_t maxF = g_maxHbFailuresCfg.load();
    if (m > static_cast<uint64_t>(maxF))
        return maxF;
    return static_cast<uint32_t>(m);
}

/* ====================== helpers ====================== */

std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket()
{
    auto now   = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    uint64_t totalSec  = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    uint64_t totalUsec = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count();
    uint64_t microPart = totalUsec - (totalSec * 1000000ULL);
    return { totalSec, microPart };
}

/* Populate ObjectRecord from the sensor slot that drove the command.
 * winningSlot >= 0 uses that index directly; winningSlot < 0 falls back to
 * the first non-STALE, healthy entry (consistent with decision-filtering). */
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
    if (slot < 0 || slot >= maxSrc) {
        slot = -1;
        for (uint8_t i = 0; i < maxSrc; ++i) {
            if (request->sensorDataSummary[i].event.status != STALE &&
                request->sensorDataSummary[i].isHealthy &&
                request->sensorDataSummary[i].isTrustedSource) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return;
    }

    const EventFusionMetadata& meta =
        request->sensorDataSummary[slot].event.fusionMetadata;

    for (int i = 0; i < COMMAND_NUM_OBJECTS; i++) {
        objects[i].object_id = meta.objectID[i];
        if (i < MAX_TRAJECTORY_COORDINATES) {
            objects[i].x = meta.coordinates[i].x;
            objects[i].y = meta.coordinates[i].y;
        }
        objects[i].z = 0.0f;
        objects[i].metadata = static_cast<uint32_t>(meta.objectType[i]);
    }
}

void sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request,
                         int winningSlot)
{
    uint16_t seqNo = cmdSeqNo++ % MAX_SEQ_NUMBER;
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

    fillObjectRecords(pkt.objects, request, winningSlot);

    pkt.crc32 = cmdPacketCRC32(&pkt);

    bool shouldPrintAndTrack = false;
    const char* cmdType = commandName(command);
    if (command == CMD_STOP || command == CMD_REDUCE || command == CMD_NORMAL ||
        command == CMD_SW_ERROR)
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
        pxc_log_info(logMsg.str());
    }

    if (trackAck && shouldPrintAndTrack) {
        std::lock_guard<std::mutex> cmdLock(commandStatusMtx);
        pendingCommands[seqNo] = CommandStatus(command, tsSec, tsMicro);
    }

    std::lock_guard<std::mutex> sockLock(plcSocketMtx);
    if (plcSock >= 0) {
        if (sendto(plcSock, &pkt, sizeof(pkt), 0,
                   (struct sockaddr*)&plcAddr, sizeof(plcAddr)) < 0)
            pxc_log_err("Failed to send decision command");
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

                if (ackPkt->identifier != PROXIMITY_PACKET_IDENTIFIER) {
                    char hexId[8];
                    snprintf(hexId, sizeof(hexId), "0x%02X", ackPkt->identifier);
                    pxc_log_warning(std::string("ACK: invalid identifier ") + hexId +
                                    " (expected 0xA5) — dropped");
                } else if (!cmdPacketValidateCRC(ackPkt)) {
                    pxc_log_warning("ACK: CRC-32 mismatch (seq=" +
                                    std::to_string(ackPkt->seq) + ") — dropped");
                } else {
                    uint16_t seqNo = ackPkt->seq;

                    std::lock_guard<std::mutex> lock(commandStatusMtx);
                    auto it = pendingCommands.find(seqNo);
                    if (it != pendingCommands.end()) {
                        it->second.acknowledged = true;
                        std::ostringstream ackMsg;
                        ackMsg << "Received acknowledgment for command: "
                               << commandName(it->second.command)
                               << " (SeqNo: " << seqNo << ")"
                               << ", ACK UTC epoch: " << ackPkt->ts_seconds
                               << "." << std::setfill('0') << std::setw(6)
                               << ackPkt->ts_microseconds;
                        pxc_log_info(ackMsg.str());
                    }
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
                        pxc_log_warning(timeoutMsg.str());
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
 * Tier-2: CMD_STOP+SW_ERROR override; cleared when m <= warnW (next event-driven
 * command at ~30 fps naturally restores the correct action).
 */
static void gatewayHeartbeatWatchdog()
{
    uint32_t prevMiss = 0U;
    while (!stopSDMThreads.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        if (hbFaultLatched.load())
            continue;

        const uint32_t maxF = g_maxHbFailuresCfg.load();
        const uint32_t warnW = g_warnThresholdCfg.load();

        int64_t elapsed = 0;
        {
            std::lock_guard<std::mutex> lk(hbMtx);
            if (!hbGatewayAlive.load())
                continue;

            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - hbLastRecvTime).count();
        }

        const uint32_t m = gatewayMissFromElapsedMs(elapsed);
        if (m == 0U)
            prevMiss = 0U;

        if (m <= warnW)
        {
            const bool hadTier2 = hbTier2SafeHoldActive.exchange(false, std::memory_order_acq_rel);
            if (hadTier2)
            {
                pxc_log_info("HB-PSD: tier-2 safe-hold cleared; next event will restore normal operation");
                if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                    sendDecisionCommand(CMD_STOP, true, nullptr);
                    sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
                }
            }
        }

        if (m > prevMiss && m >= 1U && m <= warnW)
        {
            pxc_log_warning(
                "HB-PSD: gateway HB warn tier miss_count=" + std::to_string(m) +
                "/" + std::to_string(maxF) + " elapsed_ms=" + std::to_string(elapsed));
        }
        if (m > warnW && m < maxF && prevMiss <= warnW)
        {
            regrTier2AttemptsRemaining.store(
                (maxF > warnW + 1U) ? static_cast<int>(maxF - warnW - 1U) : 0);
            pxc_log_err(
                "HB-PSD: active fault (tier 2) — safe hold + bounded REGR; miss=" + std::to_string(m));
            {
                std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                hbTier2SafeHoldActive.store(true, std::memory_order_release);
                sendDecisionCommand(CMD_STOP, true, nullptr);
                sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
            }
        }

        prevMiss = m;
        g_lastGatewayHbMissCount.store(m);

        if (m >= maxF)
        {
            if (!hbFaultLatched.exchange(true))
            {
                pxc_log_err(
                    "HB-PSD: gateway HB fault latched (tier 3) — local fail-safe, no PSS connection");
                {
                    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                    sendDecisionCommand(CMD_STOP, true, nullptr);
                    sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
                }
                stopSDMThreads.store(true);
                signalShutdownRequested.store(true);
            }
        }
    }
}

/* Latest command wins: last proximity-relevant event in sensorDataSummary[] order (index 0..n-1).
 * Returns false if no proximity event types in batch.
 * *outWinningSlot receives the index of the SensorData entry that last set the command. */
static bool computeProximityCommandFromBatch(const DecisionRequest* request,
                                             unsigned char* outCmd,
                                             int* outWinningSlot)
{
    if (!request || !outCmd)
        return false;

    bool            found = false;
    unsigned char   cmd   = CMD_NORMAL;
    int             slot  = -1;

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
            pxc_log_info(buf);
            continue;
        }

        if (!sd.isHealthy)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Sensor unhealthy -- logging only, not processing for decision: "
                     "eventId=%u pipelineID=%u",
                     (unsigned)sd.event.id, (unsigned)sd.event.fusionMetadata.pipelineID);
            pxc_log_info(buf);
            continue;
        }

        if (!sd.isTrustedSource)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "AI pipeline untrusted -- logging only, not processing for decision: "
                     "eventId=%u clientID=%u",
                     (unsigned)sd.event.id, (unsigned)sd.clientID);
            pxc_log_info(buf);
            continue;
        }

        if (sd.event.type == EVENT_2_PROXIMITY_VIOLATION_CRITICAL)
        {
            cmd   = CMD_STOP;
            slot  = static_cast<int>(i);
            found = true;
        }
        else if (sd.event.type == EVENT_1_PROXIMITY_VIOLATION_WARNING)
        {
            cmd   = CMD_REDUCE;
            slot  = static_cast<int>(i);
            found = true;
        }
        else if (sd.event.type == EVENT_0_PROXIMITY_NO_VIOLATION)
        {
            cmd   = CMD_NORMAL;
            slot  = static_cast<int>(i);
            found = true;
        }
    }

    if (!found)
        return false;
    *outCmd = cmd;
    if (outWinningSlot)
        *outWinningSlot = slot;
    return true;
}

/* Periodic re-assert of the most recent decision, so a single lost UDP
 * datagram does not leave the PLC holding a stale command. Shutdown-aware
 * sleep via proximityPeriodicDecisionCv; observes the same safe-hold /
 * ERROR-latch signals as the event path so the periodic emission always
 * reflects the current worst-case fusion view, not a stale "desired". */
static void proximityPeriodicDecisionLoop()
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
            std::unique_lock<std::mutex> lock(proximityPeriodicDecisionWaitMtx);
            proximityPeriodicDecisionCv.wait_for(lock, sleepMs, [] {
                return stopSDMThreads.load(std::memory_order_relaxed);
            });
        }
        if (stopSDMThreads.load(std::memory_order_relaxed))
            break;

        /* Tier-3 latched: the watchdog has already taken the fail-safe path
         * and requested shutdown; do not emit additional PLC traffic. */
        if (hbFaultLatched.load())
            continue;

        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
            {
                sendDecisionCommand(CMD_STOP, true, nullptr);
                sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
            }
            else if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
            {
                sendDecisionCommand(CMD_STOP, true, nullptr);
                sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
            }
            else
            {
                unsigned char cmd = CMD_NORMAL;
                {
                    std::lock_guard<std::mutex> stateLock(proximityStateMtx);
                    cmd = proximityActiveCommand;
                }
                sendDecisionCommand(cmd, true, nullptr);
            }
        }
    }
}

/* ====================== DECISION LOGIC ========================== */
void onEventNotificationReceive(const DecisionRequest* request)
{
    if (!request)
    {
        pxc_log_err("SDM: onEventNotificationReceive called with null DecisionRequest");
        return;
    }

    pxc_log_info(
        "SDM: processing DecisionRequest id=" +
        std::to_string(request->requestId));

    /* --- PSS ERROR mode ------------------------------------------------ */
    if (request->pssStatus.mode == ERROR)
    {
        pssErrorFusionSuppressLatched.store(true, std::memory_order_release);
        pxc_log_warning(
            "PSS is in error mode, sending stop and software error commands");
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            {
                /* Publish the safe-state command so the periodic re-assert
                 * thread continues to hold the PLC in STOP after the ERROR
                 * latch is cleared until the next healthy event arrives. */
                std::lock_guard<std::mutex> stateLock(proximityStateMtx);
                proximityActiveCommand = CMD_STOP;
            }
            sendDecisionCommand(CMD_STOP, true, request);
            sendDecisionCommand(CMD_SW_ERROR, true, request);
        }
        return;
    }
    pssErrorFusionSuppressLatched.store(false, std::memory_order_release);

    /* --- Normal / Degraded mode ---------------------------------------- */
    unsigned char desired = CMD_NORMAL;
    int winningSlot = -1;
    if (!computeProximityCommandFromBatch(request, &desired, &winningSlot))
        return;

    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            /* Always record the desired decision, even if we are currently
             * overriding it with tier-2 safe hold, so the periodic loop
             * restores the correct command once the safe hold clears. */
            std::lock_guard<std::mutex> stateLock(proximityStateMtx);
            proximityActiveCommand = desired;
        }
        if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
        {
            sendDecisionCommand(CMD_STOP, true, request, winningSlot);
            sendDecisionCommand(CMD_SW_ERROR, true, request, winningSlot);
        }
        else
            sendDecisionCommand(desired, true, request, winningSlot);
    }
}

/* Send REGR registration to gateway so it forwards Proximity-subscribed event types. */
static bool sendGatewayRegistration()
{
    constexpr size_t bufSize = 4 + 1 + PROXIMITY_SUBSCRIBED_COUNT * sizeof(uint32_t);
    char buf[bufSize];
    std::memcpy(buf, NVPSD_GATEWAY_REG_MAGIC, 4);
    buf[4] = static_cast<char>(PROXIMITY_SUBSCRIBED_COUNT);
    for (uint8_t i = 0; i < PROXIMITY_SUBSCRIBED_COUNT; ++i)
    {
        uint32_t val = htonl(static_cast<uint32_t>(PROXIMITY_SUBSCRIBED_EVENTS[i]));
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
        pxc_log_err("SDM: failed to send REGR to gateway");
        return false;
    }
    pxc_log_info(
        "SDM: sent registration (" + std::to_string(PROXIMITY_SUBSCRIBED_COUNT) + " event types) to gateway");
    return true;
}

/* EVENT loop: receive DecisionRequest from PSD Gateway */
static void psdGatewayEventListener()
{
    char               rawBuf[sizeof(DecisionRequest)];
    struct sockaddr_in sender;
    socklen_t          slen = sizeof(sender);
    struct pollfd      pfd;

    while (!stopSDMThreads.load() && !signalShutdownRequested.load())
    {
        if (g_signal_received)
            signalShutdownRequested.store(true);
        if (!hbFaultLatched.load())
        {
            /* Periodic REGR: tier 2 uses bounded attempts (regrTier2AttemptsRemaining); tier 3 latched skips. */
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
                if (m <= warnW)
                    allow = true;
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


        /* Check if this is a heartbeat from PSDGateway */
        if (n == NVPSD_GATEWAY_HB_MSG_SIZE &&
            std::memcmp(rawBuf, NVPSD_GATEWAY_HB_MAGIC_GATEWAY, 4) == 0)
        {
#ifdef NVPSF_DBG
            /* Extract sequence number */
            uint32_t netSeq;
            std::memcpy(&netSeq, rawBuf + 4, 4);
            uint32_t seq = ntohl(netSeq);

            pxc_log_info(
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
                        pxc_log_err("SDM: heartbeat ACK send failed");
                }
            }
            continue;
        }

        /* --- Full DecisionRequest --- */
        if (n == static_cast<ssize_t>(sizeof(DecisionRequest)))
        {
            DecisionRequest request;
            std::memcpy(&request, rawBuf, sizeof(DecisionRequest));

            uint32_t vErr = validateDecisionRequest(&request);
            if (vErr != PSS_VALID)
            {
                char vErrHex[12];
                snprintf(vErrHex, sizeof(vErrHex), "%08X", vErr);
                pxc_log_err(
                    std::string("SDM: DecisionRequest validation failed (flags=0x") +
                    vErrHex + ") — dropping");
                continue;
            }

            pxc_log_info(
                "SDM: received DecisionRequest from psdGateway, reqId=" +
                std::to_string(request.requestId) +
                " events=" +
                std::to_string(request.sensorDataSummarySize));

            onEventNotificationReceive(&request);
        }
        else if (n > 0)
        {
            pxc_log_warning(
                "SDM: received partial packet (" + std::to_string(n) +
                " bytes, expected " +
                std::to_string(sizeof(DecisionRequest)) + ")");
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

int launchProximityControlAlgo(const std::string& gatewayIP,
                               unsigned int gatewayPort,
                               const std::string& plcIP,
                               unsigned int plcPort,
                               std::uint8_t maxHbFailures,
                               std::uint32_t decisionRepeatIntervalMs)
{
    pxc_log_open();

    if (gatewayPort < 1U || gatewayPort > 65535U
        || plcPort < 1U || plcPort > 65535U)
    {
        pxc_log_err("gateway and PLC ports must be in range 1..65535");
        pxc_log_close();
        return -1;
    }

    const uint16_t gatewayPortU16 = static_cast<uint16_t>(gatewayPort);
    const uint16_t plcPortU16     = static_cast<uint16_t>(plcPort);

    if (maxHbFailures == 0)
    {
        pxc_log_err("maxHbFailures must be in 1..255");
        pxc_log_close();
        return -1;
    }

    /* Validate the same way as the CLI parser so a direct library caller
     * cannot silently install an out-of-range period. 0 = disabled;
     * otherwise must land inside [min, max]. */
    if (decisionRepeatIntervalMs > kDecisionRepeatIntervalMsMax
        || (decisionRepeatIntervalMs != 0U
            && decisionRepeatIntervalMs < kDecisionRepeatIntervalMsMinNonZero))
    {
        pxc_log_err("decisionRepeatIntervalMs must be 0 or in " +
                    std::to_string(kDecisionRepeatIntervalMsMinNonZero) + ".." +
                    std::to_string(kDecisionRepeatIntervalMsMax) +
                    " (0 = periodic repeat off)");
        pxc_log_close();
        return -1;
    }

    /* Register signal handlers */
    if (std::signal(SIGINT, signalHandler) == SIG_ERR)
        pxc_log_err("Failed to register SIGINT handler");
    if (std::signal(SIGTERM, signalHandler) == SIG_ERR)
        pxc_log_err("Failed to register SIGTERM handler");

    /* --- PLC command socket (send to PLC, receive ACKs) --- */
    plcSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (plcSock < 0)
    {
        pxc_log_err("Failed to create UDP socket");
        pxc_log_close();
        return -1;
    }

    int flags = fcntl(plcSock, F_GETFL, 0);
    if (flags == -1 || fcntl(plcSock, F_SETFL, flags | O_NONBLOCK) == -1)
        pxc_log_warning("Failed to set plcSock non-blocking");

    struct sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port        = htons(0);
    if (bind(plcSock, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0)
    {
        pxc_log_err("Failed to bind UDP socket");
        close(plcSock); plcSock = -1;
        pxc_log_close();
        return -1;
    }

    socklen_t addrLen = sizeof(localAddr);
    if (getsockname(plcSock, (struct sockaddr*)&localAddr, &addrLen) < 0)
    {
        pxc_log_err("Failed to get socket name");
        close(plcSock); plcSock = -1;
        pxc_log_close();
        return -1;
    }

    plcAddr.sin_family = AF_INET;
    plcAddr.sin_port   = htons(plcPortU16);
    if (inet_pton(AF_INET, plcIP.c_str(), &plcAddr.sin_addr) <= 0)
    {
        pxc_log_err("Invalid IP address");
        close(plcSock); plcSock = -1;
        pxc_log_close();
        return -1;
    }

    /* --- PSD Gateway socket: bind ephemeral, send REGR to gateway, recv DecisionRequests + HB --- */
    psdGatewayListenSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (psdGatewayListenSock < 0)
    {
        pxc_log_err("Failed to create psdGateway socket");
        close(plcSock); plcSock = -1;
        pxc_log_close();
        return -1;
    }

    int gwFlags = fcntl(psdGatewayListenSock, F_GETFL, 0);
    if (gwFlags == -1 || fcntl(psdGatewayListenSock, F_SETFL, gwFlags | O_NONBLOCK) == -1)
        pxc_log_warning("Failed to set psdGatewayListenSock non-blocking");

    struct sockaddr_in bindAddr = {};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port        = htons(0);
    if (bind(psdGatewayListenSock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0)
    {
        pxc_log_err("Failed to bind psdGateway socket");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        pxc_log_close();
        return -1;
    }

    gatewayAddr.sin_family = AF_INET;
    gatewayAddr.sin_port  = htons(gatewayPortU16);
    if (inet_pton(AF_INET, gatewayIP.c_str(), &gatewayAddr.sin_addr) <= 0)
    {
        pxc_log_err("Invalid gateway IP address");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        pxc_log_close();
        return -1;
    }
    if (!sendGatewayRegistration())
    {
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        pxc_log_close();
        return -1;
    }
    lastRegistrationTime = std::chrono::steady_clock::now();

    /* Reset state */
    stopSDMThreads.store(false);
    signalShutdownRequested.store(false);
    hbGatewayAlive.store(false);
    hbFaultLatched.store(false);
    /* Tier-2 flag false at launch; no tier-2-exit restore (nothing to recover from). */
    hbTier2SafeHoldActive.store(false, std::memory_order_release);
    pssErrorFusionSuppressLatched.store(false, std::memory_order_release);
    g_lastGatewayHbMissCount.store(0U);
    regrTier2AttemptsRemaining.store(0);
    g_maxHbFailuresCfg.store(static_cast<uint32_t>(maxHbFailures));
    g_warnThresholdCfg.store(g_maxHbFailuresCfg.load() / 2U);
    {
        std::lock_guard<std::mutex> lock(proximityStateMtx);
        proximityActiveCommand = CMD_NORMAL;
    }
    g_decisionRepeatIntervalMs.store(decisionRepeatIntervalMs,
                                     std::memory_order_relaxed);

    /* --- Start threads --- */
    ackHandlerThread = std::thread(ackHandlerLoop);
    heartbeatThread  = std::thread(heartbeatTransmitter);
    hbWatchdogThread = std::thread(gatewayHeartbeatWatchdog);
    if (decisionRepeatIntervalMs > 0U)
    {
        periodicDecisionThread = std::thread(proximityPeriodicDecisionLoop);
        pxc_log_info(
            "Proximity Control Algorithm initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat every " +
            std::to_string(decisionRepeatIntervalMs) + " ms)");
    }
    else
    {
        pxc_log_info(
            "Proximity Control Algorithm initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat disabled)");
    }

    /* --- Main thread runs the PSD Gateway event loop --- */
    psdGatewayEventListener();

    if (g_signal_received)
        signalShutdownRequested.store(true);
    if (signalShutdownRequested.load())
    {
        pxc_log_info("Signal received - initiating graceful shutdown");
        stopSDMThreads.store(true);
    }

    // Perform shutdown
    shutdownProximityControlAlgo();

    return 0;
}

void shutdownProximityControlAlgo()
{
    stopSDMThreads.store(true);
    /* Wake the periodic loop so it observes stopSDMThreads immediately
     * instead of sleeping out the remainder of its current interval.
     * Safe to call even when the thread was never started (interval==0). */
    proximityPeriodicDecisionCv.notify_all();

    auto joinThread = [](std::thread& t) {
        if (t.joinable())
            t.join();
    };

    joinThread(heartbeatThread);
    joinThread(ackHandlerThread);
    joinThread(hbWatchdogThread);
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

    pxc_log_close();
}