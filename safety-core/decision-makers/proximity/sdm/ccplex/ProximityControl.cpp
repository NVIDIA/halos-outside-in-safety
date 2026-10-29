/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cerrno>
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
#include "nvpsf_transport.h"
#include "pss_message_validate.h"
#include "sdm_decision_request_state.h"
#include "sdm_decision_freshness.hpp"
#include "sdm_reviewed_sensor_set.hpp"
#include "sdm_gateway_decision_sequence.hpp"

/* Simple SDM logging: console + optional log file (no NvPSB dependency) */
static std::mutex pxcLogMtx;
static FILE* pxcLogFile = nullptr;
static std::string pxcLogPath = "pxc_sdm.log";
static bool pxcLogPathFrozen = false; /* Protected by pxcLogMtx. */

bool proximitySetLogFile(const std::string& path)
{
    if (path.empty())
        return false;
    std::lock_guard<std::mutex> lock(pxcLogMtx);
    if (pxcLogPathFrozen)
        return false;
    pxcLogPath = path;
    return true;
}

static void pxc_log_open(void)
{
    std::lock_guard<std::mutex> lock(pxcLogMtx);
    pxcLogPathFrozen = true;
    if (!pxcLogFile) {
        pxcLogFile = fopen(pxcLogPath.c_str(), "a");
        if (pxcLogFile)
            setvbuf(pxcLogFile, nullptr, _IOLBF, 0);
        else
            fprintf(stderr, "[PXC][ERR] cannot open log file '%s' (%s); logging to console only\n",
                    pxcLogPath.c_str(), strerror(errno));
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

#define HEARTBEAT_PERIOD_SEC   5
#define ACK_TIMEOUT_SECONDS    3

/* Valid range for launchProximityControlAlgo(decisionRepeatIntervalMs). 0
 * disables the periodic re-assert; non-zero values are clamped to [100 ms, 36000 ms] so
 * we neither flood the PLC nor let the last decision age past a typical ACK window. */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                      = 100U;
static constexpr std::uint32_t kHbTimingMsMax                      = 600000U;
static constexpr int SAFE_RELEASE_MIN_INTERVAL_MS                  = 1000;
/* Floor between halo fan-out failure warnings. Halo is best-effort overlay
 * traffic, so a listener that stays down must not turn every decision into a
 * log line. */
static constexpr int HALO_WARN_MIN_INTERVAL_MS                     = 1000;
#define ACK_CLEANUP_SECONDS    10
#define ACK_MONITOR_INTERVAL_S 1
#define ACK_RECEIVER_SLEEP_MS  100
#define MAIN_LOOP_POLL_TIMEOUT_MS 50

/* Config-selected proximity pair groups, each with the existing 3-tier model
 * driven from the MDX mapping. Every one pairs a machine with a person;
 * machine-to-machine proximity is not tracked here. The distance bands live entirely in the
 * mapping file: mdx-client resolves a measured distance to a tier, and this SDM
 * only ever sees the tier. That is why groups can be retuned to different bands
 * without any distinction in the code below.
 * Event numbers and enabled groups come from this instance's startup config.
 * Configuration never changes tier semantics. */

/* Which physical pair shape an event describes. Descriptive only: arbitration
 * below compares tiers and never the group, since the configured groups share the
 * same bands and a critical from any of them stops the machine. It is recorded
 * per event anyway because it is what makes the table's shape checkable -- one
 * of each tier per group -- and what a group-specific rule would key off if one
 * is ever needed again. */
enum ProximityPairGroup : std::uint8_t {
    PAIR_GROUP_HUMANOID_PERSON = 0,
    PAIR_GROUP_FORKLIFT_PERSON,
    PAIR_GROUP_TRANSPORTER_PERSON,
    PAIR_GROUP_FOURIER_HUMANOID_PERSON,
};
static_assert(PAIR_GROUP_FOURIER_HUMANOID_PERSON + 1U == PROXIMITY_PAIR_GROUP_COUNT,
              "PXC pair groups must match the startup config order");

/* Severity tier within a pair group, ascending. */
enum ProximityTier : std::uint8_t {
    PROXIMITY_TIER_NO_VIOLATION = 0,
    PROXIMITY_TIER_WARNING,
    PROXIMITY_TIER_CRITICAL,
};

struct ProximityEventDescriptor {
    EventType          event;
    ProximityPairGroup group;
    ProximityTier      tier;
};

/* THE event registry for this SDM.
 *
 * Adding a pair group is three rows here plus its rules in
 * proximity_event_mapping.pb.txt. Both the gateway subscription and the batch
 * arbitration are derived from this table, so a group cannot end up arbitrated
 * but unsubscribed -- or subscribed but silently ignored -- which is what the
 * hand-maintained parallel list this replaces invited. Merging two apps that
 * each carried their own copy of that list is precisely how such a drift would
 * have arrived. */
static ProximityEventDescriptor PROXIMITY_EVENT_TABLE[] = {
    { EVENT_UNKNOWN, PAIR_GROUP_HUMANOID_PERSON,    PROXIMITY_TIER_NO_VIOLATION },
    { EVENT_UNKNOWN, PAIR_GROUP_HUMANOID_PERSON,    PROXIMITY_TIER_WARNING      },
    { EVENT_UNKNOWN, PAIR_GROUP_HUMANOID_PERSON,    PROXIMITY_TIER_CRITICAL     },
    { EVENT_UNKNOWN, PAIR_GROUP_FORKLIFT_PERSON,    PROXIMITY_TIER_NO_VIOLATION },
    { EVENT_UNKNOWN, PAIR_GROUP_FORKLIFT_PERSON,    PROXIMITY_TIER_WARNING      },
    { EVENT_UNKNOWN, PAIR_GROUP_FORKLIFT_PERSON,    PROXIMITY_TIER_CRITICAL     },
    { EVENT_UNKNOWN, PAIR_GROUP_TRANSPORTER_PERSON, PROXIMITY_TIER_NO_VIOLATION },
    { EVENT_UNKNOWN, PAIR_GROUP_TRANSPORTER_PERSON, PROXIMITY_TIER_WARNING      },
    { EVENT_UNKNOWN, PAIR_GROUP_TRANSPORTER_PERSON, PROXIMITY_TIER_CRITICAL     },
    { EVENT_UNKNOWN, PAIR_GROUP_FOURIER_HUMANOID_PERSON, PROXIMITY_TIER_NO_VIOLATION },
    { EVENT_UNKNOWN, PAIR_GROUP_FOURIER_HUMANOID_PERSON, PROXIMITY_TIER_WARNING      },
    { EVENT_UNKNOWN, PAIR_GROUP_FOURIER_HUMANOID_PERSON, PROXIMITY_TIER_CRITICAL     },
};
static constexpr std::size_t PROXIMITY_EVENT_TABLE_SIZE =
    sizeof(PROXIMITY_EVENT_TABLE) / sizeof(PROXIMITY_EVENT_TABLE[0]);
static_assert(PROXIMITY_EVENT_TABLE_SIZE == PROXIMITY_EVENT_COUNT,
              "PXC config keys must cover every pair group and tier");

static std::mutex proximityBindingMtx;
static bool proximityBindingConfigured = false;
static bool proximityBindingFrozen = false;

bool proximitySetSubscribedEvents(const ProximitySdmConfig& config)
{
    std::lock_guard<std::mutex> lock(proximityBindingMtx);
    if (proximityBindingFrozen)
        return false;
    std::size_t enabledCount = 0U;
    for (std::size_t i = 0U; i < PROXIMITY_EVENT_COUNT; ++i)
    {
        const std::size_t first = (i / PROXIMITY_TIERS_PER_GROUP) * PROXIMITY_TIERS_PER_GROUP;
        const bool enabled = config.events[first] != EVENT_UNKNOWN;
        if (enabled != (config.events[i] != EVENT_UNKNOWN))
            return false; // A group must have all three tiers or be disabled.
        if (!enabled)
            continue;
        if (static_cast<unsigned>(config.events[i]) > static_cast<unsigned>(EVENT_23))
            return false;
        for (std::size_t j = 0U; j < i; ++j)
            if (config.events[i] == config.events[j])
                return false;
        ++enabledCount;
    }
    if (enabledCount == 0U)
        return false;
    for (std::size_t i = 0U; i < PROXIMITY_EVENT_COUNT; ++i)
        PROXIMITY_EVENT_TABLE[i].event = config.events[i];
    proximityBindingConfigured = true;
    return true;
}

/* Returns nullptr for anything this SDM does not tier: sensor health, SW_FAIL.
 * Linear over at most twelve entries, on a path that already does a syscall per
 * decision. */
static const ProximityEventDescriptor* proximityEventDescriptor(int eventType)
{
    for (std::size_t i = 0; i < PROXIMITY_EVENT_TABLE_SIZE; ++i)
    {
        if (PROXIMITY_EVENT_TABLE[i].event != EVENT_UNKNOWN &&
            static_cast<int>(PROXIMITY_EVENT_TABLE[i].event) == eventType)
            return &PROXIMITY_EVENT_TABLE[i];
    }
    return nullptr;
}

/* This SDM has no notion of a machine's own operator: a Person inside any
 * machine's critical band stops that machine, including one being carried by it.
 * mdx-client can still identify a rider -- the tracker and its tuning remain
 * there -- but no rule in proximity_event_mapping.pb.txt asks it to, and nothing
 * here consumes such an event or subscribes to one. Restoring the behaviour
 * means adding the mapping rule and a consumer for its event on this side; the
 * previous implementation of both is in this file's git history.
 *
 * The consequence is deliberate and worth stating plainly: a rider repeatedly
 * stops the vehicle they are operating. That was accepted because this
 * application does not need to tell an operator from a pedestrian, and because
 * every path that could withhold a stop is one more thing a reviewer has to
 * discharge. */

/* Heartbeat protocol (NvPSDGatewayProtocol.h) */
/* Re-register with gateway periodically so we recover after gateway restart (in-memory state lost). */
static constexpr int REG_RETRY_INTERVAL_MS = 30000;

/* REGR send retry budget. The transport's send is non-blocking by contract and
 * WOULD_BLOCK is its only retryable outcome, so the retry is just "sleep
 * briefly and try again" rather than the poll(POLLOUT) ladder a raw
 * O_NONBLOCK socket needed. */
static constexpr int REGR_SEND_MAX_ATTEMPTS = 32;
static constexpr int REGR_SEND_RETRY_SLEEP_MS = 5;

/* Upstream reconnect cadence. Fixed rather than exponential: backoff would make
 * worst-case recovery depend on how long the outage had already lasted, which
 * is exactly when recovery matters most. Attempts are unbounded because a
 * gateway restart must not require restarting the SDM. */
static constexpr int GW_RECONNECT_INTERVAL_MS = 500;

/*
 * Events this SDM receives but assigns no proximity tier to.
 *
 * This list is NOT the full set of untiered events that arrive. The gateway
 * force-adds a mandatory set to every client's subscription regardless of what
 * the REGR asked for -- NvPSDGatewayAddMandatoryEventTypes() in
 * NvPSDGatewayContract.hpp inserts SW_FAIL, PSS_STATUS_NOOP, SENSOR_INVALID,
 * SENSOR_VALID, AI_PIPELINE_INVALID and AI_PIPELINE_VALID. All six reach this
 * SDM and all six are handled below: the PSS fault signal, the no-op that
 * keeps decision freshness alive, SAIM's per-sensor trust reports, and the Perception Monitor's
 * pipeline-wide trust verdict.
 *
 * SW_FAIL is therefore named here to state the intent, not to make it arrive;
 * it would arrive either way. Naming the rest buys nothing and one of them
 * cannot be named at all: NvPSDGatewayBuildRegisteredEventSet() discards any
 * REGR entry at or past EVENT_UNKNOWN unless it is one of the four health
 * types, which lets the SENSOR_* and AI_PIPELINE_* pairs through but drops
 * PSS_STATUS_NOOP -- and the mandatory set then re-adds all of them anyway.
 */
static constexpr EventType PROXIMITY_UNTIERED_EVENTS[] = { SW_FAIL };
static constexpr std::size_t PROXIMITY_UNTIERED_COUNT =
    sizeof(PROXIMITY_UNTIERED_EVENTS) / sizeof(PROXIMITY_UNTIERED_EVENTS[0]);

/* Gateway subscription is the tiered table plus the untiered list, so
 * registration cannot drift from the decision logic. */
static constexpr std::size_t PROXIMITY_MAX_SUBSCRIBED_COUNT =
    PROXIMITY_EVENT_TABLE_SIZE + PROXIMITY_UNTIERED_COUNT;

/* The REGR payload carries the count in a single byte. */
static_assert(PROXIMITY_MAX_SUBSCRIBED_COUNT <= 255U,
              "proximity subscription list must fit the REGR count byte");

/*
 * Reviewed sensor set, loaded from sensor_pipelines_config.conf at startup.
 *
 * Written once in launchProximityControlAlgo before any thread or socket
 * exists, then read-only for the life of the process. That ordering is what
 * makes an unsynchronised read safe here: there is no reader until after the
 * write, and there is never a second write. Gateway auto-subscribes
 * SENSOR_INVALID/SENSOR_VALID; PXC uses pipelineID as the safety key.
 */
static ReviewedSensorSet gReviewedSensors{};
/* Bitmask of pipelineIds already reported as absent from the reviewed set, so
 * each one is logged exactly once. Touched only from the event path, which is
 * the single decision thread. */
static uint32_t          gUnreviewedSensorWarned = 0U;

/* PLC command socket */
static int plcSock = -1;
static struct sockaddr_in plcAddr = {};
/* CmdPacket.seq is uint16_t; use low 16 bits of this monotonic counter for a full 0..65535 wrap. */
static std::atomic<uint32_t> cmdSeqNo{0};
static std::mutex plcSocketMtx;

/* Optional second fan-out target: VST/VIOS's PfpHaloCommandListener. Same wire
 * format as the PLC packet (magic 0xA5, same CRC regions/opcodes), sent on a
 * dedicated socket so its unsolicited ACKs never reach ackHandlerLoop's
 * plcSock recvfrom(). Fire-and-forget: we never read from haloSock. */
static int haloSock = -1;
static struct sockaddr_in haloAddr = {};
static std::atomic<bool> haloEnabled{false};
static std::mutex haloSocketMtx;

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

/* PSD Gateway upstream endpoint: send REGR, receive DecisionRequests + HB.
 *
 * This is an NvPSFTransport client, not a raw UDP socket. The gateway offers
 * this link as AF_UNIX SOCK_SEQPACKET at NVPSF_SOCKET_PATH_PSD_GATEWAY and
 * validates the connecting peer's credentials, so a datagram socket sending to
 * an ip:port never registers: the gateway accepts no such client and this SDM
 * would sit in safe hold on decision freshness with no visible cause.
 *
 * gwPeer is the transport's identity for the gateway, not a descriptor. */
static NvPSFTransport* gwTransport = nullptr;
static NvPSFPeerId     gwPeer      = NVPSF_PEER_INVALID;
static std::mutex gwSockMtx;               // guards gwTransport/gwPeer + the pending HB ACK

/* Endpoint selection, captured at launch. */
static NvPSFTransportKind gwTransportKind = NVPSF_TRANSPORT_UNIX_SOCKET;
static std::string        gwSocketPath;
static std::string        gwInetAddr;
static std::uint16_t      gwInetPort = 0;

/* A heartbeat ACK that hit a full peer buffer, retried on the next listener
 * iteration. Dropping it instead would turn a momentarily full buffer into a
 * missed ACK, and enough of those trip the gateway's tier ladder against a
 * healthy SDM. */
static bool hbAckPending = false;
static char hbAckPendingBuf[NVPSD_GATEWAY_HB_MSG_SIZE];

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
/* Tier-2 gateway HB safe-hold: periodic/event send CMD_STOP+CMD_SW_ERROR while true; fusion keeps proximityActiveCommand. */
static std::atomic<bool>                     hbTier2SafeHoldActive{false};
/* PSS ERROR/SW_FAIL active flag. The safe-state latch clears only on PLC safe-release. */
static std::atomic<bool>                     pssErrorFusionSuppressLatched{false};
/*
 * PSS fault, split by cause.
 *
 * One bucket could not express "this evidence recovered" -- every PSS cause
 * cleared on the same predicate, so a recovered PSS delivery link and a real
 * SW_FAIL were indistinguishable and a quiet site could stay release-denied
 * indefinitely. Every sub-cause still raises the single kSafeLatchPss bit; what
 * differs is what CLEARS each one.
 *
 * MODE_ERROR     pssStatus.mode == ERROR (PSS delivery failure or SAIM heartbeat
 *                failure). Cleared by any request carrying mode != ERROR,
 *                INCLUDING a status/no-op.
 * GATEWAY_COMM   a SW_FAIL the gateway synthesised about its own PSS link.
 *                Cleared by the gateway's recovery record, or by ordinary
 *                qualifying traffic as a floor.
 * SW_FAIL        every other SW_FAIL, including one whose cause this build does
 *                not recognise. Cleared only by the historical predicate.
 * MONITOR_LOST / PERCEPTION_MONITOR_LOST / MDX_LOST
 *                that monitor, or a numbered MDX client instance, stopped being
 *                registered, however it happened.
 *                Cleared ONLY by PSS's presence report naming it; also raised by a
 *                report that no longer names one seen before.
 * INPUT_UNUSABLE PSS is rejecting a required source's reports (cause 13).
 *                Cleared ONLY when PSS's report shows that same source confirmed
 *                usable again (SdmSourceInputStateEvaluate); other sources'
 *                traffic clears nothing.
 */
enum PssSubCause
{
    kPssSubModeError = 0,
    kPssSubGatewayComm,
    kPssSubSwFail,
    kPssSubMonitorLost,   /* the Safety Monitor was lost */
    kPssSubPerceptionMonitorLost, /* the Perception Monitor was lost */
    kPssSubMdxLost,       /* a numbered MDX client instance was lost */
    kPssSubInputUnusable, /* a source's reports are being rejected */
    kPssSubCauseCount
};
static std::atomic<bool>                     pssSubCauseActive[kPssSubCauseCount]{};
static const char* const                     kPssSubCauseName[kPssSubCauseCount] = {
    "pss_mode_error", "gateway_comm", "sw_fail", "safety_monitor_lost",
    "perception_monitor_lost", "mdx_client_lost", "input_unusable"
};
/* The monitor-lost sub-causes are applied through SdmSourcePresenceEvaluate(),
 * shared by every SDM, which speaks SDM_PSS_SUB_* bits. */
static_assert((1U << kPssSubMonitorLost) == SDM_PSS_SUB_MONITOR_LOST,
              "kPssSubMonitorLost must match SDM_PSS_SUB_MONITOR_LOST");
static_assert((1U << kPssSubPerceptionMonitorLost) == SDM_PSS_SUB_PM_LOST,
              "kPssSubPerceptionMonitorLost must match SDM_PSS_SUB_PM_LOST");
static_assert((1U << kPssSubMdxLost) == SDM_PSS_SUB_MDX_LOST,
              "kPssSubMdxLost must match SDM_PSS_SUB_MDX_LOST");
/* Every SW_FAIL is classified by SdmSwFailSubCause(), which speaks the same bits. */
static_assert((1U << kPssSubModeError) == SDM_PSS_SUB_MODE_ERROR &&
              (1U << kPssSubGatewayComm) == SDM_PSS_SUB_GATEWAY_COMM &&
              (1U << kPssSubSwFail) == SDM_PSS_SUB_SW_FAIL &&
              (1U << kPssSubInputUnusable) == SDM_PSS_SUB_INPUT_UNUSABLE,
              "PssSubCause indices must match the SDM_PSS_SUB_* bits");

/*
 * Serializes a PLC release with the processing of a whole DecisionRequest.
 *
 * One request can clear some causes and raise others -- PSS leaving ERROR while
 * the same request carries a SW_FAIL, say. Each step takes its own lock, so a
 * release landing between the clear and the raise would see no active cause and
 * be accepted -- here with CMD_NORMAL -- in a state that never existed upstream.
 * Held by the gateway listener for each whole packet -- the sequence and
 * freshness clears that precede the request as much as the request itself --
 * and taken first by handleSafeReleaseRequest(), it makes the release see the
 * packet either not at all or completely, as if both were handled in one task.
 *
 * Lock order: faultTransitionMtx -> decisionSendMtx -> safeStateLatchMtx. No
 * other thread takes it.
 */
static std::mutex                            faultTransitionMtx;
static constexpr uint32_t                    kSafeLatchGatewayHb = 1U << 0;
static constexpr uint32_t                    kSafeLatchPss       = 1U << 1;
static constexpr uint32_t                    kSafeLatchDecisionFreshness = 1U << 2;
static constexpr uint32_t                    kSafeLatchGatewayDecisionSequence = 1U << 3;
static constexpr uint32_t                    kSafeLatchAllSafetyRelevantSensorsFailed = 1U << 4;

/* SAIM emits SENSOR_INVALID with CAUSE_SAIM_INIT as a pre-frame fail-safe seed on
 * first registration, so every sensor reports invalid during startup. Without
 * this suppression the all-safety-relevant-sensors-failed latch fires on every
 * boot and needs a validated manual release before the system can run.
 *
 * This SDM previously lacked the suppression that the ATL SDM already had, so
 * PXC latched at boot where ATL did not. Added here to match. */
#ifndef AVOID_SAFETY_LATCH_FOR_SAIM_INIT
#define AVOID_SAFETY_LATCH_FOR_SAIM_INIT 1
#endif
#if AVOID_SAFETY_LATCH_FOR_SAIM_INIT
static constexpr EventCause                  kSaimInitCause = CAUSE_SAIM_INIT;
/* PSS stamps ordinary perception events isHealthy=false while SAIM is
 * initializing, but those events retain their own cause, not SAIM_INIT.
 * Remember each explicit INIT seed until recovery so that inherited unhealthy
 * evidence cannot turn initialization into an all-sensors-failed latch. */
static std::atomic<uint32_t>                 saimInitPendingSensorMask{0U};
#endif
/* AI-pipeline distrust is a SAFE-STATE CAUSE, not merely an input filter.
 * PSS reports it pipeline-wide (one Perception Monitor verdict for the whole perception
 * pipeline), and while it holds, nothing arriving from any MDX client can be
 * trusted to describe the physical world. Continuing to drive the PLC from the
 * last decision would keep the machine running on evidence known to be bad, so
 * the SDM latches instead. Release is gated on the ACTIVE flag below, which
 * only AI_PIPELINE_VALID clears.
 *
 * Bit 5 here, bit 6 in ATLControl.cpp: the two masks are independent per-SDM
 * state and PXC has no count-drift cause, so the bit positions differ. They are
 * never compared across SDMs. */
static constexpr uint32_t                    kSafeLatchAiPipelineInvalid = 1U << 5;
static std::atomic<uint32_t>                 safeStateLatchCauses{0U};
static std::mutex                            safeStateLatchMtx;
static std::atomic<uint32_t>                 failedSafetyRelevantSensorMask{0U};
static std::atomic<bool>                     allSafetyRelevantSensorsFailedActive{false};
static std::atomic<bool>                     aiPipelineInvalidActive{false};
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

static bool pssAnySubCauseActive()
{
    for (int i = 0; i < kPssSubCauseCount; ++i)
    {
        if (pssSubCauseActive[i].load(std::memory_order_acquire))
            return true;
    }
    return false;
}

static void markPssSubCause(PssSubCause cause)
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    pssSubCauseActive[cause].store(true, std::memory_order_release);
    pssErrorFusionSuppressLatched.store(true, std::memory_order_release);
    safeStateLatchCauses.fetch_or(kSafeLatchPss, std::memory_order_acq_rel);
}

/* Clears an ACTIVE sub-cause. The latch is untouched: a cause going away is not
 * consent to resume, so a validated PLC safe-release is still required. Output
 * suppression lifts only once EVERY sub-cause is clear. */
static void clearPssSubCause(PssSubCause cause)
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    if (!pssSubCauseActive[cause].exchange(false, std::memory_order_acq_rel))
        return;

    if (!pssAnySubCauseActive())
        pssErrorFusionSuppressLatched.store(false, std::memory_order_release);

    pxc_log_info(std::string("Proximity: PSS sub-cause cleared: ") + kPssSubCauseName[cause] +
                 "; latch awaits PLC release");
}

static uint32_t safetyRelevantSensorBit(uint32_t pipelineId)
{
    if (pipelineId == 0U || pipelineId > kReviewedSensorMaxPipelineId)
        return 0U;

    for (size_t i = 0U; i < gReviewedSensors.entries.count; ++i)
    {
        if (pipelineId == static_cast<uint32_t>(gReviewedSensors.entries.entries[i].pipelineId))
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
    pxc_log_err("Proximity: All safety-relevant sensors failed; entering safe hold");
    return true;
}

static void clearAllSafetyRelevantSensorsFailedActiveIfRecovered(uint32_t failedMask)
{
    if ((failedMask & gReviewedSensors.mask) == gReviewedSensors.mask)
        return;

    if (allSafetyRelevantSensorsFailedActive.exchange(false, std::memory_order_acq_rel))
    {
        pxc_log_info("Proximity: All safety-relevant sensors failed condition cleared; latch awaits PLC release");
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
        {
            /* Not in the reviewed set, so this event cannot move the latch.
             *
             * Stated, not warned about. This is ROUTINE in 3D: the daemon
             * forwards each camera's own SENSOR_INVALID/SENSOR_VALID as well as
             * the synthesised group verdict, and in 3D only the group is
             * reviewed -- so a correctly configured deployment logs one line per
             * camera here. It is ALSO how a mis-scoped
             * sensor_pipelines_config.conf presents, with the group id
             * appearing instead of the cameras. The SDM does not read
             * nvpss.conf and so cannot tell those apart; it records which ids
             * are outside the reviewed set and leaves the reading to whoever
             * knows the deployment mode. Raising it to an error would assert a
             * fault that is not established.
             *
             * Once per id: the seen-mask bounds this at 31 lines for the life of
             * the process, so a per-DecisionRequest stream cannot bury the log. */
            if (shouldWarnUnreviewedSensor(gUnreviewedSensorWarned, pipelineId))
            {
                pxc_log_info("Proximity: pipelineID " + std::to_string(pipelineId) +
                    " is outside the reviewed sensor set " +
                    describeReviewedSensorSet(gReviewedSensors) +
                    "; its health does not affect the all-safety-relevant-sensors-failed latch");
            }
            continue;
        }

        bool updateMask = false;
        bool failed = false;
        if (et == SENSOR_INVALID || !sd.isHealthy)
        {
#if AVOID_SAFETY_LATCH_FOR_SAIM_INIT
            /* Initialization-in-progress, not a genuine failure: leave the
             * sensor's failed bit unchanged (pending) so it does not contribute
             * to the all-safety-relevant-sensors-failed latch.
             *
             * For explicit SENSOR_INVALID events, only this cause suppresses:
             * every other cause, including an unknown one, still sets the
             * failed bit even while initialization is pending. */
            if (sd.event.cause == kSaimInitCause)
            {
                if (et == SENSOR_INVALID)
                    saimInitPendingSensorMask.fetch_or(bit, std::memory_order_acq_rel);
                continue;
            }
            /* Only an explicit INIT seed authorizes suppressing propagated
             * unhealthy evidence for this pipeline. Genuine SENSOR_INVALID
             * events must still fail closed during that window. */
            if (et != SENSOR_INVALID &&
                (saimInitPendingSensorMask.load(std::memory_order_acquire) & bit) != 0U)
                continue;
#endif
            updateMask = true;
            failed = true;
        }
        else if (et == SENSOR_VALID || (sd.isHealthy && sd.isTrustedSource))
        {
#if AVOID_SAFETY_LATCH_FOR_SAIM_INIT
            saimInitPendingSensorMask.fetch_and(~bit, std::memory_order_acq_rel);
#endif
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
            pxc_log_info("Proximity: safety-sensor health change pipelineID=" +
                std::to_string(pipelineId) +
                (failed ? " state=FAILED" : " state=RECOVERED") +
                " cause=" + std::to_string(static_cast<unsigned>(sd.event.cause)));
        }

        if ((newMask & gReviewedSensors.mask) == gReviewedSensors.mask)
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
    for (int i = 0; i < kPssSubCauseCount; ++i)
        pssSubCauseActive[i].store(false, std::memory_order_release);
    decisionFreshnessFaultActive.store(false, std::memory_order_release);
    gatewayDecisionSequenceFaultActive.store(false, std::memory_order_release);
    failedSafetyRelevantSensorMask.store(0U, std::memory_order_release);
#if AVOID_SAFETY_LATCH_FOR_SAIM_INIT
    saimInitPendingSensorMask.store(0U, std::memory_order_release);
#endif
    allSafetyRelevantSensorsFailedActive.store(false, std::memory_order_release);
    aiPipelineInvalidActive.store(false, std::memory_order_release);
    sdmGatewayDecisionSequenceReset(&gatewayDecisionSequenceState);
    safeStateLatchCauses.store(0U, std::memory_order_release);
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

/* Pair objects arrive through a 2-slot EventFusionMetadata, and the layout is
 * chosen by whether a Person is present rather than by pair group -- which is
 * what lets one routine serve every group. Every tracked pair
 * (Humanoid <-> Person, Forklift <-> Person, Transporter <-> Person) has one
 * PERSON-typed slot and one that is not: keep the Person in the "surrounding"
 * slot (index 1) and the machine in the "center" slot (index 0), matching the
 * upstream proximityDetectionCenterClasses/SurroundingClasses roles.
 *
 * The no-Person branch is a defensive fallback, not a supported group. It is
 * unreachable for the groups above, but a mapping or perception fault could
 * still deliver a pair with neither slot PERSON-typed; copying both slots
 * through in original order keeps the PLC packet describing the real objects
 * rather than reporting zeros. */
static void fillPairObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS],
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

    if (firstPerson >= 0) {
        /* Forklift <-> Person: non-person (Forklift) -> slot 0, Person -> slot 1. */
        if (firstNonPerson >= 0)
            copyObjectRecordFromMetadata(&objects[0], meta, firstNonPerson);
        copyObjectRecordFromMetadata(&objects[1], meta, firstPerson);
    } else {
        /* No Person in either slot: pass both metadata slots through unchanged. */
        copyObjectRecordFromMetadata(&objects[0], meta, 0);
        copyObjectRecordFromMetadata(&objects[1], meta, 1);
    }
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

    fillPairObjectRecords(objects, meta);
}

bool sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request,
                         int winningSlot)
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
        pxc_log_info(logMsg.str());
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
                pxc_log_err("Failed to send decision command");
            }
        }
    }

    /* Best-effort fan-out to VST's halo listener; does not affect sentOk/ACK
     * tracking (that remains solely tied to the PLC send above). */
    if (haloEnabled.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> haloLock(haloSocketMtx);
        if (haloSock >= 0) {
            const ssize_t haloSent = sendto(haloSock, &pkt, sizeof(pkt),
                                            MSG_DONTWAIT,
                                            (struct sockaddr*)&haloAddr, sizeof(haloAddr));
            if (haloSent != static_cast<ssize_t>(sizeof(pkt))) {
                /* Rate-limited: a halo listener that is down fails on every
                 * decision, and at the repeat cadence that would fill the log
                 * with one line per command for as long as it stays down.
                 * The suppressed count is carried into the next line so the
                 * throttling never hides the scale of the loss.
                 *
                 * Backpressure is reported separately from a real error:
                 * EAGAIN on a non-blocking datagram socket means the buffer
                 * was momentarily full, which is the expected way to shed an
                 * overlay update, not a fault worth investigating. */
                const bool wouldBlock =
                    haloSent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
                static std::chrono::steady_clock::time_point lastHaloWarn{};
                static unsigned long haloSuppressed = 0UL;
                const auto nowWarn = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        nowWarn - lastHaloWarn).count() >= HALO_WARN_MIN_INTERVAL_MS) {
                    lastHaloWarn = nowWarn;
                    pxc_log_warning(
                        std::string(wouldBlock
                            ? "VST halo listener backpressure; dropped decision overlay"
                            : "Failed to send decision command to VST halo listener") +
                        (haloSuppressed != 0UL
                            ? " (suppressed " + std::to_string(haloSuppressed) + " similar)"
                            : std::string()));
                    haloSuppressed = 0UL;
                } else {
                    ++haloSuppressed;
                }
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

static void resetProximityStateFromPlcSafeRelease()
{
    std::lock_guard<std::mutex> stateLock(proximityStateMtx);
    proximityActiveCommand = CMD_NORMAL;
}

/* Latch on AI_PIPELINE_INVALID. Returns true on the entry edge only, so the
 * caller can emit the safe hold once rather than per repeated report. */
static bool markAiPipelineInvalidLatched()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    const bool firstEdge = !aiPipelineInvalidActive.exchange(true, std::memory_order_acq_rel);
    safeStateLatchCauses.fetch_or(kSafeLatchAiPipelineInvalid, std::memory_order_acq_rel);
    if (firstEdge)
    {
        pxc_log_err("Proximity: AI pipeline reported INVALID; entering safe hold");
    }
    return firstEdge;
}

/* AI_PIPELINE_VALID clears the ACTIVE condition only. The latch itself survives
 * until a PLC safe-release is accepted -- same contract as all-sensors-failed:
 * the cause going away is not consent to resume. */
static void clearAiPipelineInvalidActive()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    if (aiPipelineInvalidActive.exchange(false, std::memory_order_acq_rel))
    {
        pxc_log_info("Proximity: AI pipeline reported VALID; condition cleared, latch awaits PLC release");
    }
}

static uint32_t activeSafeLatchCauses()
{
    uint32_t active = 0U;
    if (hbFaultLatched.load(std::memory_order_acquire) ||
        hbTier2SafeHoldActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchGatewayHb;
    }
    if (pssAnySubCauseActive())
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
    /* Gates handleSafeReleaseRequest(): a release arriving while the AI pipeline
     * is still INVALID is denied, so release is accepted only after
     * AI_PIPELINE_VALID has been received. */
    if (aiPipelineInvalidActive.load(std::memory_order_acquire))
    {
        active |= kSafeLatchAiPipelineInvalid;
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

static void sendProximitySafeHold(const DecisionRequest* request = nullptr,
                            int winningSlot = -1)
{
    sendDecisionCommand(CMD_STOP, true, request, winningSlot);
    sendDecisionCommand(CMD_SW_ERROR, true, request, winningSlot);
}

static void markDecisionRequestFresh(uint32_t requestId)
{
    {
        std::lock_guard<std::mutex> lock(decisionFreshnessMtx);
        lastValidDecisionRequestTime = std::chrono::steady_clock::now();
    }

    if (decisionFreshnessFaultActive.exchange(false, std::memory_order_acq_rel))
    {
        pxc_log_info("Proximity: DecisionRequest freshness recovered; requestId=" +
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
        pxc_log_info("Proximity: Gateway DecisionRequest sequence recovered; epoch=" +
            std::to_string(gatewayEpoch) + " txSeq=" +
            std::to_string(gatewayTxSeq) + "; latch awaits PLC release");
    }
}

static void sendLatchedProximitySafeHold()
{
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    sendProximitySafeHold();
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
            pxc_log_err("Proximity: DecisionRequest freshness timeout; entering safe hold, elapsed_ms=" +
                std::to_string(elapsedMs));
            sendLatchedProximitySafeHold();
        }
    }
}

static void handleSafeReleaseRequest()
{
    std::lock_guard<std::mutex> transitionLock(faultTransitionMtx);
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);

    const uint32_t active = activeSafeLatchCauses();
    if (active != 0U)
    {
        pxc_log_warning("Proximity: PLC safe-release denied; active safe-state fault source remains, active_causes=" +
            std::to_string(active));
        sendDecisionCommand(CMD_SAFE_RELEASE_DENIED, false, nullptr);
        sendProximitySafeHold();
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
                resetProximityStateFromPlcSafeRelease();
                hbTier2SafeHoldActive.store(false, std::memory_order_release);
                pxc_log_info("Proximity: PLC safe-release accepted; returning to normal mode");
                markSafeReleaseNoopResponseSuccess(now);
            } else {
                pxc_log_warning("Proximity: safe-release response sent but latch retained due to concurrent fault");
                sendProximitySafeHold();
            }
        } else {
            pxc_log_err("Proximity: safe-release response TX failed; latch retained and state not reset");
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool rateLimited = safeReleaseNoopResponseInRateWindow(now);
    if (rateLimited)
        pxc_log_warning("Proximity: rate-limited PLC safe-release retry; re-sending current safe-release response");
    else
        pxc_log_info("Proximity: PLC safe-release received while no safe-state latch is active");

    const bool ackSent = sendDecisionCommand(CMD_SAFE_RELEASE_ACK, false, nullptr);
    const bool normalSent = sendDecisionCommand(CMD_NORMAL, true, nullptr);
    if (ackSent && normalSent) {
        resetProximityStateFromPlcSafeRelease();
        markSafeReleaseNoopResponseSuccess(now);
    } else {
        pxc_log_err("Proximity: safe-release no-latch response TX failed; state not reset");
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
                    pxc_log_warning("SDM: dropping PLC packet from unexpected endpoint");
                    continue;
                }
                if (ackPkt->identifier != PROXIMITY_PACKET_IDENTIFIER) {
                    /* Both values are formatted from the constant rather than
                     * spelled out, so the message cannot drift from the wire the
                     * way a literal did when the identifier last changed. */
                    char hexId[8];
                    char hexExpected[8];
                    snprintf(hexId, sizeof(hexId), "0x%02X", ackPkt->identifier);
                    snprintf(hexExpected, sizeof(hexExpected), "0x%02X",
                             static_cast<unsigned>(PROXIMITY_PACKET_IDENTIFIER));
                    pxc_log_warning(std::string("ACK: invalid identifier ") + hexId +
                                    " (expected " + hexExpected + ") — dropped");
                } else if (!cmdPacketValidateCRC(ackPkt)) {
                    pxc_log_warning("ACK: CRC-32 mismatch (seq=" +
                                    std::to_string(ackPkt->seq) + ") — dropped");
                } else if (!isValidPlcInboundCommand(ackPkt->command)) {
                    pxc_log_warning("SDM: dropping PLC packet with unsupported command");
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
                        pxc_log_warning(ackLog);
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
                        pxc_log_info(ackMsg.str());
                    }
                    else
                    {
                        ackLog = "SDM: received valid ACK with no pending command (SeqNo: " +
                                 std::to_string(seqNo) + ", command: " +
                                 std::string(commandName(ackPkt->command)) + ")";
                        pxc_log_warning(ackLog);
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
                pxc_log_info("HB-PSD: gateway HB recovered; safe-state latch awaits PLC release");
        }

        if (m <= warnW)
        {
            const bool hadTier2 = hbTier2SafeHoldActive.exchange(false, std::memory_order_acq_rel);
            if (hadTier2)
            {
                pxc_log_info("HB-PSD: tier-2 safe-hold cleared");
                if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                    sendProximitySafeHold();
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
                sendProximitySafeHold();
            }
        }

        prevMiss = m;
        g_lastGatewayHbMissCount.store(m);

        if (m >= maxF)
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (markGatewayHbFaultLatched())
            {
                pxc_log_err(
                    "HB-PSD: gateway HB fault latched (tier 3) — local fail-safe, no PSS connection");
                sendProximitySafeHold();
            }
        }
    }
}

/* Ranks a command by how restrictive it is, so a batch carrying several pairs
 * resolves to its worst case rather than to whichever pair happened to sit last
 * in the array. */
static int proximityCommandSeverityRank(unsigned char command)
{
    switch (command)
    {
        case CMD_STOP:       return 2;
        case CMD_REDUCE:     return 1;
        default:             return 0;   /* CMD_NORMAL */
    }
}

/* Worst case wins: the most severe proximity-relevant event in sensorDataSummary[]
 * decides, across every pair group in PROXIMITY_EVENT_TABLE
 * (Humanoid<->Person, Forklift<->Person, Transporter<->Person).
 *
 *   CRITICAL tier                  -> CMD_STOP
 *   WARNING tier                   -> CMD_REDUCE
 *   NO_VIOLATION tier              -> CMD_NORMAL
 *
 * Tiers are compared, not distances: mdx-client has already reduced each pair to
 * a tier against its group's own thresholds, so a critical from any group ranks
 * equally here and this path never sees a distance. All three groups happen to
 * share 2m/1m bands today, but nothing here depends on that.
 *
 * A batch routinely carries several pairs from both cameras, so the command is
 * ranked by severity rather than taken from the last matching entry: otherwise a
 * stop could be masked by a lower-severity pair that happened to sit later in
 * the array.
 *
 * Returns false if no proximity event types in batch.
 * *outWinningSlot receives the index of the SensorData entry that set the command. */
/*
 * AI-PIPELINE TRUST, CONSUMED BEFORE THE BATCH COMPUTE.
 *
 * AI_PIPELINE_INVALID arrives ALREADY STAMPED UNTRUSTED: PSS sets the
 * pipeline-wide flag in OnTrustReport before routing, so QueryTrustState already
 * returns false by the time it reaches us. A handler placed after the
 * !isTrustedSource skip would never run.
 *
 * WHY THE LATCH EDGE IS RETURNED RATHER THAN DISCARDED. This used to
 * live inside computeProximityCommandFromBatch() and throw the edge away, on the
 * reasoning that "the decision path emits STOP+SW_ERROR on any latched cause, so
 * setting the bit is sufficient".
 *
 * That reasoning is false for THIS cause, and only for this one. AI-pipeline
 * distrust is precisely the condition that stops the decision path being
 * reached: every subsequent proximity event arrives isTrustedSource=false and is
 * skipped, and a status/no-op returns early by design. So nothing reached the
 * emit, and the PLC went on holding its last command -- NORMAL, in the run that
 * found this -- while the SDM knew the perception pipeline was untrustworthy.
 * Measured at 26.7 s before any safe hold reached the PLC, against the same
 * millisecond for the ATL and combined SDMs.
 *
 * Keeping the machine running on evidence the system has declared bad is the
 * exact hazard this latch exists to prevent, so the edge is now carried out to
 * the caller, which emits the safe hold immediately. Matches ATL and the
 * combined ATL+Proximity SDM.
 *
 * Returns true on the latch entry edge only, so the caller emits once rather
 * than per repeated report.
 */
static bool updateAiPipelineTrustFromDecisionRequest(const DecisionRequest* request)
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
        if (et == AI_PIPELINE_INVALID)
        {
            latchEntered = markAiPipelineInvalidLatched() || latchEntered;
        }
        else if (et == AI_PIPELINE_VALID)
        {
            clearAiPipelineInvalidActive();
        }
    }

    return latchEntered;
}

static bool computeProximityCommandFromBatch(const DecisionRequest* request,
                                             unsigned char* outCmd,
                                             int* outWinningSlot)
{
    if (!request || !outCmd)
        return false;

    bool            found    = false;
    unsigned char   cmd      = CMD_NORMAL;
    int             slot     = -1;
    int             bestRank = -1;

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

        const EventType et = static_cast<EventType>(sd.event.type);
        if (et == SENSOR_INVALID || et == SENSOR_VALID)
        {
            pxc_log_info("Proximity: Sensor health event consumed by health monitor only");
            continue;
        }

        if (et == AI_PIPELINE_INVALID || et == AI_PIPELINE_VALID)
        {
            /* Already consumed by updateAiPipelineTrustFromDecisionRequest()
             * before this function was called. Skipped explicitly rather than
             * left to fall through to the !isTrustedSource check below, which
             * would log it as an ordinary untrusted event and say nothing about
             * the verdict it carries. */
            continue;
        }

        if (!sd.isHealthy)
        {
            pxc_log_info("Sensor unhealthy -- logging only, not processing for decision");
            continue;
        }

        if (!sd.isTrustedSource)
        {
            pxc_log_info("AI pipeline untrusted -- logging only, not processing for decision");
            continue;
        }

        /* Anything without a table entry contributes no command. */
        const ProximityEventDescriptor* desc =
            proximityEventDescriptor(static_cast<int>(sd.event.type));
        if (desc == nullptr)
            continue;

        unsigned char candidate = CMD_NORMAL;

        switch (desc->tier)
        {
            case PROXIMITY_TIER_CRITICAL:
                /* Unconditional across every group: nothing here can attribute a
                 * pair to a machine's own operator, so no critical is withheld. */
                candidate = CMD_STOP;
                break;

            case PROXIMITY_TIER_WARNING:
                candidate = CMD_REDUCE;
                break;

            case PROXIMITY_TIER_NO_VIOLATION:
            default:
                candidate = CMD_NORMAL;
                break;
        }

        const int rank = proximityCommandSeverityRank(candidate);
        if (rank > bestRank)
        {
            bestRank = rank;
            cmd      = candidate;
            slot     = static_cast<int>(i);
            found    = true;
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

        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (safeStateLatched())
            {
                sendProximitySafeHold();
            }
            else if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
            {
                sendProximitySafeHold();
            }
            else if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
            {
                sendProximitySafeHold();
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

/* Sources this SDM has EVER known of (PSS_PRESENCE_*), from a presence report or
 * a source-lost SW_FAIL. Never cleared: it is what stops a PSS restart forgetting
 * a lost source. Decision-handler thread only. */
static uint32_t sourcePresenceSeen = 0U;

/*
 * Apply the source-lost rules, by the same pure function as every other
 * SDM: every source-lost SW_FAIL in the request raises its sub-cause -- wherever
 * it sits and whatever the mode, which the SW_FAIL loop below cannot promise --
 * and PSS's presence report clears a source it names and raises one this SDM
 * knows of that it omits. What the SDM knows of includes the report's expected
 * set, so a restarted SDM relearns a source that is down.
 *
 * The hold is sent HERE on a raise. The report rides a no-op, and the no-op path
 * returns before anything else would send one. Called before the PSS-ERROR
 * return too, so a raise is never discarded; the evaluator itself withholds
 * clears while mode == ERROR.
 */
static void applySourcePresence(const DecisionRequest* request)
{
    static constexpr PssSubCause kTracked[] = {kPssSubMonitorLost, kPssSubPerceptionMonitorLost,
                                               kPssSubMdxLost};
    const SdmPssSubCauseDelta d = SdmSourcePresenceEvaluate(&sourcePresenceSeen, request);
    uint32_t entered = 0U;
    for (const PssSubCause cause : kTracked)
    {
        const uint32_t bit = 1U << cause;
        if ((d.cleared & bit) != 0U)
            clearPssSubCause(cause);
        if ((d.raised & bit) != 0U && !pssSubCauseActive[cause].load(std::memory_order_acquire))
        {
            pxc_log_err(std::string("Proximity: source reported lost, or missing from the presence report -- "
                        "triggering safe hold; sub-cause=") + kPssSubCauseName[cause] +
                        " (clears only when it is back)");
            entered |= bit;
        }
    }
    if (entered == 0U)
        return;

    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    for (const PssSubCause cause : kTracked)
    {
        if ((entered & (1U << cause)) != 0U)
            markPssSubCause(cause);
    }
    {
        std::lock_guard<std::mutex> stateLock(proximityStateMtx);
        proximityActiveCommand = CMD_STOP;
    }
    sendProximitySafeHold(request);
}

/* Identities (PSS_INPUT_STATE_MASK layout) whose rejected input this SDM holds
 * the input-unusable cause for. Process lifetime, like sourcePresenceSeen. */
static uint32_t sourceInputUnusableSeen = 0U;

/*
 * Apply PSS's input-state report and every cause-13 SW_FAIL: a source whose
 * reports PSS keeps rejecting. The cause clears only when PSS reports that same
 * source confirmed usable again -- never on other sources' traffic. Called
 * before the PSS-ERROR return, like the presence report; the evaluator itself
 * withholds clears while mode == ERROR.
 */
static void applySourceInputState(const DecisionRequest* request)
{
    const SdmPssSubCauseDelta d =
        SdmSourceInputStateEvaluate(&sourceInputUnusableSeen, request);
    if ((d.cleared & SDM_PSS_SUB_INPUT_UNUSABLE) != 0U)
        clearPssSubCause(kPssSubInputUnusable);
    if ((d.raised & SDM_PSS_SUB_INPUT_UNUSABLE) == 0U ||
        pssSubCauseActive[kPssSubInputUnusable].load(std::memory_order_acquire))
        return;

    pxc_log_err(std::string("Proximity: a source's reports are being rejected -- triggering safe hold; "
                "sub-cause=input_unusable (clears only when PSS confirms that source usable again)"));
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    markPssSubCause(kPssSubInputUnusable);
    {
        std::lock_guard<std::mutex> stateLock(proximityStateMtx);
        proximityActiveCommand = CMD_STOP;
    }
    sendProximitySafeHold(request);
}

static const char* swFailSubCauseNote(uint8_t bit)
{
    switch (bit)
    {
        case SDM_PSS_SUB_GATEWAY_COMM:   return " (gateway communication)";
        case SDM_PSS_SUB_MONITOR_LOST:   return " (Safety Monitor lost -- clears only when it is back)";
        case SDM_PSS_SUB_PM_LOST:        return " (Perception Monitor lost -- clears only when it is back)";
        case SDM_PSS_SUB_MDX_LOST:       return " (MDX client instance lost -- clears only when it is back)";
        case SDM_PSS_SUB_INPUT_UNUSABLE: return " (a source's reports are being rejected -- clears only when "
                                                "PSS confirms that source usable again)";
        default:                         return "";
    }
}

/*
 * Latch on EVERY SW_FAIL in the request, each in its own sub-cause
 * (SdmSwFailSubCause), and send the hold once. Called on both sides of the
 * PSS-ERROR return: a request can carry several faults, and one that rides an
 * ERROR-mode request is still a fault. Stopping at the first, or skipping them in
 * ERROR, left the others unlatched, so restoring the first permitted a release
 * while they persisted. Returns whether any SW_FAIL was present.
 */
static bool applySwFailEvents(const DecisionRequest* request, uint8_t maxSrc)
{
    uint32_t raised = 0U;
    int firstIndex = -1;
    for (uint8_t i = 0; i < maxSrc; ++i)
    {
        const SensorData& swFailSd = request->sensorDataSummary[i];
        if (static_cast<EventType>(swFailSd.event.type) != SW_FAIL)
            continue;
        const uint8_t bit = SdmSwFailSubCause(&swFailSd.event);
        pxc_log_err(std::string("Proximity: SW_FAIL received — triggering safe hold; cause=") +
                std::to_string(static_cast<unsigned>(swFailSd.event.cause)) +
                swFailSubCauseNote(bit));
        raised |= bit;
        if (firstIndex < 0)
            firstIndex = static_cast<int>(i);
    }
    if (raised == 0U)
        return false;

    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    for (int c = 0; c < kPssSubCauseCount; ++c)
    {
        if ((raised & (1U << c)) != 0U)
            markPssSubCause(static_cast<PssSubCause>(c));
    }
    {
        std::lock_guard<std::mutex> stateLock(proximityStateMtx);
        proximityActiveCommand = CMD_STOP;
    }
    sendProximitySafeHold(request, firstIndex);
    return true;
}

/* ====================== DECISION LOGIC ========================== */
/* The request itself. The caller holds faultTransitionMtx: the gateway listener
 * for the whole packet, or the public entry point below. */
static void processDecisionRequestLocked(const DecisionRequest* request)
{
    if (!request)
    {
        pxc_log_err("SDM: onEventNotificationReceive called with null DecisionRequest");
        return;
    }

    pxc_log_info(
        "SDM: processing DecisionRequest id=" +
        std::to_string(request->requestId));

#ifdef NVPSF_DBG
    /*
     * End-to-end latency audit.
     *
     * SafetyEvent.timestamp is the PRODUCER's timestamp: mdx-client parses the
     * MDX alert's endTimestamp and runs it through a one-shot
     * REALTIME->MONOTONIC calibration (stringToNS in SafetyEventReporter.cpp),
     * so it lands in this host's CLOCK_MONOTONIC epoch. VSS runs in containers
     * on this same host, so there is no cross-host clock skew to correct for,
     * and subtracting it from a CLOCK_MONOTONIC reading here yields the real
     * age of the event at the moment the SDM received it: Kafka + mdx-client +
     * the PSS input queue + fusion + the PSD staging queue and its release
     * gate + the gateway transport.
     *
     * clock_gettime(CLOCK_MONOTONIC) rather than steady_clock::now() so the
     * epoch provably matches the producer's, instead of relying on the
     * implementation happening to map steady_clock onto CLOCK_MONOTONIC.
     *
     * Resolution is bounded by the producer: endTimestamp is parsed with
     * millisecond precision, so each age carries up to 1 ms of quantisation.
     * Debug-only -- this measures a property the SDM does not act on.
     */
    struct timespec nowTs = {};
    uint64_t nowMonoNs = 0U;
    if (clock_gettime(CLOCK_MONOTONIC, &nowTs) == 0)
    {
        nowMonoNs = static_cast<uint64_t>(nowTs.tv_sec) * 1000000000ULL +
                    static_cast<uint64_t>(nowTs.tv_nsec);
    }
    if (nowMonoNs != 0U)
    {
        const uint8_t latN =
            std::min(request->sensorDataSummarySize,
                     static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));
        uint64_t minAge = UINT64_MAX;
        uint64_t maxAge = 0U;
        uint64_t sumAge = 0U;
        unsigned counted = 0U;
        for (uint8_t i = 0; i < latN; ++i)
        {
            const uint64_t ets = request->sensorDataSummary[i].event.timestamp;
            /* A zero stamp means stringToNS() could not parse the producer's
             * value; a stamp in the future means the two clocks disagree.
             * Either way it is not a latency sample and must not skew the
             * distribution, so it is excluded rather than clamped to 0. */
            if (ets == 0U || ets > nowMonoNs)
                continue;
            const uint64_t age = nowMonoNs - ets;
            if (age < minAge) minAge = age;
            if (age > maxAge) maxAge = age;
            sumAge += age;
            ++counted;
        }
        if (counted > 0U)
        {
            pxc_log_info("PSD_E2E_LATENCY id=" + std::to_string(request->requestId) +
                         " n=" + std::to_string(counted) +
                         " minUs=" + std::to_string(minAge / 1000ULL) +
                         " maxUs=" + std::to_string(maxAge / 1000ULL) +
                         " meanUs=" + std::to_string((sumAge / counted) / 1000ULL));
        }
    }

    /*
     * SDM-side processing cost: receipt of the batch through to the decision
     * command being handed to the socket. A scope guard rather than a call at
     * the end, because this function returns from several places (null
     * request, PSS ERROR mode, NOOP-only batch) and a missed path would
     * silently under-report.
     */
    struct DecideTimer
    {
        uint64_t startNs;
        uint32_t reqId;
        ~DecideTimer()
        {
            struct timespec endTs = {};
            if (startNs == 0U || clock_gettime(CLOCK_MONOTONIC, &endTs) != 0)
                return;
            const uint64_t endNs =
                static_cast<uint64_t>(endTs.tv_sec) * 1000000000ULL +
                static_cast<uint64_t>(endTs.tv_nsec);
            if (endNs < startNs)
                return;
            pxc_log_info("PSD_DECIDE_US id=" + std::to_string(reqId) +
                         " us=" + std::to_string((endNs - startNs) / 1000ULL));
        }
    } decideTimer{nowMonoNs, request->requestId};

    /*
     * Delivery-order audit for the PSS -> PSD operational staging queue.
     *
     * The queue orders by SafetyEvent.timestamp with the enqueue sequence as
     * tie-breaker, so a batch must arrive non-decreasing in timestamp. This
     * checks that from the consumer's side rather than trusting the producer:
     * psf.log cannot settle it, because the daemon's per-event timestamp lines
     * and its NvPSBWriteData lines are written through two different paths to
     * the same file and interleave.
     *
     * Debug-only. It is a measurement of a guarantee the SDM does not itself
     * depend on -- the arbitration is worst-case severity over the whole batch
     * and so is order-independent -- so it must not cost anything in release.
     */
    {
        const uint8_t auditN =
            std::min(request->sensorDataSummarySize,
                     static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));
        std::string ts;
        unsigned inversions = 0U;
        uint64_t prev = 0U;
        for (uint8_t i = 0; i < auditN; ++i)
        {
            const uint64_t cur = request->sensorDataSummary[i].event.timestamp;
            if (i > 0U)
            {
                ts += ",";
                if (cur < prev)
                    ++inversions;
            }
            ts += std::to_string(cur);
            prev = cur;
        }
        pxc_log_info("PSD_BATCH_ORDER id=" + std::to_string(request->requestId) +
                     " n=" + std::to_string(static_cast<unsigned>(auditN)) +
                     " inversions=" + std::to_string(inversions) +
                     " ts=[" + ts + "]");
    }
#endif

    /*
     * AI-PIPELINE VERDICTS FIRST -- before the PSS-ERROR return and the SW_FAIL
     * return below.
     *
     * The AI monitor reports its verdict ONCE, on change. Either early return
     * used to discard a verdict riding the same request, and nothing would ever
     * re-send it: after PSS recovered and the PLC released, this SDM ran with the
     * pipeline untrusted, skipping its events and holding its last command. The
     * hold is sent here on the latch edge and processing CONTINUES -- the rest of
     * the request (ERROR, SW_FAIL, sensor health) still applies, and may send the
     * hold again, which is harmless.
     */
    if (updateAiPipelineTrustFromDecisionRequest(request))
    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            std::lock_guard<std::mutex> stateLock(proximityStateMtx);
            proximityActiveCommand = CMD_STOP;
        }
        sendProximitySafeHold(request);
    }

    /* PSS's presence and input-state reports, before the PSS-ERROR return for
     * the same reason. */
    applySourcePresence(request);
    applySourceInputState(request);

    /* The sensor verdicts too, before both returns below -- the PSS-ERROR return
     * and the SW_FAIL return. A SENSOR_INVALID riding either kind of request used
     * to be discarded; after PSS recovered, a PLC release was then accepted with
     * every safety-relevant sensor still failed, until PSS next re-asserted the
     * verdict. Raise and clear both apply in every mode: a clear releases
     * nothing, the latch still waits for a PLC release. A latch entered here gets
     * its hold from whichever path the request takes below. */
    const bool sensorLatchEntered = updateSensorHealthFromDecisionRequest(request);

    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    /* --- PSS ERROR mode ------------------------------------------------ */
    if (request->pssStatus.mode == ERROR)
    {
        pxc_log_warning(
            "PSS is in error mode, sending stop and software error commands");
        /* The request's SW_FAILs are faults in their own right; mode_error
         * clearing later must not take them with it. */
        (void)applySwFailEvents(request, maxSrc);
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            markPssSubCause(kPssSubModeError);
            {
                /* Publish the safe-state command so the periodic re-assert
                 * thread continues to hold the PLC in STOP after the ERROR
                 * latch is cleared until the next healthy event arrives. */
                std::lock_guard<std::mutex> stateLock(proximityStateMtx);
                proximityActiveCommand = CMD_STOP;
            }
            sendProximitySafeHold(request);
        }
        return;
    }

    /* --- Normal / Degraded mode ---------------------------------------- */

    /*
     * mode != ERROR from here, which is AFFIRMATIVE evidence that neither PSS
     * delivery nor the SAIM heartbeat is failed -- PSS recomputes the field on
     * every send from exactly those two conditions.
     *
     * Applied BEFORE the status/no-op early return below, deliberately. A no-op
     * carries this field just as a physical request does, and on a quiet site it
     * may be the only thing that ever arrives; refusing to act on it is what
     * left a recovered system release-denied indefinitely.
     */
    if (SdmDecisionRequestClearsPssModeError(request))
    {
        clearPssSubCause(kPssSubModeError);
    }

    if (maxSrc == 1U &&
        static_cast<EventType>(request->sensorDataSummary[0].event.type) == PSS_STATUS_NOOP)
    {
        /* The gateway's all-client recovery record rides a no-op. It is the only
         * evidence that reaches an SDM receiving no ordinary event traffic, which
         * is the asymmetry it exists to remove. */
        if (SdmDecisionRequestIsGatewayCommRecovered(request))
        {
            clearPssSubCause(kPssSubGatewayComm);
        }
        pxc_log_info("Proximity: PSS_STATUS_NOOP received; freshness refreshed without proximity-state update");
        return;
    }

    if (SdmDecisionRequestClearsPssSwFail(request))
    {
        clearPssSubCause(kPssSubSwFail);
        /*
         * The gateway-comm sub-cause clears here TOO, not only on the recovery
         * record. Without this floor, a gateway restart -- which forgets it owed
         * a recovery -- or a single dropped recovery record would leave this SDM
         * release-denied for the life of the process, which is worse than the
         * behaviour this change replaced.
         */
        clearPssSubCause(kPssSubGatewayComm);
    }

    if (applySwFailEvents(request, maxSrc))
        return;

    if (sensorLatchEntered)
    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            std::lock_guard<std::mutex> stateLock(proximityStateMtx);
            proximityActiveCommand = CMD_STOP;
        }
        sendProximitySafeHold(request);
        return;
    }

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
            sendProximitySafeHold(request, winningSlot);
        }
        else if (safeStateLatched())
        {
            sendProximitySafeHold(request, winningSlot);
        }
        else
        {
            sendDecisionCommand(desired, true, request, winningSlot);
        }
    }
}

void onEventNotificationReceive(const DecisionRequest* request)
{
    /* The whole request is one fault-state transition as far as a PLC release
     * can tell; see faultTransitionMtx. */
    std::lock_guard<std::mutex> transitionLock(faultTransitionMtx);
    processDecisionRequestLocked(request);
}

/* If false, one non-blocking send only (listener path); if true, retry for launch. */
static bool sendGatewayRegistration(bool retryUntilWritable);

/* ================= PSD Gateway upstream endpoint =====================
 *
 * OWNERSHIP: the transport handle is opened, polled, received from,
 * reconnected and closed by ONE thread -- the main thread, which runs
 * psdGatewayEventListener() inline and only calls
 * shutdownProximityControlAlgo() after that returns. Every
 * upstream send also originates there (flushPendingHbAck, emitHbAckTryOnce and
 * the periodic sendGatewayRegistration are all called from the listener loop;
 * the launch-time REGR runs before the listener starts). So there is exactly
 * one reader and one writer, and they are the same thread.
 *
 * gwSockMtx is retained rather than removed because
 * shutdownProximityControlAlgo() is a public entry point a future
 * caller could invoke from elsewhere, and because it still guards
 * hbAckPending/hbAckPendingBuf. It is NEVER held across NvPSFTransportPoll:
 * Poll blocks for its full timeout, and holding it there would stall every
 * send for that period.
 */

/** Open the upstream endpoint. Leaves gwTransport null on failure. */
static bool openGatewayTransport()
{
    NvPSFTransportCfg cfg = {};
    cfg.kind            = gwTransportKind;
    cfg.role            = NVPSF_ROLE_CLIENT;
    cfg.link            = NVPSF_LINK_GATEWAY_TO_SDM;
    cfg.maxPeers        = 1U;    /* forced to 1 for a client regardless */
    cfg.maxMessageBytes = sizeof(NvPSDGatewayDecisionRequestPacket);
    /* The measured floor for this link; a shortfall is a fatal startup error. */
    cfg.minQueueDepth   = NvPSFGatewaySdmMinQueueDepth();
    cfg.recvTimeoutMs   = MAIN_LOOP_POLL_TIMEOUT_MS;
    cfg.sendTimeoutMs   = 0U;
    cfg.unixPath        = gwSocketPath.c_str();
    /* Ignored for a client: the transport applies unixMode with chmod() after
     * bind(), and a client connects rather than binds. Set to 0 rather than
     * copied from the gateway, so it cannot read as a permission this end
     * enforces. Access control on this side is the SO_PEERCRED check below. */
    cfg.unixMode        = 0;
    /* The client validates the ACCEPTING process, so a rogue process that
     * pre-binds the gateway path cannot impersonate the gateway. The transport
     * performs this at connect and refuses the peer on mismatch. */
    cfg.allowedGid      = getgid();
    cfg.inetAddr        = gwInetAddr.c_str();
    cfg.inetPort        = gwInetPort;

    NvPSFTransport* t = nullptr;
    const NvPSFTransportErr oerr = NvPSFTransportOpen(&cfg, &t);
    if (oerr != NVPSF_TRANSPORT_OK || t == nullptr)
    {
        return false;
    }

    /* A client must be able to send first -- the REGR is the first message on
     * this link -- and its peer identity is assigned internally, so it cannot
     * be constructed by the caller. */
    NvPSFPeerId p = NVPSF_PEER_INVALID;
    if (NvPSFTransportSolePeer(t, &p) != NVPSF_TRANSPORT_OK)
    {
        (void)NvPSFTransportClose(t);
        return false;
    }

    std::lock_guard<std::mutex> lk(gwSockMtx);
    gwTransport = t;
    gwPeer      = p;
    return true;
}

/** Close the upstream endpoint if open. Caller must not hold gwSockMtx. */
static void closeGatewayTransport()
{
    NvPSFTransport* t = nullptr;
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        t = gwTransport;
        gwTransport = nullptr;
        gwPeer      = NVPSF_PEER_INVALID;
    }
    if (t != nullptr)
    {
        (void)NvPSFTransportClose(t);
    }
}

/*
 * Re-establish the upstream link after the gateway has gone.
 *
 * NO SEQUENCE RESET. sdmGatewayDecisionSequenceReset() is deliberately not
 * called here: a reconnected gateway sends a NEW epoch, and the sequence
 * checker re-baselines on an epoch change by itself. Resetting as well would
 * discard the evidence that a gap occurred, and an outage beyond the freshness
 * window must latch on FRESHNESS, not on a spurious sequence gap.
 *
 * @return true once connected and re-registered; false only if shutdown was
 *         requested while reconnecting.
 */
static bool reconnectGatewayTransport()
{
    pxc_log_err("SDM: gateway upstream link lost; reconnecting");

    for (uint32_t attempt = 1U;
         !stopSDMThreads.load() && !signalShutdownRequested.load(); ++attempt)
    {
        if (g_signal_received != 0)
            signalShutdownRequested.store(true);

        if (openGatewayTransport())
        {
            /* Re-register before returning. A restored connection with no
             * event-type subscription is worse than no connection: the gateway
             * would accept it, count it as a healthy client, and forward
             * nothing -- and the SDM would then latch on decision freshness
             * with a link that looks up from both ends. */
            if (sendGatewayRegistration(true))
            {
                lastRegistrationTime = std::chrono::steady_clock::now();
                pxc_log_info("SDM: gateway upstream link re-established after " +
                             std::to_string(attempt) + " attempt(s)");
                return true;
            }
            pxc_log_warning("SDM: reconnected but REGR failed; retrying");
            closeGatewayTransport();
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(GW_RECONNECT_INTERVAL_MS));
    }
    return false;
}

/*
 * One upstream send. Non-blocking by construction (sendTimeoutMs = 0), so a
 * full peer buffer returns WOULD_BLOCK rather than stalling the listener.
 * Backpressure policy: drop, count, report -- never block the safety path.
 *
 * THE LOCK IS HELD ACROSS THE SEND DELIBERATELY. DO NOT "FIX" THIS.
 *
 * gwSockMtx is what makes the handle safe: closeGatewayTransport() nulls
 * gwTransport under this same lock and only THEN calls NvPSFTransportClose, so
 * either this send runs with a valid handle or it sees nullptr and returns
 * PEER_GONE. Release the lock before the send and there is a window in which
 * Close frees the handle while Send is inside it -- a use-after-free on the
 * safety path.
 *
 * This is permitted: Send is non-blocking at the syscall level (MSG_DONTWAIT)
 * and acquires only the transport's own leaf mutex, so there is no cycle and
 * nothing to stall. Per nvpsf_transport.h "Caller lock-order obligation", Send
 * and Recv are permitted under a caller lock; Poll, DropPeer and Close are not.
 */
static NvPSFTransportErr gatewaySend(const void* buf, size_t len)
{
    std::lock_guard<std::mutex> lk(gwSockMtx);
    if (gwTransport == nullptr)
        return NVPSF_TRANSPORT_PEER_GONE;
    return NvPSFTransportSend(gwTransport, gwPeer, buf, len);
}

static void hbAckApplySendResult(NvPSFTransportErr err, bool requirePendingFlag)
{
    std::lock_guard<std::mutex> lk(gwSockMtx);
    if (requirePendingFlag && !hbAckPending)
        return;
    if (err == NVPSF_TRANSPORT_OK)
    {
        hbAckPending = false;
    }
    else if (err == NVPSF_TRANSPORT_WOULD_BLOCK)
    {
        /* Left pending on purpose: flushPendingHbAck() retries on the next
         * listener iteration. Clearing it here would convert a momentarily
         * full buffer into a missed heartbeat ACK, and enough of those trip
         * the gateway's tier ladder against a healthy SDM. */
    }
    else
    {
        /* PEER_GONE and hard errors are not retried here -- the listener's own
         * PEER_GONE handling owns reconnection, and a stale ACK for a
         * pre-restart heartbeat sequence is not worth replaying afterwards. */
        pxc_log_err("SDM: heartbeat ACK send failed (" +
                    std::string(NvPSFTransportErrName(err)) + ")");
        hbAckPending = false;
    }
}

static void flushPendingHbAck()
{
    char sendBuf[NVPSD_GATEWAY_HB_MSG_SIZE];
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        if (!hbAckPending || gwTransport == nullptr)
            return;
        std::memcpy(sendBuf, hbAckPendingBuf, NVPSD_GATEWAY_HB_MSG_SIZE);
    }

    hbAckApplySendResult(gatewaySend(sendBuf, NVPSD_GATEWAY_HB_MSG_SIZE), true);
}

static void emitHbAckTryOnce(const char ack[NVPSD_GATEWAY_HB_MSG_SIZE])
{
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        std::memcpy(hbAckPendingBuf, ack, NVPSD_GATEWAY_HB_MSG_SIZE);
        hbAckPending = true;
        if (gwTransport == nullptr)
            return;
    }

    hbAckApplySendResult(gatewaySend(ack, NVPSD_GATEWAY_HB_MSG_SIZE), false);
}

/*
 * Send REGR registration to the gateway so it forwards subscribed event types.
 *
 * @param retryUntilWritable true at launch and on reconnect, where a REGR that
 *        does not land leaves the SDM subscribed to nothing; false on the
 *        periodic path, where the next REG_RETRY_INTERVAL_MS tick retries and
 *        blocking the listener would delay decision handling.
 */
static bool sendGatewayRegistration(bool retryUntilWritable)
{
    constexpr size_t maxBufSize = 4 + 1 + PROXIMITY_MAX_SUBSCRIBED_COUNT * sizeof(uint32_t);
    char buf[maxBufSize];
    std::memcpy(buf, NVPSD_GATEWAY_REG_MAGIC, 4);

    /* Walked straight off the event table, then the untiered extras, so every
     * group the arbitration below can decide is necessarily registered here. */
    std::size_t slot = 0;
    for (std::size_t i = 0; i < PROXIMITY_EVENT_TABLE_SIZE; ++i)
    {
        if (PROXIMITY_EVENT_TABLE[i].event == EVENT_UNKNOWN)
            continue; // Unselected groups must never enter this instance's REGR.
        uint32_t val = htonl(static_cast<uint32_t>(PROXIMITY_EVENT_TABLE[i].event));
        std::memcpy(buf + 5 + slot * sizeof(uint32_t), &val, sizeof(uint32_t));
        ++slot;
    }
    for (std::size_t i = 0; i < PROXIMITY_UNTIERED_COUNT; ++i, ++slot)
    {
        uint32_t val = htonl(static_cast<uint32_t>(PROXIMITY_UNTIERED_EVENTS[i]));
        std::memcpy(buf + 5 + slot * sizeof(uint32_t), &val, sizeof(uint32_t));
    }
    /* The compile-time maximum above bounds both the count byte and buffer. */
    buf[4] = static_cast<unsigned char>(slot);
    const size_t bufSize = 5U + slot * sizeof(uint32_t);

    const int maxAttempts = retryUntilWritable ? REGR_SEND_MAX_ATTEMPTS : 1;

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        const NvPSFTransportErr err = gatewaySend(buf, bufSize);

        if (err == NVPSF_TRANSPORT_OK)
        {
            pxc_log_info("SDM: sent registration (" +
                         std::to_string(slot) +
                         " event types) to gateway");
            return true;
        }
        if (err != NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            /* PEER_GONE or a hard error. Not retried here: the listener owns
             * reconnection, and spinning the retry budget against a link that
             * is down would only delay entering it. */
            pxc_log_err("SDM: failed to send REGR to gateway (" +
                        std::string(NvPSFTransportErrName(err)) + ")");
            return false;
        }
        if (!retryUntilWritable)
            return false;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(REGR_SEND_RETRY_SLEEP_MS));
    }
    pxc_log_warning("SDM: REGR send still blocked after retries");
    return false;
}

/* EVENT loop: receive DecisionRequest from PSD Gateway */
static void psdGatewayEventListener()
{
    char rawBuf[sizeof(NvPSDGatewayDecisionRequestPacket)];
    /* The last registration refusal reported (RGNK reason), 0 once a gateway
     * heartbeat shows this SDM registered: each reason is logged once. */
    uint8_t lastRegRefusal = 0U;

    while (!stopSDMThreads.load() && !signalShutdownRequested.load())
    {
        if (g_signal_received)
            signalShutdownRequested.store(true);
        flushPendingHbAck();
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
                    if (sendGatewayRegistration(false))
                        lastRegistrationTime = now;
                }
            }
        }

        /*
         * Snapshot the handle under gwSockMtx, then use it with the lock
         * released -- Poll blocks for MAIN_LOOP_POLL_TIMEOUT_MS and must not
         * hold a lock a send path waits on.
         *
         * Using it after the unlock is safe because this thread OWNS the
         * handle's lifetime: psdGatewayEventListener() runs on the main thread,
         * reconnect and close are both on this path, and teardown runs only
         * after this function has returned and every other thread has been
         * joined. No other thread can Close it underneath the Poll below.
         */
        NvPSFTransport* t = nullptr;
        {
            std::lock_guard<std::mutex> lk(gwSockMtx);
            t = gwTransport;
        }
        if (t == nullptr)
        {
            /* Either the launch-time open failed because the gateway is not up
             * yet, or a reconnect attempt left it closed. Wait for it rather
             * than exiting the loop -- exiting would end the SDM. */
            if (!reconnectGatewayTransport())
                break;      /* shutdown requested while waiting */
            continue;
        }

        /*
         * POLL IS NOT A PASSIVE SLEEP, and its events must be inspected.
         *
         * On POLLHUP it emits PEER_GONE *and releases the peer slot*. Throwing
         * the array away consumes the disconnect: Recv then finds no slot in
         * use and returns WOULD_BLOCK rather than PEER_GONE, and the next Poll
         * builds zero pollfds and returns TIMEOUT immediately without
         * sleeping. The loop would spin a core at 100% forever and never
         * reconnect, while looking from the outside like a quiet link.
         *
         * The listener spends nearly all its idle time inside Poll, so a
         * gateway restart almost always arrives on this path, not through Recv.
         */
        NvPSFTransportEvent events[4];
        size_t              eventCount = 0;
        const NvPSFTransportErr perr =
            NvPSFTransportPoll(t, MAIN_LOOP_POLL_TIMEOUT_MS, events,
                               sizeof(events) / sizeof(events[0]), &eventCount);

        bool peerGone = false;
        for (size_t i = 0; i < eventCount; ++i)
        {
            if (events[i].kind == NVPSF_EVENT_PEER_GONE)
                peerGone = true;
        }

        if (!peerGone && perr != NVPSF_TRANSPORT_OK &&
            perr != NVPSF_TRANSPORT_TIMEOUT)
        {
            /* Back off before retrying: a persistent poll failure would
             * otherwise peg a core and flood the log without bound. */
            pxc_log_warning("SDM: poll on gateway transport failed (" +
                            std::string(NvPSFTransportErrName(perr)) + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        size_t n = 0;
        NvPSFTransportErr rerr = NVPSF_TRANSPORT_WOULD_BLOCK;
        if (!peerGone)
        {
            /* Peer-filtered by the transport: on UNIX_SOCKET by the connection
             * itself plus the SO_PEERCRED check made at connect time. This is
             * what the old isConfiguredGatewayEndpoint() check provided. */
            rerr = NvPSFTransportRecv(t, nullptr, rawBuf, sizeof(rawBuf), &n);
        }

        if (peerGone || rerr == NVPSF_TRANSPORT_PEER_GONE)
        {
            closeGatewayTransport();
            if (!reconnectGatewayTransport())
                break;      /* shutdown requested mid-reconnect */
            continue;
        }
        if (rerr == NVPSF_TRANSPORT_WOULD_BLOCK ||
            rerr == NVPSF_TRANSPORT_TIMEOUT)
        {
            continue;       /* nothing arrived -- re-check the stop flags */
        }
        if (rerr == NVPSF_TRANSPORT_TRUNCATED)
        {
            /* Oversized for this link. The message is already discarded by the
             * kernel and cannot be re-read; dropping it is the only option, and
             * it must not be mistaken for a short read of a valid packet. */
            pxc_log_warning("SDM: oversized message from gateway; discarding");
            continue;
        }
        if (rerr != NVPSF_TRANSPORT_OK)
        {
            pxc_log_warning("SDM: recv on gateway transport failed (" +
                            std::string(NvPSFTransportErrName(rerr)) + ")");
            continue;
        }

        /* Check if this is a heartbeat from PSDGateway. Cast because Recv
         * reports a size_t length and the wire constant is a plain int. */
        if (n == static_cast<size_t>(NVPSD_GATEWAY_HB_MSG_SIZE) &&
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
            lastRegRefusal = 0U;

            /* Send ACK: [HBPC][seqNo]; WOULD_BLOCK leaves it pending and
             * flushPendingHbAck() retries on the next listener iteration. */
            char ack[NVPSD_GATEWAY_HB_MSG_SIZE];
            std::memcpy(ack, NVPSD_GATEWAY_HB_MAGIC_CLIENT, 4);
            std::memcpy(ack + 4, rawBuf + 4, 4);  // echo seq in network order
            emitHbAckTryOnce(ack);
            continue;
        }

        /* --- Registration refused by the gateway (RGNK) --- */
        {
            uint8_t refusal = 0U;
            if (NvPSDGatewayParseRegReject(rawBuf, n, &refusal))
            {
                /* Diagnosis only. Unregistered, this SDM receives no decisions and
                 * latches on its gateway-heartbeat watchdog, exactly as before;
                 * the periodic REGR keeps retrying. */
                if (refusal != lastRegRefusal)
                {
                    lastRegRefusal = refusal;
                    pxc_log_err(std::string("SDM: the gateway refused this SDM's registration; reason=") +
                                NvPSDGatewayRegRejectReasonName(refusal) +
                                " -- not registered, no decisions will arrive");
                }
                continue;
            }
        }

        /* --- Gateway-wrapped DecisionRequest --- */
        if (n == sizeof(NvPSDGatewayDecisionRequestPacket))
        {
            NvPSDGatewayDecisionRequestPacket packet;
            std::memcpy(&packet, rawBuf, sizeof(packet));
            if (!NvPSDGatewayDecisionPacketHeaderIsValid(&packet))
            {
                pxc_log_err("SDM: Gateway DecisionRequest packet header validation failed — dropping");
                continue;
            }

            DecisionRequest request;
            std::memcpy(&request, &packet.request, sizeof(DecisionRequest));

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
                " gatewayTxSeq=" +
                std::to_string(packet.gatewayTxSeq) +
                " events=" +
                std::to_string(request.sensorDataSummarySize));

            /* The whole packet is one fault-state transition as far as a PLC
             * release can tell: the sequence and freshness clears below as much as
             * the request. Taken here, a release cannot land between a clear and
             * the same request's SW_FAIL -- accepted, then re-latched. */
            std::lock_guard<std::mutex> transitionLock(faultTransitionMtx);

            const SdmGatewayDecisionSequenceResult seqResult =
                sdmGatewayDecisionSequenceObserve(&gatewayDecisionSequenceState,
                                                  packet.gatewayEpoch,
                                                  packet.gatewayTxSeq);
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_DUPLICATE_OR_STALE)
            {
                pxc_log_warning("Proximity: dropping duplicate/stale Gateway DecisionRequest packet; epoch=" +
                    std::to_string(packet.gatewayEpoch) + " txSeq=" +
                    std::to_string(packet.gatewayTxSeq));
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_INVALID)
            {
                pxc_log_err("Proximity: invalid Gateway DecisionRequest packet sequence — dropping");
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_GAP)
            {
                if (markGatewayDecisionSequenceFaultLatched(packet.gatewayEpoch, packet.gatewayTxSeq))
                {
                    pxc_log_err("Proximity: Gateway DecisionRequest sequence gap; entering safe hold, epoch=" +
                        std::to_string(packet.gatewayEpoch) + " txSeq=" +
                        std::to_string(packet.gatewayTxSeq));
                    sendLatchedProximitySafeHold();
                }
            }
            else
            {
                markGatewayDecisionSequenceRecovered(packet.gatewayEpoch, packet.gatewayTxSeq);
            }

            markDecisionRequestFresh(request.requestId);
            processDecisionRequestLocked(&request);
        }
        else if (n > 0)
        {
            pxc_log_warning(
                "SDM: received partial packet (" + std::to_string(n) +
                " bytes, expected " +
                std::to_string(sizeof(NvPSDGatewayDecisionRequestPacket)) + ")");
        }
        /* Any other length is neither a heartbeat nor a decision packet on
         * this link; loop back rather than guessing at it. */
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
                               std::uint32_t decisionRepeatIntervalMs,
                               std::uint32_t hbStaleMs,
                               std::uint32_t hbPeriodMs,
                               std::uint32_t decisionFreshnessTimeoutMs,
                               const std::string& haloIP,
                               unsigned int haloPort,
                               NvPSFTransportKind gatewayTransport,
                               const std::string& gatewaySocketPath,
                               const std::string& sensorPipelinesConfigPath)
{
    /* Only startup may write the binding. Freeze it before any reader exists;
     * all runtime lookups remain read-only and keep their existing timing. */
    {
        std::lock_guard<std::mutex> lock(proximityBindingMtx);
        if (!proximityBindingConfigured || proximityBindingFrozen)
        {
            std::cerr << "error: PXC requires a valid startup event binding before launch\n";
            return -1;
        }
        proximityBindingFrozen = true;
    }
    pxc_log_open();
    std::string binding = "Proximity: events (from --config)";
    for (std::size_t i = 0U; i < PROXIMITY_EVENT_COUNT; ++i)
    {
        if (PROXIMITY_EVENT_TABLE[i].event == EVENT_UNKNOWN)
            continue;
        binding += " " + std::string(proximityEventKey(i)) + "=EVENT_" +
                   std::to_string(static_cast<unsigned>(PROXIMITY_EVENT_TABLE[i].event));
    }
    pxc_log_info(binding);

    if (gatewayPort < 1U || gatewayPort > 65535U
        || plcPort < 1U || plcPort > 65535U)
    {
        pxc_log_err("gateway and PLC ports must be in range 1..65535");
        pxc_log_close();
        return -1;
    }
    if (haloPort > 65535U)
    {
        pxc_log_err("halo port must be in range 0..65535 (0 = disabled)");
        pxc_log_close();
        return -1;
    }
    const bool haloRequested = !haloIP.empty() && haloPort > 0U;

    const uint16_t gatewayPortU16 = static_cast<uint16_t>(gatewayPort);
    const uint16_t plcPortU16     = static_cast<uint16_t>(plcPort);
    const uint16_t haloPortU16    = static_cast<uint16_t>(haloPort);

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

    if (hbStaleMs < kHbTimingMsMin || hbStaleMs > kHbTimingMsMax ||
        hbPeriodMs < kHbTimingMsMin || hbPeriodMs > kHbTimingMsMax)
    {
        pxc_log_err("hbStaleMs and hbPeriodMs must be in " +
                    std::to_string(kHbTimingMsMin) + ".." +
                    std::to_string(kHbTimingMsMax));
        pxc_log_close();
        return -1;
    }

    if (!sdmDecisionFreshnessTimeoutMsIsValid(decisionFreshnessTimeoutMs))
    {
        pxc_log_err("decisionFreshnessTimeoutMs must be in " +
                    std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN) + ".." +
                    std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX));
        pxc_log_close();
        return -1;
    }

    /*
     * Reviewed sensor set. Loaded before any socket, thread or signal handler
     * exists, so a deployment whose config is missing or malformed exits here
     * having done nothing rather than running with a latch that can never arm.
     *
     * The set is NOT cross-checked against sensor_config.conf, and must not be:
     * in 3D the only reviewed identity is the BEV group, whose pipelineId is
     * deliberately absent from sensor_config.conf. A "must appear in
     * sensor_config" rule would reject exactly the deployment this change
     * exists to support. The daemon owns that cross-check, on group MEMBERS,
     * where it is the correct one.
     */
    {
        std::string setErr;
        if (!loadReviewedSensorSet(sensorPipelinesConfigPath.c_str(), gReviewedSensors, setErr))
        {
            pxc_log_err("Proximity: cannot load the reviewed sensor set; refusing to start: " + setErr);
            std::cerr << "error: " << setErr << "\n";
            pxc_log_close();
            return -1;
        }
        /* Unconditional: a set that silently shrank is only visible if the
         * runtime states what it resolved. */
        pxc_log_info("Proximity: reviewed sensor set from " + sensorPipelinesConfigPath + ": " +
                     describeReviewedSensorSet(gReviewedSensors));
    }

    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        hbAckPending = false;
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

    /* --- Optional VST halo fan-out socket (fire-and-forget, no ACK read) --- */
    if (haloRequested)
    {
        haloSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (haloSock < 0)
        {
            pxc_log_err("Failed to create halo UDP socket");
            close(plcSock); plcSock = -1;
            pxc_log_close();
            return -1;
        }

        /* Non-blocking for the same reason plcSock is: the halo send runs on
         * the decision path, and a full socket buffer must drop the overlay
         * update rather than stall the command that acts on the floor. Halo is
         * a visualisation, so it is never worth a millisecond of the safety
         * path. MSG_DONTWAIT is passed at the send as well -- either alone
         * would do, but the flag documents the requirement at the call site
         * where a future reader sees it. */
        int haloFlags = fcntl(haloSock, F_GETFL, 0);
        if (haloFlags == -1 ||
            fcntl(haloSock, F_SETFL, haloFlags | O_NONBLOCK) == -1)
            pxc_log_warning("Failed to set haloSock non-blocking");

        haloAddr.sin_family = AF_INET;
        haloAddr.sin_port   = htons(haloPortU16);
        if (inet_pton(AF_INET, haloIP.c_str(), &haloAddr.sin_addr) <= 0)
        {
            pxc_log_err("Invalid halo IP address");
            close(plcSock); plcSock = -1;
            close(haloSock); haloSock = -1;
            pxc_log_close();
            return -1;
        }
        haloEnabled.store(true, std::memory_order_relaxed);
        pxc_log_info("VST halo fan-out enabled -> " + haloIP + ":" + std::to_string(haloPortU16));
    }
    else
    {
        pxc_log_info("VST halo fan-out disabled (no --halo_ip/--halo_port)");
    }

    /*
     * --- PSD Gateway upstream endpoint: send REGR, receive DecisionRequests + HB ---
     *
     * The socket/fcntl/bind/connect ladder this replaces is now inside the
     * transport, along with the O_NONBLOCK the old code set by hand and the
     * peer filtering the old sender-address comparison was really there to
     * obtain.
     */
    gwTransportKind = gatewayTransport;
    gwSocketPath    = gatewaySocketPath;
    gwInetAddr      = gatewayIP;
    gwInetPort      = gatewayPortU16;

    /*
     * A failure to open here is NOT fatal, and that is a deliberate change of
     * behaviour rather than a missing error path.
     *
     * On UDP an unconnected peer was invisible: connect() on a datagram socket
     * only records an address, so it succeeded whether or not a gateway
     * existed -- which is why the old code could treat REGR failure as fatal.
     * On UNIX_SOCKET connect() fails outright until the gateway has bound the
     * path, so treating that as fatal would mean an SDM launched before the
     * gateway exits instead of waiting. The listener's reconnect loop owns the
     * wait.
     */
    if (openGatewayTransport())
    {
        if (sendGatewayRegistration(true))
            lastRegistrationTime = std::chrono::steady_clock::now();
        else
        {
            pxc_log_warning("SDM: initial REGR did not complete; will retry in event loop");
            /* Elapsed >= REG_RETRY_INTERVAL_MS so the first listener iteration retries soon. */
            lastRegistrationTime = std::chrono::steady_clock::now() -
                std::chrono::milliseconds(REG_RETRY_INTERVAL_MS);
        }
    }
    else
    {
        pxc_log_warning("SDM: psdGateway transport not available at launch (" +
                        std::string(NvPSFTransportKindName(gwTransportKind)) +
                        "); waiting for the gateway");
        lastRegistrationTime = std::chrono::steady_clock::now() -
            std::chrono::milliseconds(REG_RETRY_INTERVAL_MS);
    }

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
        std::lock_guard<std::mutex> lock(proximityStateMtx);
        proximityActiveCommand = CMD_NORMAL;
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
        periodicDecisionThread = std::thread(proximityPeriodicDecisionLoop);
        pxc_log_info(
            "Proximity Control Algorithm initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat every " +
            std::to_string(decisionRepeatIntervalMs) + " ms; gateway HB stale_ms=" +
            std::to_string(hbStaleMs) + " period_ms=" + std::to_string(hbPeriodMs) +
            "; decision_freshness_timeout_ms=" +
            std::to_string(decisionFreshnessTimeoutMs) + ")");
    }
    else
    {
        pxc_log_info(
            "Proximity Control Algorithm initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat disabled; gateway HB stale_ms=" +
            std::to_string(hbStaleMs) + " period_ms=" + std::to_string(hbPeriodMs) +
            "; decision_freshness_timeout_ms=" +
            std::to_string(decisionFreshnessTimeoutMs) + ")");
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
    joinThread(decisionFreshnessWatchdogThread);
    joinThread(periodicDecisionThread);

    /* Close sockets */
    {
        std::lock_guard<std::mutex> lock(plcSocketMtx);
        if (plcSock >= 0) { close(plcSock); plcSock = -1; }
    }
    {
        std::lock_guard<std::mutex> lock(haloSocketMtx);
        haloEnabled.store(false, std::memory_order_relaxed);
        if (haloSock >= 0) { close(haloSock); haloSock = -1; }
    }
    closeGatewayTransport();

    /* Clear pending commands */
    {
        std::lock_guard<std::mutex> lock(commandStatusMtx);
        pendingCommands.clear();
    }

    pxc_log_close();
}
