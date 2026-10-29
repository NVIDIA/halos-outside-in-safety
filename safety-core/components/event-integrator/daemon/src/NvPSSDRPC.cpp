/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>

#include "NvPSSDRPC.hpp"
#include "NvPSB.h"
#include "pss_daemon.h"
#include "pss_message_validate.h"

/* Defined in NvPSSDaemon.cpp — heartbeat fail-safe policy (WARN_THRESHOLD = max/2). */
extern std::atomic<uint32_t> g_pssMaxHbFailures;
extern std::atomic<uint32_t> g_pssWarnThreshold;

namespace nvpss
{
namespace {

/* Send flags are the transport's business now; the server no longer
 * touches a descriptor, so MSG_NOSIGNAL is not its concern. */

/** Matches client interface SOCKET_READ_TIMEOUT_US (100ms) — bounds each blocking recv on this fd. */
constexpr int kPssClientSocketRecvTimeoutUs = 100000;

/* Map a CLIENT_* id to a short, stable, human-readable name for diagnostics.
 * Used in operator-facing log messages so psf.log does not misattribute
 * REPORT_SAFETY_EVENT traffic (e.g. SAIM trust reports looking like MDX
 * events). Returns a string literal; safe to concat into std::string. */
static const char* clientTypeName(uint8_t clientType) noexcept
{
    switch (clientType)
    {
        case CLIENT_MDX:                return "MDXClient";
        case CLIENT_SAFETY_MONITOR:     return "SafetyAIMonitor";
        case CLIENT_PSD_GATEWAY:        return "PSDGateway";
        case CLIENT_PERCEPTION_MONITOR: return "PerceptionMonitor";
        default:                        return "UnregisteredClient";
    }
}

/* The sources whose loss is a safety condition in its own right.
 * Each is the ONLY reporter of one class of evidence -- sensor health, AI-pipeline
 * trust, one MDX client's physical events -- so no other client's traffic can
 * stand in for it, and its loss must clear only when it comes back. An anonymous
 * MDX client (instance 0) is not identifiable, so it is not tracked. 0 for every
 * untracked client. */
static uint32_t presenceBitForClient(uint8_t clientType, uint8_t instance) noexcept
{
    switch (clientType)
    {
        case CLIENT_SAFETY_MONITOR:     return PSS_PRESENCE_SAFETY_MONITOR;
        case CLIENT_PERCEPTION_MONITOR: return PSS_PRESENCE_PERCEPTION_MONITOR;
        case CLIENT_MDX:
            return (instance >= 1U && instance <= PSS_MDX_INSTANCE_MAX)
                       ? PSS_PRESENCE_MDX_INSTANCE(instance) : 0U;
        default:                        return 0U;
    }
}

static EventCause monitorLostCause(uint8_t clientType, uint8_t instance) noexcept
{
    if (presenceBitForClient(clientType, instance) == 0U)
        return CAUSE_NONE;
    switch (clientType)
    {
        case CLIENT_SAFETY_MONITOR:     return CAUSE_SAFETY_MONITOR_LOST;
        case CLIENT_PERCEPTION_MONITOR: return CAUSE_PERCEPTION_MONITOR_LOST;
        case CLIENT_MDX:                return CAUSE_MDX_CLIENT_LOST;
        default:                        return CAUSE_NONE;
    }
}

/* Inverse of presenceBitForClient() for one bit; false for an unassigned bit. */
static bool clientForPresenceBit(uint32_t bit, uint8_t* clientType, uint8_t* instance) noexcept
{
    if (bit == PSS_PRESENCE_SAFETY_MONITOR)
    {
        *clientType = CLIENT_SAFETY_MONITOR; *instance = 0U; return true;
    }
    if (bit == PSS_PRESENCE_PERCEPTION_MONITOR)
    {
        *clientType = CLIENT_PERCEPTION_MONITOR; *instance = 0U; return true;
    }
    for (uint8_t k = 1U; k <= PSS_MDX_INSTANCE_MAX; ++k)
    {
        if (bit == PSS_PRESENCE_MDX_INSTANCE(k))
        {
            *clientType = CLIENT_MDX; *instance = k; return true;
        }
    }
    return false;
}

static const char* monitorLostRecordName(uint8_t clientType) noexcept
{
    switch (clientType)
    {
        case CLIENT_PERCEPTION_MONITOR: return "PSS_PERCEPTION_MONITOR_LOST";
        case CLIENT_MDX:                return "PSS_MDX_CLIENT_LOST";
        default:                        return "PSS_SAFETY_MONITOR_LOST";
    }
}

/**
 * Max wall-clock time to assemble one full request struct so a slow/stalled peer cannot block the RPC thread forever.
 */
constexpr int kRpcRecvAllDeadlineMs = 500;

constexpr int kRpcSendAllDeadlineMs = 500;

/** Poll wake-up period, so the run loop notices runRpcServer going false.
 *  100 ms, matching the select() timeout this replaced. */
constexpr uint32_t kRpcPollTimeoutMs = 100U;

/** Retry pause when a client's receive buffer is momentarily full. */
constexpr int kRpcSendRetrySleepMs = 1;

/**
 * Send exactly one whole message to a transport peer, or fail.
 *
 * The partial-write loop this replaced had no producer left once the socket
 * became SOCK_SEQPACKET: a datagram send is all-or-nothing, so there is no such
 * thing as half an NvPSSDRPCMsgResp on the wire.
 *
 * The DEADLINE is kept, and deliberately. The transport's Send is
 * non-blocking, so a client whose receive buffer is momentarily full returns
 * WOULD_BLOCK immediately. Failing on the first WOULD_BLOCK would convert a
 * transient into a dropped RPC response, where the code this replaces waited up
 * to kRpcSendAllDeadlineMs for the peer to drain. The transport exposes no
 * POLLOUT wait, so the wait is a short sleep-and-retry instead of a poll --
 * same bound, same semantics, and still bounded so one stalled client cannot
 * park the single-threaded RPC server thread.
 */
static bool sendAll(NvPSFTransport* t, NvPSFPeerId peer, const void* buf, size_t len)
{
    if (t == nullptr || peer == NVPSF_PEER_INVALID)
    {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kRpcSendAllDeadlineMs);

    for (;;)
    {
        const NvPSFTransportErr err = NvPSFTransportSend(t, peer, buf, len);
        if (err == NVPSF_TRANSPORT_OK)
        {
            return true;
        }
        if (err != NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            return false;      /* PEER_GONE or a hard error: not retryable */
        }
        /* Checked AFTER the attempt so a zero-length budget still tries once,
         * and before sleeping so the bound is never exceeded. */
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kRpcSendRetrySleepMs));
    }
}

static void logPssSendErrToClient(unsigned int client)
{
    const int e = errno;
    if (e == EPIPE)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING, "PSS send to client failed (peer disconnected)",
                       "client: " + std::to_string(client));
    }
    else
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSS is unable to send response to client",
                       "client: " + std::to_string(client) + ", errno: " + std::to_string(e));
    }
}

void sendReportSafetyEventResponse(NvPSFTransport* t, NvPSFPeerId peer, uint32_t reqSeqNo,
                                   uint8_t reportStatus, unsigned int client)
{
    NvPSSDRPCMsgResp resp = {};
    resp.respSeqNo = reqSeqNo;
    resp.size = 1;
    memset(resp.respPayload, 0, sizeof(resp.respPayload));
    resp.respPayload[0] = reportStatus;
    if (!sendAll(t, peer, &resp, sizeof(NvPSSDRPCMsgResp)))
        logPssSendErrToClient(client);
}


} // namespace

/**
 * Monotonic nanoseconds matching SafetyEvent.timestamp epoch (CLOCK_MONOTONIC).
 * Used for staleness checks and for timestamps on internally generated events.
 */
static uint64_t monotonicNowNs()
{
    static std::atomic<uint64_t> lastKnownNs{0U};
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "clock_gettime(CLOCK_MONOTONIC) failed, using last-known value",
                       "errno: " + std::to_string(errno));
        return lastKnownNs.load(std::memory_order_relaxed);
    }
    const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                       + static_cast<uint64_t>(ts.tv_nsec);
    lastKnownNs.store(now, std::memory_order_relaxed);
    return now;
}

/* Rate-limited rejection log for an unresolvable sensor identity.
 *
 * The floor is GLOBAL, not per (client, eventType). An earlier version keyed the
 * interval on the last client and type, which works for a single misbehaving
 * producer and fails for the normal case: with two mdx-client instances, or one
 * client emitting two bad event types, the key changes on every message, the
 * interval is skipped, and every rejected event logs a line -- exactly the flood
 * the limiter exists to prevent. A configuration error must not be able to bury
 * an unrelated fault in the same file.
 *
 * Suppressed rejections are counted and reported on the next line, so bounding
 * the log does not lose the fact that it is still happening.
 *
 * Placed after monotonicNowNs(), which it calls. Reached only from the single
 * rpcServerThread (see NvPSSDStartRPCServer), so the static state needs no lock;
 * if that ever changes this must be revisited rather than left to chance. */
static void logUnresolvedSensorRejection(unsigned int client, uint32_t eventType)
{
    static constexpr uint64_t kRepeatIntervalNs = 60ULL * 1000ULL * 1000ULL * 1000ULL;
    static uint64_t lastLogNs   = 0ULL;
    static uint64_t suppressed  = 0ULL;
    static bool     everLogged  = false;

    const uint64_t nowNs = monotonicNowNs();
    if (everLogged && (nowNs - lastLogNs) < kRepeatIntervalNs)
    {
        suppressed++;
        return;
    }

    std::string extra = "client: " + std::to_string(client) +
                        ", eventType: " + std::to_string(eventType);
    if (suppressed != 0ULL)
    {
        extra += ", suppressed since last line: " + std::to_string(suppressed);
    }
    extra += ". The producer could not resolve its sensor name against "
             "sensor_config; check the sensor name it reports.";

    lastLogNs  = nowNs;
    suppressed = 0ULL;
    everLogged = true;

    NvPSBWriteData(NVPSB_LOG_WARNING,
                   "Dropping REPORT_SAFETY_EVENT: sensor identity unresolved "
                   "(pipelineID 0 on a per-sensor event type)",
                   extra);
}


void NvPSSDRPC::processPendingDisconnects(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                          std::mutex& inputSafetyEventQueMutex)
{
    std::deque<std::pair<uint32_t, uint32_t>> batch;
    {
        std::lock_guard<std::mutex> pd(pendingDisconnectMutex);
        batch.swap(pendingDisconnectClients);
        pendingDisconnectSeen.clear();
    }

    /* A required source that never registered has no slot, so the tick cannot
     * queue it as a disconnect; it hands the source over here, where the input
     * queue is. */
    uint32_t neverRegistered = 0U;
    {
        std::lock_guard<std::mutex> hbLock(heartbeatMutex);
        neverRegistered = neverRegisteredPending_;
        neverRegisteredPending_ = 0U;
    }
    for (uint32_t bitPos = 0U; bitPos < 32U && neverRegistered != 0U; ++bitPos)
    {
        const uint32_t bit = 1U << bitPos;
        if ((neverRegistered & bit) == 0U)
            continue;
        neverRegistered &= ~bit;
        uint8_t type = 0U;
        uint8_t instance = 0U;
        if (clientForPresenceBit(bit, &type, &instance))
        {
            injectMonitorLostSwFail(UINT32_MAX, type, instance, "never_registered",
                                    inputSafetyEventQueRef, inputSafetyEventQueMutex);
        }
    }

    for (const auto& entry : batch)
    {
        const uint32_t slot = entry.first;
        const uint32_t enqueueGen = entry.second;
        if (slot >= maxClients)
            continue;
        if (slotGeneration_[slot].load(std::memory_order_relaxed) != enqueueGen)
            continue;
        const NvPSFPeerId peer = clientPeers[slot];
        if (peer == NVPSF_PEER_INVALID)
            continue;

        const uint8_t slotType = getClientType(slot);
        const uint8_t slotInstance = getClientInstance(slot);
        if (presenceBitForClient(slotType, slotInstance) != 0U)
        {
            /* A tracked source gets its own cause. Ordinary
             * traffic says nothing about whether it is still there, so the SDMs
             * must not let that clear this -- only the source coming back may. */
            injectMonitorLostSwFail(slot, slotType, slotInstance, "heartbeat_budget_exhausted",
                                    inputSafetyEventQueRef, inputSafetyEventQueMutex);
        }
        else if (slotType != 0)
        {
            SafetyEvent ev = {};
            ev.type = SW_FAIL;
            ev.timestamp = monotonicNowNs();
            ev.confidenceLevel = 1.0f;
            ev.processed = false;
            {
                int n = snprintf(ev.sensorIdentifier, sizeof(ev.sensorIdentifier), "PSS_RPC");
                if (n < 0 || static_cast<size_t>(n) >= sizeof(ev.sensorIdentifier))
                    ev.sensorIdentifier[sizeof(ev.sensorIdentifier) - 1] = '\0';
            }
            /* The client is carried in fusionMetadata.clientID below, so the
             * cause does not need to encode it the way the old
             * "heartbeat_fault_client_N" string did. */
            ev.cause = CAUSE_HEARTBEAT_FAULT;
            ev.fusionMetadata.clientID = static_cast<uint8_t>(slot & 0xFFU);
            pssSafetyEventSetCRC(&ev);

            {
                std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
                inputSafetyEventQueRef.push_back(std::make_pair(static_cast<int>(slot), ev));
            }
        }
        else
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                           "Closing unregistered connection: registration timeout",
                           "client: " + std::to_string(slot));
        }

        disconnectClient(slot);
    }
}

void NvPSSDRPC::heartbeatMonitorTick(uint32_t maxFailures, uint32_t warnThreshold)
{
    if (maxFailures == 0U)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> toDisconnect;

    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        for (auto& entry : clientLastHeartbeat)
        {
            const uint32_t clientId = entry.first;
            {
                const auto faultIt = hbFaultLatched.find(clientId);
                if (faultIt != hbFaultLatched.end() && faultIt->second)
                    continue;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - entry.second).count();
            /* HB_TIMEOUT_MS = HB_INTERVAL_MS + HB_STALE_GRACE_MS (pss_daemon.h): do not mark stale until after nominal period + slack. */
            if (elapsed <= static_cast<long long>(HB_TIMEOUT_MS))
                continue;

            const auto ctypeIt = clientTypeMap.find(clientId);
            const bool isRegistered = (ctypeIt != clientTypeMap.end());

            if (!isRegistered)
            {
                /* Connection accepted but never completed REGISTER_CLIENT.
                   Fault-latch immediately so processPendingDisconnects closes
                   it without generating a CRITICAL SW_FAIL safety event. */
                hbFaultLatched[clientId] = true;
                if (clientId < static_cast<uint32_t>(maxClients))
                    hbFaultLatchedFast[clientId].store(1U, std::memory_order_release);
                toDisconnect.push_back(clientId);
                continue;
            }

            uint32_t& miss = hbMissCount[clientId];
            if (miss < UINT32_MAX)
                ++miss;

            const uint32_t m = miss;
            if (clientId < static_cast<uint32_t>(maxClients))
                hbMissCountFast[clientId].store(m, std::memory_order_release);

            if (ctypeIt->second == CLIENT_SAFETY_MONITOR)
            {
                smMonitorClientId_ = clientId;
                smMonitorMiss_ = m;
                smOperationalCacheMaxMiss_.store(m, std::memory_order_release);
            }

            /* Log at tier milestones only (not every tick while stale) to limit CPU/string churn under many clients. */
            const char* tierNote = "";
            bool logMilestone = false;
            if (warnThreshold >= 1U && m <= warnThreshold && m == 1U)
            {
                logMilestone = true;
                tierNote = " (warn tier)";
            }
            else if (m > warnThreshold && m < maxFailures && m == warnThreshold + 1U)
            {
                logMilestone = true;
                tierNote = " (degraded tier)";
            }
            else if (m == maxFailures)
            {
                logMilestone = true;
                tierNote = " (failure tier)";
            }
            if (logMilestone)
            {
                NvPSBWriteData(NVPSB_LOG_WARNING,
                    "PSS RPC client " + std::to_string(clientId) + " heartbeat stale: miss_count=" +
                        std::to_string(m) + "/" + std::to_string(maxFailures) +
                        tierNote + " elapsed_ms=" + std::to_string(elapsed),
                    "");
            }

            {
                const auto faultIt = hbFaultLatched.find(clientId);
                const bool alreadyLatched = (faultIt != hbFaultLatched.end() && faultIt->second);
                if (m >= maxFailures && !alreadyLatched)
                {
                    hbFaultLatched[clientId] = true;
                    if (clientId < static_cast<uint32_t>(maxClients))
                        hbFaultLatchedFast[clientId].store(1U, std::memory_order_release);
                    toDisconnect.push_back(clientId);
                    if (ctypeIt->second == CLIENT_SAFETY_MONITOR)
                    {
                        smMonitorFault_ = true;
                        smOperationalCacheAnyFault_.store(true, std::memory_order_release);
                    }
                    if (presenceBitForClient(ctypeIt->second, clientInstanceLocked(clientId)) != 0U)
                        recomputePresenceMaskLocked();
                }
            }
        }
    }

    /*
     * The required sources (nvpss.conf required_safety_monitor,
     * required_perception_monitor, required_mdx_instances), once the budget a
     * registered client gets to miss heartbeats has run out since this PSS
     * started -- absence counted from PSS start exactly as a stale heartbeat is
     * counted from the last one:
     *
     *  - they join the EXPECTED set in every presence report from now on, so an
     *    SDM that never saw them -- including one restarted while they were down
     *    -- treats them as known and raises their loss while they are absent;
     *  - one that has NEVER registered since this PSS started is reported lost
     *    once, reason never_registered. The "once seen" rule cannot
     *    catch that case: nothing was ever seen.
     *
     * Not before the budget: a required source is given the same time to come up
     * at boot as it gets to recover from a late heartbeat, so a normal start does
     * not latch the SDMs.
     */
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        if (requiredSourceMask_ != 0U)
        {
            const long long budgetMs = static_cast<long long>(HB_TIMEOUT_MS) +
                static_cast<long long>(maxFailures - 1U) * static_cast<long long>(HB_INTERVAL_MS);
            const long long sinceStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - rpcStartTime_).count();
            if (sinceStartMs >= budgetMs)
            {
                if (!requiredSourcesDue_)
                {
                    requiredSourcesDue_ = true;
                    recomputeExpectedMaskLocked();
                }
                const uint32_t missing = requiredSourceMask_ & ~everRegisteredMask_ &
                                         ~neverRegisteredRaised_;
                neverRegisteredRaised_ |= missing;
                neverRegisteredPending_ |= missing;
            }
        }
    }

    if (!toDisconnect.empty())
    {
        std::lock_guard<std::mutex> pd(pendingDisconnectMutex);
        for (uint32_t cid : toDisconnect)
        {
            if (pendingDisconnectSeen.insert(cid).second)
            {
                const uint32_t gen = (cid < MAX_POSSIBLE_CLIENTS)
                    ? slotGeneration_[cid].load(std::memory_order_acquire)
                    : 0U;
                pendingDisconnectClients.push_back({cid, gen});
            }
        }
    }
}

NvPSSDRPC::NvPSSDRPC(NvPSSDRPCBackend backend, std::string channel, uint8_t maxClients,
                        uint8_t maxPendingClients):
                        backend(backend),channel(channel),maxClients(maxClients),
                        maxPendingClients(maxPendingClients),
                        hbMissCountFast(static_cast<size_t>(maxClients)),
                        hbFaultLatchedFast(static_cast<size_t>(maxClients))
{
    NvPSBWriteData(NVPSB_LOG_INFO, "Instance of NvPSSDRPC is created", "");
    transport = nullptr;
    std::fill(std::begin(clientPeers), std::end(clientPeers), NVPSF_PEER_INVALID);
    for (auto& g : slotGeneration_)
        g.store(0U, std::memory_order_relaxed);
}

NvPSSDRPC::~NvPSSDRPC(){}

NvPSSDErr NvPSSDRPC::NvPSSDInitRPCServer()
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    /*
     * `backend` was stored by the constructor and never read anywhere.
     * A caller could construct this server with NVSCI and get a socket server
     * regardless -- the member recorded an intent the code then ignored, which
     * is worse than not having it, because it reads like a supported choice.
     *
     * It is now load-bearing: an unsupported backend fails at init rather than
     * being silently overridden. The client side rejects the same value at all
     * four of its dispatch sites, so both ends now agree that SOCKET is the
     * only implemented transport instead of only the client saying so.
     */
    if (backend != SOCKET)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
                       "Unsupported PSS RPC backend requested; only AF_UNIX "
                       "SOCK_SEQPACKET socket transport is implemented",
                       "backend: " + std::to_string(static_cast<int>(backend)));
        return NVPSSD_FAIL;
    }

    /*
     * AF_UNIX SOCK_SEQPACKET through the transport.
     *
     * The RPC carries fixed-size NvPSSDRPCMsgReq/Resp structs, and a byte
     * stream gave no framing for them: every read had to be assembled by a
     * loop that could return short, and a desynchronised stream would silently
     * misinterpret one message as another on a path that carries client
     * registration, heartbeat tiering and SW_FAIL synthesis.
     *
     * There is NO wire compatibility with the old socket type: a SOCK_STREAM
     * client cannot connect to a SOCK_SEQPACKET listener at all -- connect()
     * fails with EPROTOTYPE. That is why all seven clients must
     * move in one change set, and why the client side emits an explicit
     * diagnostic for that errno rather than a generic connect failure.
     *
     * Going through the transport rather than raw socket/bind/listen also
     * retires the FD_SETSIZE ceiling (see the header), and adds SO_PEERCRED
     * validation at accept: registration
     * becomes authenticated instead of open to any local process that can
     * reach the socket path.
     *
     * NOTE for the integrator: that credential check is new on THIS link, so
     * Confirming the shared GID mapping across container runtimes
     * now covers the PSS RPC socket as well as the others. A client outside the
     * allowed group is refused at connect.
     */
    NvPSFTransportCfg cfg = {};
    cfg.kind            = NVPSF_TRANSPORT_UNIX_SOCKET;
    cfg.role            = NVPSF_ROLE_SERVER;
    cfg.link            = NVPSF_LINK_PSS_RPC;
    /* Twice maxClients: the surplus is the transport's pool of accepted-but-
     * unregistered peers, so a connect flood cannot deny registration to a
     * legitimate client (the two-stage admission model). */
    cfg.maxPeers        = static_cast<uint32_t>(maxClients) * 2U;
    cfg.maxMessageBytes = (sizeof(NvPSSDRPCMsgReq) > sizeof(NvPSSDRPCMsgResp))
                              ? sizeof(NvPSSDRPCMsgReq)
                              : sizeof(NvPSSDRPCMsgResp);
    /* Zero: this is not the gateway -> SDM link, so a failed send does not burn
     * a sequence number and cannot latch anything. */
    cfg.minQueueDepth   = 0U;
    cfg.recvTimeoutMs   = kRpcPollTimeoutMs;
    cfg.sendTimeoutMs   = 0U;
    cfg.unixPath        = channel.c_str();
    cfg.unixMode        = 0660;
    cfg.allowedGid      = getgid();

    if (NvPSFTransportOpen(&cfg, &transport) != NVPSF_TRANSPORT_OK || transport == nullptr)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error opening PSS RPC transport", "channel: " + channel);
        transport = nullptr;
        err = NVPSSD_FAIL;
        goto exit;
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon listening on", "channel: " + channel);

exit:
    if (err != NVPSSD_SUCCESS && transport != nullptr)
    {
        (void)NvPSFTransportClose(transport);
        transport = nullptr;
    }
    return err;
}

NvPSSDErr NvPSSDRPC::NvPSSDStartRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex, const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb,
                                void* trustReportCtx,
                                NvPSSDRequiredSourceCallback requiredSourceCb,
                                void* requiredSourceCtx)
{
    runRpcServer.store(true);
    rpcServerThread = std::thread(&NvPSSDRPC::NvPSSDRunRPCServer, this, std::ref(inputSafetyEventQueRef),
                        std::ref(inputSafetyEventQueMutex), thresholdConfidence, std::ref(rpcServerTerminationCV),
                        trustReportCb, trustReportCtx, requiredSourceCb, requiredSourceCtx);
    return NVPSSD_SUCCESS;
}


NvPSSDErr NvPSSDRPC::NvPSSDRunRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex, const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb,
                                void* trustReportCtx,
                                NvPSSDRequiredSourceCallback requiredSourceCb,
                                void* requiredSourceCtx)
{
    unsigned int client = 0;
    SafetyEvent reportedEvent = {};

    while (runRpcServer.load())
    {
        processPendingDisconnects(inputSafetyEventQueRef, inputSafetyEventQueMutex);

        /*
         * Poll replaces select(), and with it the FD_SETSIZE ceiling. A client
         * whose descriptor happened to land above 1024 used to be refused for a
         * reason that had nothing to do with capacity; the transport indexes by
         * NvPSFPeerId instead.
         *
         * POLL IS NOT A PASSIVE SLEEP. On POLLHUP it emits PEER_GONE *and
         * releases the peer slot*, so the events must be inspected: discarding
         * them consumes the disconnect, after which Recv reports WOULD_BLOCK
         * instead of PEER_GONE and the next Poll returns TIMEOUT immediately
         * without sleeping -- a pinned core on a link that never reclaims its
         * slots.
         */
        NvPSFTransportEvent events[MAX_POSSIBLE_CLIENTS * 2 + 4];
        size_t              eventCount = 0;
        const NvPSFTransportErr perr =
            NvPSFTransportPoll(transport, kRpcPollTimeoutMs, events,
                               sizeof(events) / sizeof(events[0]), &eventCount);
        if (perr != NVPSF_TRANSPORT_OK && perr != NVPSF_TRANSPORT_TIMEOUT)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "PSS RPC transport poll error",
                           std::string("err: ") + NvPSFTransportErrName(perr));
            /* Back off so a persistent failure cannot peg a core or flood the
             * log without bound. */
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        for (size_t e = 0; e < eventCount; e++)
        {
            const NvPSFTransportEvent& ev = events[e];

            if (ev.kind == NVPSF_EVENT_PEER_CONNECTED)
            {
                /* Credentials were validated by the transport before this
                 * event; admission here is only about capacity. This is the
                 * accept() arm, minus the FD_SETSIZE rejection. */
                for (client = 0; client < maxClients; client++)
                {
                    if (clientPeers[client] == NVPSF_PEER_INVALID)
                    {
                        clearClientState(static_cast<uint32_t>(client));
                        clientPeers[client] = ev.peer;
                        slotGeneration_[client].fetch_add(1U, std::memory_order_release);
                        {
                            /* Seed clientLastHeartbeat so heartbeatMonitorTick can
                               detect and clean up connections that never complete
                               REGISTER_CLIENT (registration timeout).  On successful
                               registration, validateAndAcceptRegistration refreshes
                               the timestamp and resets counters for real HB tracking. */
                            std::lock_guard<std::mutex> hbLock(heartbeatMutex);
                            clientLastHeartbeat[static_cast<uint32_t>(client)] =
                                std::chrono::steady_clock::now();
                            hbMissCount[static_cast<uint32_t>(client)] = 0U;
                            hbMissCountFast[client].store(0U, std::memory_order_release);
                            hbFaultLatched[static_cast<uint32_t>(client)] = false;
                            hbFaultLatchedFast[client].store(0U, std::memory_order_release);
                        }
                        break;
                    }
                }
                if (client >= maxClients)
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "admission dropped: no free client slot", "");
                    (void)NvPSFTransportDropPeer(transport, ev.peer);
                }
                continue;
            }

            if (ev.kind == NVPSF_EVENT_PEER_GONE)
            {
                for (client = 0; client < maxClients; client++)
                {
                    if (clientPeers[client] == ev.peer)
                    {
                        /* A crashed monitor used to vanish here with no fault
                         * at all. */
                        dropClient(static_cast<uint32_t>(client), "connection_dropped",
                                   inputSafetyEventQueRef, inputSafetyEventQueMutex);
                        break;
                    }
                }
                continue;
            }
        }

        /*
         * Drain the messages that are ready. Recv is round-robin across peers
         * and reports which one a message came from, so this replaces the
         * per-slot FD_ISSET scan: there is no descriptor to test, and the
         * message that comes back may belong to any admitted client.
         *
         * BOUNDED, and the bound is not decoration. The select() loop this
         * replaced serviced at most one message per client per pass and then
         * re-read runRpcServer and ran processPendingDisconnects(). An
         * unbounded drain loses both: a client that keeps the queue non-empty
         * -- any process in the allowed GID, against a single-threaded server
         * -- would park this thread inside the drain forever, so
         * NvPSSDCloseRPCServer() would block in its join and fault-latched
         * clients would never be disconnected.
         *
         * maxClients restores the original bound exactly. Nothing is lost:
         * whatever is still queued is taken on the next pass, and Poll returns
         * immediately while data remains, so no latency is added.
         */
        for (unsigned int drained = 0; drained < maxClients; drained++)
        {
            NvPSFPeerId peer = NVPSF_PEER_INVALID;
            size_t      nread = 0;
            NvPSSDRPCMsgReq reqBuf = {};
            const NvPSFTransportErr rerr =
                NvPSFTransportRecv(transport, &peer, &reqBuf, sizeof(reqBuf), &nread);

            if (rerr == NVPSF_TRANSPORT_WOULD_BLOCK || rerr == NVPSF_TRANSPORT_TIMEOUT)
            {
                break;                 /* nothing further ready this pass */
            }

            /* Resolve the peer to its slot before anything else: a message from
             * a peer with no slot has nowhere to be attributed and must not be
             * acted on. */
            uint32_t slot = maxClients;
            for (client = 0; client < maxClients; client++)
            {
                if (clientPeers[client] == peer)
                {
                    slot = client;
                    break;
                }
            }

            if (rerr == NVPSF_TRANSPORT_PEER_GONE)
            {
                /* A disconnect can surface as a return code instead of an
                 * event, and when it does the transport has already released
                 * the slot -- so no PEER_GONE event follows and this is the
                 * only chance to reclaim. */
                if (slot < maxClients)
                {
                    dropClient(slot, "connection_dropped",
                               inputSafetyEventQueRef, inputSafetyEventQueMutex);
                }
                continue;
            }
            if (slot >= maxClients)
            {
                continue;              /* unknown or already-reclaimed peer */
            }
            client = slot;

            if (rerr == NVPSF_TRANSPORT_TRUNCATED || nread != sizeof(NvPSSDRPCMsgReq))
            {
                /* Wrong-sized message. The kernel has already discarded the
                 * rest of the datagram, so this is a protocol error rather
                 * than something to accumulate towards -- and on this path a
                 * misparse would be a registration or heartbeat acted on as
                 * the wrong message. */
                NvPSBWriteData(NVPSB_LOG_WARNING,
                               "PSS Daemon: malformed request size from client; dropping client",
                               "client: " + std::to_string(client) +
                                   ", bytes: " + std::to_string(nread));
                dropClient(static_cast<uint32_t>(client), "protocol_error_malformed_request",
                           inputSafetyEventQueRef, inputSafetyEventQueMutex);
                continue;
            }
            if (rerr != NVPSF_TRANSPORT_OK)
            {
                NvPSBWriteData(NVPSB_LOG_WARNING, "PSS Daemon: recv on client failed",
                               "client: " + std::to_string(client) +
                                   ", err: " + NvPSFTransportErrName(rerr));
                dropClient(static_cast<uint32_t>(client), "protocol_error_recv_failed",
                           inputSafetyEventQueRef, inputSafetyEventQueMutex);
                continue;
            }

            {
                /* Already received and size-checked above; recvAll and its
                 * assembly loop are gone with the byte stream that needed
                 * them. `req` is exactly one whole message from `peer`. */
                const NvPSSDRPCMsgReq& req = reqBuf;
                NvPSSDRPCMsgResp resp = {};
                if (req.msg != SEND_HEARTBEAT)
                {
                    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon has received message",
                                  "Msg: " + std::to_string(req.msg) + ", SeqNo: " + std::to_string(req.reqSeqNo));
                }

                uint8_t reportStatus = REPORT_ACCEPTED;
                switch(req.msg)
                {
                        case REGISTER_CLIENT:
                        {
                            resp.respSeqNo = 0;
                            resp.size = 5; /* 4 bytes client id + 1 byte status */
                            memset(resp.respPayload, 0, sizeof(resp.respPayload));
                            NvPSFPeerId evictedPeer = NVPSF_PEER_INVALID;
                            uint32_t evictedSlot = UINT32_MAX;
                            uint8_t evictedType = 0U;
                            uint8_t evictedInstance = 0U;
                            {
                                const uint32_t gen = slotGeneration_[client].load(std::memory_order_relaxed);
                                const uint32_t encodedId = (gen << 8) | static_cast<uint32_t>(client);
                                const uint32_t wireId = htonl(encodedId);
                                memcpy(resp.respPayload, &wireId, sizeof(wireId));
                                const uint8_t regStatus = validateAndAcceptRegistration(
                                    static_cast<uint32_t>(client), req, &evictedPeer,
                                    &evictedSlot, &evictedType, &evictedInstance);
                                resp.respPayload[4] = regStatus;
                            }
                            /* Outside every lock, by construction. */
                            if (evictedPeer != NVPSF_PEER_INVALID && transport != nullptr)
                            {
                                (void)NvPSFTransportDropPeer(transport, evictedPeer);
                            }
                            /* Only a heartbeat-faulted singleton is evicted, and its
                             * pending disconnect -- which would have raised the loss --
                             * now finds the slot empty and does nothing. Raise it here
                             * so a monitor replaced in that window is not a silent gap. */
                            if (evictedSlot != UINT32_MAX)
                            {
                                injectMonitorLostSwFail(evictedSlot, evictedType, evictedInstance,
                                                        "heartbeat_budget_exhausted",
                                                        inputSafetyEventQueRef,
                                                        inputSafetyEventQueMutex);
                            }
                            /* An eviction clears the evicted slot's state; its records
                             * were deferred until the lock was released. */
                            flushDeferredRecords();
                            /* Logged here, outside heartbeatMutex, rather than in
                             * validateAndAcceptRegistration(), which holds it. */
                            if (resp.respPayload[4] == REGISTER_ACCEPTED && req.size >= 2U &&
                                req.reqPayload[1] != 0U)
                            {
                                NvPSBWriteData(NVPSB_LOG_INFO, "REGISTER_CLIENT: MDX instance registered",
                                               "client: " + std::to_string(client) +
                                                   ", instance: " +
                                                   std::to_string(static_cast<unsigned>(req.reqPayload[1])));
                            }
                            if (!sendAll(transport, peer, &resp, sizeof(NvPSSDRPCMsgResp)))
                                logPssSendErrToClient(client);
                            if (resp.respPayload[4] != REGISTER_ACCEPTED)
                                dropClient(static_cast<uint32_t>(client),
                                           "protocol_error_register_rejected",
                                           inputSafetyEventQueRef, inputSafetyEventQueMutex);
                            break;
                        }

                        case REPORT_SAFETY_EVENT:
                            {
                                reportStatus = REPORT_ACCEPTED;
                                if (getClientType(static_cast<uint32_t>(client)) == 0)
                                {
                                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                                   "Dropping REPORT_SAFETY_EVENT: client not registered",
                                                   "client: " + std::to_string(client));
                                    reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                    sendReportSafetyEventResponse(transport, peer, req.reqSeqNo, reportStatus, client);
                                    disconnectClient(static_cast<uint32_t>(client));
                                    break;
                                }
                                if (reportStatus == REPORT_ACCEPTED)
                                {
                                    uint32_t miss = 0U;
                                    uint32_t faultLatched = 0U;
                                    if (client < maxClients)
                                    {
                                        miss = hbMissCountFast[client].load(std::memory_order_acquire);
                                        faultLatched = hbFaultLatchedFast[client].load(std::memory_order_acquire);
                                    }
                                    const uint32_t maxF = g_pssMaxHbFailures.load();
                                    const uint32_t warnW = g_pssWarnThreshold.load();
                                    if (maxF > 0U)
                                    {
                                        if (faultLatched != 0U)
                                        {
                                            NvPSBWriteData(NVPSB_LOG_WARNING,
                                                           "Dropping REPORT_SAFETY_EVENT: heartbeat fault latched (tier-3)",
                                                           "client: " + std::to_string(client));
                                            reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                        }
                                        else if (miss >= maxF)
                                        {
                                            NvPSBWriteData(NVPSB_LOG_WARNING,
                                                           "Dropping REPORT_SAFETY_EVENT: miss_count >= max (tier-3, disconnect pending)",
                                                           "client: " + std::to_string(client) +
                                                               " miss_count: " + std::to_string(miss));
                                            reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                        }
                                        else if (miss > warnW && miss < maxF)
                                        {
                                            NvPSBWriteData(NVPSB_LOG_WARNING,
                                                           "Dropping REPORT_SAFETY_EVENT: client in heartbeat fault tier (degraded)",
                                                           "client: " + std::to_string(client) +
                                                               " miss_count: " + std::to_string(miss));
                                            reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                        }
                                    }
                                }
                                if (reportStatus == REPORT_ACCEPTED)
                                {
                                    if (static_cast<size_t>(req.size) != sizeof(SafetyEvent))
                                    {
                                        NvPSBWriteData(NVPSB_LOG_WARNING, "Dropping REPORT_SAFETY_EVENT: payload size mismatch",
                                                      "size: " + std::to_string(req.size) + ", expected: " + std::to_string(sizeof(SafetyEvent)));
                                        reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                    }
                                    else
                                    {
                                    memcpy(&reportedEvent, req.reqPayload, sizeof(SafetyEvent));

                                    {
                                        const uint32_t valErr = validateSafetyEvent(&reportedEvent);
                                        if (valErr != PSS_VALID)
                                        {
                                            char hexBuf[12];
                                            int n = snprintf(hexBuf, sizeof(hexBuf), "0x%x", valErr);
                                            if (n < 0 || static_cast<size_t>(n) >= sizeof(hexBuf))
                                                hexBuf[0] = '\0';
                                            NvPSBWriteData(NVPSB_LOG_WARNING,
                                                "Dropping REPORT_SAFETY_EVENT: validation failed",
                                                "client: " + std::to_string(client) +
                                                    ", errors: " + hexBuf);
                                            reportStatus = REPORT_REJECTED_VALIDATION_FAILED;
                                        }
                                    }
                                    /* Resolve the reporter's registered client type once; reused by
                                     * the diagnostic log below and the trust-report authorization
                                     * gate further down. Avoids the pre-existing "from MDXClient"
                                     * misattribution for SAIM trust reports in psf.log. */
                                    const uint32_t clientId = static_cast<uint32_t>(client);
                                    const uint8_t reporterClientType = getClientType(clientId);

                                    if (reportStatus == REPORT_ACCEPTED)
                                    {
                                    NvPSBWriteData(NVPSB_LOG_INFO,
                                                  std::string("ENTRY POINT: Received safety event from ") +
                                                      clientTypeName(reporterClientType),
                                                  "client: " + std::to_string(client) +
                                                  ", Event Type: " + std::to_string(reportedEvent.type) +
                                                  ", Confidence: " + std::to_string(reportedEvent.confidenceLevel));
                                    }

                                    if (reportStatus == REPORT_ACCEPTED)
                                    {

                                    /* Per-event-type authorization:
                                     *   SENSOR_*      -> only CLIENT_SAFETY_MONITOR
                                     *   AI_PIPELINE_* -> only CLIENT_PERCEPTION_MONITOR
                                     * Both require a recent heartbeat. The mirror
                                     * check in SafetyEventManager::OnTrustReport must
                                     * stay consistent. */
                                    const bool isSensorTrustReport     = (reportedEvent.type == SENSOR_INVALID      || reportedEvent.type == SENSOR_VALID);
                                    const bool isAIPipelineTrustReport = (reportedEvent.type == AI_PIPELINE_INVALID || reportedEvent.type == AI_PIPELINE_VALID);
                                    const bool isTrustReportType       = isSensorTrustReport || isAIPipelineTrustReport;
                                    const bool sensorReporterOk        = (reporterClientType == CLIENT_SAFETY_MONITOR);
                                    const bool aiPipelineReporterOk    = (reporterClientType == CLIENT_PERCEPTION_MONITOR);
                                    const bool reporterAuthorized      = (isSensorTrustReport && sensorReporterOk) ||
                                                                         (isAIPipelineTrustReport && aiPipelineReporterOk);

                                    /* Sensor identity is resolvable, or the event does not name a
                                     * sensor at all.
                                     *
                                     * pipelineID == 0 carries three legitimate meanings, and each
                                     * must keep working:
                                     *   - AI_PIPELINE_*   Perception Monitor reports pipeline-wide trust, not a
                                     *                     camera (pcm_event_reporter.cpp).
                                     *   - SW_FAIL         no sensor is named; the daemon's own
                                     *                     heartbeat fault and a client's Kafka
                                     *                     fault both use it.
                                     *   - PSS_STATUS_NOOP daemon-built, never arrives here.
                                     *
                                     * A PER-SENSOR event is different: pipelineID == 0 there means
                                     * the producer could not resolve the sensor name and fell back
                                     * to the daemon-internal passthrough channel. Such an event
                                     * would skip the sensor_config membership check in
                                     * ValidateEventSource and would be stamped isHealthy=true
                                     * forever, because QueryTrustState(0) can never report a
                                     * sensor invalid. Neither CCPLEX decision loop filters by
                                     * pipelineID, so it would drive a real ATL or PXC decision
                                     * while being structurally exempt from perception-health
                                     * gating. Refuse it here, at the trust boundary, rather than
                                     * relying on every client being correct. */
                                    const bool isPipelineWideType = isAIPipelineTrustReport;
                                    const bool isSensorBoundType  = !isPipelineWideType &&
                                                                    (reportedEvent.type != SW_FAIL) &&
                                                                    (reportedEvent.type != PSS_STATUS_NOOP);
                                    const bool unresolvedSensor   = isSensorBoundType &&
                                                                    (reportedEvent.fusionMetadata.pipelineID == 0U);
                                    if (unresolvedSensor)
                                    {
                                        /* Deduplicated per (client, type): a misconfigured producer
                                         * emits continuously, and an unbounded log here could mask
                                         * an unrelated fault. */
                                        logUnresolvedSensorRejection(client, reportedEvent.type);
                                        reportStatus = REPORT_REJECTED_UNRESOLVED_SENSOR;
                                    }
                                    else if (isTrustReportType && (!reporterAuthorized || !hasRecentHeartbeat(clientId)))
                                    {
                                        NvPSBWriteData(NVPSB_LOG_WARNING,
                                                       "Dropping trust report: client type not authorized for this event type or heartbeat stale",
                                                       "client: " + std::to_string(client) +
                                                           ", reporterType: " + std::to_string(reporterClientType) +
                                                           ", eventType: " + std::to_string(reportedEvent.type));
                                        reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                    }
                                    else if (isTrustReportType)
                                    {
                                        /* Safety Monitor with recent heartbeat: accept via trust-report callback. */
                                        bool accept = (trustReportCb != nullptr && trustReportCtx != nullptr) &&
                                            trustReportCb(trustReportCtx, clientId, reporterClientType, &reportedEvent);
                                        if (!accept)
                                        {
                                            NvPSBWriteData(NVPSB_LOG_WARNING, "Dropping unauthorized trust report", "client: " + std::to_string(client));
                                            reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                        }
                                        else
                                        {
                                            std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
                                            inputSafetyEventQueRef.push_back(std::make_pair(client, reportedEvent));
                                        }
                                    }
                                    else
                                    {
                                        /* Enforce payload clientID matches connection id for non-trust-report events. */
                                        const bool clientIdMismatch = (client > 0xFFU || static_cast<uint8_t>(client) != reportedEvent.fusionMetadata.clientID);
                                        if (clientIdMismatch)
                                        {
                                            NvPSBWriteData(NVPSB_LOG_WARNING, "Dropping safety event: payload clientID does not match RPC connection",
                                                          "connection: " + std::to_string(client) +
                                                          ", payload clientID: " + std::to_string(reportedEvent.fusionMetadata.clientID));
                                            reportStatus = REPORT_REJECTED_CLIENTID_MISMATCH;
                                        }
                                        else if (reportedEvent.confidenceLevel >= thresholdConfidence)
                                        {
                                            std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
                                            inputSafetyEventQueRef.push_back(std::make_pair(client, reportedEvent));
                                        }
                                        else
                                        {
                                            NvPSBWriteData(NVPSB_LOG_WARNING, "Dropping the reported event due to low confidenceLevel",
                                                          "Confidence: " + std::to_string(reportedEvent.confidenceLevel));
                                            reportStatus = REPORT_REJECTED_LOW_CONFIDENCE;
                                        }
                                    }
                                    }
                                    }
                                }
                                /*
                                 * Consecutive-rejection accounting, at the
                                 * one place where reportStatus is final for this
                                 * path. The unregistered-client branch above
                                 * returns early and tears the slot down, so it
                                 * has no episode to track.
                                 */
                                if (reportStatus == REPORT_ACCEPTED)
                                {
                                    noteReportAccepted(static_cast<uint32_t>(client),
                                                       reportedEvent.fusionMetadata.pipelineID,
                                                       reportedEvent.type);
                                }
                                else if (noteReportRejected(static_cast<uint32_t>(client),
                                                            reportStatus,
                                                            requiredSourceCb,
                                                            requiredSourceCtx))
                                {
                                    injectClientInputUnusableSwFail(
                                        static_cast<uint32_t>(client),
                                        inputSafetyEventQueRef,
                                        inputSafetyEventQueMutex);
                                }
                                sendReportSafetyEventResponse(transport, peer, req.reqSeqNo, reportStatus, client);
                            }
                            break;

                        case TERMINATE_CLIENT:
                            resp.respSeqNo = req.reqSeqNo;
                            resp.size = 1;
                            memset(resp.respPayload, 0, sizeof(resp.respPayload));
                            resp.respPayload[0] = TERMINATE_ACCEPTED;
                            if (!sendAll(transport, peer, &resp, sizeof(NvPSSDRPCMsgResp)))
                                logPssSendErrToClient(client);
                            /* An orderly unregister is a loss too: the site is
                             * exactly as unmonitored as after a crash. */
                            dropClient(static_cast<uint32_t>(client), "unregistered",
                                       inputSafetyEventQueRef, inputSafetyEventQueMutex);
                            break;

                        case SEND_HEARTBEAT:
                            if (req.size < 2U)
                            {
                                NvPSBWriteData(NVPSB_LOG_WARNING, "SEND_HEARTBEAT: payload size too small",
                                               "client: " + std::to_string(client) +
                                                   ", size: " + std::to_string(req.size));
                                break;
                            }
                            if (req.reqPayload[0] != HB_MSG)
                            {
                                NvPSBWriteData(NVPSB_LOG_WARNING, "SEND_HEARTBEAT: invalid HB marker",
                                               "client: " + std::to_string(client));
                                break;
                            }
                            {
                                const uint8_t registeredType = getClientType(static_cast<uint32_t>(client));
                                if (registeredType == 0)
                                {
                                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                                   "SEND_HEARTBEAT rejected: client not registered (REGISTER_CLIENT required first)",
                                                   "client: " + std::to_string(client));
                                    disconnectClient(static_cast<uint32_t>(client));
                                    break;
                                }
                                const uint8_t hbClientType = req.reqPayload[1];
                                if (hbClientType != registeredType)
                                {
                                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                                   "SEND_HEARTBEAT: type mismatch, disconnecting",
                                                   "client: " + std::to_string(client) +
                                                       ", hb_type: " + std::to_string(hbClientType) +
                                                       ", registered: " + std::to_string(registeredType));
                                    dropClient(static_cast<uint32_t>(client),
                                               "protocol_error_heartbeat_type_mismatch",
                                               inputSafetyEventQueRef, inputSafetyEventQueMutex);
                                    break;
                                }
                                updateClientHeartbeat(static_cast<uint32_t>(client), registeredType);
                                {
                                    NvPSSDRPCMsgResp hbResp = {};
                                    hbResp.respSeqNo = req.reqSeqNo;
                                    hbResp.size = 1;
                                    hbResp.respPayload[0] = HEARTBEAT_ACK;
                                    if (!sendAll(transport, peer, &hbResp, sizeof(NvPSSDRPCMsgResp)))
                                        logPssSendErrToClient(client);
                                }
                            }
                            break;

                        default:
                            NvPSBWriteData(NVPSB_LOG_WARNING, "Invalid message from the client", "");
                            break;
                }
            }
        }
    }
    return NVPSSD_SUCCESS;
}

uint8_t NvPSSDRPC::validateAndAcceptRegistration(uint32_t clientSlot, const NvPSSDRPCMsgReq& req,
                                                 NvPSFPeerId* outEvictedPeer,
                                                 uint32_t* outEvictedSlot,
                                                 uint8_t* outEvictedType,
                                                 uint8_t* outEvictedInstance)
{
    /* Set before any early return: an eviction that happens under the lock
     * below has to be dropped by the caller, once the lock is gone. */
    if (outEvictedPeer != nullptr)
    {
        *outEvictedPeer = NVPSF_PEER_INVALID;
    }
    if (outEvictedSlot != nullptr)
    {
        *outEvictedSlot = UINT32_MAX;
    }
    if (outEvictedType != nullptr)
    {
        *outEvictedType = 0U;
    }
    if (outEvictedInstance != nullptr)
    {
        *outEvictedInstance = 0U;
    }
    if (req.size < 1U)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "REGISTER_CLIENT: no clientType in payload", "");
        return REGISTER_REJECTED_INVALID_TYPE;
    }

    const uint8_t reqClientType = req.reqPayload[0];
    if (reqClientType != CLIENT_MDX &&
        reqClientType != CLIENT_SAFETY_MONITOR &&
        reqClientType != CLIENT_PSD_GATEWAY &&
        reqClientType != CLIENT_PERCEPTION_MONITOR)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "REGISTER_CLIENT: invalid clientType",
                       "type: " + std::to_string(reqClientType));
        return REGISTER_REJECTED_INVALID_TYPE;
    }

    /* Optional second byte: the MDX instance id. A client built before it
     * existed sends size 1 and is instance 0 -- anonymous, exactly as before. */
    const uint8_t reqInstance = (req.size >= 2U) ? req.reqPayload[1] : 0U;
    if (reqInstance > PSS_MDX_INSTANCE_MAX || (reqInstance != 0U && reqClientType != CLIENT_MDX))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "REGISTER_CLIENT: invalid instance id (MDX only, 1.." +
                           std::to_string(PSS_MDX_INSTANCE_MAX) + ")",
                       "type: " + std::to_string(reqClientType) +
                           ", instance: " + std::to_string(reqInstance));
        return REGISTER_REJECTED_INVALID_INSTANCE;
    }

    /* A numbered MDX instance is a singleton by (type, instance): two clients
     * claiming the same identity would make "that instance came back" mean
     * nothing. */
    const bool isSingletonType = (reqClientType == CLIENT_SAFETY_MONITOR ||
                                  reqClientType == CLIENT_PSD_GATEWAY ||
                                  reqClientType == CLIENT_PERCEPTION_MONITOR ||
                                  reqInstance != 0U);
    std::lock_guard<std::mutex> hbLock(heartbeatMutex);
    if (isSingletonType)
    {
        uint32_t staleSlot = UINT32_MAX;
        for (const auto& kv : clientTypeMap)
        {
            if (kv.second != reqClientType ||
                clientInstanceLocked(kv.first) != reqInstance ||
                kv.first == clientSlot ||
                kv.first >= MAX_POSSIBLE_CLIENTS ||
                clientPeers[kv.first] == NVPSF_PEER_INVALID)
                continue;

            {
                const auto faultIt = hbFaultLatched.find(kv.first);
                const bool isLatched = (faultIt != hbFaultLatched.end() && faultIt->second);
                if (!isLatched)
                {
                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                   "REGISTER_CLIENT: duplicate singleton client type rejected",
                                   "type: " + std::to_string(reqClientType) +
                                       ", instance: " + std::to_string(reqInstance));
                    return REGISTER_REJECTED_DUPLICATE_TYPE;
                }
            }
            staleSlot = kv.first;
        }
        if (staleSlot != UINT32_MAX)
        {
            /* Handed out rather than dropped here: heartbeatMutex is held for
             * the rest of this function, and DropPeer must not run under it. */
            const NvPSFPeerId evicted = disconnectClientLocked(staleSlot);
            if (outEvictedPeer != nullptr)
            {
                *outEvictedPeer = evicted;
            }
            if (outEvictedSlot != nullptr)
            {
                *outEvictedSlot = staleSlot;
            }
            if (outEvictedType != nullptr)
            {
                *outEvictedType = reqClientType;
            }
            if (outEvictedInstance != nullptr)
            {
                *outEvictedInstance = reqInstance;
            }
            NvPSBWriteData(NVPSB_LOG_WARNING,
                           "REGISTER_CLIENT: evicted fault-latched stale singleton",
                           "type: " + std::to_string(reqClientType));
        }
    }

    clientTypeMap[clientSlot] = reqClientType;
    clientInstanceMap_[clientSlot] = reqInstance;
    {
        /* Registered: enough to rule out never_registered, NOT enough to be
         * expected. It becomes expected on its first heartbeat, when it also
         * becomes present (see updateClientHeartbeat). */
        const uint32_t bit = presenceBitForClient(reqClientType, reqInstance);
        everRegisteredMask_ |= bit;
    }

    /* Refresh HB tracking to registration time.  The accept handler seeds
       clientLastHeartbeat at connect time (doubles as a registration-timeout
       clock: heartbeatMonitorTick fast-tracks unregistered stale slots).
       Refreshing here resets the timeout to registration time and ensures
       the invariant: registration always implies HB tracking. */
    clientLastHeartbeat[clientSlot] = std::chrono::steady_clock::now();
    hbMissCount[clientSlot] = 0U;
    if (clientSlot < static_cast<uint32_t>(maxClients))
        hbMissCountFast[clientSlot].store(0U, std::memory_order_release);
    hbFaultLatched[clientSlot] = false;
    if (clientSlot < static_cast<uint32_t>(maxClients))
        hbFaultLatchedFast[clientSlot].store(0U, std::memory_order_release);

    if (reqClientType == CLIENT_SAFETY_MONITOR)
    {
        smMonitorClientId_ = clientSlot;
        smMonitorMiss_ = 0U;
        smMonitorFault_ = false;
        smOperationalCacheMaxMiss_.store(0U, std::memory_order_release);
        smOperationalCacheAnyFault_.store(false, std::memory_order_release);
    }
    /* A registration is not presence: this client must heartbeat first. */
    clientHeartbeatCount.erase(clientSlot);
    recomputePresenceMaskLocked();
    return REGISTER_ACCEPTED;
}

void NvPSSDRPC::setRequiredSourceMask(uint32_t mask)
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    requiredSourceMask_ = mask & (PSS_PRESENCE_SAFETY_MONITOR | PSS_PRESENCE_PERCEPTION_MONITOR |
                                  PSS_PRESENCE_MDX_MASK);
    recomputeExpectedMaskLocked();
}

void NvPSSDRPC::setClientRejectErrorThreshold(uint32_t threshold)
{
    if (threshold < NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_MIN ||
        threshold > NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_MAX)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    clientRejectErrorThreshold_ = threshold;
}

void NvPSSDRPC::noteReportAccepted(uint32_t clientId, uint8_t pipelineId, EventType eventType)
{
    std::unique_lock<std::mutex> lock(heartbeatMutex);

    /* An episode that produced a record also records how it ended. The streak
     * keeps counting after the threshold precisely so this number is exact; an
     * episode that never reached the threshold produced no record and needs no
     * closing one. */
    const uint32_t endedStreak = rejectStreak_[clientId];
    const bool escalated = rejectFaultRaised_[clientId];
    const bool episodeRecorded = escalated || rejectStreakNoted_[clientId];

    /* Any accepted report ends the streak. The fault this feeds is an input that
     * has become unusable, not one that is occasionally imperfect. */
    rejectStreak_[clientId] = 0U;
    rejectFaultRaised_[clientId] = false;
    rejectStreakNoted_[clientId] = false;

    /* The only evidence that this source's input is usable again: an accepted
     * report from it. The SDMs clear its input-unusable cause on this and on
     * nothing else. */
    inputConfirmedMask_ |= inputIdentityLocked(clientId);
    recomputeInputStateLocked();

    /*
     * Establish identity from PERCEPTION traffic only.
     *
     * A monitor is not a perception source. SAIM reports SENSOR_* for whichever
     * camera it is describing, so letting those set the identity would attribute
     * an eventual fault to whichever camera SAIM happened to mention last --
     * arbitrary, and wrong about what failed. the Perception Monitor's AI_PIPELINE_* are
     * pipeline-wide and carry pipelineID 0 anyway. Both monitors already have a
     * dedicated failure path through the heartbeat/disconnect SW_FAIL, so
     * excluding them here loses no coverage.
     *
     * A rejected report never sets this: its pipelineId may have failed CRC or
     * never have been present, and those are exactly the rejections that matter.
     */
    const bool isMonitorReport = (eventType == SENSOR_INVALID || eventType == SENSOR_VALID ||
                                  eventType == AI_PIPELINE_INVALID || eventType == AI_PIPELINE_VALID);
    if (!isMonitorReport && pipelineId != 0U)
        establishedPipeline_[clientId] = pipelineId;

    lock.unlock();
    if (episodeRecorded)
    {
        NvPSBWriteData(NVPSB_LOG_INFO,
            "PSS_CLIENT_REJECT_STREAK_ENDED",
            "client=" + std::to_string(clientId) +
                ", consecutiveRejections=" + std::to_string(endedStreak) +
                ", escalated=" + std::string(escalated ? "yes" : "no") +
                ", endedBy=accepted_report");
    }
}

bool NvPSSDRPC::noteReportRejected(uint32_t clientId, uint8_t rejectReason,
                                   NvPSSDRequiredSourceCallback requiredSourceCb,
                                   void* requiredSourceCtx)
{
    uint8_t  pipelineId = 0U;
    uint32_t streak = 0U;
    uint32_t threshold = 0U;
    bool     identityRequired = false;
    bool     established = false;

    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);

        streak = ++rejectStreak_[clientId];
        threshold = clientRejectErrorThreshold_;

        if (streak < threshold)
            return false;

        /* One injection per unusable episode. The streak keeps climbing so the
         * count stays exact for audit, but a client that never recovers must not
         * flood the critical lane with identical SW_FAILs. */
        if (rejectFaultRaised_[clientId])
            return false;

        /* Required by IDENTITY: a numbered MDX instance or a monitor that
         * nvpss.conf requires. Its registration already declares it a safety
         * source, so it escalates even if none of its reports was ever
         * accepted -- a source misconfigured from the start is exactly that
         * case. Healthy heartbeats keep it present, so without this nothing
         * would ever report it. */
        const auto typeIt = clientTypeMap.find(clientId);
        if (typeIt != clientTypeMap.end())
        {
            identityRequired = (presenceBitForClient(typeIt->second, clientInstanceLocked(clientId)) &
                                requiredSourceMask_) != 0U;
        }

        const auto it = establishedPipeline_.find(clientId);
        if (it != establishedPipeline_.end())
        {
            established = true;
            pipelineId = it->second;
        }
        else if (!identityRequired)
        {
            /* Anonymous, or not required, and no accepted report ever seen, so
             * nothing establishes it as a safety source. Counting continues;
             * escalation does not. This is what keeps a rogue peer from latching
             * every SDM just by sending garbage. */
            return false;
        }
    }

    /* Called WITHOUT heartbeatMutex: the callback reaches into
     * SafetyEventManager, and taking a manager lock under an RPC lock would
     * invert the one-way dependency the callback shape exists to preserve. */
    const bool pipelineRequired = established && (requiredSourceCb != nullptr) &&
                                  requiredSourceCb(requiredSourceCtx, pipelineId);
    const bool required = identityRequired || pipelineRequired;

    if (!required)
    {
        /* Once per episode, like the escalation. Without this latch a source
         * outside the reviewed set that never recovers logged a WARNING for
         * every report it sent -- unbounded, at the camera's frame rate. The
         * closing count is recorded when the episode ends. */
        {
            std::lock_guard<std::mutex> lock(heartbeatMutex);
            if (rejectStreakNoted_[clientId] || rejectStreak_[clientId] < threshold)
                return false;
            rejectStreakNoted_[clientId] = true;
        }
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "PSS_CLIENT_REJECT_STREAK",
            "client=" + std::to_string(clientId) +
                ", pipelineID=" + std::to_string(static_cast<unsigned>(pipelineId)) +
                ", consecutiveRejections=" + std::to_string(streak) +
                ", threshold=" + std::to_string(threshold) +
                ", lastReason=" + std::to_string(static_cast<unsigned>(rejectReason)) +
                ", escalated=no (source is not in the reviewed set)");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        /* Re-check under the lock: another report from this client may have been
         * accepted while the callback ran, which resets the episode. */
        if (rejectFaultRaised_[clientId] || rejectStreak_[clientId] < threshold)
            return false;
        rejectFaultRaised_[clientId] = true;
        /* Published BEFORE the caller enqueues the SW_FAIL, as every loss path
         * does, so no presence report built after it can omit the episode. The
         * identity is no longer confirmed usable: only a new accepted report from
         * it confirms it again. */
        inputConfirmedMask_ &= ~inputIdentityLocked(clientId);
        recomputeInputStateLocked();
    }

    NvPSBWriteData(NVPSB_LOG_ERR,
        "PSS_CLIENT_INPUT_UNUSABLE",
        "client=" + std::to_string(clientId) +
            ", pipelineID=" + std::to_string(static_cast<unsigned>(pipelineId)) +
            ", consecutiveRejections=" + std::to_string(streak) +
            ", threshold=" + std::to_string(threshold) +
            ", lastReason=" + std::to_string(static_cast<unsigned>(rejectReason)) +
            ", requiredBy=" + std::string(identityRequired ? "identity" : "pipeline") +
            ", action=SW_FAIL"
            ", auditPath=PSB_LOG");
    return true;
}

void NvPSSDRPC::injectClientInputUnusableSwFail(
    uint32_t clientId,
    std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
    std::mutex& inputSafetyEventQueMutex)
{
    /* Same shape as the heartbeat-disconnect fault in processPendingDisconnects:
     * daemon-originated, sensorIdentifier PSS_RPC, pipelineID left 0 so
     * ValidateEventSource treats it as the daemon passthrough channel rather
     * than looking it up in sensor_config. The client is named in clientID and
     * the reason is in the cause. */
    SafetyEvent ev = {};
    ev.type = SW_FAIL;
    ev.timestamp = monotonicNowNs();
    ev.confidenceLevel = 1.0f;
    ev.processed = false;
    {
        int n = snprintf(ev.sensorIdentifier, sizeof(ev.sensorIdentifier), "PSS_RPC");
        if (n < 0 || static_cast<size_t>(n) >= sizeof(ev.sensorIdentifier))
            ev.sensorIdentifier[sizeof(ev.sensorIdentifier) - 1] = '\0';
    }
    ev.cause = CAUSE_CLIENT_INPUT_UNUSABLE;
    ev.fusionMetadata.clientID = static_cast<uint8_t>(clientId & 0xFFU);
    {
        /* The identity the SDMs hold the cause for, until PSS reports it
         * confirmed usable again. */
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        ev.fusionMetadata.objectID[0] = inputIdentityLocked(clientId);
    }
    pssSafetyEventSetCRC(&ev);

    {
        std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
        inputSafetyEventQueRef.push_back(
            std::make_pair(static_cast<int>(clientId), ev));
    }
}

void NvPSSDRPC::clearClientStateLocked(uint32_t clientId)
{
    /* Slot reuse must not inherit the previous client's rejection episode. An
     * episode that produced a record is closed here with its final count, or
     * the count would be lost with the slot. */
    {
        const auto streakIt = rejectStreak_.find(clientId);
        const bool escalated = (rejectFaultRaised_.count(clientId) != 0U) && rejectFaultRaised_[clientId];
        const bool noted = (rejectStreakNoted_.count(clientId) != 0U) && rejectStreakNoted_[clientId];
        if ((escalated || noted) && streakIt != rejectStreak_.end())
        {
            /* Deferred: this runs under heartbeatMutex, and the record is written
             * by flushDeferredRecords() once the caller has released it. */
            deferredInfoRecords_.emplace_back(
                "PSS_CLIENT_REJECT_STREAK_ENDED",
                "client=" + std::to_string(clientId) +
                    ", consecutiveRejections=" + std::to_string(streakIt->second) +
                    ", escalated=" + std::string(escalated ? "yes" : "no") +
                    ", endedBy=client_state_cleared");
        }
    }
    rejectStreak_.erase(clientId);
    establishedPipeline_.erase(clientId);
    rejectFaultRaised_.erase(clientId);
    rejectStreakNoted_.erase(clientId);

    const auto ctypeIt = clientTypeMap.find(clientId);
    const bool wasSm = (ctypeIt != clientTypeMap.end() && ctypeIt->second == CLIENT_SAFETY_MONITOR);

    clientLastHeartbeat.erase(clientId);
    clientHeartbeatCount.erase(clientId);
    clientTypeMap.erase(clientId);
    clientInstanceMap_.erase(clientId);
    hbMissCount.erase(clientId);
    if (clientId < static_cast<uint32_t>(maxClients))
    {
        hbMissCountFast[clientId].store(0U, std::memory_order_release);
        hbFaultLatchedFast[clientId].store(0U, std::memory_order_release);
    }
    hbFaultLatched.erase(clientId);

    if (wasSm && smMonitorClientId_ == clientId)
    {
        smMonitorClientId_ = kNoSafetyMonitorClientId;
        smMonitorMiss_ = 0U;
        smMonitorFault_ = false;
        smOperationalCacheMaxMiss_.store(0U, std::memory_order_release);
        smOperationalCacheAnyFault_.store(false, std::memory_order_release);
    }
    recomputePresenceMaskLocked();
    /* A client that leaves mid-episode leaves the unusable set. Its identity
     * stays unconfirmed (the escalation cleared it), so the SDMs hold the cause
     * until a client with that identity has a report accepted. */
    recomputeInputStateLocked();
}

void NvPSSDRPC::clearClientState(uint32_t clientId)
{
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        clearClientStateLocked(clientId);
    }
    flushDeferredRecords();
}

void NvPSSDRPC::flushDeferredRecords()
{
    std::vector<std::pair<std::string, std::string>> records;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        records.swap(deferredInfoRecords_);
    }
    for (const auto& r : records)
        NvPSBWriteData(NVPSB_LOG_INFO, r.first, r.second);
}

void NvPSSDRPC::disconnectClient(uint32_t slot)
{
    /*
     * Two-phase, and the split is the point: take the peer identity under the
     * lock, release, then call the transport.
     *
     * DROPPEER MUST NOT BE CALLED UNDER A CALLER LOCK. The code this
     * replaced called close() here, which is a bare syscall; DropPeer tears down
     * peer state and takes the transport's own mutex, so doing it under
     * heartbeatMutex would hold a lock across another component's lock on a
     * safety path. There is no cycle today -- the transport never calls back
     * into this class -- but "no cycle today" is precisely the reasoning the
     * rule exists to stop anyone relying on.
     *
     * THE RULE IS PER ENTRY POINT, not blanket, and this comment used to say
     * otherwise ("no transport call may be made while holding a caller lock").
     * That phrasing was too strong and the over-strong direction is not the safe
     * one: Send and Recv are non-blocking and take only the transport's leaf
     * mutex, and ATLControl::gatewaySend deliberately holds its lock ACROSS Send
     * because that is what keeps the handle alive against a concurrent Close.
     * A reader who applied the old wording there would have introduced a
     * use-after-free while believing they were fixing a lock-order bug.
     *
     * The authoritative table is in nvpsf_transport.h under "Caller lock-order
     * obligation": Send and Recv permitted, Poll/DropPeer/Close not.
     */
    NvPSFPeerId doomed = NVPSF_PEER_INVALID;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        doomed = disconnectClientLocked(slot);
    }
    flushDeferredRecords();
    if (doomed != NVPSF_PEER_INVALID && transport != nullptr)
    {
        (void)NvPSFTransportDropPeer(transport, doomed);
    }
}

/**
 * Clear a slot's state and surrender its peer identity.
 *
 * @return the peer the CALLER must drop after releasing heartbeatMutex, or
 *         NVPSF_PEER_INVALID if there was nothing to drop. Returning it rather
 *         than dropping it here is what keeps the transport call outside the
 *         lock.
 */
NvPSFPeerId NvPSSDRPC::disconnectClientLocked(uint32_t slot)
{
    NvPSFPeerId doomed = NVPSF_PEER_INVALID;
    if (slot < MAX_POSSIBLE_CLIENTS && clientPeers[slot] != NVPSF_PEER_INVALID)
    {
        /* Surrendered, not closed: the transport owns the descriptor, and
         * DropPeer is a no-op for a peer it has already reclaimed -- which is
         * exactly the case when this is reached from a PEER_GONE event. */
        doomed = clientPeers[slot];
        clientPeers[slot] = NVPSF_PEER_INVALID;
    }
    clearClientStateLocked(slot);
    return doomed;
}

void NvPSSDRPC::refreshSafetyMonitorOperationalCacheLocked()
{
    smMonitorClientId_ = kNoSafetyMonitorClientId;
    smMonitorMiss_ = 0U;
    smMonitorFault_ = false;
    for (const auto& kv : clientTypeMap)
    {
        if (kv.second != CLIENT_SAFETY_MONITOR)
            continue;

        const uint32_t cid = kv.first;
        uint32_t miss = 0U;
        if (cid < static_cast<uint32_t>(maxClients))
            miss = hbMissCountFast[cid].load(std::memory_order_relaxed);
        else
        {
            const auto mit = hbMissCount.find(cid);
            if (mit != hbMissCount.end())
                miss = mit->second;
        }
        smMonitorClientId_ = cid;
        smMonitorMiss_ = miss;
        const auto fit = hbFaultLatched.find(cid);
        smMonitorFault_ = (fit != hbFaultLatched.end() && fit->second);
        break;
    }
    smOperationalCacheMaxMiss_.store(smMonitorMiss_, std::memory_order_release);
    smOperationalCacheAnyFault_.store(smMonitorFault_, std::memory_order_release);
    recomputePresenceMaskLocked();
}

bool NvPSSDRPC::hasRecentHeartbeat(uint32_t clientId) const
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    auto it = clientLastHeartbeat.find(clientId);
    if (it == clientLastHeartbeat.end())
        return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - it->second).count();
    return (elapsed >= 0 && static_cast<uint32_t>(elapsed) <= HB_TIMEOUT_MS);
}

NvPSSDErr NvPSSDRPC::NvPSSDCloseRPCServer()
{
    runRpcServer.store(false);

    if (rpcServerThread.joinable())
        rpcServerThread.join();

    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        clientLastHeartbeat.clear();
        clientHeartbeatCount.clear();
        clientTypeMap.clear();
        hbMissCount.clear();
        for (uint8_t i = 0; i < maxClients; ++i)
        {
            hbMissCountFast[i].store(0U, std::memory_order_release);
            hbFaultLatchedFast[i].store(0U, std::memory_order_release);
        }
        hbFaultLatched.clear();
        refreshSafetyMonitorOperationalCacheLocked();
    }
    {
        std::lock_guard<std::mutex> pd(pendingDisconnectMutex);
        pendingDisconnectClients.clear();
        pendingDisconnectSeen.clear();
    }

    for (int i = 0; i < maxClients; i++)
    {
        clientPeers[i] = NVPSF_PEER_INVALID;
    }

    /* Close the transport LAST, after the run loop thread has been joined
     * above: the transport header requires that no thread still holds the
     * handle. Close also unlinks the bound path, so the explicit unlink() that
     * used to follow is gone rather than duplicated. */
    if (transport != nullptr)
    {
        (void)NvPSFTransportClose(transport);
        transport = nullptr;
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "RPC server closed successfully", "");

    return NVPSSD_SUCCESS;
}

void NvPSSDRPC::updateClientHeartbeat(uint32_t clientId, uint8_t clientType)
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    const auto faultIt = hbFaultLatched.find(clientId);
    if (faultIt != hbFaultLatched.end() && faultIt->second)
        return;

    const bool isSm = (clientType == CLIENT_SAFETY_MONITOR);
    const bool isMonitor = (presenceBitForClient(clientType, clientInstanceLocked(clientId)) != 0U);

    bool isNewClient = (clientLastHeartbeat.find(clientId) == clientLastHeartbeat.end());
    clientLastHeartbeat[clientId] = std::chrono::steady_clock::now();
    hbMissCount[clientId] = 0U;
    if (clientId < static_cast<uint32_t>(maxClients))
        hbMissCountFast[clientId].store(0U, std::memory_order_release);

    if (isSm)
    {
        smMonitorClientId_ = clientId;
        smMonitorMiss_ = 0U;
        smOperationalCacheMaxMiss_.store(0U, std::memory_order_release);
    }

    if (isNewClient)
    {
        clientHeartbeatCount[clientId] = 1;
    }
    else
    {
        clientHeartbeatCount[clientId]++;
#ifdef NVPSF_DBG
        if (clientHeartbeatCount[clientId] % 10 == 0)
        {
            std::string clientName = (clientType == CLIENT_MDX) ? "MDXClient" :
                (clientType == CLIENT_SAFETY_MONITOR) ? "SafetyMonitor" :
                (clientType == CLIENT_PERCEPTION_MONITOR) ? "PerceptionMonitor" :
                "PSDGateway";
            NvPSBWriteData(NVPSB_LOG_INFO,
                "Connection alive: " + clientName + " - " +
                std::to_string(clientHeartbeatCount[clientId]) + " heartbeats received", "");
        }
#endif
    }

    /* A heartbeat, not the registration, is what makes a monitor present -- and
     * expected. Expecting it from the registration let a report built between
     * the two raise a loss on a healthy start. */
    if (isMonitor)
    {
        const uint32_t bit = presenceBitForClient(clientType, clientInstanceLocked(clientId));
        if ((everPresentMask_ & bit) == 0U)
        {
            everPresentMask_ |= bit;
            recomputeExpectedMaskLocked();
        }
        recomputePresenceMaskLocked();
    }
}

size_t NvPSSDRPC::getActiveClientCount() const
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    return clientLastHeartbeat.size();
}

uint8_t NvPSSDRPC::getClientType(uint32_t clientId) const
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    auto it = clientTypeMap.find(clientId);
    return (it != clientTypeMap.end()) ? it->second : 0;
}

uint8_t NvPSSDRPC::clientInstanceLocked(uint32_t clientId) const
{
    const auto it = clientInstanceMap_.find(clientId);
    return (it != clientInstanceMap_.end()) ? it->second : 0U;
}

uint8_t NvPSSDRPC::getClientInstance(uint32_t clientId) const
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    return clientInstanceLocked(clientId);
}

/* getActiveClientIdsOfType was removed: AI-pipeline trust is now a single
 * pipeline-wide latch in SafetyEventManager, so the per-MDX-id fan-out (its only
 * caller) no longer exists. */

OperationalMode NvPSSDRPC::getSafetyMonitorOperationalMode(uint32_t maxFailures, uint32_t warnThreshold) const
{
    if (maxFailures == 0U)
        return NORMAL;

    /* smOperationalCache* updated under heartbeatMutex in heartbeatMonitorTick/updateClientHeartbeat; full rebuild via refreshSafetyMonitorOperationalCacheLocked on CloseRPCServer. */
    const bool anyFault = smOperationalCacheAnyFault_.load(std::memory_order_acquire);
    const uint32_t maxMiss = smOperationalCacheMaxMiss_.load(std::memory_order_acquire);
    if (anyFault || maxMiss >= maxFailures)
        return ERROR;
    if (maxMiss > warnThreshold && maxMiss < maxFailures)
        return DEGRADED;
    return NORMAL;
}

uint32_t NvPSSDRPC::sourcePresenceMask() const
{
    return presenceMask_.load(std::memory_order_acquire);
}

uint32_t NvPSSDRPC::sourceExpectedMask() const
{
    return expectedMask_.load(std::memory_order_acquire);
}

uint32_t NvPSSDRPC::sourceInputUnusableMask() const
{
    return inputUnusablePublished_.load(std::memory_order_acquire);
}

uint32_t NvPSSDRPC::sourceInputConfirmedMask() const
{
    return inputConfirmedPublished_.load(std::memory_order_acquire);
}

uint32_t NvPSSDRPC::inputIdentityLocked(uint32_t clientId) const
{
    const auto typeIt = clientTypeMap.find(clientId);
    if (typeIt == clientTypeMap.end())
        return 0U;
    const uint8_t instance = clientInstanceLocked(clientId);
    const uint32_t bit = presenceBitForClient(typeIt->second, instance);
    if (bit != 0U)
        return bit;
    /* Anonymous MDX clients share one identity: they are not individually
     * tracked, so any of them confirming usable input speaks for all. */
    return (typeIt->second == CLIENT_MDX && instance == 0U) ? PSS_INPUT_ANONYMOUS_CLIENT : 0U;
}

void NvPSSDRPC::recomputeInputStateLocked()
{
    uint32_t unusable = 0U;
    for (const auto& kv : rejectFaultRaised_)
    {
        if (kv.second)
            unusable |= inputIdentityLocked(kv.first);
    }
    inputUnusablePublished_.store(unusable & PSS_INPUT_STATE_MASK, std::memory_order_release);
    inputConfirmedPublished_.store(inputConfirmedMask_ & PSS_INPUT_STATE_MASK,
                                   std::memory_order_release);
}

void NvPSSDRPC::recomputeExpectedMaskLocked()
{
    expectedMask_.store(everPresentMask_ | (requiredSourcesDue_ ? requiredSourceMask_ : 0U),
                        std::memory_order_release);
}

void NvPSSDRPC::recomputePresenceMaskLocked()
{
    /* Presence is registered + heartbeated since registering + not faulted. A
     * miss inside the heartbeat budget does NOT remove a monitor: the SDMs raise
     * a loss when a seen monitor's bit clears, so a late heartbeat must not be
     * able to latch them. The budget running out is what removes it, and that
     * also raises the *_LOST SW_FAIL -- the two agree. */
    uint32_t mask = 0U;
    for (const auto& kv : clientTypeMap)
    {
        const uint32_t bit = presenceBitForClient(kv.second, clientInstanceLocked(kv.first));
        if (bit == 0U)
            continue;
        const auto hbIt = clientHeartbeatCount.find(kv.first);
        if (hbIt == clientHeartbeatCount.end() || hbIt->second == 0U)
            continue;
        const auto fIt = hbFaultLatched.find(kv.first);
        if (fIt != hbFaultLatched.end() && fIt->second)
            continue;
        mask |= bit;
    }
    presenceMask_.store(mask, std::memory_order_release);
}

void NvPSSDRPC::injectMonitorLostSwFail(uint32_t slot, uint8_t clientType, uint8_t instance,
                                        const char* reason,
                                        std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                        std::mutex& inputSafetyEventQueMutex)
{
    const EventCause cause = monitorLostCause(clientType, instance);
    if (cause == CAUSE_NONE)
        return;
    const bool hasSlot = (slot != UINT32_MAX);

    SafetyEvent ev = {};
    ev.type = SW_FAIL;
    ev.timestamp = monotonicNowNs();
    ev.confidenceLevel = 1.0f;
    ev.processed = false;
    (void)snprintf(ev.sensorIdentifier, sizeof(ev.sensorIdentifier), "%s", "PSS_RPC");
    ev.cause = cause;
    ev.fusionMetadata.clientID = hasSlot ? static_cast<uint8_t>(slot & 0xFFU) : 0U;
    /* Which source must come back, for an SDM that never saw it present. */
    ev.fusionMetadata.objectID[0] = presenceBitForClient(clientType, instance);
    pssSafetyEventSetCRC(&ev);

    NvPSBWriteData(NVPSB_LOG_ERR, monitorLostRecordName(clientType),
        "client=" + (hasSlot ? std::to_string(slot) : std::string("none")) +
            (clientType == CLIENT_MDX ? ", instance=" + std::to_string(instance) : std::string()) +
            ", reason=" + std::string(reason != nullptr ? reason : "unspecified") +
            ", action=SW_FAIL"
            ", clears=only when " +
            (clientType == CLIENT_MDX ? "MDX instance " + std::to_string(instance)
                                      : "a " + std::string(clientTypeName(clientType))) +
            " registers and heartbeats again");

    std::lock_guard<std::mutex> qLock(inputSafetyEventQueMutex);
    inputSafetyEventQueRef.push_back(std::make_pair(hasSlot ? static_cast<int>(slot) : -1, ev));
}

void NvPSSDRPC::dropClient(uint32_t slot, const char* reason,
                           std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                           std::mutex& inputSafetyEventQueMutex)
{
    /* Identity read BEFORE the teardown, which erases it; the SW_FAIL queued
     * AFTER it, which removes the source from the presence set. That order is
     * what lets the no-op builder promise no report can be staler than the loss. */
    const uint8_t clientType = getClientType(slot);
    const uint8_t instance = getClientInstance(slot);
    disconnectClient(slot);
    injectMonitorLostSwFail(slot, clientType, instance, reason,
                            inputSafetyEventQueRef, inputSafetyEventQueMutex);
}

}
