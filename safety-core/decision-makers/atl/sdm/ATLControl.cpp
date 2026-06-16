/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <string>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <map>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <ctime>
#include <cstring>
#include <algorithm>
#include <cerrno>
#include <cstdint>

#include "ATLControl.h"
#include <NvPSDGatewayProtocol.h>
#include "pss_message_validate.h"

/* Simple SDM logging: console + optional log file (no NvPSB dependency) */
static std::mutex atlLogMtx;
static FILE* atlLogFile = nullptr;

static void atl_log_open(void)
{
    std::lock_guard<std::mutex> lock(atlLogMtx);
    if (!atlLogFile) {
        atlLogFile = fopen("atl_sdm.log", "a");
        if (atlLogFile)
            setvbuf(atlLogFile, nullptr, _IOLBF, 0);
    }
}

static void atl_log_close(void)
{
    std::lock_guard<std::mutex> lock(atlLogMtx);
    if (atlLogFile) {
        fclose(atlLogFile);
        atlLogFile = nullptr;
    }
}

static void atl_log(const char* level, const char* msg)
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

    std::lock_guard<std::mutex> lock(atlLogMtx);
    FILE* out = (strcmp(level, "ERR") == 0 || strcmp(level, "WARNING") == 0)
                ? stderr : stdout;
    fprintf(out, "%s [ATL][%s] %s\n", prefix, level, msg);
    if (atlLogFile)
        fprintf(atlLogFile, "%s [ATL][%s] %s\n", prefix, level, msg);
}

static void atl_log_info(const std::string& msg)    { atl_log("INFO", msg.c_str()); }
static void atl_log_warning(const std::string& msg) { atl_log("WARNING", msg.c_str()); }
static void atl_log_err(const std::string& msg)     { atl_log("ERR", msg.c_str()); }

#define HEARTBEAT_PERIOD_SEC   5
#define ACK_TIMEOUT_SECONDS    3
#define ACK_CLEANUP_SECONDS    10
#define ACK_MONITOR_INTERVAL_S 1
#define ACK_RECEIVER_SLEEP_MS  100
#define MAIN_LOOP_POLL_TIMEOUT_MS 50
/* After HB ACK send returns EAGAIN/EWOULDBLOCK, poll(POLLOUT) before next main POLLIN wait (reduces latency vs deferring to next loop). */
#define HB_ACK_POLL_TIMEOUT_MS    10
#define HB_ACK_POLL_MAX_POLLS     32

// Tripwire = trailer entry line. TW OUT = entry into trailer, TW IN = exit from trailer.
// event_mapping_atl.pb.txt: EVENT_0=forklift TW OUT, EVENT_1=forklift TW IN,
// EVENT_2=person TW OUT, EVENT_3=person TW IN, EVENT_4=person restricted ROI violation,
// EVENT_5=person restricted ROI violation cleared

/* Heartbeat / registration (NvPSDGatewayProtocol.h) */
static constexpr int HB_WATCHDOG_TIMEOUT_MS = 8000;  // no HB for 8 s -> alarm
/* Re-register with gateway periodically so we recover after gateway restart (in-memory state lost). */
static constexpr int REG_RETRY_INTERVAL_MS = 30000;
/* Non-blocking UDP: retry REGR with poll(POLLOUT) when send returns EAGAIN/EWOULDBLOCK. */
static constexpr int REGR_SEND_MAX_ATTEMPTS = 32;
static constexpr int REGR_SEND_EINTR_MAX_RETRIES = 32;
static constexpr int REGR_SEND_POLL_TIMEOUT_MS = 5;
/* Max poll timeouts (or spurious wakeups) per outer send attempt; avoids infinite inner loop on launch. */
static constexpr int REGR_POLL_MAX_POLLS_PER_ATTEMPT = 64;

/* launchATLControlAlgo decisionRepeatIntervalMs: 0 = periodic repeat off; when non-zero, inclusive
 * [kDecisionRepeatIntervalMsMinNonZero, kDecisionRepeatIntervalMsMax] to avoid PLC overload. */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax       = 36000U;

/* Event types this SDM subscribes to (must match onEventNotificationReceive); gateway filters by REGR. */
static constexpr EventType ATL_SUBSCRIBED_EVENTS[] = {
    EVENT_0, EVENT_1, EVENT_2, EVENT_3, EVENT_4, EVENT_5, SW_FAIL
};
static constexpr uint8_t ATL_SUBSCRIBED_COUNT =
    sizeof(ATL_SUBSCRIBED_EVENTS) / sizeof(ATL_SUBSCRIBED_EVENTS[0]);

/* PLC command socket */
static int                   plcSock    = -1;
static struct sockaddr_in    plcAddr    = {};
/* Monotonic counter; CmdPacket.seq is uint16_t — use low 16 bits for a full 0..65535 wrap.
 * (std::atomic<uint16_t> fetch_add wrapped before a separate modulo step and could duplicate seq.) */
static std::atomic<uint32_t> cmdSeqNo{0};
static std::mutex            plcSocketMtx;

/* ACK tracking */
struct CommandStatus {
    std::chrono::system_clock::time_point sentTime;
    unsigned char command;
    bool          acknowledged;
    uint64_t      sentTimeSec;
    uint64_t      sentTimeMicro;
    CommandStatus()
        : sentTime(std::chrono::system_clock::now()), command(0),
          acknowledged(false), sentTimeSec(0), sentTimeMicro(0) {}
    CommandStatus(unsigned char cmd, const uint64_t sec, uint64_t micro)
        : sentTime(std::chrono::system_clock::now()), command(cmd),
          acknowledged(false), sentTimeSec(sec), sentTimeMicro(micro) {}
};

static std::map<uint16_t, CommandStatus> pendingCommands;
static std::mutex commandStatusMtx;

// ATL-specific state (tripwire = trailer entry line; TW OUT = entry, TW IN = exit)
static bool forkliftInTrailer = false;              // EVENT_0: entered trailer; EVENT_1: exited
static int personsInTrailerCount = 0;                // EVENT_2: person entered (+); EVENT_3: person exited (-)
static constexpr int PERSONS_IN_TRAILER_MAX = 10000;  // cap to avoid overflow from EVENT_2/3 skew
static bool restrictedAreaViolationByPerson = false;   // EVENT_4 set; EVENT_5 clear
// Master mutex for atomic state reading
static std::mutex masterStateMtx;
/* Serializes PLC decision sequences: evaluateATLDecision (periodic + event), gateway-HB watchdog
 * safe-hold (UNMUTE+SW_ERROR), so ordering vs plcSocketMtx-level sends is consistent. Order: this
 * mutex first, then masterStateMtx inside evaluateATLDecision; sendDecisionCommand uses
 * commandStatusMtx and plcSocketMtx. */
static std::mutex decisionEvalSendMtx;

/* De-duplication: track last-processed event ID per sensor slot to avoid
 * re-processing persistent entries in the sensorDataSummary snapshot. */
static constexpr uint32_t EVENT_ID_UNSEEN = 0xFFFFFFFFU;
static uint32_t lastEventId[MAX_SENSORS_DATA_SUMMARY_SIZE] = {
    EVENT_ID_UNSEEN, EVENT_ID_UNSEEN, EVENT_ID_UNSEEN, EVENT_ID_UNSEEN,
    EVENT_ID_UNSEEN, EVENT_ID_UNSEEN, EVENT_ID_UNSEEN, EVENT_ID_UNSEEN,
};

/* threads */
static std::atomic<bool> stopSDMThreads{false};
static std::atomic<bool> signalShutdownRequested{false};
static std::thread       heartbeatThread;
static std::thread       ackHandlerThread;    // receiver + timeout monitor
static std::thread       hbWatchdogThread;
static std::thread       periodicDecisionThread;
static std::mutex        atlPeriodicDecisionWaitMtx;
static std::condition_variable atlPeriodicDecisionCv;

/* 0 = periodic repeat disabled; otherwise re-send evaluateATLDecision() on this interval. */
static std::atomic<std::uint32_t> g_decisionRepeatIntervalMs{5000U};

/* SIGINT/SIGTERM latch: only `volatile sig_atomic_t` assignment in handler (POSIX async-signal-safe). */
static volatile sig_atomic_t g_signal_received = 0;

/* PSD Gateway UDP socket: bind ephemeral, send REGR to gateway, recv DecisionRequests + HB */
static int psdGatewayListenSock = -1;
static std::mutex gwSockMtx;
static struct sockaddr_in gatewayAddr = {};
/* HB ACK: one non-blocking send per HB; if EAGAIN, flushPendingHbAck() retries each loop (no inline poll). */
static bool                     hbAckPending = false;
static char                     hbAckPendingBuf[NVPSD_GATEWAY_HB_MSG_SIZE];
/* Time of last successful REGR (launch sets this after send or in the past to allow immediate retry). Periodic path advances only on success so failed sends retry next loop, not after REG_RETRY_INTERVAL_MS. */
static std::chrono::steady_clock::time_point lastRegistrationTime;

/* Heartbeat watchdog state */
static std::mutex                            hbMtx;
static std::chrono::steady_clock::time_point hbLastRecvTime;
static std::atomic<bool>                     hbGatewayAlive{false};

/* Heartbeat fail-safe: WARN = max/2 (integer division); miss count derived from time since last gateway HB. */
static std::atomic<uint32_t>                 g_maxHbFailuresCfg{10U};
static std::atomic<uint32_t>                 g_warnThresholdCfg{5U};
static std::atomic<bool>                     hbFaultLatched{false};
/* PSS ERROR latched: repeat fail-safe UNMUTE+SW_ERROR on periodic evaluate until a non-ERROR request clears it. */
static std::atomic<bool>                     pssErrorFusionSuppressLatched{false};
static std::atomic<int>                      regrTier2AttemptsRemaining{0};
static std::atomic<uint32_t>                 g_lastGatewayHbMissCount{0U};

static uint32_t gatewayMissFromElapsedMs(int64_t elapsedMs)
{
    const int64_t kStaleStartMs = 5000;
    const int64_t kPeriodMs     = 5500; /* ~ gateway HB_SEND + ACK window */
    if (elapsedMs <= kStaleStartMs)
        return 0U;
    const uint64_t m = 1U + static_cast<uint64_t>((elapsedMs - kStaleStartMs) / kPeriodMs);
    const uint32_t maxF = g_maxHbFailuresCfg.load();
    if (m > static_cast<uint64_t>(maxF))
        return maxF;
    return static_cast<uint32_t>(m);
}

/* Fusion-driven MUTE/UNMUTE must not override watchdog safe-hold (tier-2 UNMUTE+SW_ERROR, tier-3 same + shutdown).
 * Suppress while HB miss count is past warn tier or fault is latched; resume when m <= warnW again (e.g. m == 0). */
static bool gatewayHbSuppressFusionDecisions()
{
    if (hbFaultLatched.load(std::memory_order_relaxed))
        return true;
    const uint32_t m     = g_lastGatewayHbMissCount.load(std::memory_order_relaxed);
    const uint32_t warnW = g_warnThresholdCfg.load(std::memory_order_relaxed);
    return m > warnW;
}

/* Tier-2 gateway HB band (recoverable): watchdog sends UNMUTE+SW_ERROR once on entry; evaluateATLDecision repeats same pair on periodic/event. */
static bool gatewayHbTier2SafeHoldBand()
{
    if (hbFaultLatched.load(std::memory_order_relaxed))
        return false;
    const uint32_t m     = g_lastGatewayHbMissCount.load(std::memory_order_relaxed);
    const uint32_t maxF  = g_maxHbFailuresCfg.load(std::memory_order_relaxed);
    const uint32_t warnW = g_warnThresholdCfg.load(std::memory_order_relaxed);
    return m > warnW && m < maxF;
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


/* Populate ObjectRecord from the first sensor's fusionMetadata */
static void fillObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS],
                               const SensorData* data)
{
    std::memset(objects, 0, sizeof(ObjectRecord) * COMMAND_NUM_OBJECTS);
    if (!data)
        return;

    const EventFusionMetadata& meta =
        data->event.fusionMetadata;

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
                         const SensorData* data)
{
    const uint32_t n = cmdSeqNo.fetch_add(1);
    const uint16_t seqNo = static_cast<uint16_t>(n);
    auto timeResult = getCurrentUTCTimeForPacket();
    uint64_t tsSec   = timeResult.first;
    uint64_t tsMicro = timeResult.second;


    CmdPacket pkt;
    std::memset(&pkt, 0, sizeof(pkt));

    pkt.identifier      = ATL_PACKET_IDENTIFIER;
    pkt.seq             = seqNo;
    pkt.command         = command;
    pkt.ts_seconds      = tsSec;
    pkt.ts_microseconds = tsMicro;

    fillObjectRecords(pkt.objects, data);

    pkt.crc32 = cmdPacketCRC32(&pkt);

    bool shouldPrintAndTrack = false;
    const char* cmdType = commandName(command);
    if (command == CMD_MUTE || command == CMD_UNMUTE || command == CMD_SW_ERROR)
        shouldPrintAndTrack = true;

    if (shouldPrintAndTrack) {
        std::ostringstream logMsg;
        logMsg << "Sending decision command: " << cmdType
               << " (0x" << std::hex << std::setfill('0') << std::setw(2)
               << (int)command << std::dec
               << "), SeqNo: " << seqNo
               << ", UTC epoch: " << tsSec << "." << std::setfill('0')
               << std::setw(6) << tsMicro
               << ", Obj0 ID: " << pkt.objects[0].object_id;
        atl_log_info(logMsg.str());
    }

    if (trackAck && shouldPrintAndTrack) {
        std::lock_guard<std::mutex> cmdLock(commandStatusMtx);
        pendingCommands[seqNo] = CommandStatus(command, tsSec, tsMicro);
    }

    std::lock_guard<std::mutex> sockLock(plcSocketMtx);
    if (plcSock >= 0) {
        if (sendto(plcSock, &pkt, sizeof(pkt), 0,
                   (struct sockaddr*)&plcAddr, sizeof(plcAddr)) < 0)
            atl_log_err("Failed to send decision command");
    }
}

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
        if (ready < 0)
        {
            if (errno != EINTR)
            {
                atl_log_warning("SDM: poll on PLC ACK socket failed");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            /* EINTR: retry poll next iteration without tight spin */
        }

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
                    atl_log_info(ackMsg.str());
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
                        atl_log_warning(timeoutMsg.str());
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
        sendDecisionCommand(CMD_HEARTBEAT, true, nullptr);
    }
}

/*
 * Gateway-heartbeat watchdog (3-tier fail-safe)
 * ---------------------------------------------------------------
 * Miss count from time since last gateway HB datagram (see gatewayMissFromElapsedMs).
 * max_hb_failures N => tier-3 latch on miss count m >= N; tier 2 is warnW < m < N.
 * Tier 1: warn; Tier 2/3: safe PLC hold (UNMUTE + SW_ERROR) under decisionEvalSendMtx + bounded REGR / stop.
 * ================================================================ */
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

        if (m > prevMiss && m >= 1U && m <= warnW)
        {
            atl_log_warning("HB-PSD: gateway HB warn tier miss_count=" + std::to_string(m) +
                "/" + std::to_string(maxF) + " elapsed_ms=" + std::to_string(elapsed));
        }
        if (m > warnW && m < maxF && prevMiss <= warnW)
        {
            regrTier2AttemptsRemaining.store(
                (maxF > warnW + 1U) ? static_cast<int>(maxF - warnW - 1U) : 0);
            atl_log_err("HB-PSD: active fault (tier 2) — safe hold + bounded REGR; miss=" + std::to_string(m));
            {
                std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
                sendDecisionCommand(CMD_UNMUTE, true, nullptr);
                sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
            }
        }

        prevMiss = m;
        g_lastGatewayHbMissCount.store(m);

        if (m >= maxF)
        {
            if (!hbFaultLatched.exchange(true))
            {
                atl_log_err("HB-PSD: gateway HB fault latched (tier 3) — local fail-safe, no PSS connection");
                {
                    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
                    sendDecisionCommand(CMD_UNMUTE, true, nullptr);
                    sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
                }
                stopSDMThreads.store(true);
                atlPeriodicDecisionCv.notify_all();
                signalShutdownRequested.store(true);
            }
        }
    }
}

/* Periodic PLC decision repeat (independent of new events). Skips while gateway HB fault latched;
 * evaluateATLDecision() repeats UNMUTE+SW_ERROR while PSS ERROR latched or HB tier-2 band, else fusion.
 * Uses condition_variable so shutdown can wake the wait immediately (no long join delay). */
static void atlPeriodicDecisionLoop()
{
    while (!stopSDMThreads.load(std::memory_order_relaxed))
    {
        const std::uint32_t intervalMs = g_decisionRepeatIntervalMs.load(std::memory_order_relaxed);
        const auto sleepMs = std::chrono::milliseconds(intervalMs);

        {
            std::unique_lock<std::mutex> lock(atlPeriodicDecisionWaitMtx);
            atlPeriodicDecisionCv.wait_for(lock, sleepMs, [] {
                return stopSDMThreads.load(std::memory_order_relaxed);
            });
        }
        if (stopSDMThreads.load(std::memory_order_relaxed))
            break;
        if (hbFaultLatched.load())
            continue;
        evaluateATLDecision();
    }
}

/* ====================== DECISION LOGIC ========================== */
void onEventNotificationReceive(const DecisionRequest* request)
{
    atl_log_info("SDM: processing DecisionRequest id=" +
        std::to_string(request->requestId));

    /* --- PSS ERROR mode ------------------------------------------------ */
    if (request->pssStatus.mode == ERROR)
    {
        pssErrorFusionSuppressLatched.store(true, std::memory_order_release);
        atl_log_warning("PSS is in error mode, sending stop + software error command");
        {
            std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
            sendDecisionCommand(CMD_UNMUTE, true, nullptr);
            sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
        }
        return;
    }
    pssErrorFusionSuppressLatched.store(false, std::memory_order_release);

    /* --- Normal / Degraded mode ---------------------------------------- */

    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    bool stateChanged = false;

    for (uint8_t i = 0; i < maxSrc; ++i)
    {
        const SensorData& sd = request->sensorDataSummary[i];

        /* Defensive: drop STALE (already filtered by psdGateway) */
        if (sd.event.status == STALE)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "Dropping STALE event id=%u", (unsigned)sd.event.id);
            atl_log_info(buf);
            continue;
        }

        /* Dedup before unhealthy check to suppress repeated logs for persistent faults */
        if (sd.event.id == lastEventId[i])
            continue;
        lastEventId[i] = sd.event.id;

        if (!sd.isHealthy)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Sensor unhealthy -- logging only, not processing for decision: "
                     "eventId=%u pipelineID=%u",
                     (unsigned)sd.event.id, (unsigned)sd.event.fusionMetadata.pipelineID);
            atl_log_info(buf);
            continue;
        }

        if (!sd.isTrustedSource)
        {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "AI pipeline untrusted -- logging only, not processing for decision: "
                     "eventId=%u clientID=%u",
                     (unsigned)sd.event.id, (unsigned)sd.clientID);
            atl_log_info(buf);
            continue;
        }

        const EventType et = static_cast<EventType>(sd.event.type);

        switch (et)
        {
        case EVENT_0:  /* Forklift TW OUT = forklift entered trailer */
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                forkliftInTrailer = true;
                atl_log_info("ATL: Forklift entered trailer (TW OUT)");
            }
            stateChanged = true;
            break;
        case EVENT_1:  /* Forklift TW IN = forklift exited trailer */
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                forkliftInTrailer = false;
                atl_log_info("ATL: Forklift exited trailer (TW IN)");
            }
            stateChanged = true;
            break;
        case EVENT_2:  /* Person TW OUT = person entered trailer */
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                if (personsInTrailerCount < PERSONS_IN_TRAILER_MAX)
                    personsInTrailerCount++;
                atl_log_info("ATL: Person entered trailer (TW OUT), personsInTrailerCount=" +
                    std::to_string(personsInTrailerCount));
            }
            stateChanged = true;
            break;
        case EVENT_3:  /* Person TW IN = person exited trailer */
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                if (personsInTrailerCount > 0) {
                    personsInTrailerCount--;
                    atl_log_info("ATL: Person exited trailer (TW IN), personsInTrailerCount=" +
                        std::to_string(personsInTrailerCount));
                } else {
                    atl_log_warning("ATL: Person exited trailer but personsInTrailerCount already 0");
                }
            }
            stateChanged = true;
            break;
        case EVENT_4:  /* Person restricted area ROI violation */
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                restrictedAreaViolationByPerson = true;
                atl_log_info("ATL: Person restricted area violation set");
            }
            stateChanged = true;
            break;
        case EVENT_5:  /* Person restricted area ROI violation cleared */
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                restrictedAreaViolationByPerson = false;
                atl_log_info("ATL: Person restricted area violation cleared");
            }
            stateChanged = true;
            break;
        case SW_FAIL:
            atl_log_err("ATL: SW_FAIL received — triggering safe hold");
            {
                std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
                sendDecisionCommand(CMD_UNMUTE, true, nullptr);
                sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
            }
            return;
        default:
            break;
        }
    }

    /* Immediate decision when fusion/state changes; periodic thread repeats same logic on a timer. */
    if (stateChanged)
        evaluateATLDecision();
}

/* If false, one non-blocking send only (listener path); if true, poll/retry for launch. */
static bool sendGatewayRegistration(bool retryUntilWritable);

/* Wait for POLLOUT on fd (bounded); used after HB ACK send EAGAIN so we do not wait for MAIN_LOOP_POLL_TIMEOUT_MS. */
static bool hbAckPollForWritable(int fd)
{
    struct pollfd pfd = {};
    pfd.fd     = fd;
    pfd.events = POLLOUT;
    int pollBudget = 0;
    while (pollBudget < HB_ACK_POLL_MAX_POLLS)
    {
        pfd.revents = 0;
        const int pr = poll(&pfd, 1, HB_ACK_POLL_TIMEOUT_MS);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (pr == 0)
        {
            ++pollBudget;
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return false;
        if (pfd.revents & POLLOUT)
            return true;
        ++pollBudget;
    }
    return false;
}

static void hbAckApplySendResult(ssize_t sent, int sendErr, bool requirePendingFlag)
{
    std::lock_guard<std::mutex> lk(gwSockMtx);
    if (requirePendingFlag && !hbAckPending)
        return;
    if (sent == static_cast<ssize_t>(NVPSD_GATEWAY_HB_MSG_SIZE))
        hbAckPending = false;
    else if (sent >= 0)
    {
        atl_log_err("SDM: short send on HB ACK");
        hbAckPending = false;
    }
    else if (sendErr != EAGAIN && sendErr != EWOULDBLOCK && sendErr != EINTR)
    {
        atl_log_err("SDM: heartbeat ACK send failed");
        hbAckPending = false;
    }
}

static void flushPendingHbAck()
{
    int  fd = -1;
    char sendBuf[NVPSD_GATEWAY_HB_MSG_SIZE];
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        if (!hbAckPending || psdGatewayListenSock < 0)
            return;
        fd = psdGatewayListenSock;
        std::memcpy(sendBuf, hbAckPendingBuf, NVPSD_GATEWAY_HB_MSG_SIZE);
    }

    ssize_t sent = send(fd, sendBuf, NVPSD_GATEWAY_HB_MSG_SIZE, 0);
    int     sendErr = (sent < 0) ? errno : 0;

    if (sent == static_cast<ssize_t>(NVPSD_GATEWAY_HB_MSG_SIZE) || sent >= 0 ||
        (sendErr != EAGAIN && sendErr != EWOULDBLOCK && sendErr != EINTR))
    {
        hbAckApplySendResult(sent, sendErr, true);
        return;
    }
    if (sendErr == EINTR)
    {
        hbAckApplySendResult(sent, sendErr, true);
        return;
    }

    if (hbAckPollForWritable(fd))
    {
        sent = send(fd, sendBuf, NVPSD_GATEWAY_HB_MSG_SIZE, 0);
        sendErr = (sent < 0) ? errno : 0;
    }
    hbAckApplySendResult(sent, sendErr, true);
}

static void emitHbAckTryOnce(const char ack[NVPSD_GATEWAY_HB_MSG_SIZE])
{
    int  fd = -1;
    char sendBuf[NVPSD_GATEWAY_HB_MSG_SIZE];
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        std::memcpy(hbAckPendingBuf, ack, NVPSD_GATEWAY_HB_MSG_SIZE);
        hbAckPending = true;
        if (psdGatewayListenSock < 0)
            return;
        fd = psdGatewayListenSock;
        std::memcpy(sendBuf, ack, NVPSD_GATEWAY_HB_MSG_SIZE);
    }

    ssize_t sent = send(fd, sendBuf, NVPSD_GATEWAY_HB_MSG_SIZE, 0);
    int     sendErr = (sent < 0) ? errno : 0;

    if (sent == static_cast<ssize_t>(NVPSD_GATEWAY_HB_MSG_SIZE) || sent >= 0 ||
        (sendErr != EAGAIN && sendErr != EWOULDBLOCK && sendErr != EINTR))
    {
        hbAckApplySendResult(sent, sendErr, false);
        return;
    }
    if (sendErr == EINTR)
    {
        hbAckApplySendResult(sent, sendErr, false);
        return;
    }

    if (hbAckPollForWritable(fd))
    {
        sent = send(fd, sendBuf, NVPSD_GATEWAY_HB_MSG_SIZE, 0);
        sendErr = (sent < 0) ? errno : 0;
    }
    hbAckApplySendResult(sent, sendErr, false);
}

/* EVENT loop receive DecisionRequest from PSD Gateway */
static void psdGatewayEventListener()
{
    char               rawBuf[sizeof(DecisionRequest)];
    struct pollfd      pfd;

    while (!stopSDMThreads.load() && !signalShutdownRequested.load())
    {
        if (g_signal_received != 0)
            signalShutdownRequested.store(true);
        flushPendingHbAck();
        /* Periodic REGR: tier 2 uses bounded attempts (regrTier2AttemptsRemaining); tier 3 latched skips. */
        if (!hbFaultLatched.load())
        {
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
                    if (sendGatewayRegistration(false))
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
        if (ready < 0)
        {
            if (errno == EINTR)
                continue; /* retry poll after signal */
            atl_log_warning("SDM: poll on psdGateway socket failed");
            continue;
        }
        if (ready == 0)
            continue; /* timeout -- re-check stop flags */

        /* Connected UDP: kernel only delivers datagrams from gateway peer. */
        int recvFd = -1;
        {
            std::lock_guard<std::mutex> lk(gwSockMtx);
            if (psdGatewayListenSock < 0)
                break;
            recvFd = psdGatewayListenSock;
        }
        ssize_t n = recv(recvFd, rawBuf, sizeof(rawBuf), MSG_DONTWAIT);

        if (n < 0)
        {
            if (errno == EINTR)
                continue; /* retry recv after signal */
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                atl_log_warning("SDM: recv on psdGateway socket failed");
            continue;
        }

        /* Check if this is a heartbeat from PSDGateway */
        if (n == NVPSD_GATEWAY_HB_MSG_SIZE &&
            std::memcmp(rawBuf, NVPSD_GATEWAY_HB_MAGIC_GATEWAY, 4) == 0)
        {
            uint32_t netSeq;
            std::memcpy(&netSeq, rawBuf + 4, 4);
            uint32_t seq = ntohl(netSeq);
            atl_log_info("HB-PSD: received heartbeat seq=" +
                std::to_string(seq) + " from Gateway");

            /* Update watchdog timestamp */
            {
                std::lock_guard<std::mutex> lk(hbMtx);
                hbLastRecvTime = std::chrono::steady_clock::now();
                hbGatewayAlive.store(true);
            }

            /* Send ACK: [HBPC][seqNo]; EAGAIN leaves pending — flushPendingHbAck() each loop iteration. */
            char ack[NVPSD_GATEWAY_HB_MSG_SIZE];
            std::memcpy(ack, NVPSD_GATEWAY_HB_MAGIC_CLIENT, 4);
            std::memcpy(ack + 4, rawBuf + 4, 4);  // echo seq in network order
            emitHbAckTryOnce(ack);
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
                char hexBuf[12];
                snprintf(hexBuf, sizeof(hexBuf), "%08X", vErr);
                atl_log_err(std::string("SDM: DecisionRequest validation failed (flags=0x") +
                    hexBuf + ") — dropping");
                continue;
            }

            atl_log_info("SDM: received DecisionRequest from psdGateway, reqId=" +
                std::to_string(request.requestId) +
                " events=" +
                std::to_string(request.sensorDataSummarySize));

            onEventNotificationReceive(&request);
        }
        else if (n > 0)
        {
            atl_log_warning("SDM: received partial packet (" + std::to_string(n) +
                " bytes, expected " +
                std::to_string(sizeof(DecisionRequest)) + ")");
        }
        /* n < 0 after poll said POLLIN: spurious -- just loop back */
    }
}

/* REGR send helpers: socket is O_NONBLOCK; send/EINTR loop runs without gwSockMtx (fd snapshot only). */
enum class RegrSendResult { Ok, Failed, NeedPoll };

static RegrSendResult regrTrySend(int fd, const char* buf, size_t bufSize, bool retryUntilWritable)
{
    int eintrCount = 0;
    while (eintrCount < REGR_SEND_EINTR_MAX_RETRIES)
    {
        ssize_t sent = send(fd, buf, bufSize, 0);
        if (sent == static_cast<ssize_t>(bufSize))
            return RegrSendResult::Ok;
        if (sent >= 0)
        {
            atl_log_err("SDM: short send on REGR");
            return RegrSendResult::Failed;
        }
        const int err = errno;
        if (err == EINTR)
        {
            ++eintrCount;
            continue;
        }
        if (err != EAGAIN && err != EWOULDBLOCK)
        {
            atl_log_err("SDM: failed to send REGR to gateway");
            return RegrSendResult::Failed;
        }
        if (!retryUntilWritable)
            return RegrSendResult::Failed;
        return RegrSendResult::NeedPoll;
    }
    atl_log_err("SDM: REGR send failed (EINTR retries exhausted)");
    return RegrSendResult::Failed;
}

enum class RegrPollResult { Writable, RetryLater, FatalError };

static RegrPollResult regrPollUntilWritable(int fd)
{
    struct pollfd pfd = {};
    pfd.fd     = fd;
    pfd.events = POLLOUT;
    int pollBudget = 0;
    while (pollBudget < REGR_POLL_MAX_POLLS_PER_ATTEMPT)
    {
        pfd.revents = 0;
        const int pr = poll(&pfd, 1, REGR_SEND_POLL_TIMEOUT_MS);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            atl_log_warning("SDM: poll before REGR resend failed");
            return RegrPollResult::FatalError;
        }
        if (pr == 0)
        {
            ++pollBudget;
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            atl_log_err("SDM: gateway socket poll error before REGR resend");
            return RegrPollResult::FatalError;
        }
        if (pfd.revents & POLLOUT)
            return RegrPollResult::Writable;
        ++pollBudget;
    }
    return RegrPollResult::RetryLater;
}

/* Send REGR registration to gateway so it forwards subscribed event types.
 * Bounded: REGR_SEND_MAX_ATTEMPTS × (REGR_SEND_EINTR_MAX_RETRIES send tries, REGR_POLL_MAX_POLLS_PER_ATTEMPT polls per wait).
 * gwSockMtx: brief fd snapshot only; regrTrySend / poll run without mutex. */
static bool sendGatewayRegistration(bool retryUntilWritable)
{
    constexpr size_t bufSize = 4 + 1 + ATL_SUBSCRIBED_COUNT * sizeof(uint32_t);
    char buf[bufSize];
    std::memcpy(buf, NVPSD_GATEWAY_REG_MAGIC, 4);
    /* Wire format: one byte count (gateway reads as uint8_t); avoid signed char. */
    buf[4] = static_cast<unsigned char>(ATL_SUBSCRIBED_COUNT);
    for (uint8_t i = 0; i < ATL_SUBSCRIBED_COUNT; ++i)
    {
        uint32_t val = htonl(static_cast<uint32_t>(ATL_SUBSCRIBED_EVENTS[i]));
        std::memcpy(buf + 5 + i * sizeof(uint32_t), &val, sizeof(uint32_t));
    }

    const int maxAttempts = retryUntilWritable ? REGR_SEND_MAX_ATTEMPTS : 1;

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lk(gwSockMtx);
            if (psdGatewayListenSock < 0)
                return false;
            fd = psdGatewayListenSock;
        }
        const RegrSendResult sr = regrTrySend(fd, buf, bufSize, retryUntilWritable);

        if (sr == RegrSendResult::Ok)
        {
            atl_log_info("SDM: sent registration (" + std::to_string(ATL_SUBSCRIBED_COUNT) + " event types) to gateway");
            return true;
        }
        if (sr == RegrSendResult::Failed)
            return false;

        switch (regrPollUntilWritable(fd))
        {
        case RegrPollResult::FatalError:
            return false;
        case RegrPollResult::RetryLater:
            continue;
        case RegrPollResult::Writable:
            break;
        }
    }
    atl_log_warning("SDM: REGR send still blocked after retries");
    return false;
}

/* ====================== LAUNCH / SHUTDOWN ====================== */

/* Async-signal-safe: assign only to volatile sig_atomic_t (not std::atomic — not guaranteed in a handler). */
static void signalHandler(int /*sig*/)
{
    g_signal_received = 1;
}

/* Async-signal-safe: log and exit on segfault (optional hardening) */
static void segvHandler(int sig)
{
    const char msg[] = "ATL SDM: SEGFAULT - terminating\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(128 + sig);
}

int launchATLControlAlgo(const std::string& gatewayIP,
                         std::uint16_t gatewayPort,
                         const std::string& plcIP,
                         std::uint16_t plcPort,
                         std::uint8_t maxHbFailures,
                         std::uint32_t decisionRepeatIntervalMs)
{
    atl_log_open();

    if (gatewayPort == 0 || plcPort == 0)
    {
        atl_log_err("gateway and PLC ports must be in range 1..65535 (got 0)");
        atl_log_close();
        return -1;
    }

    if (maxHbFailures == 0)
    {
        atl_log_err("maxHbFailures must be in 1..255");
        atl_log_close();
        return -1;
    }

    if (decisionRepeatIntervalMs > kDecisionRepeatIntervalMsMax
        || (decisionRepeatIntervalMs != 0U
            && decisionRepeatIntervalMs < kDecisionRepeatIntervalMsMinNonZero))
    {
        atl_log_err("decisionRepeatIntervalMs must be 0 or in " +
                    std::to_string(kDecisionRepeatIntervalMsMinNonZero) + ".." +
                    std::to_string(kDecisionRepeatIntervalMsMax) + " (0 = periodic repeat off)");
        atl_log_close();
        return -1;
    }

    g_signal_received = 0;
    signalShutdownRequested.store(false);

    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        hbAckPending = false;
    }

    /* Register signal handlers */
    if (std::signal(SIGINT, signalHandler) == SIG_ERR)
        atl_log_err("Failed to register SIGINT handler");
    if (std::signal(SIGTERM, signalHandler) == SIG_ERR)
        atl_log_err("Failed to register SIGTERM handler");
    if (std::signal(SIGSEGV, segvHandler) == SIG_ERR)
        atl_log_err("Failed to register SIGSEGV handler");

    /* --- PLC command socket (send to PLC, receive ACKs) --- */
    plcSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (plcSock < 0)
    { atl_log_err("Failed to create UDP socket"); atl_log_close(); return -1; }

    int flags = fcntl(plcSock, F_GETFL, 0);
    fcntl(plcSock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port        = htons(0);
    if (bind(plcSock, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0)
    {
        atl_log_err("Failed to bind UDP socket");
        close(plcSock); plcSock = -1;
        atl_log_close(); return -1;
    }

    socklen_t addrLen = sizeof(localAddr);
    if (getsockname(plcSock, (struct sockaddr*)&localAddr, &addrLen) < 0)
    {
        atl_log_err("Failed to get socket name");
        close(plcSock); plcSock = -1;
        atl_log_close(); return -1;
    }

    plcAddr.sin_family = AF_INET;
    plcAddr.sin_port   = htons(plcPort);
    if (inet_pton(AF_INET, plcIP.c_str(), &plcAddr.sin_addr) <= 0)
    {
        atl_log_err("Invalid IP address");
        close(plcSock); plcSock = -1;
        atl_log_close(); return -1;
    }

    /* --- PSD Gateway socket: bind ephemeral, send REGR to gateway, recv DecisionRequests + HB --- */
    psdGatewayListenSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (psdGatewayListenSock < 0)
    {
        atl_log_err("Failed to create psdGateway socket");
        close(plcSock); plcSock = -1;
        atl_log_close(); return -1;
    }

    const int gwFlags = fcntl(psdGatewayListenSock, F_GETFL, 0);
    if (gwFlags < 0)
    {
        atl_log_err("Failed to get psdGateway socket flags");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        atl_log_close(); return -1;
    }
    if (fcntl(psdGatewayListenSock, F_SETFL, gwFlags | O_NONBLOCK) < 0)
    {
        atl_log_err("Failed to set psdGateway socket non-blocking");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        atl_log_close(); return -1;
    }

    struct sockaddr_in bindAddr = {};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port        = htons(0);
    if (bind(psdGatewayListenSock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0)
    {
        atl_log_err("Failed to bind psdGateway socket");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        atl_log_close(); return -1;
    }

    gatewayAddr.sin_family = AF_INET;
    gatewayAddr.sin_port  = htons(gatewayPort);
    if (inet_pton(AF_INET, gatewayIP.c_str(), &gatewayAddr.sin_addr) <= 0)
    {
        atl_log_err("Invalid gateway IP address");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        atl_log_close(); return -1;
    }
    /* Connected UDP: receive path is limited to this peer. */
    if (connect(psdGatewayListenSock, reinterpret_cast<const struct sockaddr*>(&gatewayAddr),
                sizeof(gatewayAddr)) < 0)
    {
        atl_log_err("Failed to connect psdGateway UDP socket to gateway");
        close(plcSock); plcSock = -1;
        close(psdGatewayListenSock); psdGatewayListenSock = -1;
        atl_log_close(); return -1;
    }
    if (sendGatewayRegistration(true))
        lastRegistrationTime = std::chrono::steady_clock::now();
    else
    {
        atl_log_warning("SDM: initial REGR did not complete; will retry in event loop");
        /* Elapsed time >= REG_RETRY_INTERVAL_MS so first listener iteration retries soon. */
        lastRegistrationTime = std::chrono::steady_clock::now() -
            std::chrono::milliseconds(REG_RETRY_INTERVAL_MS);
    }

    /* Reset state */
    stopSDMThreads.store(false);
    signalShutdownRequested.store(false);
    hbGatewayAlive.store(false);
    hbFaultLatched.store(false);
    pssErrorFusionSuppressLatched.store(false, std::memory_order_release);
    g_lastGatewayHbMissCount.store(0U);
    regrTier2AttemptsRemaining.store(0);
    g_maxHbFailuresCfg.store(static_cast<uint32_t>(maxHbFailures));
    g_warnThresholdCfg.store(g_maxHbFailuresCfg.load() / 2U);

    g_decisionRepeatIntervalMs.store(decisionRepeatIntervalMs, std::memory_order_relaxed);

    /* --- Start threads --- */
    ackHandlerThread = std::thread(ackHandlerLoop);
    heartbeatThread  = std::thread(heartbeatTransmitter);
    hbWatchdogThread = std::thread(gatewayHeartbeatWatchdog);

    std::ostringstream cfg;
    cfg << "ATL Control Algorithm initialized (PSD gateway registration and PLC command path active";
    if (decisionRepeatIntervalMs > 0U)
    {
        periodicDecisionThread = std::thread(atlPeriodicDecisionLoop);
        cfg << "; periodic decision repeat every " << decisionRepeatIntervalMs << " ms";
    }
    else
    {
        cfg << "; periodic decision repeat disabled (event-driven immediate only)";
    }
    cfg << ")";
    atl_log_info(cfg.str());

    /* --- Main thread runs the PSD Gateway event loop --- */
    psdGatewayEventListener();

    if (g_signal_received != 0)
        signalShutdownRequested.store(true);
    if (signalShutdownRequested.load())
    {
        atl_log_info("Signal received - initiating graceful shutdown");
        stopSDMThreads.store(true);
        atlPeriodicDecisionCv.notify_all();
    }

    // Perform shutdown
    shutdownATLControlAlgo();

    return 0;
}

void shutdownATLControlAlgo()
{
    // Ensure stop flag is set so threads can exit their loops
    stopSDMThreads.store(true);
    atlPeriodicDecisionCv.notify_all();

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
        hbAckPending = false;
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

    atl_log_close();
}

void evaluateATLDecision()
{
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);

    if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
    {
        sendDecisionCommand(CMD_UNMUTE, true, nullptr);
        sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
        return;
    }

    if (gatewayHbTier2SafeHoldBand())
    {
        sendDecisionCommand(CMD_UNMUTE, true, nullptr);
        sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
        return;
    }

    if (gatewayHbSuppressFusionDecisions())
        return;

    bool forkliftOut = false;
    int personOut = 0;
    bool restrictedViol = false;

    {
        std::lock_guard<std::mutex> stateLock(masterStateMtx);
        forkliftOut = forkliftInTrailer;
        personOut = personsInTrailerCount;
        restrictedViol = restrictedAreaViolationByPerson;
    }

    // Forklift safety MUTED + loading allowed when: forklift in trailer, zero persons in trailer,
    // and no person restricted area violation. Otherwise UNMUTE.
    if (forkliftOut && personOut == 0 && !restrictedViol)
    {
        atl_log_info("ATL: Forklift in trailer, no persons in trailer, no restricted violation - MUTE (Allow Loading)");
        sendDecisionCommand(CMD_MUTE, true, nullptr);
    }
    else
    {
        atl_log_info("ATL: Unmute - forkliftInTrailer=" + std::string(forkliftOut ? "1" : "0") +
            " personsInTrailer=" + std::to_string(personOut) +
            " restrictedAreaViolationByPerson=" + std::string(restrictedViol ? "1" : "0"));
        sendDecisionCommand(CMD_UNMUTE, true, nullptr);
    }
}