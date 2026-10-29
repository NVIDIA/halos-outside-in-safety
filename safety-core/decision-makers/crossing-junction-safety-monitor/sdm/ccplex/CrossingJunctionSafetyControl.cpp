/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Crossing-Junction-Safety-Monitor SDM.
 *
 * Sounds the junction siren while a forklift that entered the crossing shares it
 * with a person on foot. The rule is one conjunction:
 *
 *     siren = A1 AND A2
 *     A1  a Forklift crossed a tripwire heading into the junction and has not
 *         left the junction ROI     (latched on the crossing, released on exit)
 *     A2  at least one Person is inside the junction ROI
 *
 * The four states of the control state machine -- Clear, PersonInRoi,
 * ForkliftInRoi and Alarm -- are the four combinations of those two conditions,
 * and are derived rather than stored. What the simplification deliberately drops
 * is worth stating, because each omission widens the alarm: there is no
 * crossing-axis test, so the two parties need not be on lanes that cross; no
 * direction test on the person, only on the forklift; no separate head-on case,
 * so a person and forklift in the same aisle alarm whether they face each other
 * or not; and no confirmation delay, so the siren is raised on the crossing
 * frame itself. Every one of those makes the monitor more willing to alarm, not
 * less, which is the correct direction for a safety function to err in.
 *
 * Structurally this SDM is the proximity-forklift-person SDM with its decision
 * core replaced: the registration, heartbeat, ACK, safe-release, freshness and
 * fail-safe plumbing is deliberately identical, so a fix to any of that applies
 * to both. Only the event set, the decision logic and the packet identifier
 * (0xA7) differ.
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
#include <cmath>
#include <mutex>
#include <map>
#include <csignal>
#include <condition_variable>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "CrossingJunctionSafetyControl.h"
#include "NvPSDGatewayProtocol.h"
#include "nvpsf_transport.h"
#include "pss_message_validate.h"
#include "sdm_decision_request_state.h"
#include "sdm_decision_freshness.hpp"
#include "sdm_gateway_decision_sequence.hpp"

/* Simple SDM logging: console + optional log file (no NvPSB dependency) */
static std::mutex cjsLogMtx;
static FILE* cjsLogFile = nullptr;

static void cjs_log_open(void)
{
    std::lock_guard<std::mutex> lock(cjsLogMtx);
    if (!cjsLogFile) {
        cjsLogFile = fopen("cjs_sdm.log", "a");
        if (cjsLogFile)
            setvbuf(cjsLogFile, nullptr, _IOLBF, 0);
    }
}

static void cjs_log_close(void)
{
    std::lock_guard<std::mutex> lock(cjsLogMtx);
    if (cjsLogFile) {
        fclose(cjsLogFile);
        cjsLogFile = nullptr;
    }
}

/* fprintf to stdout/stderr and optional cjs_sdm.log (under cjsLogMtx). */
static void cjs_log(const char* level, const char* msg)
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

    std::lock_guard<std::mutex> lock(cjsLogMtx);
    const bool toStderr = (strcmp(level, "ERR") == 0 || strcmp(level, "WARNING") == 0);
    FILE* out = toStderr ? stderr : stdout;
    // The launcher points both stdout and stderr at one log file. stderr is
    // unbuffered while a redirected stdout is block-buffered, so an unflushed
    // stdout buffer would otherwise be cut in half by the next stderr write --
    // which is exactly what junction phase transitions (WARNING) do to the
    // surrounding per-decision INFO stream. Draining the other stream first and
    // this one after keeps whole lines together in file order.
    fflush(toStderr ? stdout : stderr);
    fprintf(out, "%s [CJS][%s] %s\n", prefix, level, msg);
    fflush(out);
    if (cjsLogFile) {
        fprintf(cjsLogFile, "%s [CJS][%s] %s\n", prefix, level, msg);
        fflush(cjsLogFile);
    }
}

static void cjs_log_info(const std::string& msg)    { cjs_log("INFO", msg.c_str()); }
static void cjs_log_warning(const std::string& msg) { cjs_log("WARNING", msg.c_str()); }
static void cjs_log_err(const std::string& msg)     { cjs_log("ERR", msg.c_str()); }

#define HEARTBEAT_PERIOD_SEC   5
#define ACK_TIMEOUT_SECONDS    3

/* Valid range for launchCrossingJunctionSafetyControlAlgo(decisionRepeatIntervalMs). 0
 * disables the periodic re-assert; non-zero values are clamped to [100 ms, 36000 ms] so
 * we neither flood the PLC nor let the last decision age past a typical ACK window. */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                      = 100U;
static constexpr std::uint32_t kHbTimingMsMax                      = 600000U;
static constexpr int SAFE_RELEASE_MIN_INTERVAL_MS                  = 1000;
#define ACK_CLEANUP_SECONDS    10
#define ACK_MONITOR_INTERVAL_S 1
#define ACK_RECEIVER_SLEEP_MS  100
#define MAIN_LOOP_POLL_TIMEOUT_MS 50

/* The junction alarm rests on exactly two conditions, named A1 and A2 in the
 * factory-crossing state machine this SDM implements:
 *
 *   A1  forkliftEngaged  a Forklift crossed a tripwire heading into the junction
 *                        and has not left it again. Latched on the crossing,
 *                        released once the vehicle is out of the ROI.
 *   A2  personInRoi      at least one Person is inside the junction ROI. The
 *                        person's own direction is never considered.
 *
 * siren = A1 AND A2. Three events carry those conditions from mdx-client:
 *
 *   EVENT_0  the inbound crossing itself, an edge. From the mdx-events
 *             TripEvent for tripwire-id-1 with direction OUT and a Forklift.
 *             "OUT" is this deployment's outward-normal convention for the wire,
 *             and is the direction that means "entering the junction"; the
 *             opposite crossing arrives as IN and is deliberately unmapped.
 *   EVENT_1  a Person is in the junction ROI, a level. From the mdx-frames ROI
 *             restrictedAreaViolation for roi-id-1.
 *   EVENT_2  a Forklift is in the junction ROI, also a level, and what releases
 *             the A1 latch when it stops arriving.
 *   EVENT_3  a Person is *not* in the junction ROI, the positive counterpart of
 *             EVENT_1, asserted per frame by mdx-client.
 *   EVENT_4  the same vacated report for a Forklift, and what normally releases
 *             the A1 latch.
 *
 * The two ROI conditions are levels rather than ROI ENTRY/EXIT edges on purpose.
 * mdx-events does carry ENTRY/EXIT pairs, but pairing them requires the tracker
 * ID to survive from entry to exit, and IDs churn heavily in this deployment: a
 * dropped EXIT would leave the junction occupied forever.
 *
 * The vacated reports carry no such risk, because they are not edges either --
 * mdx-client restates them every frame from the same ROI payload that produces
 * the occupied ones, so a lost report is corrected by the next frame rather than
 * latching. They are what actually releases a level here; the hold windows below
 * remain only as a backstop for a sensor that stops reporting altogether, which
 * is the one case no report of either polarity can cover.
 *
 * Codes start at EVENT_0 rather than continuing past the proximity SDMs because
 * EVENT_18..EVENT_23 are the range PSS schema v4 added: a v3 peer does not have
 * them and would misread SafetyEvent.type on the wire (see PSS_SCHEMA_VERSION in
 * pss_protocol.h). EVENT_0..EVENT_4 carry the same value under both versions, so
 * this module does not require the whole stack to be rebuilt in lockstep.
 *
 * They stay distinct from the proximity SDMs' EVENT_8..EVENT_17, so those can
 * still share a PSD Gateway with this one. They do overlap ATL's EVENT_0..EVENT_5,
 * which is safe only because the two never run together: ATL loads
 * event_mapping_atl.pb.txt, while --app cjs loads
 * crossing_junction_safety_event_mapping.pb.txt (or a combined mapping carrying
 * these rules too, when a proximity SDM shares one mdx-client). Running ATL and
 * this module against one gateway would cross-talk. */
#define EVENT_0_FORKLIFT_TRIPWIRE_INBOUND   0  // Forklift crossed tripwire into junction
#define EVENT_1_PERSON_IN_JUNCTION_ROI      1  // Person occupies the junction ROI
#define EVENT_2_FORKLIFT_IN_JUNCTION_ROI    2  // Forklift occupies the junction ROI
#define EVENT_3_PERSON_VACATED_JUNCTION     3  // Person is not in the junction ROI
#define EVENT_4_FORKLIFT_VACATED_JUNCTION   4  // Forklift is not in the junction ROI

/* Heartbeat protocol (NvPSDGatewayProtocol.h) */
/* Re-register with gateway periodically so we recover after gateway restart (in-memory state lost). */
static constexpr int REG_RETRY_INTERVAL_MS = 30000;

/* A REGR that never lands leaves this SDM subscribed to nothing, so the launch
 * and reconnect paths retry a blocked send rather than accepting the loss. The
 * periodic path does not: its next tick retries anyway, and blocking the
 * listener there would delay decision handling. */
static constexpr int REGR_SEND_MAX_ATTEMPTS   = 32;
static constexpr int REGR_SEND_RETRY_SLEEP_MS = 5;
/* Spacing between reconnect attempts once the upstream link has gone. */
static constexpr int GW_RECONNECT_INTERVAL_MS = 500;

/* Event types Crossing-Junction-Safety-Monitor subscribes to (must match
 * onEventNotificationReceive). The proximity SDMs' event codes are deliberately
 * not subscribed: a forklift being near a person elsewhere on the floor is their
 * decision to make, not this one's. */
static constexpr EventType CJS_SUBSCRIBED_EVENTS[] = { EVENT_0, EVENT_1, EVENT_2,
                                                        EVENT_3, EVENT_4, SW_FAIL };
static constexpr uint8_t  CJS_SUBSCRIBED_COUNT =
    sizeof(CJS_SUBSCRIBED_EVENTS) / sizeof(CJS_SUBSCRIBED_EVENTS[0]);

struct SafetyRelevantSensor {
    uint8_t pipelineId;
    const char* sensorName;
};

/* Reviewed sensor set from pkg/sensor_config.conf. Gateway auto-subscribes
 * SENSOR_INVALID/SENSOR_VALID; this SDM uses pipelineID as the safety key.
 * NOTE: this deployment is a 2-camera setup (cam1_1/cam2_1); update
 * to match the sensors actually configured for a given deployment. */
static constexpr SafetyRelevantSensor kSafetyRelevantSensors[] = {
    {1U, "Camera"},
    {2U, "Camera_01"},
};
static constexpr uint32_t kSafetyRelevantSensorMask =
    (1U << (kSafetyRelevantSensors[0].pipelineId - 1U)) |
    (1U << (kSafetyRelevantSensors[1].pipelineId - 1U));
static_assert(kSafetyRelevantSensors[0].pipelineId >= 1U &&
              kSafetyRelevantSensors[1].pipelineId >= 1U,
              "Pipeline IDs are one-based");
static_assert(kSafetyRelevantSensors[0].pipelineId <= 8U &&
              kSafetyRelevantSensors[1].pipelineId <= 8U,
              "Pipeline IDs must stay within sensor_config bounds");
static_assert(kSafetyRelevantSensors[0].pipelineId != kSafetyRelevantSensors[1].pipelineId,
              "Pipeline IDs must be unique");

/* PLC command socket */
static int plcSock = -1;
static struct sockaddr_in plcAddr = {};
/* CmdPacket.seq is uint16_t; use low 16 bits of this monotonic counter for a full 0..65535 wrap. */
static std::atomic<uint32_t> cmdSeqNo{0};
static std::mutex plcSocketMtx;

/* Optional second fan-out target: VST/VIOS halo command listener. Same wire
 * format as the PLC packet (magic 0xA7, same CRC regions/opcodes), sent on a
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
/* Re-reads the junction state machine on a fixed tick. Separate from the
 * periodic re-assert thread above because the two answer different questions:
 * that one re-sends the standing decision so a lost datagram cannot leave the
 * PLC holding a stale command, while this one asks whether the decision itself
 * has changed now that time has passed. The siren's release depends on the
 * latter, so it must not be tied to the re-assert period. */
static std::thread       junctionEvalThread;
/* Shutdown-aware sleep for the periodic decision loop: waiting on this CV
 * instead of sleep_for lets shutdownCrossingJunctionSafetyControlAlgo() return in O(ms)
 * rather than waiting up to one full decisionRepeatIntervalMs interval. */
static std::mutex              cjsPeriodicDecisionWaitMtx;
static std::condition_variable cjsPeriodicDecisionCv;
static std::mutex              cjsJunctionEvalWaitMtx;
static std::condition_variable cjsJunctionEvalCv;

/* Serializes decision sends (event-driven vs HB watchdog vs periodic repeat).
 * Ordering: always lock decisionSendMtx first, then cjsStateMtx. */
static std::mutex                            decisionSendMtx;

/* Last committed decision command, published by onEventNotificationReceive()
 * and replayed by the periodic repeat thread. Guarded by cjsStateMtx so
 * the periodic loop observes a consistent value while the event path is
 * mid-update. CMD_NORMAL is the safe initial value before the first event. */
static std::mutex                            cjsStateMtx;
static unsigned char                         cjsActiveCommand = CMD_NORMAL;
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
 * pass rather than dropped: enough missed ACKs trip the gateway's tier ladder
 * against an SDM that is in fact healthy. Guarded by gwSockMtx. */
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
/* Tier-2 gateway HB safe-hold: periodic/event send CMD_STOP+CMD_SW_ERROR while true; fusion keeps cjsActiveCommand. */
static std::atomic<bool>                     hbTier2SafeHoldActive{false};
/* Output suppression while any PSS sub-cause is active. The safe-state latch
 * itself clears only on PLC safe-release. */
static std::atomic<bool>                     pssErrorFusionSuppressLatched{false};
/*
 * PSS fault, split by cause -- the same model as the ATL and proximity SDMs.
 *
 * One flag could not express "this evidence recovered": every PSS cause cleared
 * on the same predicate, so the first usable request after a lost Safety Monitor
 * cleared the fault while the monitor was still gone. Every sub-cause still
 * raises the single kSafeLatchPss bit; what differs is what CLEARS each one.
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
 *                report that omits one this SDM knows of.
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
    kPssSubMonitorLost,           /* the Safety Monitor was lost */
    kPssSubPerceptionMonitorLost, /* the Perception Monitor was lost */
    kPssSubMdxLost,               /* a numbered MDX client instance was lost */
    kPssSubInputUnusable,         /* a source's reports are being rejected */
    kPssSubCauseCount
};
static std::atomic<bool>                     pssSubCauseActive[kPssSubCauseCount]{};
static const char* const                     kPssSubCauseName[kPssSubCauseCount] = {
    "pss_mode_error", "gateway_comm", "sw_fail", "safety_monitor_lost",
    "perception_monitor_lost", "mdx_client_lost", "input_unusable"
};
/* The source-lost sub-causes are applied through SdmSourcePresenceEvaluate(),
 * shared with the other SDMs, which speaks SDM_PSS_SUB_* bits. */
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
 * be accepted in a state that never existed upstream. Held by the gateway
 * listener for each whole packet -- the sequence and freshness clears that
 * precede the request as much as the request itself -- and taken first by
 * handleSafeReleaseRequest(), it makes the release see the packet either not at
 * all or completely, as if both were handled in one task.
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
/* AI-pipeline distrust is a SAFE-STATE CAUSE, not merely an input filter. PSS
 * reports it pipeline-wide, and while it holds nothing arriving from any MDX
 * client can be trusted to describe the junction. Driving the PLC from the last
 * verdict would keep the machine running on evidence known to be bad, so the SDM
 * latches instead. Release is gated on the ACTIVE flag below, which only
 * AI_PIPELINE_VALID clears. Same contract as the ATL and proximity SDMs. */
static constexpr uint32_t                    kSafeLatchAiPipelineInvalid = 1U << 5;
static std::atomic<uint32_t>                 safeStateLatchCauses{0U};
static std::mutex                            safeStateLatchMtx;
static std::atomic<uint32_t>                 failedSafetyRelevantSensorMask{0U};
static std::atomic<bool>                     allSafetyRelevantSensorsFailedActive{false};
static std::atomic<bool>                     aiPipelineInvalidActive{false};

/* SAIM seeds every sensor SENSOR_INVALID with cause SAIM_INIT before its first
 * frame, which is initialization in progress rather than a fault. Without this
 * suppression the all-safety-relevant-sensors-failed latch fires on every boot
 * and needs a validated manual release before the system can run.
 *
 * Matches the ATL and ATL_PROXIMITY CCPLEX SDMs. */
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

    cjs_log_info(std::string("CJS: PSS sub-cause cleared: ") + kPssSubCauseName[cause] +
                 "; latch awaits PLC release");
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
        cjs_log_err("CJS: AI pipeline reported INVALID; entering safe hold");
    }
    return firstEdge;
}

/* AI_PIPELINE_VALID clears the ACTIVE condition only. The latch itself survives
 * until a PLC safe-release is accepted -- the cause going away is not consent to
 * resume. */
static void clearAiPipelineInvalidActive()
{
    std::lock_guard<std::mutex> lock(safeStateLatchMtx);
    if (aiPipelineInvalidActive.exchange(false, std::memory_order_acq_rel))
    {
        cjs_log_info("CJS: AI pipeline reported VALID; condition cleared, latch awaits PLC release");
    }
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
    cjs_log_err("CJS: All safety-relevant sensors failed; entering safe hold");
    return true;
}

static void clearAllSafetyRelevantSensorsFailedActiveIfRecovered(uint32_t failedMask)
{
    if ((failedMask & kSafetyRelevantSensorMask) == kSafetyRelevantSensorMask)
        return;

    if (allSafetyRelevantSensorsFailedActive.exchange(false, std::memory_order_acq_rel))
    {
        cjs_log_info("CJS: All safety-relevant sensors failed condition cleared; latch awaits PLC release");
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
            cjs_log_info("CJS: safety-sensor health change pipelineID=" +
                std::to_string(pipelineId) +
                (failed ? " state=FAILED" : " state=RECOVERED") +
                " cause=" + std::to_string(static_cast<unsigned>(sd.event.cause)));
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

/* The index below is bounded by COMMAND_NUM_OBJECTS, which sizes the outgoing
 * packet's record array (crossing_junction_safety_cmd_pkt.h), yet it also
 * indexes the inbound PSS metadata arrays (pss_protocol.h). Nothing in either
 * header ties the two together, so widening the packet -- or PSS narrowing its
 * metadata -- would turn that guard into an out-of-bounds read with no build
 * error. State the dependency here instead: coordinates[] is larger and keeps
 * its own guard below, so assert the two that the packet count alone bounds. */
static_assert(COMMAND_NUM_OBJECTS <=
                  static_cast<int>(sizeof(EventFusionMetadata::objectType) /
                                   sizeof(EventFusionMetadata::objectType[0])),
              "COMMAND_NUM_OBJECTS exceeds EventFusionMetadata::objectType[]");
static_assert(COMMAND_NUM_OBJECTS <=
                  static_cast<int>(sizeof(EventFusionMetadata::objectID) /
                                   sizeof(EventFusionMetadata::objectID[0])),
              "COMMAND_NUM_OBJECTS exceeds EventFusionMetadata::objectID[]");

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

/* Every event this SDM consumes names a single object -- a tripwire crossing or
 * an ROI occupancy, never a pair -- so mdx-client fills metadata slot 0 and
 * leaves slot 1 zeroed, and the records are copied straight through.
 *
 * The pair-shaping below is retained from the proximity SDMs so the packet
 * layout stays identical across the metropolis family: a PLC-side parser reads
 * the same 20-byte records at the same offsets whichever SDM sent them. It is a
 * no-op for single-object events, since a zeroed slot 1 is neither PERSON-typed
 * nor carries an ID. */
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
        /* Forklift <-> Forklift (or any other symmetric pair): pass both
         * metadata slots through unchanged. */
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

/* Defined with the junction state machine below; an alarm packet names the pair
 * from that state rather than from whatever the current batch happened to hold. */
static bool fillJunctionAlarmObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS]);

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

    pkt.identifier      = CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER;
    pkt.seq             = seqNo;
    pkt.command         = command;
    pkt.ts_seconds      = tsSec;
    pkt.ts_microseconds = tsMicro;

    if (command != CMD_JUNCTION_ALARM || !fillJunctionAlarmObjectRecords(pkt.objects))
        fillObjectRecords(pkt.objects, request, winningSlot);

    pkt.crc32 = cmdPacketCRC32(&pkt);

    bool shouldPrintAndTrack = false;
    const char* cmdType = commandName(command);
    if (command == CMD_STOP || command == CMD_JUNCTION_ALARM || command == CMD_NORMAL ||
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
        cjs_log_info(logMsg.str());
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
                cjs_log_err("Failed to send decision command");
            }
        }
    }

    /* Best-effort fan-out to VST's halo listener; does not affect sentOk/ACK
     * tracking (that remains solely tied to the PLC send above). */
    if (haloEnabled.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> haloLock(haloSocketMtx);
        if (haloSock >= 0) {
            const ssize_t haloSent = sendto(haloSock, &pkt, sizeof(pkt), 0,
                                            (struct sockaddr*)&haloAddr, sizeof(haloAddr));
            if (haloSent != static_cast<ssize_t>(sizeof(pkt))) {
                cjs_log_warning("Failed to send decision command to VST halo listener");
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

/* No gateway-endpoint check here: the upstream link is peer-filtered by the
 * transport itself -- by the connection on UNIX_SOCKET, plus the SO_PEERCRED
 * check made at connect time. isConfiguredPlcEndpoint() above is still needed
 * because the PLC link remains an unconnected UDP socket. */

static bool isValidPlcInboundCommand(uint8_t command)
{
    return command == CMD_HEARTBEAT ||
           command == CMD_STOP ||
           command == CMD_JUNCTION_ALARM ||
           command == CMD_NORMAL ||
           command == CMD_HW_ERROR ||
           command == CMD_SW_ERROR ||
           command == CMD_SAFE_RELEASE_REQUEST;
}

static void resetCjsStateFromPlcSafeRelease()
{
    std::lock_guard<std::mutex> stateLock(cjsStateMtx);
    cjsActiveCommand = CMD_NORMAL;
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
    /* The AI-pipeline cause stays active (and release denied) until
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

static void sendCjsSafeHold(const DecisionRequest* request = nullptr,
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
        cjs_log_info("CJS: DecisionRequest freshness recovered; requestId=" +
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
        cjs_log_info("CJS: Gateway DecisionRequest sequence recovered; epoch=" +
            std::to_string(gatewayEpoch) + " txSeq=" +
            std::to_string(gatewayTxSeq) + "; latch awaits PLC release");
    }
}

static void sendLatchedCjsSafeHold()
{
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    sendCjsSafeHold();
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
            cjs_log_err("CJS: DecisionRequest freshness timeout; entering safe hold, elapsed_ms=" +
                std::to_string(elapsedMs));
            sendLatchedCjsSafeHold();
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
        cjs_log_warning("CJS: PLC safe-release denied; active safe-state fault source remains, active_causes=" +
            std::to_string(active));
        sendDecisionCommand(CMD_SAFE_RELEASE_DENIED, false, nullptr);
        sendCjsSafeHold();
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
            /* Only return local state to normal after the
             * peer can observe the accepted release sequence. Do not clear a
             * new latch cause that arrives while TX is in flight. */
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
                resetCjsStateFromPlcSafeRelease();
                hbTier2SafeHoldActive.store(false, std::memory_order_release);
                cjs_log_info("CJS: PLC safe-release accepted; returning to normal mode");
                markSafeReleaseNoopResponseSuccess(now);
            } else {
                cjs_log_warning("CJS: safe-release response sent but latch retained due to concurrent fault");
                sendCjsSafeHold();
            }
        } else {
            cjs_log_err("CJS: safe-release response TX failed; latch retained and state not reset");
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool rateLimited = safeReleaseNoopResponseInRateWindow(now);
    if (rateLimited)
        cjs_log_warning("CJS: rate-limited PLC safe-release retry; re-sending current safe-release response");
    else
        cjs_log_info("CJS: PLC safe-release received while no safe-state latch is active");

    const bool ackSent = sendDecisionCommand(CMD_SAFE_RELEASE_ACK, false, nullptr);
    const bool normalSent = sendDecisionCommand(CMD_NORMAL, true, nullptr);
    if (ackSent && normalSent) {
        resetCjsStateFromPlcSafeRelease();
        markSafeReleaseNoopResponseSuccess(now);
    } else {
        cjs_log_err("CJS: safe-release no-latch response TX failed; state not reset");
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
                /* MSG_TRUNC so the test below is the exact-size test it reads
                 * as. Without it a datagram longer than the buffer is copied
                 * short and still reports COMMAND_PACKET_SIZE, so an oversized
                 * packet whose first 64 bytes happen to carry our identifier
                 * and a matching CRC would be accepted as a well-formed one.
                 * With it recvfrom() reports the datagram's true length and an
                 * oversized packet is dropped alongside the short ones. */
                bytes_received = recvfrom(plcSock, ackBuf, COMMAND_PACKET_SIZE,
                                          MSG_DONTWAIT | MSG_TRUNC,
                                          (struct sockaddr*)&sender_addr,
                                          &sender_len);
            }
            if (bytes_received == COMMAND_PACKET_SIZE) {
                const CmdPacket* ackPkt =
                    reinterpret_cast<const CmdPacket*>(ackBuf);

                if (!isConfiguredPlcEndpoint(sender_addr))
                {
                    cjs_log_warning("SDM: dropping PLC packet from unexpected endpoint");
                    continue;
                }
                  if (ackPkt->identifier != CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER) {
                      /* Both values are formatted from the packet and the
                       * constant, so the message cannot drift from the
                       * identifier this build actually enforces. */
                      char hexId[8];
                      char hexExpected[8];
                      snprintf(hexId, sizeof(hexId), "0x%02X", ackPkt->identifier);
                      snprintf(hexExpected, sizeof(hexExpected), "0x%02X",
                               CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER);
                      cjs_log_warning(std::string("ACK: invalid identifier ") + hexId +
                                      " (expected " + hexExpected + ") — dropped");
                } else if (!cmdPacketValidateCRC(ackPkt)) {
                    cjs_log_warning("ACK: CRC-32 mismatch (seq=" +
                                    std::to_string(ackPkt->seq) + ") — dropped");
                } else if (!isValidPlcInboundCommand(ackPkt->command)) {
                    cjs_log_warning("SDM: dropping PLC packet with unsupported command");
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
                        cjs_log_warning(ackLog);
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
                        cjs_log_info(ackMsg.str());
                    }
                    else
                    {
                        ackLog = "SDM: received valid ACK with no pending command (SeqNo: " +
                                 std::to_string(seqNo) + ", command: " +
                                 std::string(commandName(ackPkt->command)) + ")";
                        cjs_log_warning(ackLog);
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
                        cjs_log_warning(timeoutMsg.str());
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
                cjs_log_info("HB-PSD: gateway HB recovered; safe-state latch awaits PLC release");
        }

        if (m <= warnW)
        {
            const bool hadTier2 = hbTier2SafeHoldActive.exchange(false, std::memory_order_acq_rel);
            if (hadTier2)
            {
                cjs_log_info("HB-PSD: tier-2 safe-hold cleared");
                if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                    sendCjsSafeHold();
                }
            }
        }

        if (m > prevMiss && m >= 1U && m <= warnW)
        {
            cjs_log_warning(
                "HB-PSD: gateway HB warn tier miss_count=" + std::to_string(m) +
                "/" + std::to_string(maxF) + " elapsed_ms=" + std::to_string(elapsed));
        }
        if (m > warnW && m < maxF && prevMiss <= warnW)
        {
            regrTier2AttemptsRemaining.store(
                (maxF > warnW + 1U) ? static_cast<int>(maxF - warnW - 1U) : 0);
            cjs_log_err(
                "HB-PSD: active fault (tier 2) — safe hold + bounded REGR; miss=" + std::to_string(m));
            {
                std::lock_guard<std::mutex> sendLock(decisionSendMtx);
                hbTier2SafeHoldActive.store(true, std::memory_order_release);
                sendCjsSafeHold();
            }
        }

        prevMiss = m;
        g_lastGatewayHbMissCount.store(m);

        if (m >= maxF)
        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (markGatewayHbFaultLatched())
            {
                cjs_log_err(
                    "HB-PSD: gateway HB fault latched (tier 3) — local fail-safe, no PSS connection");
                sendCjsSafeHold();
            }
        }
    }
}

/* Ranks a command by how restrictive it is, so one scan can resolve to its worst
 * case regardless of the order events arrived in.
 *
 * CMD_STOP outranks the siren because it is only ever raised by the fail-safe
 * paths -- PSS fault, gateway heartbeat loss, stale DecisionRequest -- and a sick
 * safety chain must not be masked by a junction verdict computed from data that
 * may itself be stale. */
static int cjsCommandSeverityRank(unsigned char command)
{
    switch (command)
    {
        case CMD_STOP:           return 2;
        case CMD_JUNCTION_ALARM: return 1;
        default:                 return 0;   /* CMD_NORMAL */
    }
}

/* ================== JUNCTION STATE MACHINE ==================
 *
 * Implements the factory-crossing state machine directly: four states, of which
 * only Alarm sounds the siren.
 *
 *          A2                 A1
 *   Clear ----> PersonInRoi ----> Alarm
 *     |                             ^
 *     | A1                          | A2
 *     v                             |
 *   ForkliftInRoi ------------------+
 *
 * The states are not stored. They are derived from A1 and A2 every time the
 * machine is evaluated, which makes every transition in that diagram fall out
 * automatically -- including Alarm returning straight to Clear when A1 and A2
 * drop together, which a hand-written transition table has to special-case.
 * Only A1's latch is genuine state, because "has crossed inbound and not yet
 * left" is not a property of any single frame.
 *
 * The siren is raised on the crossing frame; the hold times below only delay its
 * release. That asymmetry is deliberate: the hazard must be announced with no
 * added latency, while releasing early on a momentary detection gap would let
 * the siren stutter while the danger persists. */

/* How long after crossing the tripwire a Forklift counts as inbound before the
 * ROI has to confirm it. The tripwire sits on the ROI boundary, so this only has
 * to cover the lag from the crossing frame until perception first reports the
 * vehicle inside -- a couple of seconds is generous. Too long and a vehicle that
 * crossed the wire and then turned away stays engaged; the ROI check below is
 * what normally ends the engagement, not this window. */
static std::atomic<std::int64_t> g_tripwireEngageWindowMs{5000};
/* A1's second way in, for the crossing the wire never reported.
 *
 * The tripwire is a poor census of who enters this junction: only one of the two
 * cameras sees it, and a crossing is a single-shot edge, so anything that drops
 * it -- a camera without the view, a saturated DecisionRequest -- is lost with no
 * second chance, while the ROI levels behind it re-assert several times a second.
 * Measured over 13 minutes here, a Forklift entered the junction ROI 20 times and
 * only 2 of those entries had an inbound tripwire event in the preceding 10s.
 *
 * So a Forklift first appearing inside the ROI arms A1 too, for this long. It is
 * deliberately a pulse and not a latch: were it sustained by ROI occupancy the way
 * the tripwire's latch is, A1 would collapse into "a forklift is present" and the
 * siren would sound for 201s of that same 13 minutes rather than 69s. A pulse
 * announces the entry -- which is the hazard the tripwire was meant to catch --
 * and then falls silent for a vehicle that simply stays parked in the junction.
 *
 * 0 disables the backstop, restoring tripwire-only arming. */
static std::atomic<std::int64_t> g_roiEntryPulseMs{5000};
/* T_hold for each ROI level: how long a level survives without a fresh report
 * before it is treated as absent. Sized against the mapping's scale_factor --
 * at 25 fps with scale_factor 5 an event is due every 200ms, so these tolerate
 * roughly 7 and 10 consecutive misses respectively. */
static std::atomic<std::int64_t> g_personRoiHoldMs{1500};
static std::atomic<std::int64_t> g_forkliftRoiHoldMs{2000};
/* How long a level must read vacated on every sensor before it is released.
 *
 * With the vacated reports mapped, absence is stated rather than inferred, so
 * the hold times above no longer set release latency -- this does. It exists
 * only to absorb single-frame detection dropouts, which are common: on this
 * deployment the pooled vacancy durations are sharply bimodal, clustering at
 * 29-232ms for a dropout and starting again at 1388ms for a real departure.
 * Anything in that gap separates the two, and 500ms sits in it with margin at
 * both ends while still releasing three to four times sooner than the holds. */
static std::atomic<std::int64_t> g_clearDebounceMs{500};

/* Bounds for the three tunables above live in the header, shared with the CLI
 * parser so a direct library caller cannot install a value that would either
 * make the siren chatter or wedge it on. */

/* Cadence of the standalone evaluation tick. The levels are released by
 * staleness, so something has to notice that time has passed even when no event
 * arrives -- otherwise a siren would keep sounding after the last person left
 * simply because nothing prompted a re-read. 200ms is well inside the shortest
 * hold time, so the release lands within one tick of the hold expiring. */
static std::atomic<std::uint32_t> g_junctionEvalPeriodMs{200U};

enum class JunctionPhase
{
    kClear,
    kPersonInRoi,
    kForkliftInRoi,
    kAlarm
};

static const char* junctionPhaseName(JunctionPhase phase)
{
    switch (phase)
    {
        case JunctionPhase::kAlarm:         return "ALARM";
        case JunctionPhase::kForkliftInRoi: return "FORKLIFT_IN_ROI";
        case JunctionPhase::kPersonInRoi:   return "PERSON_IN_ROI";
        case JunctionPhase::kClear:
        default:                            return "CLEAR";
    }
}

/* One sensor's view of the junction.
 *
 * Occupancy is kept per sensor and pooled by OR because the two cameras
 * genuinely disagree: measured here, Camera_01 reports the junction occupied by
 * a person on 97% of its frames against Camera's 73%. A single pooled flag
 * written by whichever event arrived last would follow that disagreement
 * instead of resolving it, and would read vacated whenever the camera with the
 * poorer view happened to speak last.
 *
 * The report timestamps are what let a silent sensor be discounted: they are
 * refreshed by a report of either polarity, so they measure whether the sensor
 * is talking at all, not what it is saying. */
struct SensorJunctionView
{
    bool         personOccupied        = false;
    bool         forkliftOccupied      = false;
    std::int64_t lastPersonReportMs    = 0;
    std::int64_t lastForkliftReportMs  = 0;
};

struct JunctionZoneState
{
    /* Steady-clock milliseconds of the last report of each condition; 0 = never. */
    std::int64_t  lastTripwireInboundMs = 0;
    std::int64_t  lastForkliftInRoiMs   = 0;
    std::int64_t  lastPersonInRoiMs     = 0;
    /* Per-sensor occupancy, pooled by the update below. */
    std::map<std::uint8_t, SensorJunctionView> bySensor;
    /* Pooled levels after debouncing -- what A1 and A2 are actually read from. */
    bool          personInRoi           = false;
    bool          forkliftInRoi         = false;
    /* When each pooled level first read vacated, 0 while it reads occupied. */
    std::int64_t  personVacantSinceMs   = 0;
    std::int64_t  forkliftVacantSinceMs = 0;
    /* Rising edge of the pooled forklift level: when a Forklift was last seen to
     * *enter* the ROI, as opposed to still being in it. Drives the backstop
     * pulse; 0 = no entry observed yet. */
    std::int64_t  lastForkliftRoiEntryMs = 0;
    /* A1's latch. Set by the inbound crossing, cleared once the vehicle is
     * neither freshly crossed nor still inside the ROI. */
    bool          engaged               = false;
    std::uint32_t engagingForkliftId    = 0;
    std::uint32_t forkliftInRoiId       = 0;
    std::uint32_t personInRoiId         = 0;
    /* Last reported record for each participant, so an alarm packet can name the
     * pair the siren is actually about. The two conditions are levels held over
     * their hold windows and are reported by separate events, so the batch that
     * finally completes the alarm routinely contains only one of them -- and the
     * staleness tick that raises or drops an alarm has no batch at all. Caching
     * here is what lets both paths emit the same, complete pair. */
    ObjectRecord  forkliftRecord        = {};
    ObjectRecord  personRecord          = {};
    /* Last reported phase, so a transition can be logged once instead of at the
     * evaluation cadence. */
    JunctionPhase reportedPhase         = JunctionPhase::kClear;
    bool          phaseEverReported     = false;
};

/* Junction identity.
 *
 * This deployment's calibration gives roi-id-1 and tripwire-id-1 the same global
 * coordinates under both cameras, so the two sensors are two views of one
 * physical crossing, and their evidence is pooled under a single key. That
 * pooling is what makes the alarm work at all here: the tripwire is only ever
 * crossed within Camera's field of view, while a person standing in the junction
 * may be reported by either camera or both. Keying per sensor would mean a
 * person seen only by Camera_01 could never complete an alarm.
 *
 * --per_sensor_junctions keys by pipelineID instead, for a deployment that
 * reuses one set of ROI and tripwire names for genuinely separate crossings. */
static std::atomic<bool> g_perSensorJunctions{false};
static constexpr std::uint8_t kAggregateJunctionKey = 0U;

static std::map<std::uint8_t, JunctionZoneState> g_junctions;
static std::mutex                                g_junctionMtx;

static std::int64_t cjsSteadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool junctionSignalFresh(std::int64_t lastMs, std::int64_t nowMs, std::int64_t windowMs)
{
    return lastMs != 0 && (nowMs - lastMs) <= windowMs;
}

struct JunctionVerdict
{
    bool          a1      = false;
    bool          a2      = false;
    JunctionPhase phase   = JunctionPhase::kClear;
    unsigned char command = CMD_NORMAL;
};

/* Pools the per-sensor views into the two debounced levels.
 *
 * A level reads occupied if any sensor that is still reporting says so. A
 * sensor that has gone quiet past its hold window is discounted rather than
 * believed, so losing a stream releases the level instead of wedging it on --
 * the same failure direction the hold windows had before the vacated reports
 * existed, now applied per sensor rather than to the pool.
 *
 * Rising is immediate; falling has to hold for the debounce first. Caller holds
 * g_junctionMtx. */
static void updateJunctionLevelsLocked(JunctionZoneState& st, std::int64_t nowMs)
{
    const std::int64_t personHold   = g_personRoiHoldMs.load(std::memory_order_relaxed);
    const std::int64_t forkliftHold = g_forkliftRoiHoldMs.load(std::memory_order_relaxed);
    const std::int64_t debounce     = g_clearDebounceMs.load(std::memory_order_relaxed);

    bool rawPerson   = false;
    bool rawForklift = false;
    for (std::map<std::uint8_t, SensorJunctionView>::const_iterator it = st.bySensor.begin();
         it != st.bySensor.end(); ++it)
    {
        const SensorJunctionView& view = it->second;
        if (view.personOccupied &&
            junctionSignalFresh(view.lastPersonReportMs, nowMs, personHold))
            rawPerson = true;
        if (view.forkliftOccupied &&
            junctionSignalFresh(view.lastForkliftReportMs, nowMs, forkliftHold))
            rawForklift = true;
    }

    if (rawPerson)
    {
        st.personInRoi         = true;
        st.personVacantSinceMs = 0;
    }
    else
    {
        if (st.personVacantSinceMs == 0)
            st.personVacantSinceMs = nowMs;
        if ((nowMs - st.personVacantSinceMs) >= debounce)
            st.personInRoi = false;
    }

    if (rawForklift)
    {
        /* Only a genuine false->true edge counts as an entry. Because the level
         * falls only after clearDebounceMs but rises at once, a detection
         * dropout shorter than the debounce never reaches false and so cannot
         * be mistaken for the vehicle re-entering. */
        if (!st.forkliftInRoi)
            st.lastForkliftRoiEntryMs = nowMs;
        st.forkliftInRoi         = true;
        st.forkliftVacantSinceMs = 0;
    }
    else
    {
        if (st.forkliftVacantSinceMs == 0)
            st.forkliftVacantSinceMs = nowMs;
        if ((nowMs - st.forkliftVacantSinceMs) >= debounce)
            st.forkliftInRoi = false;
    }
}

/* Derives A1, A2 and the resulting command for one junction, and releases the A1
 * latch when the vehicle is gone. Caller holds g_junctionMtx. */
static JunctionVerdict junctionVerdictLocked(const JunctionZoneState& st,
                                             std::int64_t nowMs)
{
    const bool crossedInbound =
        junctionSignalFresh(st.lastTripwireInboundMs, nowMs,
                            g_tripwireEngageWindowMs.load(std::memory_order_relaxed));

    /* The backstop: a Forklift seen entering the ROI arms A1 for a bounded pulse
     * even when no crossing was ever reported for it. Disabled at 0. */
    const std::int64_t entryPulseMs = g_roiEntryPulseMs.load(std::memory_order_relaxed);
    const bool recentlyEnteredRoi =
        entryPulseMs > 0 &&
        junctionSignalFresh(st.lastForkliftRoiEntryMs, nowMs, entryPulseMs);

    JunctionVerdict v;
    /* A1 is armed either way the vehicle can be known to have entered.
     *
     * Via the wire: the latch *and* current evidence the vehicle is still
     * involved. The latch alone would never drop; current evidence alone would
     * drop the moment perception blinked mid-junction. The crossing window
     * covers the lag from the wire until the ROI first confirms it inside.
     *
     * Via the ROI: a pulse from the entry edge, which needs no latch because it
     * expires on its own. */
    v.a1 = (st.engaged && (crossedInbound || st.forkliftInRoi)) || recentlyEnteredRoi;

    v.a2 = st.personInRoi;

    if (v.a1 && v.a2)
        v.phase = JunctionPhase::kAlarm;
    else if (v.a1)
        v.phase = JunctionPhase::kForkliftInRoi;
    else if (v.a2)
        v.phase = JunctionPhase::kPersonInRoi;
    else
        v.phase = JunctionPhase::kClear;

    v.command = (v.a1 && v.a2) ? CMD_JUNCTION_ALARM : CMD_NORMAL;
    return v;
}

/* Same verdict, plus the one state update it implies: A1 falling means the
 * engagement is over, so the latch is dropped and a later crossing has to arm it
 * again. Only the deciding paths call this; the send path uses the pure form
 * above so that emitting a packet can never advance the machine. */
static JunctionVerdict evaluateJunctionLocked(JunctionZoneState& st, std::int64_t nowMs)
{
    updateJunctionLevelsLocked(st, nowMs);
    const JunctionVerdict v = junctionVerdictLocked(st, nowMs);
    if (!v.a1)
        st.engaged = false;
    return v;
}

/* Logs a phase change once per transition. Caller holds g_junctionMtx. */
static void logJunctionPhaseTransitionLocked(std::uint8_t key,
                                             JunctionZoneState& st,
                                             const JunctionVerdict& v)
{
    if (st.phaseEverReported && st.reportedPhase == v.phase)
        return;

    char buf[352];
    snprintf(buf, sizeof(buf),
             "Junction %s: %s -> %s (A1 forkliftEngaged=%s A2 personInRoi=%s) "
             "forklift=%u person=%u -> %s",
             g_perSensorJunctions.load(std::memory_order_relaxed)
                 ? ("pipeline " + std::to_string(static_cast<unsigned>(key))).c_str()
                 : "crossing",
             st.phaseEverReported ? junctionPhaseName(st.reportedPhase) : "INIT",
             junctionPhaseName(v.phase),
             v.a1 ? "true" : "false",
             v.a2 ? "true" : "false",
             st.engaged ? st.engagingForkliftId : st.forkliftInRoiId,
             st.personInRoiId,
             commandName(v.command));

    if (v.phase == JunctionPhase::kAlarm)
        cjs_log_warning(buf);
    else
        cjs_log_info(buf);

    st.reportedPhase     = v.phase;
    st.phaseEverReported = true;
}

/* Folds one junction-relevant event into the state. */
static void ingestJunctionEvent(const FusedSafetyEvent& event, std::int64_t nowMs)
{
    const std::uint8_t key = g_perSensorJunctions.load(std::memory_order_relaxed)
        ? event.fusionMetadata.pipelineID
        : kAggregateJunctionKey;
    /* Tripwire and ROI alerts are single-object, so mdx-client puts the object in
     * slot 0 and leaves slot 1 zeroed. */
    const std::uint32_t objectId = event.fusionMetadata.objectID[0];

    std::lock_guard<std::mutex> lock(g_junctionMtx);
    JunctionZoneState& st = g_junctions[key];
    /* Occupancy is always tracked against the reporting sensor, even when the
     * junction itself is pooled -- pooling decides which crossing the evidence
     * belongs to, not which camera saw it. */
    SensorJunctionView& view = st.bySensor[event.fusionMetadata.pipelineID];

    switch (event.type)
    {
        case EVENT_0_FORKLIFT_TRIPWIRE_INBOUND:
            /* The latch is (re)armed on every inbound crossing. A second vehicle
             * crossing while the first is still inside simply refreshes it: A1
             * asks whether *a* forklift is engaged, not which one. */
            st.lastTripwireInboundMs = nowMs;
            st.engaged               = true;
            st.engagingForkliftId    = objectId;
            copyObjectRecordFromMetadata(&st.forkliftRecord, event.fusionMetadata, 0);
            break;
        case EVENT_1_PERSON_IN_JUNCTION_ROI:
            st.lastPersonInRoiMs      = nowMs;
            st.personInRoiId          = objectId;
            view.personOccupied       = true;
            view.lastPersonReportMs   = nowMs;
            copyObjectRecordFromMetadata(&st.personRecord, event.fusionMetadata, 0);
            break;
        case EVENT_2_FORKLIFT_IN_JUNCTION_ROI:
            st.lastForkliftInRoiMs    = nowMs;
            st.forkliftInRoiId        = objectId;
            view.forkliftOccupied     = true;
            view.lastForkliftReportMs = nowMs;
            copyObjectRecordFromMetadata(&st.forkliftRecord, event.fusionMetadata, 0);
            break;
        /* The vacated reports say only that the class is absent from this
         * sensor's view of the ROI. They carry no object, so the cached records
         * are deliberately left alone: an alarm raised moments later still has
         * to name the pair it is about. */
        case EVENT_3_PERSON_VACATED_JUNCTION:
            view.personOccupied     = false;
            view.lastPersonReportMs = nowMs;
            break;
        case EVENT_4_FORKLIFT_VACATED_JUNCTION:
            view.forkliftOccupied     = false;
            view.lastForkliftReportMs = nowMs;
            break;
        default:
            break;
    }

    /* Fold the change straight into the pooled levels so a release lands on the
     * report that earned it rather than on the next evaluation tick. */
    updateJunctionLevelsLocked(st, nowMs);
}

/* Evaluates every tracked junction and returns the worst command across them.
 * With junctions pooled there is only one, but the aggregation is written out so
 * --per_sensor_junctions needs no separate path. */
static unsigned char resolveJunctionCommand(JunctionPhase* outWorstPhase)
{
    const std::int64_t nowMs = cjsSteadyNowMs();

    unsigned char worstCmd   = CMD_NORMAL;
    int           worstRank  = -1;
    JunctionPhase worstPhase = JunctionPhase::kClear;

    std::lock_guard<std::mutex> lock(g_junctionMtx);
    for (auto& entry : g_junctions)
    {
        JunctionZoneState&    st = entry.second;
        const JunctionVerdict v  = evaluateJunctionLocked(st, nowMs);
        logJunctionPhaseTransitionLocked(entry.first, st, v);

        const int rank = cjsCommandSeverityRank(v.command);
        if (rank > worstRank)
        {
            worstRank  = rank;
            worstCmd   = v.command;
            worstPhase = v.phase;
        }
    }

    if (outWorstPhase)
        *outWorstPhase = worstPhase;
    return worstCmd;
}

/* Fills objects[] with the forklift/person pair of a junction that is currently
 * in alarm, forklift in slot 0 and person in slot 1 to match the packet layout
 * the rest of the metropolis family uses.
 *
 * Returns false when no junction is in alarm, leaving objects[] untouched so the
 * caller can fall back to the batch-derived records. */
static bool fillJunctionAlarmObjectRecords(ObjectRecord objects[COMMAND_NUM_OBJECTS])
{
    const std::int64_t nowMs = cjsSteadyNowMs();

    std::lock_guard<std::mutex> lock(g_junctionMtx);
    for (const auto& entry : g_junctions)
    {
        const JunctionZoneState& st = entry.second;
        if (junctionVerdictLocked(st, nowMs).command != CMD_JUNCTION_ALARM)
            continue;
        objects[0] = st.forkliftRecord;
        objects[1] = st.personRecord;
        return true;
    }
    return false;
}

/* Consumes a DecisionRequest batch into the state machine and reports the
 * resulting command.
 *
 * Every usable event in the batch is folded in before anything is decided, so
 * position within sensorDataSummary[] carries no meaning -- a crossing and the
 * person it endangers may arrive in either order in the same batch.
 *
 * Returns false when the batch held no junction event at all, which leaves the
 * current decision untouched; the evaluation tick is what re-reads the machine
 * when events stop arriving.
 * *outWinningSlot receives the slot that justifies the reported command. */
/*
 * AI-PIPELINE TRUST, CONSUMED BEFORE THE BATCH COMPUTE.
 *
 * AI_PIPELINE_INVALID arrives ALREADY STAMPED UNTRUSTED: PSS sets the
 * pipeline-wide flag before routing, so a handler placed after the
 * !isTrustedSource skip in computeJunctionCommandFromBatch() would never run.
 *
 * The latch EDGE is returned to the caller, which sends the safe hold at once.
 * Setting the bit alone is not enough for this cause: distrust is exactly what
 * stops the decision path being reached -- every later junction event arrives
 * untrusted and is skipped, and a no-op returns early -- so the PLC would go on
 * holding its last command. (The proximity SDM measured 26.7 s before any hold
 * reached the PLC when it relied on the bit alone.)
 *
 * The gateway force-adds AI_PIPELINE_INVALID/VALID to every client's
 * subscription, so no registration change is needed for them to arrive.
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

static bool computeJunctionCommandFromBatch(const DecisionRequest* request,
                                            unsigned char* outCmd,
                                            int* outWinningSlot)
{
    if (!request || !outCmd)
        return false;

    const uint8_t maxSrc =
        std::min(request->sensorDataSummarySize,
                 static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));

    const std::int64_t nowMs = cjsSteadyNowMs();

    bool ingestedAny = false;
    int  personSlot  = -1;
    int  lastSlot    = -1;

    for (uint8_t i = 0; i < maxSrc; ++i)
    {
        const SensorData& sd = request->sensorDataSummary[i];

        if (sd.event.status == STALE)
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "Dropping STALE junction event id=%u type=%d",
                     (unsigned)sd.event.id, (int)sd.event.type);
            cjs_log_info(buf);
            continue;
        }

        const EventType et = static_cast<EventType>(sd.event.type);
        if (et == SENSOR_INVALID || et == SENSOR_VALID)
        {
            cjs_log_info("CJS: Sensor health event consumed by health monitor only");
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
            cjs_log_info("Sensor unhealthy -- logging only, not processing for decision");
            continue;
        }

        if (!sd.isTrustedSource)
        {
            cjs_log_info("AI pipeline untrusted -- logging only, not processing for decision");
            continue;
        }

        if (sd.event.type != EVENT_0_FORKLIFT_TRIPWIRE_INBOUND &&
            sd.event.type != EVENT_1_PERSON_IN_JUNCTION_ROI &&
            sd.event.type != EVENT_2_FORKLIFT_IN_JUNCTION_ROI &&
            sd.event.type != EVENT_3_PERSON_VACATED_JUNCTION &&
            sd.event.type != EVENT_4_FORKLIFT_VACATED_JUNCTION)
            continue;

        ingestJunctionEvent(sd.event, nowMs);
        ingestedAny = true;
        lastSlot    = static_cast<int>(i);
        if (sd.event.type == EVENT_1_PERSON_IN_JUNCTION_ROI && personSlot < 0)
            personSlot = static_cast<int>(i);
    }

    if (!ingestedAny)
        return false;

    JunctionPhase phase = JunctionPhase::kClear;
    *outCmd = resolveJunctionCommand(&phase);

    if (outWinningSlot)
    {
        /* Attribute an alarm to the person in the junction when the batch names
         * one: the object records that ride along with the command then identify
         * whoever is being protected, which is the useful half of the pair. */
        *outWinningSlot = (*outCmd == CMD_JUNCTION_ALARM && personSlot >= 0)
            ? personSlot
            : lastSlot;
    }
    return true;
}

/* Re-evaluates the junction state machine on a fixed tick and sends only when
 * the verdict changes.
 *
 * The vacated reports mean the event path can now end an alarm by itself, but
 * only while something is still arriving. This tick covers what it cannot: the
 * debounce expiring after the last report, the crossing window closing with no
 * ROI confirmation behind it, and a sensor falling silent so that its occupancy
 * has to be discounted. It sends on change only, so a quiet junction costs no
 * traffic -- re-asserting the standing command is the other thread's job.
 *
 * The safe-hold latches are checked exactly as the event path checks them, so a
 * junction verdict computed here can never talk over a fail-safe hold. */
static void cjsJunctionEvalLoop()
{
    while (!stopSDMThreads.load(std::memory_order_relaxed))
    {
        const std::uint32_t periodMs = g_junctionEvalPeriodMs.load(std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(cjsJunctionEvalWaitMtx);
            cjsJunctionEvalCv.wait_for(lock, std::chrono::milliseconds(periodMs), [] {
                return stopSDMThreads.load(std::memory_order_relaxed);
            });
        }
        if (stopSDMThreads.load(std::memory_order_relaxed))
            break;

        JunctionPhase       phase   = JunctionPhase::kClear;
        const unsigned char desired = resolveJunctionCommand(&phase);

        std::lock_guard<std::mutex> sendLock(decisionSendMtx);

        bool changed = false;
        {
            std::lock_guard<std::mutex> stateLock(cjsStateMtx);
            changed          = (cjsActiveCommand != desired);
            cjsActiveCommand = desired;
        }
        if (!changed)
            continue;

        /* While any fail-safe hold is asserted the PLC is already being held in
         * the safe state by the thread that latched it. Record the new junction
         * verdict above so it is restored when the hold clears, but do not emit
         * it now. */
        if (safeStateLatched()
            || pssErrorFusionSuppressLatched.load(std::memory_order_acquire)
            || hbTier2SafeHoldActive.load(std::memory_order_acquire))
            continue;

        sendDecisionCommand(desired, true, nullptr);
    }
}

/* Periodic re-assert of the most recent decision, so a single lost UDP
 * datagram does not leave the PLC holding a stale command. Shutdown-aware
 * sleep via cjsPeriodicDecisionCv; observes the same safe-hold /
 * ERROR-latch signals as the event path so the periodic emission always
 * reflects the current worst-case fusion view, not a stale "desired". */
static void cjsPeriodicDecisionLoop()
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
            std::unique_lock<std::mutex> lock(cjsPeriodicDecisionWaitMtx);
            cjsPeriodicDecisionCv.wait_for(lock, sleepMs, [] {
                return stopSDMThreads.load(std::memory_order_relaxed);
            });
        }
        if (stopSDMThreads.load(std::memory_order_relaxed))
            break;

        {
            std::lock_guard<std::mutex> sendLock(decisionSendMtx);
            if (safeStateLatched())
            {
                sendCjsSafeHold();
            }
            else if (pssErrorFusionSuppressLatched.load(std::memory_order_acquire))
            {
                sendCjsSafeHold();
            }
            else if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
            {
                sendCjsSafeHold();
            }
            else
            {
                unsigned char cmd = CMD_NORMAL;
                {
                    std::lock_guard<std::mutex> stateLock(cjsStateMtx);
                    cmd = cjsActiveCommand;
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
 * Apply the source-lost rules by the same pure function as every other SDM:
 * every source-lost SW_FAIL in the request raises its sub-cause -- wherever it
 * sits and whatever the mode, which the SW_FAIL loop below cannot promise --
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
            cjs_log_err(std::string("CJS: source reported lost, or missing from the presence report -- "
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
        std::lock_guard<std::mutex> stateLock(cjsStateMtx);
        cjsActiveCommand = CMD_STOP;
    }
    sendCjsSafeHold(request);
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

    cjs_log_err(std::string("CJS: a source's reports are being rejected -- triggering safe hold; "
                "sub-cause=input_unusable (clears only when PSS confirms that source usable again)"));
    std::lock_guard<std::mutex> sendLock(decisionSendMtx);
    markPssSubCause(kPssSubInputUnusable);
    {
        std::lock_guard<std::mutex> stateLock(cjsStateMtx);
        cjsActiveCommand = CMD_STOP;
    }
    sendCjsSafeHold(request);
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
        cjs_log_err(std::string("CJS: SW_FAIL received — triggering safe hold; cause=") +
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
        std::lock_guard<std::mutex> stateLock(cjsStateMtx);
        cjsActiveCommand = CMD_STOP;
    }
    sendCjsSafeHold(request, firstIndex);
    return true;
}

/* ====================== DECISION LOGIC ========================== */
/* The request itself. The caller holds faultTransitionMtx: the gateway listener
 * for the whole packet, or the public entry point below. */
static void processDecisionRequestLocked(const DecisionRequest* request)
{
    if (!request)
    {
        cjs_log_err("SDM: onEventNotificationReceive called with null DecisionRequest");
        return;
    }

    cjs_log_info(
        "SDM: processing DecisionRequest id=" +
        std::to_string(request->requestId));

    /*
     * AI-PIPELINE VERDICTS FIRST -- before the PSS-ERROR return and the SW_FAIL
     * return below.
     *
     * The AI monitor reports its verdict ONCE, on change. Either early return
     * would discard a verdict riding the same request, and nothing would ever
     * re-send it. The hold is sent here on the latch edge and processing
     * CONTINUES -- the rest of the request still applies, and may send the hold
     * again, which is harmless.
     */
    if (updateAiPipelineTrustFromDecisionRequest(request))
    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            std::lock_guard<std::mutex> stateLock(cjsStateMtx);
            cjsActiveCommand = CMD_STOP;
        }
        sendCjsSafeHold(request);
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
        cjs_log_warning(
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
                std::lock_guard<std::mutex> stateLock(cjsStateMtx);
                cjsActiveCommand = CMD_STOP;
            }
            sendCjsSafeHold(request);
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
     * may be the only thing that ever arrives.
     */
    if (SdmDecisionRequestClearsPssModeError(request))
    {
        clearPssSubCause(kPssSubModeError);
    }

    if (maxSrc == 1U &&
        static_cast<EventType>(request->sensorDataSummary[0].event.type) == PSS_STATUS_NOOP)
    {
        /* The gateway's all-client recovery record rides a no-op. It is the only
         * evidence that reaches an SDM receiving no ordinary event traffic. */
        if (SdmDecisionRequestIsGatewayCommRecovered(request))
        {
            clearPssSubCause(kPssSubGatewayComm);
        }
        cjs_log_info("CJS: PSS_STATUS_NOOP received; freshness refreshed without proximity-state update");
        return;
    }

    if (SdmDecisionRequestClearsPssSwFail(request))
    {
        clearPssSubCause(kPssSubSwFail);
        /* The gateway-comm sub-cause clears here TOO, not only on the recovery
         * record: a gateway restart forgets it owed one, and a single dropped
         * record must not leave this SDM release-denied for good. */
        clearPssSubCause(kPssSubGatewayComm);
    }

    if (applySwFailEvents(request, maxSrc))
        return;

    if (sensorLatchEntered)
    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            std::lock_guard<std::mutex> stateLock(cjsStateMtx);
            cjsActiveCommand = CMD_STOP;
        }
        sendCjsSafeHold(request);
        return;
    }

    unsigned char desired = CMD_NORMAL;
    int winningSlot = -1;
    if (!computeJunctionCommandFromBatch(request, &desired, &winningSlot))
        return;

    {
        std::lock_guard<std::mutex> sendLock(decisionSendMtx);
        {
            /* Always record the desired decision, even if we are currently
             * overriding it with tier-2 safe hold, so the periodic loop
             * restores the correct command once the safe hold clears. */
            std::lock_guard<std::mutex> stateLock(cjsStateMtx);
            cjsActiveCommand = desired;
        }
        if (hbTier2SafeHoldActive.load(std::memory_order_acquire))
        {
            sendCjsSafeHold(request, winningSlot);
        }
        else if (safeStateLatched())
        {
            sendCjsSafeHold(request, winningSlot);
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

static bool sendGatewayRegistration(bool retryUntilWritable);

/*
 * UPSTREAM LINK OWNERSHIP.
 *
 * Every upstream send originates on the listener thread (flushPendingHbAck,
 * emitHbAckTryOnce and the periodic REGR), and the launch-time REGR runs
 * before the listener starts. So there is exactly one reader and one writer,
 * and they are the same thread.
 *
 * gwSockMtx is retained rather than removed because
 * shutdownCrossingJunctionSafetyControlAlgo() is a public entry point a future
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
    if (NvPSFTransportOpen(&cfg, &t) != NVPSF_TRANSPORT_OK || t == nullptr)
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
    cjs_log_err("SDM: gateway upstream link lost; reconnecting");

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
                cjs_log_info("SDM: gateway upstream link re-established after " +
                             std::to_string(attempt) + " attempt(s)");
                return true;
            }
            cjs_log_warning("SDM: reconnected but REGR failed; retrying");
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
        cjs_log_err("SDM: heartbeat ACK send failed (" +
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
 * Send REGR registration to gateway so it forwards the junction-subscribed
 * event types.
 *
 * @param retryUntilWritable true at launch and on reconnect, where a REGR that
 *        does not land leaves the SDM subscribed to nothing; false on the
 *        periodic path, where the next REG_RETRY_INTERVAL_MS tick retries and
 *        blocking the listener would delay decision handling.
 */
static bool sendGatewayRegistration(bool retryUntilWritable)
{
    constexpr size_t bufSize = 4 + 1 + CJS_SUBSCRIBED_COUNT * sizeof(uint32_t);
    char buf[bufSize];
    std::memcpy(buf, NVPSD_GATEWAY_REG_MAGIC, 4);
    /* Wire format: one byte count (gateway reads as uint8_t); avoid signed char. */
    buf[4] = static_cast<unsigned char>(CJS_SUBSCRIBED_COUNT);
    for (uint8_t i = 0; i < CJS_SUBSCRIBED_COUNT; ++i)
    {
        uint32_t val = htonl(static_cast<uint32_t>(CJS_SUBSCRIBED_EVENTS[i]));
        std::memcpy(buf + 5 + i * sizeof(uint32_t), &val, sizeof(uint32_t));
    }

    const int maxAttempts = retryUntilWritable ? REGR_SEND_MAX_ATTEMPTS : 1;

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        const NvPSFTransportErr err = gatewaySend(buf, bufSize);

        if (err == NVPSF_TRANSPORT_OK)
        {
            cjs_log_info("SDM: sent registration (" +
                         std::to_string(CJS_SUBSCRIBED_COUNT) +
                         " event types) to gateway");
            return true;
        }
        if (err != NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            /* PEER_GONE or a hard error. Not retried here: the listener owns
             * reconnection, and spinning the retry budget against a link that
             * is down would only delay entering it. */
            cjs_log_err("SDM: failed to send REGR to gateway (" +
                        std::string(NvPSFTransportErrName(err)) + ")");
            return false;
        }
        if (!retryUntilWritable)
            return false;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(REGR_SEND_RETRY_SLEEP_MS));
    }
    cjs_log_warning("SDM: REGR send still blocked after retries");
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
            cjs_log_warning("SDM: poll on gateway transport failed (" +
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
            cjs_log_warning("SDM: oversized message from gateway; discarding");
            continue;
        }
        if (rerr != NVPSF_TRANSPORT_OK)
        {
            cjs_log_warning("SDM: recv on gateway transport failed (" +
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

            cjs_log_info(
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

            /* Send ACK: [HBPC][seqNo]. A full peer buffer leaves it pending and
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
                    cjs_log_err(std::string("SDM: the gateway refused this SDM's registration; reason=") +
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
                cjs_log_err("SDM: Gateway DecisionRequest packet header validation failed — dropping");
                continue;
            }

            DecisionRequest request;
            std::memcpy(&request, &packet.request, sizeof(DecisionRequest));

            uint32_t vErr = validateDecisionRequest(&request);
            if (vErr != PSS_VALID)
            {
                char vErrHex[12];
                snprintf(vErrHex, sizeof(vErrHex), "%08X", vErr);
                cjs_log_err(
                    std::string("SDM: DecisionRequest validation failed (flags=0x") +
                    vErrHex + ") — dropping");
                continue;
            }

            cjs_log_info(
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
                cjs_log_warning("CJS: dropping duplicate/stale Gateway DecisionRequest packet; epoch=" +
                    std::to_string(packet.gatewayEpoch) + " txSeq=" +
                    std::to_string(packet.gatewayTxSeq));
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_INVALID)
            {
                cjs_log_err("CJS: invalid Gateway DecisionRequest packet sequence — dropping");
                continue;
            }
            if (seqResult == SDM_GATEWAY_DECISION_SEQUENCE_GAP)
            {
                if (markGatewayDecisionSequenceFaultLatched(packet.gatewayEpoch, packet.gatewayTxSeq))
                {
                    cjs_log_err("CJS: Gateway DecisionRequest sequence gap; entering safe hold, epoch=" +
                        std::to_string(packet.gatewayEpoch) + " txSeq=" +
                        std::to_string(packet.gatewayTxSeq));
                    sendLatchedCjsSafeHold();
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
            cjs_log_warning(
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

int launchCrossingJunctionSafetyControlAlgo(const std::string& gatewayIP,
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
                               std::uint32_t tripwireEngageWindowMs,
                               std::uint32_t personRoiHoldMs,
                               std::uint32_t forkliftRoiHoldMs,
                               std::uint32_t junctionEvalPeriodMs,
                               bool perSensorJunctions,
                               std::uint32_t clearDebounceMs,
                               std::uint32_t roiEntryPulseMs,
                               NvPSFTransportKind gatewayTransport,
                               const std::string& gatewaySocketPath)
{
    cjs_log_open();

    if (gatewayPort < 1U || gatewayPort > 65535U
        || plcPort < 1U || plcPort > 65535U)
    {
        cjs_log_err("gateway and PLC ports must be in range 1..65535");
        cjs_log_close();
        return -1;
    }
    if (haloPort > 65535U)
    {
        cjs_log_err("halo port must be in range 0..65535 (0 = disabled)");
        cjs_log_close();
        return -1;
    }
    const bool haloRequested = !haloIP.empty() && haloPort > 0U;

    const uint16_t gatewayPortU16 = static_cast<uint16_t>(gatewayPort);
    const uint16_t plcPortU16     = static_cast<uint16_t>(plcPort);
    const uint16_t haloPortU16    = static_cast<uint16_t>(haloPort);

    if (maxHbFailures == 0)
    {
        cjs_log_err("maxHbFailures must be in 1..255");
        cjs_log_close();
        return -1;
    }

    /* Validate the same way as the CLI parser so a direct library caller
     * cannot silently install an out-of-range period. 0 = disabled;
     * otherwise must land inside [min, max]. */
    if (decisionRepeatIntervalMs > kDecisionRepeatIntervalMsMax
        || (decisionRepeatIntervalMs != 0U
            && decisionRepeatIntervalMs < kDecisionRepeatIntervalMsMinNonZero))
    {
        cjs_log_err("decisionRepeatIntervalMs must be 0 or in " +
                    std::to_string(kDecisionRepeatIntervalMsMinNonZero) + ".." +
                    std::to_string(kDecisionRepeatIntervalMsMax) +
                    " (0 = periodic repeat off)");
        cjs_log_close();
        return -1;
    }

    if (hbStaleMs < kHbTimingMsMin || hbStaleMs > kHbTimingMsMax ||
        hbPeriodMs < kHbTimingMsMin || hbPeriodMs > kHbTimingMsMax)
    {
        cjs_log_err("hbStaleMs and hbPeriodMs must be in " +
                    std::to_string(kHbTimingMsMin) + ".." +
                    std::to_string(kHbTimingMsMax));
        cjs_log_close();
        return -1;
    }

    if (!sdmDecisionFreshnessTimeoutMsIsValid(decisionFreshnessTimeoutMs))
    {
        cjs_log_err("decisionFreshnessTimeoutMs must be in " +
                    std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN) + ".." +
                    std::to_string(SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX));
        cjs_log_close();
        return -1;
    }

    /* Validated here as well as in the CLI parser: an out-of-range hold either
     * makes the siren chatter or leaves it latched on, so a direct library
     * caller must not be able to install one. */
    const std::int64_t holdMin = kJunctionHoldMsMin;
    const std::int64_t holdMax = kJunctionHoldMsMax;
    if (static_cast<std::int64_t>(tripwireEngageWindowMs) < holdMin
        || static_cast<std::int64_t>(tripwireEngageWindowMs) > holdMax
        || static_cast<std::int64_t>(personRoiHoldMs) < holdMin
        || static_cast<std::int64_t>(personRoiHoldMs) > holdMax
        || static_cast<std::int64_t>(forkliftRoiHoldMs) < holdMin
        || static_cast<std::int64_t>(forkliftRoiHoldMs) > holdMax
        || static_cast<std::int64_t>(clearDebounceMs) < holdMin
        || static_cast<std::int64_t>(clearDebounceMs) > holdMax)
    {
        cjs_log_err("tripwire engage window, ROI hold times and clear debounce must be in " +
                    std::to_string(holdMin) + ".." + std::to_string(holdMax) + " ms");
        cjs_log_close();
        return -1;
    }

    /* 0 is the one value outside the range that is meaningful: it turns the
     * ROI-entry backstop off and leaves A1 armed by the tripwire alone. */
    if (roiEntryPulseMs != 0U
        && (static_cast<std::int64_t>(roiEntryPulseMs) < holdMin
            || static_cast<std::int64_t>(roiEntryPulseMs) > holdMax))
    {
        cjs_log_err("ROI entry pulse must be 0 (disabled) or in " +
                    std::to_string(holdMin) + ".." + std::to_string(holdMax) + " ms");
        cjs_log_close();
        return -1;
    }

    if (junctionEvalPeriodMs < kJunctionEvalPeriodMsMin
        || junctionEvalPeriodMs > kJunctionEvalPeriodMsMax)
    {
        cjs_log_err("junction evaluation period must be in " +
                    std::to_string(kJunctionEvalPeriodMsMin) + ".." +
                    std::to_string(kJunctionEvalPeriodMsMax) + " ms");
        cjs_log_close();
        return -1;
    }

    /* The tick has to be able to observe the debounce expiring; one longer than
     * it would round the release up to the tick's own period. The debounce is
     * the shortest of the release timers now, so checking it covers the holds
     * as well. */
    if (junctionEvalPeriodMs > clearDebounceMs)
    {
        cjs_log_err("junction evaluation period (" + std::to_string(junctionEvalPeriodMs) +
                    " ms) must not exceed the clear debounce (" +
                    std::to_string(clearDebounceMs) + " ms)");
        cjs_log_close();
        return -1;
    }

    g_tripwireEngageWindowMs.store(static_cast<std::int64_t>(tripwireEngageWindowMs),
                                   std::memory_order_relaxed);
    g_personRoiHoldMs.store(static_cast<std::int64_t>(personRoiHoldMs),
                            std::memory_order_relaxed);
    g_forkliftRoiHoldMs.store(static_cast<std::int64_t>(forkliftRoiHoldMs),
                              std::memory_order_relaxed);
    g_junctionEvalPeriodMs.store(junctionEvalPeriodMs, std::memory_order_relaxed);
    g_perSensorJunctions.store(perSensorJunctions, std::memory_order_relaxed);
    g_clearDebounceMs.store(static_cast<std::int64_t>(clearDebounceMs),
                            std::memory_order_relaxed);
    g_roiEntryPulseMs.store(static_cast<std::int64_t>(roiEntryPulseMs),
                            std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(gwSockMtx);
        hbAckPending = false;
    }

    /* Register signal handlers */
    if (std::signal(SIGINT, signalHandler) == SIG_ERR)
        cjs_log_err("Failed to register SIGINT handler");
    if (std::signal(SIGTERM, signalHandler) == SIG_ERR)
        cjs_log_err("Failed to register SIGTERM handler");

    /* --- PLC command socket (send to PLC, receive ACKs) --- */
    plcSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (plcSock < 0)
    {
        cjs_log_err("Failed to create UDP socket");
        cjs_log_close();
        return -1;
    }

    int flags = fcntl(plcSock, F_GETFL, 0);
    if (flags == -1 || fcntl(plcSock, F_SETFL, flags | O_NONBLOCK) == -1)
        cjs_log_warning("Failed to set plcSock non-blocking");

    struct sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port        = htons(0);
    if (bind(plcSock, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0)
    {
        cjs_log_err("Failed to bind UDP socket");
        close(plcSock); plcSock = -1;
        cjs_log_close();
        return -1;
    }

    socklen_t addrLen = sizeof(localAddr);
    if (getsockname(plcSock, (struct sockaddr*)&localAddr, &addrLen) < 0)
    {
        cjs_log_err("Failed to get socket name");
        close(plcSock); plcSock = -1;
        cjs_log_close();
        return -1;
    }

    plcAddr.sin_family = AF_INET;
    plcAddr.sin_port   = htons(plcPortU16);
    if (inet_pton(AF_INET, plcIP.c_str(), &plcAddr.sin_addr) <= 0)
    {
        cjs_log_err("Invalid IP address");
        close(plcSock); plcSock = -1;
        cjs_log_close();
        return -1;
    }

    /* --- Optional VST halo fan-out socket (fire-and-forget, no ACK read) --- */
    if (haloRequested)
    {
        haloSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (haloSock < 0)
        {
            cjs_log_err("Failed to create halo UDP socket");
            close(plcSock); plcSock = -1;
            cjs_log_close();
            return -1;
        }

        haloAddr.sin_family = AF_INET;
        haloAddr.sin_port   = htons(haloPortU16);
        if (inet_pton(AF_INET, haloIP.c_str(), &haloAddr.sin_addr) <= 0)
        {
            cjs_log_err("Invalid halo IP address");
            close(plcSock); plcSock = -1;
            close(haloSock); haloSock = -1;
            cjs_log_close();
            return -1;
        }
        haloEnabled.store(true, std::memory_order_relaxed);
        cjs_log_info("VST halo fan-out enabled -> " + haloIP + ":" + std::to_string(haloPortU16));
    }
    else
    {
        cjs_log_info("VST halo fan-out disabled (no --halo_ip/--halo_port)");
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
            cjs_log_warning("SDM: initial REGR did not complete; will retry in event loop");
            /* Elapsed >= REG_RETRY_INTERVAL_MS so the first listener iteration retries soon. */
            lastRegistrationTime = std::chrono::steady_clock::now() -
                std::chrono::milliseconds(REG_RETRY_INTERVAL_MS);
        }
    }
    else
    {
        cjs_log_warning("SDM: psdGateway transport not available at launch (" +
                        std::string(NvPSFTransportKindName(gwTransportKind)) +
                        "); waiting for the gateway");
        lastRegistrationTime = std::chrono::steady_clock::now() -
            std::chrono::milliseconds(REG_RETRY_INTERVAL_MS);
    }

    /* Reset state */
    stopSDMThreads.store(false);
    signalShutdownRequested.store(false);
    /* The signal flag outlives a shutdown -- it is process-global and the
     * handler stays installed -- so a relaunch in the same process would read
     * the previous run's SIGINT and shut down before serving a decision. Every
     * other flag this block clears exists for that same reason. ATL clears it
     * here too; the current launcher runs one launch per process, so this is
     * the in-process relaunch path only. */
    g_signal_received = 0;
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
        std::lock_guard<std::mutex> lock(g_junctionMtx);
        g_junctions.clear();
    }
    {
        const long long entryPulse = static_cast<long long>(g_roiEntryPulseMs.load());
        char entryPulseDesc[128];
        if (entryPulse > 0)
            snprintf(entryPulseDesc, sizeof(entryPulseDesc),
                     "or EVENT_%d (Forklift entered roi-id-1) for %lldms after the entry edge",
                     EVENT_2_FORKLIFT_IN_JUNCTION_ROI, entryPulse);
        else
            snprintf(entryPulseDesc, sizeof(entryPulseDesc),
                     "with the ROI-entry backstop disabled");

        char buf[768];
        snprintf(buf, sizeof(buf),
                 "Junction alarm: siren = A1 AND A2. A1 = EVENT_%d (Forklift crossed "
                 "tripwire-id-1 inbound), latched, released once neither the crossing "
                 "(<=%lldms) nor Forklift-in-roi-id-1 is current, %s. A2 = Person in "
                 "roi-id-1. Occupancy is set by EVENT_%d/EVENT_%d and cleared by "
                 "EVENT_%d/EVENT_%d, per sensor, pooled by OR, released after %lldms "
                 "vacated; a sensor silent for %lld/%lldms (person/forklift) is "
                 "discounted. Junctions %s; re-evaluated every %ums",
                 EVENT_0_FORKLIFT_TRIPWIRE_INBOUND,
                 static_cast<long long>(g_tripwireEngageWindowMs.load()),
                 entryPulseDesc,
                 EVENT_1_PERSON_IN_JUNCTION_ROI,
                 EVENT_2_FORKLIFT_IN_JUNCTION_ROI,
                 EVENT_3_PERSON_VACATED_JUNCTION,
                 EVENT_4_FORKLIFT_VACATED_JUNCTION,
                 static_cast<long long>(g_clearDebounceMs.load()),
                 static_cast<long long>(g_personRoiHoldMs.load()),
                 static_cast<long long>(g_forkliftRoiHoldMs.load()),
                 g_perSensorJunctions.load() ? "keyed per sensor"
                                             : "pooled across sensors (one crossing)",
                 g_junctionEvalPeriodMs.load());
        cjs_log_info(buf);
    }
    {
        std::lock_guard<std::mutex> lock(cjsStateMtx);
        cjsActiveCommand = CMD_NORMAL;
    }
    g_decisionRepeatIntervalMs.store(decisionRepeatIntervalMs,
                                     std::memory_order_relaxed);

    /* --- Start threads --- */
    ackHandlerThread = std::thread(ackHandlerLoop);
    heartbeatThread  = std::thread(heartbeatTransmitter);
    hbWatchdogThread = std::thread(gatewayHeartbeatWatchdog);
    decisionFreshnessWatchdogThread = std::thread(decisionFreshnessWatchdog);
    junctionEvalThread = std::thread(cjsJunctionEvalLoop);
    if (decisionRepeatIntervalMs > 0U)
    {
        periodicDecisionThread = std::thread(cjsPeriodicDecisionLoop);
        cjs_log_info(
            "Crossing-Junction-Safety-Monitor Control Algorithm initialized (event-driven, gateway and PLC configured; "
            "periodic decision repeat every " +
            std::to_string(decisionRepeatIntervalMs) + " ms; gateway HB stale_ms=" +
            std::to_string(hbStaleMs) + " period_ms=" + std::to_string(hbPeriodMs) +
            "; decision_freshness_timeout_ms=" +
            std::to_string(decisionFreshnessTimeoutMs) + ")");
    }
    else
    {
        cjs_log_info(
            "Crossing-Junction-Safety-Monitor Control Algorithm initialized (event-driven, gateway and PLC configured; "
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
        cjs_log_info("Signal received - initiating graceful shutdown");
        stopSDMThreads.store(true);
    }

    // Perform shutdown
    shutdownCrossingJunctionSafetyControlAlgo();

    return 0;
}

void shutdownCrossingJunctionSafetyControlAlgo()
{
    stopSDMThreads.store(true);
    /* Wake the periodic loop so it observes stopSDMThreads immediately
     * instead of sleeping out the remainder of its current interval.
     * Safe to call even when the thread was never started (interval==0). */
    cjsPeriodicDecisionCv.notify_all();
    cjsJunctionEvalCv.notify_all();

    auto joinThread = [](std::thread& t) {
        if (t.joinable())
            t.join();
    };

    joinThread(heartbeatThread);
    joinThread(ackHandlerThread);
    joinThread(hbWatchdogThread);
    joinThread(decisionFreshnessWatchdogThread);
    joinThread(periodicDecisionThread);
    joinThread(junctionEvalThread);

    /* Drop the junction state so a relaunch cannot inherit a latched A1 and
     * sound the siren for a vehicle that left while this SDM was down. */
    {
        std::lock_guard<std::mutex> lock(g_junctionMtx);
        g_junctions.clear();
    }

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
    /* Takes gwSockMtx itself, so it must not be called holding it. */
    closeGatewayTransport();

    /* Clear pending commands */
    {
        std::lock_guard<std::mutex> lock(commandStatusMtx);
        pendingCommands.clear();
    }

    cjs_log_close();
}
