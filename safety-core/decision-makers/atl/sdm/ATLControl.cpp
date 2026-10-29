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
#include "atl_sdm_config.h"
#include <NvPSDGatewayProtocol.h>
#include "nvpsf_transport.h"
#include "pss_message_validate.h"
#include "sdm_decision_request_state.h"
#include "sdm_decision_freshness.hpp"
#include "sdm_reviewed_sensor_set.hpp"
#include "sdm_gateway_decision_sequence.hpp"
#include "atl_fault_report.h"

/* Simple SDM logging: console + optional log file (no NvPSB dependency) */
static std::mutex atlLogMtx;
static FILE* atlLogFile = nullptr;

/* Relative by default, so the launcher's working directory decides where it
 * lands (/var/log/psf under launch_psf.sh). Set once by atlSetLogFile() before
 * any control thread exists, and read only under atlLogMtx afterwards. */
static std::string atlLogPath = "atl_sdm.log";
static bool atlLogPathFrozen = false;  /* Protected by atlLogMtx. */

bool atlSetLogFile(const std::string& path)
{
    if (path.empty())
        return false;

    std::lock_guard<std::mutex> lock(atlLogMtx);
    if (atlLogPathFrozen)
        return false;
    atlLogPath = path;
    return true;
}

static void atl_log_open(void)
{
    std::lock_guard<std::mutex> lock(atlLogMtx);
    /* Freeze even if fopen fails: console-only logging is still runtime. */
    atlLogPathFrozen = true;
    if (!atlLogFile) {
        atlLogFile = fopen(atlLogPath.c_str(), "a");
        if (atlLogFile) {
            setvbuf(atlLogFile, nullptr, _IOLBF, 0);
        } else {
            /* Not fatal -- the console stream still carries every line. Said out
             * loud because the operator now chooses this path, and a silently
             * unopened file is exactly the case where two instances' evidence
             * ends up indistinguishable. */
            fprintf(stderr, "[ATL][ERR] cannot open log file '%s' (%s); logging to console only\n",
                    atlLogPath.c_str(), strerror(errno));
        }
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

// Tripwire = trailer entry line. TW OUT = entry into trailer, TW IN = exit from trailer.
// Default event_mapping_atl.pb.txt binding: EVENT_0=forklift TW OUT, EVENT_1=forklift TW IN,
// EVENT_2=person TW OUT, EVENT_3=person TW IN, EVENT_4=person restricted ROI violation,
// EVENT_5=person restricted ROI violation cleared. --config rebinds these at
// startup, which is what lets a second ATL SDM cover a different zone of the
// same site from the same binary.

/* Heartbeat / registration (NvPSDGatewayProtocol.h) */
/* Re-register with gateway periodically so we recover after gateway restart (in-memory state lost). */
static constexpr int REG_RETRY_INTERVAL_MS = 30000;
/*
 * REGR retry budget when the caller asks for it (launch and reconnect).
 * The transport send is non-blocking, so WOULD_BLOCK means the peer buffer is
 * momentarily full rather than that anything is wrong; a short sleep between
 * attempts is the whole of the back-off this needs. The poll(POLLOUT) ladder
 * this replaces existed because the raw socket gave no other way to wait.
 */
static constexpr int REGR_SEND_MAX_ATTEMPTS = 32;
static constexpr int REGR_SEND_RETRY_SLEEP_MS = 5;
/*
 * Reconnect cadence for the upstream link. Fixed, not
 * exponential, and matched to the PSS -> PSD link.
 */
static constexpr int GW_RECONNECT_INTERVAL_MS = 500;
static constexpr int SAFE_RELEASE_MIN_INTERVAL_MS = 1000;

/* launchATLControlAlgo decisionRepeatIntervalMs: 0 = periodic repeat off; when non-zero, inclusive
 * [kDecisionRepeatIntervalMsMinNonZero, kDecisionRepeatIntervalMsMax] to avoid PLC overload. */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax       = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                     = 100U;
static constexpr std::uint32_t kHbTimingMsMax                     = 600000U;

/* The six situations this SDM reacts to, and the event code carrying each.
 *
 * A generic EVENT_n has no meaning of its own: it means whatever the
 * deployment's mdx-client event map assigns to it. The values below are only
 * the historic single-deployment convention, and each is independent of the
 * others -- there is no requirement that they be contiguous, ordered, or start
 * at EVENT_0. --config rebinds them to match the output_event codes the event
 * map actually emits, which is what lets two ATL SDMs cover two zones of one
 * site: each subscribes to its own codes, and the gateway's REGR filter keeps
 * the other zone's events away from it.
 *
 * The configuration file is the only source; there is no command-line
 * override. These fields are filled positionally from AtlSdmConfig::events, so
 * this declaration order must stay in step with kEventKeys in
 * atl_sdm_config.cpp: reordering the fields silently rebinds every situation.
 *
 * Startup-only and immutable: written by atlSetSubscribedEvents() before any
 * control thread exists, read by the listener thread afterwards.
 *
 * Initialised to EVENT_UNKNOWN, which no event map can emit, so this is a
 * placeholder and NOT a default: launchATLControlAlgo refuses to run until
 * atlSetSubscribedEvents has replaced it. Leaving a real code here is the
 * defect this guards -- a second ATL SDM launched without a configuration
 * would register for the first one's codes and emit decisions about the first
 * one's trailer on its own PLC port. */
static struct {
    EventType forkliftEntered;  /* forklift tripwire OUT */
    EventType forkliftExited;   /* forklift tripwire IN  */
    EventType personEntered;    /* person tripwire OUT   */
    EventType personExited;     /* person tripwire IN    */
    EventType roiViolation;     /* a person is in the restricted area */
    EventType roiCleared;       /* that violation cleared             */
} atlEvent = { EVENT_UNKNOWN, EVENT_UNKNOWN, EVENT_UNKNOWN,
               EVENT_UNKNOWN, EVENT_UNKNOWN, EVENT_UNKNOWN };

/* What supplied the binding above, for the startup log -- always "--config"
 * for atl_sdm. Empty until it is set, which is also how launchATLControlAlgo
 * tells "never set" from "set". */
static std::string atlEventSource;
static std::mutex atlBindingMtx;
static bool atlBindingFrozen = false;

/* The REGR list: the six situations plus SW_FAIL, which is always subscribed
 * and handled as a fault rather than as a situation. */
static constexpr uint8_t ATL_SUBSCRIBED_COUNT = ATL_EVENT_COUNT + 1U;
/* The REGR payload carries the count in a single byte. */
static_assert(ATL_SUBSCRIBED_COUNT <= 255U,
              "ATL subscription list must fit the REGR count byte");

static void atlSubscribedEvents(EventType out[ATL_SUBSCRIBED_COUNT])
{
    out[0] = atlEvent.forkliftEntered;
    out[1] = atlEvent.forkliftExited;
    out[2] = atlEvent.personEntered;
    out[3] = atlEvent.personExited;
    out[4] = atlEvent.roiViolation;
    out[5] = atlEvent.roiCleared;
    out[6] = SW_FAIL;
}
static_assert(ATL_SUBSCRIBED_COUNT == 7U, "atlSubscribedEvents fills every slot");

bool atlSetSubscribedEvents(const EventType* events, std::size_t count,
                            const char* source)
{
    std::lock_guard<std::mutex> lock(atlBindingMtx);
    if (atlBindingFrozen)
        return false;

    if (events == nullptr || count != ATL_EVENT_COUNT
        || source == nullptr || source[0] == '\0')
        return false;

    /* Two situations sharing a code would make the dispatch order decide which
     * one an event means, so reject rather than silently prefer one. */
    for (std::size_t i = 0U; i < count; ++i)
    {
        /* Public callers may bypass the config parser. Faults and health
         * reports must never be bound to physical ATL situations. */
        if (events[i] < EVENT_0 || events[i] > EVENT_23)
            return false;
        for (std::size_t j = i + 1U; j < count; ++j)
        {
            if (events[i] == events[j])
                return false;
        }
    }

    atlEvent.forkliftEntered = events[0];
    atlEvent.forkliftExited  = events[1];
    atlEvent.personEntered   = events[2];
    atlEvent.personExited    = events[3];
    atlEvent.roiViolation    = events[4];
    atlEvent.roiCleared      = events[5];
    atlEventSource           = source;
    return true;
}

/* Record which code carries each situation. When several ATL SDMs share a
 * gateway this is the only thing telling their logs apart, and it is otherwise
 * visible only on the command line that
 * started them. */
static void atlLogStartupBindings()
{
    atl_log_info("ATL: events (from " + atlEventSource + ")" +
                 " forklift-entered=" + atlEventTypeName(atlEvent.forkliftEntered) +
                 " forklift-exited="  + atlEventTypeName(atlEvent.forkliftExited) +
                 " person-entered="   + atlEventTypeName(atlEvent.personEntered) +
                 " person-exited="    + atlEventTypeName(atlEvent.personExited) +
                 " roi-violation="    + atlEventTypeName(atlEvent.roiViolation) +
                 " roi-cleared="      + atlEventTypeName(atlEvent.roiCleared));

}
/*
 * Reviewed sensor set, loaded from sensor_pipelines_config.conf at startup.
 *
 * Written once in launchATLControlAlgo before any thread or socket exists, then
 * read-only for the life of the process. That ordering is what makes an
 * unsynchronised read safe here: there is no reader until after the write, and
 * there is never a second write. Gateway auto-subscribes
 * SENSOR_INVALID/SENSOR_VALID; ATL uses pipelineID as the safety key.
 */
static ReviewedSensorSet gReviewedSensors{};
/* Bitmask of pipelineIds already reported as absent from the reviewed set, so
 * each one is logged exactly once. Touched only from the event path, which is
 * the single decision thread. */
static uint32_t          gUnreviewedSensorWarned = 0U;

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
/* Physical state. The comments name the SITUATION, not an event code: which
 * code carries each situation is chosen at startup (see atlEvent above), so
 * naming EVENT_0 here would be wrong for any instance that was configured. */
static bool forkliftInTrailer = false;              // forkliftEntered sets; forkliftExited clears
static int personsInTrailerCount = 0;                // personEntered (+); personExited (-)
static constexpr int PERSONS_IN_TRAILER_MAX = 10000;  // cap to avoid overflow from entered/exited skew
static bool restrictedAreaViolationByPerson = false;   // roiViolation sets; roiCleared clears
// Master mutex for atomic state reading
static std::mutex masterStateMtx;
static ObjectRecord lastForkliftRecord = {};
static bool lastForkliftRecordValid = false;
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
static std::thread       decisionFreshnessWatchdogThread;
static std::thread       periodicDecisionThread;
static std::mutex        atlPeriodicDecisionWaitMtx;
static std::condition_variable atlPeriodicDecisionCv;

/* 0 = periodic repeat disabled; otherwise re-send evaluateATLDecision() on this interval. */
static std::atomic<std::uint32_t> g_decisionRepeatIntervalMs{5000U};

/* SIGINT/SIGTERM latch: only `volatile sig_atomic_t` assignment in handler (POSIX async-signal-safe). */
static volatile sig_atomic_t g_signal_received = 0;

/*
 * PSD Gateway upstream endpoint. Send REGR to the gateway, receive
 * filtered DecisionRequests and heartbeats.
 *
 * Was a raw AF_INET SOCK_DGRAM descriptor with a connect() applied purely to
 * get peer filtering on the receive path. It is now an NvPSFTransport in the
 * client role, which gives the same filtering plus the SO_PEERCRED check
 * requires on the UNIX_SOCKET path -- a client must validate the accepting
 * process before trusting envelopes, so that a rogue process cannot pre-bind
 * the gateway socket path and impersonate it.
 *
 * gwPeer is the transport's identity for the gateway, not a descriptor. It
 * carries a generation counter, so an identity naming a slot that has since
 * been recycled is detected rather than acted on.
 */
static NvPSFTransport* gwTransport = nullptr;
static NvPSFPeerId     gwPeer      = NVPSF_PEER_INVALID;
static std::mutex gwSockMtx;

/*
 * Endpoint configuration, retained for reconnect. openGatewayTransport() is
 * called again on every reconnect attempt, so the parameters have to outlive
 * launchATLControlAlgo's arguments rather than being consumed once.
 */
static NvPSFTransportKind gwTransportKind = NVPSF_TRANSPORT_UNIX_SOCKET;
static std::string        gwSocketPath;
static std::string        gwInetAddr;
static std::uint16_t      gwInetPort = 0;
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
static std::atomic<std::uint32_t>            g_hbStaleMs{5000U};
static std::atomic<std::uint32_t>            g_hbPeriodMs{5500U};
static std::atomic<bool>                     hbFaultLatched{false};
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
 * be accepted, in a state that never existed upstream. Held by the gateway
 * listener for each whole packet -- the sequence and freshness clears that
 * precede the request as much as the request itself -- and taken first by
 * handleSafeReleaseRequest(), it makes the release see the packet either not at
 * all or completely, as if both were handled in one task.
 *
 * Lock order: faultTransitionMtx -> decisionEvalSendMtx -> safeStateLatchMtx. No
 * other thread takes it.
 */
static std::mutex                            faultTransitionMtx;
static constexpr uint32_t                    kSafeLatchGatewayHb = 1U << 0;
static constexpr uint32_t                    kSafeLatchPss       = 1U << 1;
static constexpr uint32_t                    kSafeLatchCountDrift = 1U << 2;
static constexpr uint32_t                    kSafeLatchDecisionFreshness = 1U << 3;
static constexpr uint32_t                    kSafeLatchGatewayDecisionSequence = 1U << 4;
static constexpr uint32_t                    kSafeLatchAllSafetyRelevantSensorsFailed = 1U << 5;
/* AI-pipeline distrust is a SAFE-STATE CAUSE, not merely an input filter.
 * PSS reports it pipeline-wide (one Perception Monitor verdict for the whole perception
 * pipeline), and while it holds, nothing arriving from any MDX client can be
 * trusted to describe the physical world. Continuing to drive the PLC from the
 * last decision would keep the machine running on evidence known to be bad, so
 * the SDM latches instead. Release is gated on the ACTIVE flag below, which
 * only AI_PIPELINE_VALID clears. */
static constexpr uint32_t                    kSafeLatchAiPipelineInvalid = 1U << 6;
static std::atomic<uint32_t>                 safeStateLatchCauses{0U};
static std::mutex                            safeStateLatchMtx;
static std::atomic<uint32_t>                 failedSafetyRelevantSensorMask{0U};
static std::atomic<bool>                     allSafetyRelevantSensorsFailedActive{false};
static std::atomic<bool>                     aiPipelineInvalidActive{false};

/* AVOID_SAFETY_LATCH_FOR_SAIM_INIT (compile-time; ON by default on this build).
 * When enabled, a SENSOR_INVALID whose cause is SAIM's pre-first-frame SAIM_INIT
 * seed does NOT count toward the all-safety-relevant-sensors-failed safe-hold
 * latch: at startup every SAIM sensor is briefly SENSOR_INVALID/SAIM_INIT before
 * its first decoded frame, which would otherwise deterministically latch safe
 * hold that -- absent a PLC safe-release -- never clears and permanently
 * suppresses MUTE/UNMUTE. Build with -DAVOID_SAFETY_LATCH_FOR_SAIM_INIT=0 to
 * enforce strict latching. Genuine failures carry a distinct cause
 * (SAIM_STREAM_DISCONNECT, SAIM_INTERNAL_ERROR, SAIM_INPUT_DEGRADED,
 * SAIM_FRAME_DROP, SAIM_UNKNOWN, ...) and still latch. */
#ifndef AVOID_SAFETY_LATCH_FOR_SAIM_INIT
#define AVOID_SAFETY_LATCH_FOR_SAIM_INIT 1
#endif
#if AVOID_SAFETY_LATCH_FOR_SAIM_INIT
/* SAIM pre-first-frame seed cause. Compared as an enum, not a string: a
 * truncated or non-terminated buffer can no longer be mistaken for this value,
 * and the comparison is identical in every SDM. */
static constexpr EventCause kSaimInitCause = CAUSE_SAIM_INIT;
/* Pipelines whose SAIM_INIT seed has not been resolved by a SENSOR_VALID yet.
 * PSS folds SENSOR_INVALID into its trust state WITHOUT keeping the cause
 * (SafetyEventManager::OnTrustReport), then stamps the result onto ordinary
 * perception events as isHealthy=false. Those entries carry their own cause,
 * CAUSE_NONE, so the kSaimInitCause test below cannot recognise them and the
 * seed latches through that second path even though the first suppresses it.
 * The seed is a sustained condition rather than one event, so the pending
 * state is held per pipeline until SAIM reports the sensor valid. */
static std::atomic<uint32_t> saimInitPendingSensorMask{0U};
#endif
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

static void sendLatchedSafeHold();

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

    atl_log_info(std::string("ATL: PSS sub-cause cleared: ") + kPssSubCauseName[cause] +
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
    atl_log_err("ATL: All safety-relevant sensors failed; entering safe hold");
    return true;
}

static void clearAllSafetyRelevantSensorsFailedActiveIfRecovered(uint32_t failedMask)
{
    if ((failedMask & gReviewedSensors.mask) == gReviewedSensors.mask)
        return;

    if (allSafetyRelevantSensorsFailedActive.exchange(false, std::memory_order_acq_rel))
    {
        atl_log_info("ATL: All safety-relevant sensors failed condition cleared; latch awaits PLC release");
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
                atl_log_info("ATL: pipelineID " + std::to_string(pipelineId) +
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
            /* Initialization-in-progress (SAIM_INIT), not a genuine failure: leave
             * the sensor's failed bit unchanged (pending) so it does not contribute
             * to the all-safety-relevant-sensors-failed latch. Real post-init
             * failures carry a different cause and still latch. */
            /* ONLY this cause suppresses. Every other value -- including one
             * this build does not recognise -- falls through and sets the
             * failed bit, so an unknown cause latches rather than being
             * silently ignored. */
            if (sd.event.cause == kSaimInitCause)
            {
                /* Record the seed so the propagated evidence below can be
                 * attributed to it. Only the explicit SENSOR_INVALID is
                 * authoritative for that; the suppression itself is unchanged. */
                if (et == SENSOR_INVALID)
                    saimInitPendingSensorMask.fetch_or(bit, std::memory_order_acq_rel);
                continue;
            }
            /* The same seed arriving as propagated evidence rather than as the
             * seed event: PSS dropped the cause on the way, so this entry says
             * only isHealthy=false with CAUSE_NONE. Suppress it for exactly the
             * pipelines SAIM has declared init-pending. An explicit
             * SENSOR_INVALID is excluded and still latches whatever the pending
             * state, so a genuine fault during the window fails closed. */
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
            /* SAIM has resolved this sensor, so the init window is over for it
             * and any later unhealthy evidence must latch again. */
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
            atl_log_info("ATL: safety-sensor health change pipelineID=" +
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

static bool markCountDriftFaultLatched()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    const uint32_t previousLatch =
        safeStateLatchCauses.load(std::memory_order_acquire);
    safeStateLatchCauses.fetch_or(kSafeLatchCountDrift, std::memory_order_acq_rel);
    return previousLatch == 0U;
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
    sdmGatewayDecisionSequenceReset(&gatewayDecisionSequenceState);
    safeStateLatchCauses.store(0U, std::memory_order_release);
}

static void markDecisionRequestFresh(uint32_t requestId)
{
    {
        std::lock_guard<std::mutex> lock(decisionFreshnessMtx);
        lastValidDecisionRequestTime = std::chrono::steady_clock::now();
    }

    if (decisionFreshnessFaultActive.exchange(false, std::memory_order_acq_rel))
    {
        atl_log_info("ATL: DecisionRequest freshness recovered; requestId=" +
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
        atl_log_info("ATL: Gateway DecisionRequest sequence recovered; epoch=" +
            std::to_string(gatewayEpoch) + " txSeq=" +
            std::to_string(gatewayTxSeq) + "; latch awaits PLC release");
    }
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
            atl_log_err("ATL: DecisionRequest freshness timeout; entering safe hold, elapsed_ms=" +
                std::to_string(elapsedMs));
            sendLatchedSafeHold();
        }
    }
}

/* Fusion-driven MUTE/UNMUTE must not override watchdog safe-hold.
 * Suppress while HB miss count is past warn tier or fault is latched; resume when m <= warnW again (e.g. m == 0). */
static bool gatewayHbSuppressFusionDecisions()
{
    if (safeStateLatched())
        return true;
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


static ObjectRecord makeObjectRecordFromSensorData(const SensorData& data)
{
    const EventFusionMetadata& meta = data.event.fusionMetadata;
    ObjectRecord record = {};
    record.object_id = meta.objectID[0];
    record.x = meta.coordinates[0].x;
    record.y = meta.coordinates[0].y;
    record.z = 0.0f;
    record.metadata = static_cast<uint32_t>(meta.objectType[0]);
    return record;
}

static void updateLastForkliftRecordLocked(const SensorData& data)
{
    lastForkliftRecord = makeObjectRecordFromSensorData(data);
    lastForkliftRecordValid = true;
}

static void resetLastForkliftRecordLocked()
{
    lastForkliftRecord = {};
    lastForkliftRecordValid = false;
}

static bool snapshotLastForkliftRecord(ObjectRecord* record)
{
    if (record == nullptr)
        return false;

    std::lock_guard<std::mutex> stateLock(masterStateMtx);
    if (!lastForkliftRecordValid)
        return false;
    *record = lastForkliftRecord;
    return true;
}

/* ATL uses only Object 1 for the last-known forklift; Object 2 remains zero. */
static void fillObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS],
                               const SensorData* data,
                               const ObjectRecord* object1Record)
{
    std::memset(objects, 0, sizeof(ObjectRecord) * COMMAND_NUM_OBJECTS);
    if (object1Record != nullptr) {
        objects[0] = *object1Record;
        return;
    }

    if (data != nullptr)
        objects[0] = makeObjectRecordFromSensorData(*data);
}

static bool sendDecisionCommandInternal(unsigned char command, bool trackAck,
                                        const SensorData* data,
                                        const ObjectRecord* object1Record)
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

    fillObjectRecords(pkt.objects, data, object1Record);

    pkt.crc32 = cmdPacketCRC32(&pkt);

    bool shouldPrintAndTrack = false;
    const char* cmdType = commandName(command);
    if (command == CMD_MUTE || command == CMD_UNMUTE || command == CMD_SW_ERROR ||
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
               << ", Obj0 ID: " << pkt.objects[0].object_id;
        atl_log_info(logMsg.str());
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
                atl_log_err("Failed to send decision command");
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

bool sendDecisionCommand(unsigned char command, bool trackAck,
                         const SensorData* data)
{
    return sendDecisionCommandInternal(command, trackAck, data, nullptr);
}

static bool sendDecisionCommandWithObject1(unsigned char command, bool trackAck,
                                           const ObjectRecord* object1Record)
{
    return sendDecisionCommandInternal(command, trackAck, nullptr, object1Record);
}

static bool isConfiguredPlcEndpoint(const struct sockaddr_in& sender,
                                    socklen_t sender_len)
{
    return sender_len >= static_cast<socklen_t>(sizeof(sender)) &&
           sender.sin_family == AF_INET &&
           sender.sin_port == plcAddr.sin_port &&
           sender.sin_addr.s_addr == plcAddr.sin_addr.s_addr;
}

static bool isValidPlcInboundCommand(uint8_t command)
{
    return command == CMD_HEARTBEAT ||
           command == CMD_MUTE ||
           command == CMD_UNMUTE ||
           command == CMD_HW_ERROR ||
           command == CMD_SW_ERROR ||
           command == CMD_SAFE_RELEASE_REQUEST;
}

static void resetAtlStateFromPlcSafeRelease()
{
    std::lock_guard<std::mutex> stateLock(masterStateMtx);
    forkliftInTrailer = false;
    personsInTrailerCount = 0;
    restrictedAreaViolationByPerson = false;
    resetLastForkliftRecordLocked();
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
        atl_log_err("ATL: AI pipeline reported INVALID; entering safe hold");
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
        atl_log_info("ATL: AI pipeline reported VALID; condition cleared, latch awaits PLC release");
    }
}

/*
 * Apply every AI-pipeline verdict in the request. Returns true on the latch
 * entry edge. Same shape as the PXC and combined SDMs.
 *
 * AI_PIPELINE_INVALID arrives ALREADY STAMPED UNTRUSTED: PSS sets the
 * pipeline-wide flag in OnTrustReport before routing the event. So this reads
 * the verdict without looking at isTrustedSource, and must run before anything
 * that skips untrusted events -- or returns early.
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

static uint32_t activeSafeLatchCauses()
{
    uint32_t active = 0U;
    if (hbFaultLatched.load(std::memory_order_acquire) ||
        gatewayHbTier2SafeHoldBand())
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
    /* Count drift is an SDM state-consistency fault. It latches safe output,
     * but has no continuing external source once PLC/operator confirms clear. */
    return active;
}

static void sendLatchedSafeHold()
{
    /* Fault callers publish the latch before waiting on this sequence mutex so
     * the periodic decision path cannot emit MUTE from stale state. */
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
    const bool unmuteSent = sendDecisionCommand(CMD_UNMUTE, true, nullptr);
    const bool swErrorSent = sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
    if (!unmuteSent || !swErrorSent)
        atl_log_err("ATL: latched safe-hold TX failed; latch retained for retry");
}

static void sendPhysicalEventSafeHold(const ObjectRecord* object1Record)
{
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
    const bool unmuteSent =
        sendDecisionCommandWithObject1(CMD_UNMUTE, true, object1Record);
    const bool swErrorSent =
        sendDecisionCommandWithObject1(CMD_SW_ERROR, true, object1Record);
    if (!unmuteSent || !swErrorSent)
        atl_log_err("ATL: physical-event safe-hold TX failed; latch retained for retry");
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

static void handleSafeReleaseRequest()
{
    std::lock_guard<std::mutex> transitionLock(faultTransitionMtx);
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);

    const uint32_t active = activeSafeLatchCauses();
    if (active != 0U)
    {
        atl_log_warning("ATL: PLC safe-release denied; active safe-state fault source remains, active_causes=" +
            std::to_string(active));
        sendDecisionCommand(CMD_SAFE_RELEASE_DENIED, false, nullptr);
        sendDecisionCommand(CMD_UNMUTE, true, nullptr);
        sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
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
        const bool unmuteSent = sendDecisionCommand(CMD_UNMUTE, true, nullptr);
        if (ackSent && unmuteSent) {
            /* Only apply PLC-authoritative clear state after the peer can
             * observe the accepted release sequence.  Do not clear a new latch
             * cause that arrives while TX is in flight. */
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
                resetAtlStateFromPlcSafeRelease();
                atl_log_info("ATL: PLC safe-release accepted; local state reset to clear condition");
                markSafeReleaseNoopResponseSuccess(now);
            } else {
                atl_log_warning("ATL: safe-release response sent but latch retained due to concurrent fault");
                sendDecisionCommand(CMD_UNMUTE, true, nullptr);
                sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
            }
        } else {
            atl_log_err("ATL: safe-release response TX failed; latch retained and state not reset");
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool rateLimited = safeReleaseNoopResponseInRateWindow(now);
    if (rateLimited)
        atl_log_warning("ATL: rate-limited PLC safe-release retry; re-sending current safe-release response");
    else
        atl_log_info("ATL: PLC safe-release received while no safe-state latch is active");

    const bool ackSent = sendDecisionCommand(CMD_SAFE_RELEASE_ACK, false, nullptr);
    const bool unmuteSent = sendDecisionCommand(CMD_UNMUTE, true, nullptr);
    if (ackSent && unmuteSent) {
        resetAtlStateFromPlcSafeRelease();
        markSafeReleaseNoopResponseSuccess(now);
    } else {
        atl_log_err("ATL: safe-release no-latch response TX failed; state not reset");
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
                std::memset(&sender_addr, 0, sizeof(sender_addr));
                sender_len = sizeof(sender_addr);
                bytes_received = recvfrom(plcSock, ackBuf, COMMAND_PACKET_SIZE,
                                          MSG_DONTWAIT,
                                          (struct sockaddr*)&sender_addr,
                                          &sender_len);
            }

            if (bytes_received == COMMAND_PACKET_SIZE) {
                const CmdPacket* ackPkt =
                    reinterpret_cast<const CmdPacket*>(ackBuf);
                if (!isConfiguredPlcEndpoint(sender_addr, sender_len))
                {
                    atl_log_warning("SDM: dropping PLC packet from unexpected endpoint");
                    continue;
                }
                if (ackPkt->identifier != ATL_PACKET_IDENTIFIER)
                {
                    atl_log_warning("SDM: dropping PLC packet with invalid identifier");
                    continue;
                }
                if (!cmdPacketValidateCRC(ackPkt))
                {
                    atl_log_warning("SDM: dropping PLC packet with invalid CRC");
                    continue;
                }
                if (!isValidPlcInboundCommand(ackPkt->command))
                {
                    atl_log_warning("SDM: dropping PLC packet with unsupported command");
                    continue;
                }

                if (ackPkt->command == CMD_SAFE_RELEASE_REQUEST)
                {
                    handleSafeReleaseRequest();
                    continue;
                }

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
                    atl_log_warning(ackLog);
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
                    atl_log_info(ackMsg.str());
                }
                else
                {
                    ackLog = "SDM: received valid ACK with no pending command (SeqNo: " +
                             std::to_string(seqNo) + ", command: " +
                             std::string(commandName(ackPkt->command)) + ")";
                    atl_log_warning(ackLog);
                }
                if (dropAck)
                    continue;
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
 * Tier 1: warn; Tier 2/3: safe PLC hold (UNMUTE + SW_ERROR) under decisionEvalSendMtx.
 * ================================================================ */
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
        g_lastGatewayHbMissCount.store(m);
        if (m == 0U)
        {
            if (clearGatewayHbFaultActive())
                atl_log_info("HB-PSD: gateway HB recovered; safe-state latch awaits PLC release");
        }

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
            AtlFaultReport(ATL_FAULT_REPORT_GATEWAY_HB_TIER2, m, elapsed);
        }

        prevMiss = m;

        if (m >= maxF)
        {
            if (markGatewayHbFaultLatched())
            {
                atl_log_err("HB-PSD: gateway HB fault latched (tier 3) — local fail-safe, no PSS connection");
                AtlFaultReport(ATL_FAULT_REPORT_GATEWAY_HB_TIER3, m, elapsed);
                /* Keep ATL alive on gateway HB tier-3: repeat safe-hold, keep REGR active,
                 * and allow PLC safe-release after the heartbeat source recovers. */
                sendLatchedSafeHold();
            }
        }
    }
}

/* Periodic PLC decision repeat (independent of new events).
 * evaluateATLDecision() repeats UNMUTE+SW_ERROR while safe-state latched
 * or while PSS ERROR / HB tier-2 suppression is active; otherwise it runs fusion.
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
        evaluateATLDecision();
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
            atl_log_err(std::string("ATL: source reported lost, or missing from the presence report -- "
                        "triggering safe hold; sub-cause=") + kPssSubCauseName[cause] +
                        " (clears only when it is back)");
            entered |= bit;
        }
    }
    if (entered == 0U)
        return;

    AtlFaultReport(ATL_FAULT_REPORT_SW_FAIL, 0U, 0);
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
    for (const PssSubCause cause : kTracked)
    {
        if ((entered & (1U << cause)) != 0U)
            markPssSubCause(cause);
    }
    sendDecisionCommand(CMD_UNMUTE, true, nullptr);
    sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
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

    atl_log_err(std::string("ATL: a source's reports are being rejected -- triggering safe hold; "
                "sub-cause=input_unusable (clears only when PSS confirms that source usable again)"));
    AtlFaultReport(ATL_FAULT_REPORT_SW_FAIL, 0U, 0);
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
    markPssSubCause(kPssSubInputUnusable);
    sendDecisionCommand(CMD_UNMUTE, true, nullptr);
    sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
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
 * Latch on EVERY new, non-STALE SW_FAIL in the request, each in its own
 * sub-cause (SdmSwFailSubCause), and send the hold once. Called on both sides of
 * the PSS-ERROR return: a request can carry several faults, and one that rides an
 * ERROR-mode request is still a fault. Stopping at the first, or skipping them in
 * ERROR, left the others unlatched, so restoring the first permitted a release
 * while they persisted. Returns whether any SW_FAIL was applied.
 */
static bool applySwFailEvents(const DecisionRequest* request, uint8_t maxSrc)
{
    uint32_t raised = 0U;
    for (uint8_t i = 0; i < maxSrc; ++i)
    {
        const SensorData& sd = request->sensorDataSummary[i];
        if (static_cast<EventType>(sd.event.type) != SW_FAIL)
            continue;
        if (sd.event.status == STALE)
            continue;
        if (sd.event.id == lastEventId[i])
            continue;
        lastEventId[i] = sd.event.id;

        const uint8_t bit = SdmSwFailSubCause(&sd.event);
        atl_log_err(std::string("ATL: SW_FAIL received — triggering safe hold; cause=") +
                    std::to_string(static_cast<unsigned>(sd.event.cause)) +
                    swFailSubCauseNote(bit));
        raised |= bit;
    }
    if (raised == 0U)
        return false;

    AtlFaultReport(ATL_FAULT_REPORT_SW_FAIL, 0U, 0);
    std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
    for (int c = 0; c < kPssSubCauseCount; ++c)
    {
        if ((raised & (1U << c)) != 0U)
            markPssSubCause(static_cast<PssSubCause>(c));
    }
    sendDecisionCommand(CMD_UNMUTE, true, nullptr);
    sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
    return true;
}

/* ====================== DECISION LOGIC ========================== */
/* The request itself. The caller holds faultTransitionMtx: the gateway listener
 * for the whole packet, or the public entry point below. */
static void processDecisionRequestLocked(const DecisionRequest* request)
{
    atl_log_info("SDM: processing DecisionRequest id=" +
        std::to_string(request->requestId));

    /*
     * AI-PIPELINE VERDICTS FIRST -- before the PSS-ERROR return and the SW_FAIL
     * return below.
     *
     * The AI monitor reports its verdict ONCE, on change. Either early return
     * used to discard a verdict riding the same request, and nothing would ever
     * re-send it: after PSS recovered and the PLC released, this SDM ran with the
     * pipeline untrusted, skipping its events and holding its last command. The
     * hold is sent here on the latch edge -- the same physical-event hold the
     * event loop used to send -- and processing CONTINUES; the rest of the
     * request may send the hold again, which is harmless.
     */
    if (updateAiPipelineTrustFromDecisionRequest(request))
    {
        ObjectRecord forkliftRecord = {};
        const bool hasForkliftRecord = snapshotLastForkliftRecord(&forkliftRecord);
        sendPhysicalEventSafeHold(hasForkliftRecord ? &forkliftRecord : nullptr);
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
        atl_log_warning("PSS is in error mode, sending stop + software error command");
        if (!pssErrorFusionSuppressLatched.exchange(true, std::memory_order_acq_rel))
        {
            AtlFaultReport(ATL_FAULT_REPORT_PSS_ERROR, 0U, 0);
        }
        /* The request's SW_FAILs are faults in their own right; mode_error
         * clearing later must not take them with it. */
        (void)applySwFailEvents(request, maxSrc);
        {
            std::lock_guard<std::mutex> evalLock(decisionEvalSendMtx);
            markPssSubCause(kPssSubModeError);
            sendDecisionCommand(CMD_UNMUTE, true, nullptr);
            sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
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
        atl_log_info("ATL: PSS_STATUS_NOOP received; freshness refreshed without physical-state update");
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
        sendLatchedSafeHold();
        return;
    }

    bool stateChanged = false;
    bool latchEnteredThisRequest = false;

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

        const EventType et = static_cast<EventType>(sd.event.type);
        if (et == SENSOR_INVALID || et == SENSOR_VALID)
        {
            atl_log_info("ATL: Sensor health event consumed by health monitor only");
            continue;
        }

        /* Already consumed by updateAiPipelineTrustFromDecisionRequest() at
         * the top of this function, ahead of every early return. */
        if (et == AI_PIPELINE_INVALID || et == AI_PIPELINE_VALID)
            continue;

        if (!sd.isHealthy)
        {
            atl_log_info("Sensor unhealthy -- logging only, not processing for decision");
            continue;
        }

        if (!sd.isTrustedSource)
        {
            atl_log_info("AI pipeline untrusted -- logging only, not processing for decision");
            continue;
        }

        /* Configured codes are runtime values, so these cannot be case labels.
         * atlSetSubscribedEvents rejects duplicates, so at most one arm matches;
         * a code this SDM did not subscribe to matches none and is ignored,
         * which is what keeps a second ATL SDM's zone events out of this one's
         * state even if the gateway ever forwarded them. */
        if (et == atlEvent.forkliftEntered)  /* Forklift TW OUT = forklift entered trailer */
        {
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                updateLastForkliftRecordLocked(sd);
                forkliftInTrailer = true;
                atl_log_info("ATL: Forklift entered trailer (TW OUT)");
            }
            stateChanged = true;
        }
        else if (et == atlEvent.forkliftExited)  /* Forklift TW IN = forklift exited trailer */
        {
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                updateLastForkliftRecordLocked(sd);
                forkliftInTrailer = false;
                atl_log_info("ATL: Forklift exited trailer (TW IN)");
            }
            stateChanged = true;
        }
        else if (et == atlEvent.personEntered)  /* Person TW OUT = person entered trailer */
        {
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                if (personsInTrailerCount < PERSONS_IN_TRAILER_MAX) {
                    personsInTrailerCount++;
                    atl_log_info("ATL: Person entered trailer (TW OUT), personsInTrailerCount=" +
                        std::to_string(personsInTrailerCount));
                } else {
                    latchEnteredThisRequest =
                        markCountDriftFaultLatched() || latchEnteredThisRequest;
                    atl_log_warning("ATL: Person entered trailer but personsInTrailerCount saturated at " +
                        std::to_string(PERSONS_IN_TRAILER_MAX) + "; entering fault-safe lockout");
                }
            }
            stateChanged = true;
        }
        else if (et == atlEvent.personExited)  /* Person TW IN = person exited trailer */
        {
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                if (personsInTrailerCount > 0) {
                    personsInTrailerCount--;
                    atl_log_info("ATL: Person exited trailer (TW IN), personsInTrailerCount=" +
                        std::to_string(personsInTrailerCount));
                } else {
                    latchEnteredThisRequest =
                        markCountDriftFaultLatched() || latchEnteredThisRequest;
                    atl_log_warning("ATL: Person exited trailer but personsInTrailerCount already 0; entering fault-safe lockout");
                }
            }
            stateChanged = true;
        }
        else if (et == atlEvent.roiViolation)  /* Person restricted area ROI violation */
        {
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                restrictedAreaViolationByPerson = true;
                atl_log_info("ATL: Person restricted area violation set");
            }
            stateChanged = true;
        }
        else if (et == atlEvent.roiCleared)  /* Person restricted area ROI violation cleared */
        {
            {
                std::lock_guard<std::mutex> lock(masterStateMtx);
                restrictedAreaViolationByPerson = false;
                atl_log_info("ATL: Person restricted area violation cleared");
            }
            stateChanged = true;
        }
    }

    if (latchEnteredThisRequest)
    {
        ObjectRecord forkliftRecord = {};
        const bool hasForkliftRecord = snapshotLastForkliftRecord(&forkliftRecord);
        sendPhysicalEventSafeHold(hasForkliftRecord ? &forkliftRecord : nullptr);
        return;
    }

    if (safeStateLatched())
        return;

    /* Immediate decision when fusion/state changes; periodic thread repeats same logic on a timer. */
    if (stateChanged)
        evaluateATLDecision();
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
 * OWNERSHIP, stated because getting this wrong causes a two-reader race:
 * the transport handle is opened, polled, received from, reconnected and
 * closed by ONE thread -- the main thread, which runs psdGatewayEventListener()
 * inline and only calls shutdownATLControlAlgo() after that returns. Every
 * upstream send also originates there (flushPendingHbAck, emitHbAckTryOnce and
 * the periodic sendGatewayRegistration are all called from the listener loop;
 * the launch-time REGR runs before the listener starts). So there is exactly
 * one reader and one writer, and they are the same thread.
 *
 * gwSockMtx is retained rather than removed because shutdownATLControlAlgo()
 * is a public entry point that a future caller could invoke from elsewhere,
 * and because it still guards hbAckPending/hbAckPendingBuf. It is NEVER held
 * across NvPSFTransportPoll: Poll blocks for its full timeout, and holding it
 * there would stall every send for that period.
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
    /* The measured floor for this link --
     * see NvPSFGatewaySdmMinQueueDepth() for the derivation and the deployment
     * sysctl this now requires. A shortfall is a fatal startup error. */
    cfg.minQueueDepth   = NvPSFGatewaySdmMinQueueDepth();
    cfg.recvTimeoutMs   = MAIN_LOOP_POLL_TIMEOUT_MS;
    cfg.sendTimeoutMs   = 0U;
    cfg.unixPath        = gwSocketPath.c_str();
    /* Ignored for a client: the transport applies unixMode with chmod() after
     * bind(), and a client connects rather than binds. Set to 0 rather than
     * copied from the gateway, so it cannot read as a permission this end
     * enforces. Access control on this side is the SO_PEERCRED check below. */
    cfg.unixMode        = 0;
    /* The client validates the ACCEPTING process, so a rogue process
     * that pre-binds the gateway path cannot impersonate the gateway. The
     * transport performs this at connect and refuses the peer on mismatch. */
    cfg.allowedGid      = getgid();
    cfg.inetAddr        = gwInetAddr.c_str();
    cfg.inetPort        = gwInetPort;

    NvPSFTransport* t = nullptr;
    const NvPSFTransportErr oerr = NvPSFTransportOpen(&cfg, &t);
    if (oerr != NVPSF_TRANSPORT_OK || t == nullptr)
    {
        return false;
    }

    /*
     * A client must be able to send first -- the REGR is the first message on
     * this link -- and its peer identity is assigned internally, so it cannot
     * be constructed by the caller.
     */
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
 * Count-unbounded at a fixed 500 ms cadence with no exponential backoff, per
 * matching the PSS -> PSD link. Unbounded
 * because a gateway restart must not require restarting the SDM; fixed cadence
 * because backoff would make worst-case recovery depend on how long the outage
 * had already lasted, which is exactly when recovery matters most.
 *
 * NO SEQUENCE RESET. sdmGatewayDecisionSequenceReset() is deliberately not
 * called here: a reconnected gateway sends a NEW epoch, and the sequence
 * checker re-baselines on an epoch change by itself. Resetting as well would
 * discard the evidence that a gap occurred, and the acceptance criterion is
 * that an outage beyond the freshness window latches on FRESHNESS, not on a
 * spurious sequence gap.
 *
 * @return true once connected and re-registered; false only if shutdown was
 *         requested while reconnecting.
 */
static bool reconnectGatewayTransport()
{
    /* Logged on entry and on success, so a reconnect never masks an outage
     * This is the SDM-side counterpart of the PSS -> PSD link-lost pair. */
    atl_log_err("SDM: gateway upstream link lost; reconnecting");

    for (uint32_t attempt = 1U;
         !stopSDMThreads.load() && !signalShutdownRequested.load(); ++attempt)
    {
        if (g_signal_received != 0)
            signalShutdownRequested.store(true);

        if (openGatewayTransport())
        {
            /*
             * Re-register before returning. A restored connection with no
             * event-type subscription is worse than no connection: the gateway
             * would accept it, count it as a healthy client, and forward
             * nothing -- and the SDM would then latch on decision freshness
             * with a link that looks up from both ends.
             */
            if (sendGatewayRegistration(true))
            {
                lastRegistrationTime = std::chrono::steady_clock::now();
                atl_log_info("SDM: gateway upstream link re-established after " +
                             std::to_string(attempt) + " attempt(s)");
                return true;
            }
            atl_log_warning("SDM: reconnected but REGR failed; retrying");
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
 * This is permitted. Send is non-blocking at the syscall
 * level (MSG_DONTWAIT) and acquires only the transport's own leaf mutex, so
 * there is no cycle and nothing to stall. See the per-entry-point table in
 * nvpsf_transport.h under "Caller lock-order obligation": Send and Recv are
 * permitted under a caller lock; Poll, DropPeer and Close are not.
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
        atl_log_err("SDM: heartbeat ACK send failed (" +
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

/* EVENT loop receive DecisionRequest from PSD Gateway */
static void psdGatewayEventListener()
{
    char rawBuf[sizeof(NvPSDGatewayDecisionRequestPacket)];
    /* The last registration refusal reported (RGNK reason), 0 once a gateway
     * heartbeat shows this SDM registered: each reason is logged once. */
    uint8_t lastRegRefusal = 0U;

    while (!stopSDMThreads.load() && !signalShutdownRequested.load())
    {
        if (g_signal_received != 0)
            signalShutdownRequested.store(true);
        flushPendingHbAck();
        /* Periodic REGR: keep trying while latched so a restarted gateway can rediscover this SDM. */
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
         * handle's lifetime: psdGatewayEventListener() runs on the main thread
         * (:2167), reconnect and close are both on this path, and teardown
         * (:2185) runs only after this function has returned and every other
         * thread has been joined. No other thread can Close it underneath the
         * Poll below.
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
         * That is not hypothetical: the same defect was found by review on the
         * PSS -> PSD side of exactly this pattern. The listener spends nearly
         * all its idle time inside Poll, so a gateway restart almost always
         * arrives on this path rather than through Recv.
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
            atl_log_warning("SDM: poll on gateway transport failed (" +
                            std::string(NvPSFTransportErrName(perr)) + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        size_t n = 0;
        NvPSFTransportErr rerr = NVPSF_TRANSPORT_WOULD_BLOCK;
        if (!peerGone)
        {
            /* Peer-filtered by the transport: on UDP by the connect() it
             * applies internally, on UNIX_SOCKET by the connection itself
             * plus the SO_PEERCRED check made at connect time. */
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
            atl_log_warning("SDM: oversized message from gateway; discarding");
            continue;
        }
        if (rerr != NVPSF_TRANSPORT_OK)
        {
            atl_log_warning("SDM: recv on gateway transport failed (" +
                            std::string(NvPSFTransportErrName(rerr)) + ")");
            continue;
        }

        /* Check if this is a heartbeat from PSDGateway. Cast because Recv
         * reports a size_t length and the wire constant is a plain int. */
        if (n == static_cast<size_t>(NVPSD_GATEWAY_HB_MSG_SIZE) &&
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
            lastRegRefusal = 0U;

            /* Send ACK: [HBPC][seqNo]; EAGAIN leaves pending — flushPendingHbAck() each loop iteration. */
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
                    atl_log_err(std::string("SDM: the gateway refused this SDM's registration; reason=") +
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
                atl_log_err("SDM: Gateway DecisionRequest packet header validation failed — dropping");
                continue;
            }

            DecisionRequest request;
            std::memcpy(&request, &packet.request, sizeof(DecisionRequest));

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
                atl_log_warning("ATL: dropping duplicate/stale Gateway DecisionRequest packet; epoch=" +
                    std::to_string(packet.gatewayEpoch) + " txSeq=" +
                    std::to_string(packet.gatewayTxSeq));
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_INVALID)
            {
                atl_log_err("ATL: invalid Gateway DecisionRequest packet sequence — dropping");
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_GAP)
            {
                if (markGatewayDecisionSequenceFaultLatched(packet.gatewayEpoch, packet.gatewayTxSeq))
                {
                    atl_log_err("ATL: Gateway DecisionRequest sequence gap; entering safe hold, epoch=" +
                        std::to_string(packet.gatewayEpoch) + " txSeq=" +
                        std::to_string(packet.gatewayTxSeq));
                    sendLatchedSafeHold();
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
            atl_log_warning("SDM: received partial packet (" + std::to_string(n) +
                " bytes, expected " +
                std::to_string(sizeof(NvPSDGatewayDecisionRequestPacket)) + ")");
        }
        /* Any other length is neither a heartbeat nor a decision packet on
         * this link; loop back rather than guessing at it. */
    }
}

/*
 * Send REGR registration to the gateway so it forwards subscribed event types.
 *
 * The poll(POLLOUT) ladder this replaces existed only because a raw
 * O_NONBLOCK socket offered no other way to wait for writability. The
 * transport's send is non-blocking by contract, and the only retryable outcome
 * is WOULD_BLOCK, so the retry reduces to "sleep briefly and try again",
 * bounded by REGR_SEND_MAX_ATTEMPTS.
 *
 * @param retryUntilWritable true at launch and on reconnect, where a REGR that
 *        does not land leaves the SDM subscribed to nothing; false on the
 *        periodic path, where the next REG_RETRY_INTERVAL_MS tick retries and
 *        blocking the listener would delay decision handling.
 */
static bool sendGatewayRegistration(bool retryUntilWritable)
{
    constexpr size_t bufSize = 4 + 1 + ATL_SUBSCRIBED_COUNT * sizeof(uint32_t);
    char buf[bufSize];
    std::memcpy(buf, NVPSD_GATEWAY_REG_MAGIC, 4);
    /* Wire format: one byte count (gateway reads as uint8_t); avoid signed char. */
    buf[4] = static_cast<unsigned char>(ATL_SUBSCRIBED_COUNT);
    EventType subscribed[ATL_SUBSCRIBED_COUNT];
    atlSubscribedEvents(subscribed);
    for (uint8_t i = 0; i < ATL_SUBSCRIBED_COUNT; ++i)
    {
        uint32_t val = htonl(static_cast<uint32_t>(subscribed[i]));
        std::memcpy(buf + 5 + i * sizeof(uint32_t), &val, sizeof(uint32_t));
    }

    const int maxAttempts = retryUntilWritable ? REGR_SEND_MAX_ATTEMPTS : 1;

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        const NvPSFTransportErr err = gatewaySend(buf, bufSize);

        if (err == NVPSF_TRANSPORT_OK)
        {
            atl_log_info("SDM: sent registration (" + std::to_string(ATL_SUBSCRIBED_COUNT) + " event types) to gateway");
            return true;
        }
        if (err != NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            /* PEER_GONE or a hard error. Not retried here: the listener owns
             * reconnection, and spinning the retry budget against a link that
             * is down would only delay entering it. */
            atl_log_err("SDM: failed to send REGR to gateway (" +
                        std::string(NvPSFTransportErrName(err)) + ")");
            return false;
        }
        if (!retryUntilWritable)
            return false;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(REGR_SEND_RETRY_SLEEP_MS));
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
                         std::uint32_t decisionRepeatIntervalMs,
                         std::uint32_t hbStaleMs,
                         std::uint32_t hbPeriodMs,
                         std::uint32_t decisionFreshnessTimeoutMs,
                         NvPSFTransportKind gatewayTransport,
                         const std::string& gatewaySocketPath,
                         const std::string& sensorPipelinesConfigPath)
{
    /* Refuse before anything else: with no binding this process would register
     * for nothing meaningful and decide on evidence it never subscribed to.
     * Checked here rather than only in main() because this is a public entry
     * point shipped in the dev package, so an out-of-tree caller reaches it
     * without passing through the CLI at all. Freeze the binding under the
     * setter's lock before any reader exists; runtime lookups stay read-only. */
    {
        std::lock_guard<std::mutex> lock(atlBindingMtx);
        if (atlEventSource.empty() || atlBindingFrozen)
        {
            std::cerr << "error: ATL requires a valid startup event binding before launch\n";
            return -1;
        }
        atlBindingFrozen = true;
    }
    atl_log_open();
    atlLogStartupBindings();

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

    if (hbStaleMs < kHbTimingMsMin || hbStaleMs > kHbTimingMsMax ||
        hbPeriodMs < kHbTimingMsMin || hbPeriodMs > kHbTimingMsMax)
    {
        atl_log_err("hbStaleMs and hbPeriodMs must be in " +
                    std::to_string(kHbTimingMsMin) + ".." +
                    std::to_string(kHbTimingMsMax));
        atl_log_close();
        return -1;
    }

    if (!sdmDecisionFreshnessTimeoutMsIsValid(decisionFreshnessTimeoutMs))
    {
        atl_log_err("decisionFreshnessTimeoutMs must be in " +
            std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN) + ".." +
            std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX));
        atl_log_close();
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
            atl_log_err("ATL: cannot load the reviewed sensor set; refusing to start: " + setErr);
            std::cerr << "error: " << setErr << "\n";
            atl_log_close();
            return -1;
        }
        /* Unconditional: a set that silently shrank is only visible if the
         * runtime states what it resolved. */
        atl_log_info("ATL: reviewed sensor set from " + sensorPipelinesConfigPath + ": " +
                     describeReviewedSensorSet(gReviewedSensors));
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

    /*
     * --- PSD Gateway upstream endpoint: send REGR, receive DecisionRequests + HB ---
     *
     * The socket/fcntl/bind/connect ladder this replaces is now inside the
     * transport, along with the O_NONBLOCK the old code set by hand and the
     * peer filtering the old connect() was really there to obtain.
     *
     * The plcSock cleanup ladder below is preserved verbatim: every failure
     * path from here on still closes plcSock before returning, because nothing
     * else will.
     */
    gwTransportKind = gatewayTransport;
    gwSocketPath    = gatewaySocketPath;
    gwInetAddr      = gatewayIP;
    gwInetPort      = gatewayPort;

    /*
     * A failure to open here is NOT fatal, and that is a deliberate change of
     * behaviour rather than a missing error path.
     *
     * On UDP an unconnected peer was invisible: connect() on a datagram socket
     * only records an address, so it succeeded whether or not a gateway
     * existed. On UNIX_SOCKET it does not -- connect() fails outright until
     * the gateway has bound the path. Treating that as fatal would mean an SDM
     * launched before the gateway exits instead of waiting, which is exactly
     * what the "SDM started before the gateway retries until the path appears
     * and then registers" criterion forbids. The listener's reconnect loop
     * owns the wait.
     */
    if (openGatewayTransport())
    {
        if (sendGatewayRegistration(true))
            lastRegistrationTime = std::chrono::steady_clock::now();
        else
        {
            atl_log_warning("SDM: initial REGR did not complete; will retry in event loop");
            /* Elapsed time >= REG_RETRY_INTERVAL_MS so first listener iteration retries soon. */
            lastRegistrationTime = std::chrono::steady_clock::now() -
                std::chrono::milliseconds(REG_RETRY_INTERVAL_MS);
        }
    }
    else
    {
        atl_log_warning("SDM: psdGateway transport not available at launch (" +
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
    {
        std::lock_guard<std::mutex> stateLock(masterStateMtx);
        resetLastForkliftRecordLocked();
    }
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
    g_decisionFreshnessTimeoutMs.store(decisionFreshnessTimeoutMs, std::memory_order_relaxed);

    g_decisionRepeatIntervalMs.store(decisionRepeatIntervalMs, std::memory_order_relaxed);

    /* --- Start threads --- */
    ackHandlerThread = std::thread(ackHandlerLoop);
    heartbeatThread  = std::thread(heartbeatTransmitter);
    hbWatchdogThread = std::thread(gatewayHeartbeatWatchdog);
    decisionFreshnessWatchdogThread = std::thread(decisionFreshnessWatchdog);

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
    cfg << "; gateway HB stale_ms=" << hbStaleMs
        << " period_ms=" << hbPeriodMs
        << "; decision_freshness_timeout_ms=" << decisionFreshnessTimeoutMs;
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
    joinThread(decisionFreshnessWatchdogThread);
    joinThread(periodicDecisionThread);

    /* Close sockets */
    {
        std::lock_guard<std::mutex> lock(plcSocketMtx);
        if (plcSock >= 0) { close(plcSock); plcSock = -1; }
    }
    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        hbAckPending = false;
    }
    /* Not under gwSockMtx: Close tears down descriptors and must not run with
     * a lock held that a send path could be waiting on. Safe here because
     * every thread that touches the handle has been joined above, and the
     * listener returned before this function was called. */
    closeGatewayTransport();

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

    if (safeStateLatched())
    {
        sendDecisionCommand(CMD_UNMUTE, true, nullptr);
        sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
        return;
    }

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
    ObjectRecord forkliftRecord = {};
    bool hasForkliftRecord = false;

    {
        std::lock_guard<std::mutex> stateLock(masterStateMtx);
        forkliftOut = forkliftInTrailer;
        personOut = personsInTrailerCount;
        restrictedViol = restrictedAreaViolationByPerson;
        if (lastForkliftRecordValid) {
            forkliftRecord = lastForkliftRecord;
            hasForkliftRecord = true;
        }
    }

    /* Re-check immediately before emitting a normal decision; another thread
     * may have latched fault-safe state while this path waited on state. */
    if (safeStateLatched() ||
        pssErrorFusionSuppressLatched.load(std::memory_order_acquire) ||
        gatewayHbTier2SafeHoldBand())
    {
        sendDecisionCommand(CMD_UNMUTE, true, nullptr);
        sendDecisionCommand(CMD_SW_ERROR, true, nullptr);
        return;
    }

    if (gatewayHbSuppressFusionDecisions())
        return;

    // Forklift safety MUTED + loading allowed when: forklift in trailer, zero persons in trailer,
    // and no person restricted area violation. Otherwise UNMUTE.
    if (forkliftOut && personOut == 0 && !restrictedViol)
    {
        atl_log_info("ATL: Forklift in trailer, no persons in trailer, no restricted violation - MUTE (Allow Loading)");
        sendDecisionCommandWithObject1(CMD_MUTE, true,
            hasForkliftRecord ? &forkliftRecord : nullptr);
    }
    else
    {
        atl_log_info("ATL: Unmute - forkliftInTrailer=" + std::string(forkliftOut ? "1" : "0") +
            " personsInTrailer=" + std::to_string(personOut) +
            " restrictedAreaViolationByPerson=" + std::string(restrictedViol ? "1" : "0"));
        sendDecisionCommandWithObject1(CMD_UNMUTE, true,
            hasForkliftRecord ? &forkliftRecord : nullptr);
    }
}
