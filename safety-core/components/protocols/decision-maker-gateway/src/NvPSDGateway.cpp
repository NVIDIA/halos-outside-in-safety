/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <signal.h>
#include <arpa/inet.h>   /* ntohl/htonl for the heartbeat sequence field */
#include <unistd.h>

#include "NvPSD.h"
#include "NvPSB.h"
#include "pss_daemon.h"
#include "pss_protocol.h"
#include "pss_message_validate.h"
#include "NvPSDGateway.h"
#include "NvPSDGatewayContract.hpp"
#include "nvpsf_transport.h"

static constexpr uint8_t  MAX_EVENT_TYPES_PER_CLIENT = NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT;
static constexpr unsigned int MAX_DECISION_MAKER_CLIENTS = NVPSD_GATEWAY_MAX_CLIENTS;
static constexpr int      HB_SEND_INTERVAL_MS   = 2000;
static constexpr int      HB_ACK_TIMEOUT_MS     = 3000;
/* Default max consecutive HB misses before tier-3 reclaim; CLI range 1..255 (--max_hb_failures). */
static constexpr uint32_t HB_MAX_MISSED_DEFAULT = 10U;
static constexpr int      HB_FALLBACK_SLEEP_MS  = 500;  /* fallback when ACK timeout <= send interval */
static constexpr int      HB_ACK_GRACE_MS       = 300;  /* extra wait before miss check to reduce false reclaims under load */
/* Value for lastAckedSeq meaning "no HB ACK received yet"; any sent seq is ahead until first ACK. */
static constexpr uint32_t HB_SEQ_NONE           = (uint32_t)-1;
/*
 * There is no EINTR retry ladder around send, for a structural reason as well
 * as a policy one, and the structural reason is the load-bearing half.
 *
 * Structural: the transport issues every send with MSG_DONTWAIT on a
 * SOCK_NONBLOCK descriptor, and a send that never blocks cannot realistically be
 * interrupted before transferring data. The ladder is unreachable, not merely
 * unwanted.
 *
 * Policy: the transport maps EINTR to WOULD_BLOCK and does not retry internally,
 * and the approved backpressure policy for this link is drop-and-count so that
 * nothing on the safety path can block.
 *
 * Caveat worth knowing when reading a drop log: the transport deliberately does
 * NOT increment the per-peer drop counter for EINTR, so a WOULD_BLOCK caused by
 * a signal is reported with a drops= value that excludes it.
 */
/* Poll wake-up period, so the loop notices stopGateway without a dedicated
 * wake-up channel. Was SO_RCVTIMEO on the raw socket. */
static constexpr uint32_t GATEWAY_POLL_TIMEOUT_MS = 1000U;
/*
 * Events drained per Poll call.
 *
 * One pass can produce an accept event for each of the transport's
 * NVPSF_ACCEPTS_PER_POLL (8) admissions *plus* one event per peer descriptor,
 * and the peer pool is twice the client count. Undersizing this does not lose
 * events -- poll() is level-triggered and the transport bounds its loop by the
 * capacity given -- but it defers them, and because the listener is always
 * drained first, connect pressure would push PEER_GONE and MESSAGE_READY for
 * already-registered SDMs to later passes.
 */
static constexpr size_t   NVPSD_GATEWAY_POLL_EVENTS = 2U * NVPSD_GATEWAY_MAX_CLIENTS + 8U + 2U;
/*
 * How long an accepted-but-unregistered peer may hold a transport slot.
 * Long enough that a legitimate SDM which connects and then takes its time
 * building a REGR is never reaped mid-handshake; short enough that a peer which
 * will never register frees its slot well inside the heartbeat ladder, so the
 * pending pool cannot become the new exhaustion target.
 */
static constexpr int      ADMISSION_TIMEOUT_MS = 5000;
/*
 * Mode applied to the AF_UNIX socket after bind. The 1777 parent grants no
 * protection of its own, so the socket restricts itself to owner and group and
 * the transport additionally validates SO_PEERCRED against the allowlisted GID.
 */
static constexpr mode_t   NVPSD_GATEWAY_SOCKET_MODE = 0660;
/* PSS RPC socket may not listen at process start; retry NvPSSRegisterPSSClient so gateway HB to PSS is reliable. */
static constexpr int      PSS_REGISTER_MAX_ATTEMPTS    = 60;
static constexpr int      PSS_REGISTER_RETRY_DELAY_MS  = 500;
/* PSS -> PSD socket connect retry. Same shape and cadence as the RPC
 * registration above, for the same reason: the peer may not have bound yet. */
static constexpr int      PSD_SOCKET_CONNECT_MAX_ATTEMPTS   = 60;
static constexpr int      PSD_SOCKET_CONNECT_RETRY_DELAY_MS = 500;

/* Signal-safe flag: set in signal handler (async-signal-safe); main loop checks it and calls shutdownPSDControl(). */
static volatile sig_atomic_t g_signal_received = 0;

/* static state */
/*
 * NvPSFTransport owns every descriptor and is internally thread-safe on a leaf
 * lock, so no dup()-per-I/O dance is needed to stay safe against a concurrent
 * close(). The
 * handle is published once before any worker thread starts and cleared only
 * after every worker has been joined, so an atomic pointer is sufficient and no
 * socket mutex remains.
 *
 * Caller lock-order obligation: no NvPSFTransport* call -- Send, Recv,
 * Poll, DropPeer, PeerCred, PeerDropCount -- may be made while clientTableMutex
 * is held. clientTableMutex is a leaf. Every site below snapshots under the lock
 * and acts after releasing it.
 */
static std::atomic<NvPSFTransport*> g_transport{nullptr};
static std::atomic<bool> stopGateway{false};
static std::mutex        clientTableMutex;
static std::atomic<uint32_t> hbSeqNo{0};
static std::atomic<uint64_t> gatewayClientEpochSeed{1U};
static unsigned int      g_maxClients = MAX_DECISION_MAKER_CLIENTS;
static std::mutex        pssSequenceMutex;
static NvPSDGatewayPssSequenceState pssSequenceState;

/*
 * RECOVERY DEBT, PER CLIENT.
 *
 * A synthetic SW_FAIL about the PSS link leaves every SDM that received it
 * holding its gateway_comm sub-cause, and only the all-client recovery record
 * reaches an SDM whose subscriptions carry no ordinary traffic. So each client
 * carries its own debt, recoveryOwedGen in ClientEntry (0 = owes nothing):
 *
 *   - every client registered when such a fault is broadcast owes a record, and
 *     so does every client that registers while any debt is outstanding (an SDM
 *     that reconnected in between may still hold the sub-cause);
 *   - the record goes to the clients that owe one, on the next accepted request;
 *   - a client's debt clears only once the record was actually handed to the
 *     transport for it. A client the fanout skips -- the degraded heartbeat tier
 *     receives no DecisionRequests -- or whose send fails keeps its debt, and the
 *     next accepted request tries it again. A single global flag used to clear
 *     for everyone, so a client skipped at that moment never got the record.
 *
 * WHY A GENERATION AND NOT A FLAG. The fanout cannot run under clientTableMutex
 * -- the sends happen after it is released -- so a new synthetic fault can be
 * raised between choosing the recipients and clearing their debt. The new fault
 * re-marks the clients with a newer generation, and the fanout clears only the
 * generation it sent, so the newer debt survives and is paid next time.
 *
 * Guarded by clientTableMutex, like the client table itself.
 */
static uint64_t g_pssFaultGeneration = 0U;

/* Wrap-safe: true when sentSeq is ahead of ackedSeq in the 32-bit sequence ring (ACK not yet received). */
static inline bool isSeqAhead(uint32_t sentSeq, uint32_t ackedSeq)
{
    uint32_t diff = sentSeq - ackedSeq;
    return (diff != 0 && diff <= 0x7FFFFFFFU);
}

struct ClientEntry
{
    /*
     * Transport identity, replacing the sockaddr_in pair. NvPSFPeerId carries a
     * generation counter, so a stale identity naming a recycled slot fails the
     * transport's own check instead of acting on the new occupant -- the class
     * of misroute that address-derived identity could not detect at all.
     */
    NvPSFPeerId        peer{NVPSF_PEER_INVALID};
    std::unordered_set<EventType> eventTypes;
    bool               inUse;
    uint32_t           lastAckedSeq{0};
    uint64_t           gatewayTxEpoch{0};
    uint32_t           nextGatewayTxSeq{NVPSD_GATEWAY_DECISION_TX_SEQ_MIN};
    int                missedCount{0};
    /* Recovery-record debt: the synthetic-fault generation owed, 0 for none. */
    uint64_t           recoveryOwedGen{0};
};
static ClientEntry clientTable[MAX_DECISION_MAKER_CLIENTS];

/* Caller must hold clientTableMutex. */
static bool recoveryOwedByAnyLocked()
{
    for (unsigned int c = 0; c < g_maxClients; ++c)
    {
        if (clientTable[c].inUse && clientTable[c].recoveryOwedGen != 0U)
            return true;
    }
    return false;
}

/* Call after broadcasting any synthetic SW_FAIL about the PSS link: every
 * registered client now owes a recovery record. */
static void notePssSyntheticFaultBroadcast()
{
    std::lock_guard<std::mutex> lock(clientTableMutex);
    ++g_pssFaultGeneration;
    for (unsigned int c = 0; c < g_maxClients; ++c)
    {
        if (clientTable[c].inUse)
            clientTable[c].recoveryOwedGen = g_pssFaultGeneration;
    }
}

/*
 * Two-stage admission.
 *
 * Stage 1 is the transport's own peer pool: a connection is accepted, its
 * credentials validated, and it sits here holding a transport slot and nothing
 * else. Stage 2 is a clientTable slot, allocated only when a *validated* REGR
 * arrives. The old code conflated the two, so anyone who could reach the socket
 * could consume a decision-maker slot and lock out a legitimate SDM.
 *
 * The pending pool must be reaped or stage 1 simply becomes the new exhaustion
 * target: a peer that connects and never registers would hold its transport slot
 * forever. Every pending peer therefore carries a deadline, and the registration
 * loop drops the expired ones on each pass.
 */
struct PendingPeer
{
    NvPSFPeerId peer;
    std::chrono::steady_clock::time_point deadline;
};
static std::vector<PendingPeer> pendingPeers;   /* guarded by clientTableMutex */

/*
 * Registered-client lookup is a linear scan rather than a peer->slot index.
 * With at most NVPSD_GATEWAY_MAX_CLIENTS entries the cost is irrelevant, and
 * the client table stays the single source of truth: register, re-register and
 * reclaim each touch one structure, so no second index can fall out of step
 * with it.
 *
 * Caller must hold clientTableMutex. Returns -1 when the peer is not registered.
 */
static int findSlotByPeerLocked(NvPSFPeerId peer)
{
    if (peer == NVPSF_PEER_INVALID)
        return -1;
    for (unsigned int c = 0; c < g_maxClients; ++c)
    {
        if (clientTable[c].inUse && clientTable[c].peer == peer)
            return static_cast<int>(c);
    }
    return -1;
}

/*
 * Free a registered slot and reproduce the tier-3 action set exactly.
 *
 * Disconnect and heartbeat-exhaustion must be indistinguishable downstream: the
 * same slot release, the same epoch zeroing, the same sequence reset, and an
 * error-level log. If a disconnect took a shorter path, evidence visible under
 * UDP would go missing under UNIX_SOCKET and the SDM's latch would lose its
 * attribution. Caller must hold clientTableMutex.
 */
/*
 * Work deferred out of a clientTableMutex critical section, for two distinct
 * reasons that happen to have the same remedy.
 *
 * DropPeer is a transport call, and no transport call may run under a caller
 * lock. Logging is subtler and was missed on the first pass: NvPSB
 * reaches syslog() on a LOG_CONS handle, so under syslog backpressure it falls
 * back to an unbounded write to /dev/console. clientTableMutex is *also* the
 * DecisionRequest fan-out lock, so a stalled syslogd holding it would stall
 * delivery to every healthy SDM. Neither may happen with the lock held.
 *
 * Both vectors are guarded by clientTableMutex and drained by
 * flushDeferredActions() after it is released.
 */
static std::vector<NvPSFPeerId> g_deferredDrops;
static std::vector<std::string> g_deferredLogs;

/* Per-peer REGR rate limit (--regr_max_per_window / --regr_window_ms). Owned by
 * the registration listener thread; see NvPSDGatewayRegrRateLimiter. */
static NvPSDGatewayRegrRateLimiter g_regrLimiter;

static uint64_t gatewayNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/*
 * Answer a refused REGR with its reason (RGNK), so the refused client can tell
 * refusal -- and why -- from silence. Call outside clientTableMutex: it is a
 * transport call. A failed send is logged and not retried; the refusal itself is
 * logged either way, so the gateway's record does not depend on delivery.
 */
static void refuseRegistration(NvPSFTransport* t, NvPSFPeerId peer, uint8_t reason)
{
    uint8_t pkt[NVPSD_GATEWAY_REG_REJECT_MSG_SIZE];
    NvPSDGatewayBuildRegReject(pkt, reason);
    const NvPSFTransportErr err = NvPSFTransportSend(t, peer, pkt, sizeof(pkt));
    NvPSBWriteData(NVPSB_LOG_WARNING, "PSD-Gateway: registration refused",
                   std::string("reason=") + NvPSDGatewayRegRejectReasonName(reason) +
                       ", peer=" + std::to_string(peer) +
                       ", replySent=" + (err == NVPSF_TRANSPORT_OK ? "yes" : NvPSFTransportErrName(err)));
}

/**
 * Free a registered slot and reproduce the tier-3 action set exactly.
 *
 * @param needsTransportDrop false when the transport has already released the
 *        peer itself -- which it does when Recv reports PEER_GONE -- so that the
 *        slot is not dropped twice.
 */
static void releaseClientSlotLocked(unsigned int slot, const char* reason,
                                    bool needsTransportDrop)
{
    /*
     * Hand the peer back to the transport, or its slot leaks. This was the
     * defect on the first pass: clearing the clientTable entry without dropping
     * the peer meant a client that stayed connected but stopped ACKing was
     * reclaimed from the table and kept its transport slot forever. With enough
     * such peers the pool fills, accept() refuses everything, and no SDM can
     * register again without a gateway restart -- turning stage one into exactly
     * the exhaustion target two-stage admission exists to remove.
     */
    if (needsTransportDrop && clientTable[slot].peer != NVPSF_PEER_INVALID)
        g_deferredDrops.push_back(clientTable[slot].peer);

    clientTable[slot].inUse = false;
    clientTable[slot].peer = NVPSF_PEER_INVALID;
    clientTable[slot].gatewayTxEpoch = 0U;
    clientTable[slot].nextGatewayTxSeq = NVPSD_GATEWAY_DECISION_TX_SEQ_MIN;
    clientTable[slot].missedCount = 0;
    clientTable[slot].lastAckedSeq = HB_SEQ_NONE;
    clientTable[slot].recoveryOwedGen = 0U;
    g_deferredLogs.push_back("PSD-Gateway: client " + std::to_string(slot) +
                             " reclaimed (" + std::string(reason) +
                             "); SDM may REGR again");
}

/* Drain the deferred work. Must be called with clientTableMutex NOT held. */
static void flushDeferredActions(NvPSFTransport* t)
{
    std::vector<NvPSFPeerId> drops;
    std::vector<std::string> logs;
    {
        std::lock_guard<std::mutex> lock(clientTableMutex);
        drops.swap(g_deferredDrops);
        logs.swap(g_deferredLogs);
    }
    for (const std::string& m : logs)
        NvPSBWriteData(NVPSB_LOG_ERR, m, "");
    if (t != nullptr)
    {
        for (const NvPSFPeerId p : drops)
            (void)NvPSFTransportDropPeer(t, p);
    }
}

/* Fail-safe HB policy: WARN tier = max/2 (integer division). Configured via --max_hb_failures. */
static std::atomic<uint32_t> g_maxHbFailures{HB_MAX_MISSED_DEFAULT};

static std::atomic<uint32_t> gatewayPssClientId{UINT32_MAX};
static std::mutex            gatewayPssRpcMtx;
static std::atomic<bool>                            stopGatewayPssHb{true};
static std::thread                                  gatewayPssHbThread;

struct RecoverySent;
static void sendToRelevantClients(const DecisionRequest& req, bool sendFullRequest,
                                  std::vector<RecoverySent>* recoverySent = nullptr);

static uint64_t makeGatewayEpochSeed()
{
    const uint64_t nowCount = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t pidPart = static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 16U;
    const uint64_t mixed = nowCount ^ pidPart;
    uint64_t seed = mixed ^ (mixed << 17U) ^ (mixed >> 29U);
    if (!NvPSDGatewayDecisionPacketEpochIsValid(seed))
        seed = 1ULL;
    return seed;
}

static uint64_t allocateGatewayClientEpoch()
{
    uint64_t epoch = gatewayClientEpochSeed.fetch_add(1U, std::memory_order_relaxed);
    if (!NvPSDGatewayDecisionPacketEpochIsValid(epoch))
    {
        gatewayClientEpochSeed.store(2ULL, std::memory_order_relaxed);
        epoch = 1ULL;
    }
    return epoch;
}

static uint32_t nextGatewayTxSeqAfter(uint32_t txSeq)
{
    return (txSeq >= NVPSD_GATEWAY_DECISION_TX_SEQ_MAX)
        ? NVPSD_GATEWAY_DECISION_TX_SEQ_MIN
        : (txSeq + 1U);
}

static void terminatePssClientLocked()
{
    const uint32_t pssId = gatewayPssClientId.load(std::memory_order_relaxed);
    if (pssId != UINT32_MAX)
    {
        NvPSSTerminatePSSClient(pssId);
        gatewayPssClientId.store(UINT32_MAX);
    }
}

static void cleanupGatewayPssClient()
{
    stopGatewayPssHb.store(true);
    if (gatewayPssHbThread.joinable())
        gatewayPssHbThread.join();
    std::lock_guard<std::mutex> lock(gatewayPssRpcMtx);
    terminatePssClientLocked();
}

static void gatewayPssHeartbeatLoop()
{
    uint32_t consecutiveAckFailures = 0;
    const uint32_t maxAckFailures = g_maxHbFailures.load(std::memory_order_relaxed);

    while (!stopGatewayPssHb.load())
    {
        bool pssDeadExit = false;
        DecisionRequest errorReq = {};

        {
            std::lock_guard<std::mutex> lock(gatewayPssRpcMtx);
            const uint32_t pssId = gatewayPssClientId.load(std::memory_order_relaxed);
            if (pssId != UINT32_MAX)
            {
                if (NvPSSSendHeartbeat(pssId, CLIENT_PSD_GATEWAY) != NVPSSD_SUCCESS)
                {
                    if (consecutiveAckFailures < UINT32_MAX)
                        ++consecutiveAckFailures;
                    NvPSBWriteData(NVPSB_LOG_WARNING,
                        "PSD-Gateway: NvPSSSendHeartbeat failed, miss=" +
                            std::to_string(consecutiveAckFailures) + "/" +
                            std::to_string(maxAckFailures), "");

                    /*
                     * Try to re-register before escalating.
                     *
                     * The PSS -> PSD decision link survives a daemon restart,
                     * but the PSS RPC link is separate and its client id is
                     * obtained once at startup. After a restart the
                     * daemon's client table is empty, so every heartbeat fails
                     * against a daemon that has never heard of us -- and the
                     * failure is permanent, because nothing re-registers.
                     *
                     * Without this the two links end in contradictory states:
                     * the decision link is restored and logged re-established,
                     * while this loop counts to the limit, fans ERROR out to
                     * every SDM, and breaks for good. Every SDM then sits in
                     * PSS-ERROR lockout needing a manual release, on a system
                     * whose decision path is actually healthy.
                     *
                     * Re-registration is attempted at the escalation threshold
                     * rather than on every miss, so a genuinely dead PSS still
                     * escalates on the same schedule as before.
                     */
                    {
                        uint32_t reRegId = UINT32_MAX;
                        if (NvPSSRegisterPSSClient(&reRegId, CLIENT_PSD_GATEWAY) == NVPSSD_SUCCESS &&
                            reRegId != UINT32_MAX)
                        {
                            gatewayPssClientId.store(reRegId);
                            consecutiveAckFailures = 0;
                            NvPSBWriteData(NVPSB_LOG_ERR,
                                "PSD-Gateway: PSS re-registered after daemon restart; "
                                "heartbeat resumed instead of declaring PSS dead", "");
                            continue;
                        }
                    }

                    if (maxAckFailures > 0 && consecutiveAckFailures >= maxAckFailures)
                    {
                        NvPSBWriteData(NVPSB_LOG_ERR,
                            "PSD-Gateway: PSS heartbeat ACK failure limit reached — PSS presumed dead", "");

                        errorReq.requestId = UINT32_MAX;
                        errorReq.pssStatus.mode = ERROR;
                        errorReq.sensorDataSummarySize = 0;

                        terminatePssClientLocked();
                        pssDeadExit = true;
                    }
                }
                else
                {
                    consecutiveAckFailures = 0;
                }
            }
        }

        if (pssDeadExit)
        {
            sendToRelevantClients(errorReq, true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(HB_SEND_INTERVAL_MS));
    }
}

/* Async-signal-safe: only set volatile sig_atomic_t. Main loop checks g_signal_received and calls shutdownPSDControl(). */
static void signalHandler(int)
{
    g_signal_received = 1;
}

/* One-pass: build map EventType -> indices in req. Used to build per-client requests without rescanning. */
static void buildEventTypeToIndices(const DecisionRequest& req,
                                    std::unordered_map<EventType, std::vector<uint8_t>>* out)
{
    out->clear();
    const uint8_t maxSrc = std::min(req.sensorDataSummarySize,
                                    static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));
    for (uint8_t i = 0; i < maxSrc; ++i)
        (*out)[req.sensorDataSummary[i].event.type].push_back(i);
}

struct SendItem
{
    NvPSFPeerId        peer;
    NvPSDGatewayDecisionRequestPacket packet;
    uint8_t            eventCount;
    unsigned int       slot;
    uint64_t           recoveryOwedGen;   /* the debt this send would pay */
};

/* One recovery record handed to the transport: whose debt it pays. */
struct RecoverySent
{
    unsigned int slot;
    NvPSFPeerId  peer;
    uint64_t     recoveryOwedGen;
};

/* Reused in sendToRelevantClients / buildRequestFromIndex (single callback thread) to avoid per-request allocations. */
static thread_local std::vector<SendItem> g_toSendList;
static thread_local std::vector<RecoverySent> g_recoverySent;
static thread_local std::unordered_map<EventType, std::vector<uint8_t>> g_eventTypeToIndices;
static thread_local std::vector<uint8_t> g_indices;

/* Build DecisionRequest for one client from precomputed eventTypeToIndices; return event count.
 * Events are written in deterministic order (by original request index) so sensorDataSummary[0] is stable.
 * Uses thread_local g_indices to avoid per-client vector allocation in the hot path. */
static uint8_t buildRequestFromIndex(const DecisionRequest& fullReq,
                                     const std::unordered_map<EventType, std::vector<uint8_t>>& eventTypeToIndices,
                                     const std::unordered_set<EventType>& subscribedTypes,
                                     DecisionRequest* outReq)
{
    std::memset(outReq, 0, sizeof(DecisionRequest));
    outReq->requestId = fullReq.requestId;
    outReq->pssStatus = fullReq.pssStatus;
    g_indices.clear();
    for (EventType et : subscribedTypes)
    {
        auto it = eventTypeToIndices.find(et);
        if (it == eventTypeToIndices.end())
            continue;
        for (uint8_t idx : it->second)
            g_indices.push_back(idx);
    }
    std::sort(g_indices.begin(), g_indices.end());
    uint8_t outCount = 0;
    for (uint8_t idx : g_indices)
    {
        if (outCount >= MAX_SENSORS_DATA_SUMMARY_SIZE)
            break;
        outReq->sensorDataSummary[outCount++] = fullReq.sensorDataSummary[idx];
    }
    outReq->sensorDataSummarySize = outCount;
    return outCount;
}

/* Send full or filtered DecisionRequest to all registered clients. Build send list under lock; send outside lock.
 * recoverySent != nullptr makes this the recovery fanout: only clients that owe a recovery record
 * are sent it, and each one the transport accepted is appended to *recoverySent. */
static void sendToRelevantClients(const DecisionRequest& req, bool sendFullRequest,
                                  std::vector<RecoverySent>* recoverySent)
{
    g_toSendList.clear();
    g_eventTypeToIndices.clear();

    if (!sendFullRequest)
        buildEventTypeToIndices(req, &g_eventTypeToIndices);

    {
        std::lock_guard<std::mutex> lock(clientTableMutex);
        const uint32_t maxF = g_maxHbFailures.load();
        const uint32_t warnW = maxF / 2U;
        for (unsigned int c = 0; c < g_maxClients; ++c)
        {
            if (!clientTable[c].inUse)
                continue;
            /* Tier 2 (active fault): do not forward DecisionRequest; tier 3 slots are reclaimed (not inUse). */
            const int mc = clientTable[c].missedCount;
            if (mc > 0 && static_cast<uint32_t>(mc) > warnW && static_cast<uint32_t>(mc) < maxF)
                continue;
            if (recoverySent != nullptr &&
                !NvPSDGatewayRecoveryRecordOwed(clientTable[c].recoveryOwedGen))
                continue;

            SendItem item = {};
            DecisionRequest payload = {};
            item.peer = clientTable[c].peer;
            item.slot = c;
            item.recoveryOwedGen = clientTable[c].recoveryOwedGen;
            if (sendFullRequest)
            {
                payload = req;
                payload.sensorDataSummarySize = std::min(req.sensorDataSummarySize,
                    static_cast<uint8_t>(MAX_SENSORS_DATA_SUMMARY_SIZE));
                item.eventCount = payload.sensorDataSummarySize;
            }
            else
            {
                item.eventCount = buildRequestFromIndex(req, g_eventTypeToIndices,
                                                        clientTable[c].eventTypes, &payload);
            }
            if (!NvPSDGatewayShouldSendDecisionRequest(sendFullRequest,
                                                       item.eventCount,
                                                       payload.pssStatus.mode))
                continue;

            if (!NvPSDGatewayDecisionPacketEpochIsValid(clientTable[c].gatewayTxEpoch))
            {
                clientTable[c].gatewayTxEpoch = allocateGatewayClientEpoch();
                clientTable[c].nextGatewayTxSeq = NVPSD_GATEWAY_DECISION_TX_SEQ_MIN;
            }

            /*
             * ALLOCATE BEFORE SEND. This is a deliberate decision, not an
             * accident of ordering.
             *
             * A send that fails later has still spent this number, so the SDM
             * sees a gap and latches. Moving the allocation after a successful
             * send would stop the latches and make the losses causing them
             * INVISIBLE -- the gap is the SDM's only evidence that a decision
             * it should have acted on never arrived. Read the ADR before
             * changing this.
             */
            const uint32_t gatewayTxSeq = clientTable[c].nextGatewayTxSeq;
            /* Payload may differ per client (filtering); recompute CRC before each packet is wrapped. */
            pssDecisionRequestSetCRC(&payload);
            if (!NvPSDGatewayBuildDecisionRequestPacket(payload,
                                                        clientTable[c].gatewayTxEpoch,
                                                        gatewayTxSeq,
                                                        &item.packet))
            {
                /*
                 * SPEND THE SEQUENCE ANYWAY, for the same reason the allocation
                 * sits above the send: a request this client was going to
                 * receive is not going to arrive, and the gap is the only
                 * evidence it can ever have of that. Skipping the number here
                 * would let the next packet carry the expected txSeq and hide
                 * the loss, leaving freshness as the only fallback.
                 *
                 * Unreachable as written -- the epoch is re-allocated and
                 * validated immediately above and nextGatewayTxSeqAfter() wraps
                 * inside the valid range, so the build cannot fail. Consuming
                 * here makes the invariant hold by construction rather than by
                 * reading three adjacent lines, so a later change to either
                 * allocator cannot quietly reintroduce a silent skip.
                 */
                clientTable[c].nextGatewayTxSeq = nextGatewayTxSeqAfter(gatewayTxSeq);
                g_deferredLogs.push_back(
                    "PSD-Gateway: failed to build Gateway DecisionRequest packet; "
                    "transmit sequence consumed so the client observes a gap");
                continue;
            }
            clientTable[c].nextGatewayTxSeq = nextGatewayTxSeqAfter(gatewayTxSeq);
            g_toSendList.push_back(item);
        }
    }

    if (g_toSendList.empty())
        return;

    /*
     * Phase two: every transport call happens here, after clientTableMutex has
     * been released. The send list is built under the lock and drained outside
     * it, because a Send or a
     * DropPeer under the table lock would reintroduce the lock-order inversion
     * the transport's leaf-lock contract exists to prevent.
     */
    NvPSFTransport* const t = g_transport.load(std::memory_order_acquire);

    /* Emit anything the build phase queued, now that the lock is released. */
    flushDeferredActions(t);

    if (t == nullptr)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: transport closed (DecisionRequest)", "");
        return;
    }

    for (const SendItem& item : g_toSendList)
    {
        const NvPSFTransportErr err =
            NvPSFTransportSend(t, item.peer, &item.packet, sizeof(item.packet));
        if (recoverySent != nullptr && err == NVPSF_TRANSPORT_OK)
            recoverySent->push_back({item.slot, item.peer, item.recoveryOwedGen});

        /*
         * A short send is not a reachable outcome on a message-oriented
         * transport, so FAILED_SHORT_SEND has no producer here and the
         * byte-count contract helper and its NvPSDGatewayPacketSendResult enum
         * are therefore not used on this path at all -- the transport's own
         * error code is the finer-grained signal. The EINTR retry
         * ladder is gone for the same structural reason: the transport reports
         * an interrupted send as WOULD_BLOCK, and the approved backpressure
         * policy for this link is to drop and count rather than to retry, so
         * that nothing on the safety path can block.
         */
        if (err == NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            /*
             * Fault evidence, not a transient, and attributable.
             *
             * A drop here has two possible causes that would otherwise be
             * indistinguishable at this point: an under-provisioned receive
             * buffer, or a consumer that has genuinely stopped reading. Both
             * produced the same runtime WOULD_BLOCK, the same burnt sequence
             * number and the same SDM latch, so an operator could not tell a
             * misconfigured host from a broken SDM.
             *
             * With minQueueDepth set to the measured floor, the provisioning
             * case can no longer reach here at all: every endpoint reads its own
             * buffer back at startup and REFUSES TO START on a shortfall,
             * naming granted, required and the sysctl. So a drop at this point
             * means the buffer was provisioned for the worst legitimate burst
             * and the consumer still could not keep up -- a genuinely broken
             * consumer, nothing else. The two diagnoses are now separated by
             * construction rather than by interpretation: one is a startup
             * abort, the other this log line.
             *
             * The sequence number was already allocated above, so the SDM will
             * see a gap and latch. The per-peer count distinguishes a one-off
             * from a sustained stall.
             */
            uint64_t drops = 0;
            (void)NvPSFTransportPeerDropCount(t, item.peer, &drops);
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "PSD-Gateway: DecisionRequest dropped, consumer not keeping up",
                           "seq=" + std::to_string(item.packet.gatewayTxSeq) +
                               ", drops=" + std::to_string(drops));
        }
        else if (err == NVPSF_TRANSPORT_PEER_GONE)
        {
            /* Reclaim is driven by the PEER_GONE event in the registration
             * loop, which owns the tier-3 action set. Only log here, so the
             * action set has exactly one producer. */
            /* ERR, not WARNING: this burns an already-allocated sequence and
             * will latch the SDM. A site collecting at ERR must not lose it. */
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "PSD-Gateway: DecisionRequest to a departed peer", "");
        }
        else if (err != NVPSF_TRANSPORT_OK)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: failed to send DecisionRequest",
                           std::string("err=") + NvPSFTransportErrName(err));
        }
#ifdef NVPSF_DBG
        if (err == NVPSF_TRANSPORT_OK)
            NvPSBWriteData(NVPSB_LOG_INFO,
                           "PSD-Gateway: sent Gateway packet seq=" +
                               std::to_string(item.packet.gatewayTxSeq) +
                               " request id=" + std::to_string(item.packet.request.requestId) +
                               " with " + std::to_string(item.eventCount) + " events", "");
#endif
    }
}

/*
 * Registration, HB-ACK and peer-lifecycle loop.
 *
 * Owns three things that used to be spread across a blocking recvfrom and the
 * heartbeat thread: draining inbound messages, admitting peers into the
 * clientTable on a validated REGR (stage two), and reclaiming slots when
 * the transport reports a peer gone.
 */
static void registrationListenerLoop()
{
    const size_t bufSize = 4 + 1 + MAX_EVENT_TYPES_PER_CLIENT * sizeof(uint32_t);
    std::vector<char> buf(bufSize);
    std::vector<NvPSFPeerId> toDrop;
    NvPSFTransportEvent events[NVPSD_GATEWAY_POLL_EVENTS];

    while (!stopGateway.load())
    {
        NvPSFTransport* const t = g_transport.load(std::memory_order_acquire);
        if (t == nullptr)
            break;

        size_t eventCount = 0;
        const NvPSFTransportErr pollErr =
            NvPSFTransportPoll(t, GATEWAY_POLL_TIMEOUT_MS, events,
                               NVPSD_GATEWAY_POLL_EVENTS, &eventCount);
        if (pollErr != NVPSF_TRANSPORT_OK && pollErr != NVPSF_TRANSPORT_TIMEOUT)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: transport poll error",
                           std::string("err=") + NvPSFTransportErrName(pollErr));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        for (size_t e = 0; e < eventCount; ++e)
        {
            const NvPSFTransportEvent& ev = events[e];

            if (ev.kind == NVPSF_EVENT_PEER_CONNECTED)
            {
                /*
                 * Stage one only. The peer has been accepted and its credentials
                 * validated by the transport, but it gets no clientTable slot and
                 * no traffic until it sends a REGR we accept. It is put on the
                 * clock immediately, so a connection that never registers cannot
                 * hold a transport slot indefinitely.
                 */
                std::lock_guard<std::mutex> lock(clientTableMutex);
                pendingPeers.push_back(
                    { ev.peer, std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(ADMISSION_TIMEOUT_MS) });
                continue;
            }

            if (ev.kind == NVPSF_EVENT_PEER_GONE)
            {
                /*
                 * Reclaim directly rather than waiting for the heartbeat ladder
                 * to notice. The action set is the tier-3 one, byte for byte.
                 */
                g_regrLimiter.forget(ev.peer);
                std::lock_guard<std::mutex> lock(clientTableMutex);
                pendingPeers.erase(
                    std::remove_if(pendingPeers.begin(), pendingPeers.end(),
                                   [&ev](const PendingPeer& p) { return p.peer == ev.peer; }),
                    pendingPeers.end());
                const int slot = findSlotByPeerLocked(ev.peer);
                if (slot >= 0)
                    releaseClientSlotLocked(static_cast<unsigned int>(slot),
                                            "peer disconnected", true);
                continue;
            }

            if (ev.kind != NVPSF_EVENT_MESSAGE_READY)
                continue;

            /*
             * Read the message. On a shared-socket (UDP) server the event
             * carries NVPSF_PEER_INVALID and the sender is only known after the
             * read, so the sender is always taken from Recv rather than from the
             * event.
             */
            NvPSFPeerId from = NVPSF_PEER_INVALID;
            size_t      n    = 0;
            const NvPSFTransportErr rerr =
                NvPSFTransportRecv(t, &from, buf.data(), bufSize, &n);
            if (rerr == NVPSF_TRANSPORT_WOULD_BLOCK)
                continue;
            if (rerr == NVPSF_TRANSPORT_TRUNCATED)
            {
                /* Oversized for a control message: a protocol error, not a
                 * transient. The message is gone and cannot be re-read. */
                NvPSBWriteData(NVPSB_LOG_WARNING,
                               "PSD-Gateway: oversized control message discarded", "");
                continue;
            }
            if (rerr == NVPSF_TRANSPORT_PEER_GONE)
            {
                /*
                 * A disconnect can surface here as a return code instead of as a
                 * PEER_GONE event, and when it does there will be no event at
                 * all: Recv releases the transport slot itself before returning,
                 * so the descriptor is closed and gone from the poll set.
                 *
                 * Recv is round-robin over every slot rather than the one the
                 * event named, so a Recv issued for peer A can land on peer B's
                 * disconnect first. Swallowing this -- which the first pass did
                 * -- orphaned B's clientTable entry: it stayed inUse with a dead
                 * peer id until the heartbeat ladder expired ~20-30 s later, and
                 * the eventual log said "heartbeat exhausted" rather than "peer
                 * disconnected", so the SDM's latch lost its attribution. That
                 * is precisely the evidence loss this ordering prevents.
                 *
                 * No transport drop is queued: the transport has already
                 * released this slot.
                 */
                if (from != NVPSF_PEER_INVALID)
                {
                    g_regrLimiter.forget(from);
                    std::lock_guard<std::mutex> lock(clientTableMutex);
                    pendingPeers.erase(
                        std::remove_if(pendingPeers.begin(), pendingPeers.end(),
                                       [&from](const PendingPeer& p) { return p.peer == from; }),
                        pendingPeers.end());
                    const int slot = findSlotByPeerLocked(from);
                    if (slot >= 0)
                        releaseClientSlotLocked(static_cast<unsigned int>(slot),
                                                "peer disconnected", false);
                }
                continue;
            }
            if (rerr != NVPSF_TRANSPORT_OK)
            {
                NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: transport recv error",
                               std::string("err=") + NvPSFTransportErrName(rerr));
                continue;
            }

            /*
             * Put every sender that is not already registered on the admission
             * clock, not just those seen through a PEER_CONNECTED event.
             *
             * This is what makes the reaper cover UDP. A datagram server has no
             * connections, so the transport learns a UDP peer at its first
             * message and no connect event is ever reported; without this, a
             * UDP sender that never registers would hold a transport slot for
             * the life of the process and stage one would be exhaustible after
             * all. Registering the peer here is idempotent -- a valid REGR
             * removes it below, and a peer already pending is not re-added, so
             * a chatty non-registering sender cannot refresh its own deadline.
             */
            {
                std::lock_guard<std::mutex> lock(clientTableMutex);
                if (findSlotByPeerLocked(from) < 0)
                {
                    const bool alreadyPending =
                        std::any_of(pendingPeers.begin(), pendingPeers.end(),
                                    [&from](const PendingPeer& p) { return p.peer == from; });
                    if (!alreadyPending)
                    {
                        pendingPeers.push_back(
                            { from, std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(ADMISSION_TIMEOUT_MS) });
                    }
                }
            }

            /* Heartbeat ACK. */
            if (n == NVPSD_GATEWAY_HB_MSG_SIZE &&
                std::memcmp(buf.data(), NVPSD_GATEWAY_HB_MAGIC_CLIENT, 4) == 0)
            {
                uint32_t netSeq;
                std::memcpy(&netSeq, buf.data() + 4, 4);
                const uint32_t ackSeq = ntohl(netSeq);
                std::lock_guard<std::mutex> lock(clientTableMutex);
                const int slot = findSlotByPeerLocked(from);
                if (slot >= 0)
                {
                    /* Only advance on a genuinely newer ACK; a stale or duplicate
                     * ACK must not clear missedCount, or a dead client could keep
                     * itself alive forever and never be reclaimed. */
                    ClientEntry& ce = clientTable[static_cast<unsigned int>(slot)];
                    if (isSeqAhead(ackSeq, ce.lastAckedSeq))
                    {
                        ce.lastAckedSeq = ackSeq;
                        ce.missedCount = 0;
                    }
                }
                continue;
            }

            if (n < 4 || std::memcmp(buf.data(), NVPSD_GATEWAY_REG_MAGIC, 4) != 0)
                continue;

            /* Under UNIX_SOCKET the peer's credentials were validated against the
             * allowlisted GID before this message could arrive. Under UDP_SOCKET
             * REGR remains unauthenticated on that path.
             *
             * Rate limit first: a REGR over the limit is refused unread, and only
             * the first such refusal in a window is answered. */
            const NvPSDGatewayRegrAdmission admission = g_regrLimiter.admit(from, gatewayNowMs());
            if (admission != NvPSDGatewayRegrAdmission::ADMIT)
            {
                if (admission == NvPSDGatewayRegrAdmission::REFUSE_NOTIFY)
                    refuseRegistration(t, from, NVPSD_REG_REJECT_RATE_LIMITED);
                continue;
            }

            /* Every event type is external input. Mandatory system/safety types
             * are added only once a requested type is valid, so an empty or
             * all-invalid REGR stays refused. */
            std::unordered_set<EventType> types;
            const uint8_t malformedOrInvalid = NvPSDGatewayClassifyRegr(buf.data(), n, &types);
            if (malformedOrInvalid != 0U)
            {
                refuseRegistration(t, from, malformedOrInvalid);
                continue;
            }

            bool tableFull = false;
            {
                std::lock_guard<std::mutex> lock(clientTableMutex);

                int slot = findSlotByPeerLocked(from);
                const bool existingSameClient = (slot >= 0);
                if (slot < 0)
                {
                    for (unsigned int c = 0; c < g_maxClients && slot < 0; ++c)
                    {
                        if (!clientTable[c].inUse)
                            slot = static_cast<int>(c);
                    }
                }
                if (slot >= 0)
                {
                    /*
                     * Stage two, and only now. The peer stops being pending at
                     * the point it actually gets a clientTable slot -- not
                     * before the search, which is where the first pass put it.
                     * Un-pending on the way to a *rejection* left the peer in
                     * neither structure: no table slot, and invisible to the
                     * admission reaper, so its transport slot was held for the
                     * life of the process. Ten such REGRs against a full table
                     * would fill the pool and lock out every legitimate SDM --
                     * a slot-exhaustion DoS through the very path two-stage
                     * admission closes.
                     */
                    pendingPeers.erase(
                        std::remove_if(pendingPeers.begin(), pendingPeers.end(),
                                       [&from](const PendingPeer& p) { return p.peer == from; }),
                        pendingPeers.end());

                    ClientEntry& ce = clientTable[static_cast<unsigned int>(slot)];
                    ce.peer = from;
                    ce.eventTypes = std::move(types);
                    ce.inUse = true;
                    ce.lastAckedSeq = HB_SEQ_NONE;  /* first HB counts as missed until ACKed */
                    ce.missedCount = 0;
                    if (!existingSameClient)
                    {
                        /* A genuinely new client gets a fresh epoch, which
                         * re-baselines its sequence stream. A re-registration
                         * from the same peer keeps both, so an SDM that re-REGRs
                         * mid-stream does not see a spurious gap. */
                        ce.gatewayTxEpoch = allocateGatewayClientEpoch();
                        ce.nextGatewayTxSeq = NVPSD_GATEWAY_DECISION_TX_SEQ_MIN;
                        /* An SDM that reconnected while a recovery is still owed
                         * may hold gateway_comm from before; owing it a record is
                         * harmless if it does not. */
                        ce.recoveryOwedGen =
                            recoveryOwedByAnyLocked() ? g_pssFaultGeneration : 0U;
                    }
                    g_deferredLogs.push_back(
                        "PSD-Gateway: registered client " + std::to_string(slot) + " (" +
                        std::to_string(ce.eventTypes.size()) + " event types)");
                }
                else
                {
                    /* Peer stays pending, so the reaper still owns it. */
                    tableFull = true;
                }
            }
            if (tableFull)
                refuseRegistration(t, from, NVPSD_REG_REJECT_TABLE_FULL);
        }

        /*
         * Admission-timeout reaper. Without this, stage one becomes the new
         * exhaustion target: connections that never register would accumulate
         * until the transport pool is full and no legitimate SDM could connect.
         *
         * Snapshot under the lock, drop outside it -- DropPeer is a transport
         * call and must never run under clientTableMutex.
         */
        toDrop.clear();
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(clientTableMutex);
            for (auto it = pendingPeers.begin(); it != pendingPeers.end();)
            {
                if (it->deadline <= now)
                {
                    toDrop.push_back(it->peer);
                    it = pendingPeers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (const NvPSFPeerId p : toDrop)
        {
            g_regrLimiter.forget(p);
            (void)NvPSFTransportDropPeer(t, p);
            NvPSBWriteData(NVPSB_LOG_WARNING,
                           "PSD-Gateway: dropped a connection that never registered", "");
        }

        /* Emit deferred logs and hand reclaimed peers back to the transport.
         * Outside every lock, by construction. */
        flushDeferredActions(t);
    }
}

/* Heartbeat sender: copy client list under lock; send outside lock; reclaim dead client slots. */
static void heartbeatSenderLoop()
{
    while (!stopGateway.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(HB_SEND_INTERVAL_MS));
        if (stopGateway.load())
            break;

        NvPSFTransport* const t = g_transport.load(std::memory_order_acquire);
        if (t == nullptr)
            break;

        /* Start at 1 so first HB is always ahead of new clients' lastAckedSeq (HB_SEQ_NONE); first miss is counted. */
        uint32_t seq = hbSeqNo.fetch_add(1);
        char pkt[NVPSD_GATEWAY_HB_MSG_SIZE];
        std::memcpy(pkt, NVPSD_GATEWAY_HB_MAGIC_GATEWAY, 4);
        uint32_t netSeq = htonl(seq);
        std::memcpy(pkt + 4, &netSeq, 4);

        /* Same two-phase shape as the DecisionRequest fan-out: snapshot the peer
         * set under the lock, transmit after releasing it. */
        std::vector<NvPSFPeerId> peers;
        {
            std::lock_guard<std::mutex> lock(clientTableMutex);
            for (unsigned int c = 0; c < g_maxClients; ++c)
            {
                if (!clientTable[c].inUse)
                    continue;
                peers.push_back(clientTable[c].peer);
            }
        }
        for (const NvPSFPeerId hbPeer : peers)
        {
            const NvPSFTransportErr herr = NvPSFTransportSend(t, hbPeer, pkt, sizeof(pkt));
            if (herr != NVPSF_TRANSPORT_OK)
            {
                /* A missed heartbeat is not reclaimed here. Letting the send
                 * path reclaim would give the ladder two producers and could
                 * skip the warn and degraded tiers entirely; the miss is counted
                 * below when no ACK arrives, exactly as before. */
                /* ERR: a missed HB send is fault evidence. */
                NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: HB send failed",
                               std::string("err=") + NvPSFTransportErrName(herr));
            }
        }

        /* Wait for ACKs; add grace to avoid false misses during transient send/recv delays. */
        int waitMs = (HB_ACK_TIMEOUT_MS > HB_SEND_INTERVAL_MS)
            ? (HB_ACK_TIMEOUT_MS - HB_SEND_INTERVAL_MS + HB_ACK_GRACE_MS)
            : (HB_FALLBACK_SLEEP_MS + HB_ACK_GRACE_MS);
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));

        if (stopGateway.load())
            break;

        /*
         * SCOPED, and the braces are the fix. P0.
         *
         * This lock_guard was declared at the while-body scope, so it lived
         * until the closing brace of the loop -- which put flushDeferredActions()
         * at the end of this body INSIDE the critical section, despite the
         * comment there saying "outside the lock" and flushDeferredActions' own
         * contract saying "must be called with clientTableMutex NOT held".
         *
         * flushDeferredActions() re-acquires clientTableMutex. std::mutex is not
         * recursive, so this thread deadlocked against ITSELF on its first pass
         * through the ladder -- roughly one HB interval after the first client
         * registered -- and never released the mutex again.
         * sendToRelevantClients() and registrationListenerLoop() then blocked
         * behind it permanently.
         *
         * Symptom: the gateway delivers ~13 DecisionRequests and then stops for
         * good. No fan-out, no further registrations, and a UDP receive queue
         * that grows without bound. Every registered SDM latches on decision
         * freshness, and a restarting SDM can never re-register -- which is what
         * makes this failure mode hard to evidence.
         *
         * Found by evidence/b1_g19_lockorder_driver.cpp: all three worker
         * threads showed futex_wait on clientTableMutex, and the heartbeat
         * thread's own stack named flushDeferredActions as the second acquire.
         *
         * The identical ladder in sendToRelevantClients (:493) was ALREADY
         * scoped correctly. One of two copies had braces; this was the other.
         */
        {
        std::lock_guard<std::mutex> lock(clientTableMutex);
        const uint32_t maxF = g_maxHbFailures.load();
        const uint32_t warnW = maxF / 2U;
        for (unsigned int c = 0; c < g_maxClients; ++c)
        {
            if (!clientTable[c].inUse)
                continue;
            /* Wrap-safe: mark missed when seq is ahead of last acked. */
            if (isSeqAhead(seq, clientTable[c].lastAckedSeq))
            {
                clientTable[c].missedCount++;
                const int m = clientTable[c].missedCount;
                const uint32_t mu = static_cast<uint32_t>(m);
                /* One log line per client per miss interval (avoid duplicate tier lines + string churn under multi-client loss). */
                if (mu >= maxF)
                {
                    /* Tier 3. Routed through the one action set so that
                     * heartbeat exhaustion and an observed disconnect are
                     * indistinguishable downstream. */
                    g_deferredLogs.push_back(
                        "PSD-Gateway: client " + std::to_string(c) + " HB failure miss=" +
                        std::to_string(m) + "/" + std::to_string(maxF) + " seq=" +
                        std::to_string(seq));
                    releaseClientSlotLocked(c, "heartbeat exhausted", true);
                }
                else
                {
                    std::string tier;
                    if (mu >= 1U && mu <= warnW)
                        tier = " [warn tier]";
                    else if (mu > warnW && mu < maxF)
                        tier = " [degraded: DecisionRequest forwarding stopped]";
                    g_deferredLogs.push_back(
                        "PSD-Gateway: no HB ACK from client " + std::to_string(c) +
                        " seq=" + std::to_string(seq) + " miss=" + std::to_string(m) +
                        "/" + std::to_string(maxF) + tier);
                }
            }
        }

        }   /* clientTableMutex released HERE, before the flush below. */

        /* Ladder logs and any reclaimed peers, outside the lock. */
        flushDeferredActions(t);
    }
}

/* Daemon callbacks */
NvPSDErr onEventNotificationReceive(const DecisionRequest* request,
                                    DecisionResponse* response)
{
    if (!request || !response)
        return NVPSD_FAIL;

    NvPSBWriteData(
        NVPSB_LOG_INFO,
        "PSD-Gateway: received DecisionRequest id=" +
            std::to_string(request->requestId) +
            " with " + std::to_string(request->sensorDataSummarySize) +
            " events", "");

    response->decisionId = request->requestId;

    /* PSS request ids are Gateway delivery evidence. Suppress exact duplicates
     * to avoid replaying non-idempotent physical events; convert invalid ids
     * and sequence gaps/resets into SW_FAIL evidence while preserving current
     * valid traffic for restart/liveness tolerance. */
    NvPSDGatewayPssRequestObservation pssObservation =
        NvPSDGatewayPssRequestObservation::INVALID;
    {
        std::lock_guard<std::mutex> lock(pssSequenceMutex);
        pssObservation = NvPSDGatewayObservePssRequestId(&pssSequenceState, request->requestId);
    }

    if (!NvPSDGatewayShouldForwardObservedPssRequest(pssObservation))
    {
        if (pssObservation == NvPSDGatewayPssRequestObservation::DUPLICATE_OR_STALE)
        {
            NvPSBWriteData(
                NVPSB_LOG_WARNING,
                "PSD-Gateway: duplicate PSS DecisionRequest suppressed",
                "requestId=" + std::to_string(request->requestId));
            response->action = NO_ACTION_REQUIRED;
            response->confidenceLevel = 1.0F;
            return NVPSD_SUCCESS;
        }

        NvPSBWriteData(
            NVPSB_LOG_ERR,
            "PSD-Gateway: invalid PSS DecisionRequest id suppressed; sending SW_FAIL to all clients",
            "requestId=" + std::to_string(request->requestId));
        DecisionRequest swFailReq = {};
        if (NvPSDGatewayBuildInvalidPssRequestIdSwFailRequest(*request, &swFailReq))
        {
            sendToRelevantClients(swFailReq, true);
            notePssSyntheticFaultBroadcast();
        }
        else
        {
            NvPSBWriteData(
                NVPSB_LOG_ERR,
                "PSD-Gateway: failed to build invalid PSS request-id SW_FAIL request",
                "requestId=" + std::to_string(request->requestId));
        }
        response->action = NO_ACTION_REQUIRED;
        response->confidenceLevel = 0.0F;
        return NVPSD_SUCCESS;
    }

    if (pssObservation == NvPSDGatewayPssRequestObservation::GAP &&
        request->pssStatus.mode != ERROR)
    {
        DecisionRequest swFailReq = {};
        if (NvPSDGatewayBuildPssSequenceGapSwFailRequest(*request, &swFailReq))
        {
            NvPSBWriteData(
                NVPSB_LOG_ERR,
                "PSD-Gateway: PSS DecisionRequest sequence gap/reset detected; sending SW_FAIL before current request",
                "currentRequestId=" + std::to_string(request->requestId));
            sendToRelevantClients(swFailReq, true);
            notePssSyntheticFaultBroadcast();
        }
        else
        {
            NvPSBWriteData(
                NVPSB_LOG_ERR,
                "PSD-Gateway: failed to build PSS sequence-gap SW_FAIL request",
                "currentRequestId=" + std::to_string(request->requestId));
        }
    }

    /* PSS ERROR mode: send an explicit all-client fault before the filtered original request. */
    if (request->pssStatus.mode == ERROR)
    {
        DecisionRequest swFailReq = {};
        /* Local output is non-null; helper's false path exists only for
         * public null-output validation, so there is no runtime failure branch
         * to handle here. */
        (void)NvPSDGatewayBuildPssErrorSwFailRequest(*request, &swFailReq);
        NvPSBWriteData(
            NVPSB_LOG_WARNING,
            "PSD-Gateway: PSS in ERROR mode; sending SW_FAIL to all clients before filtered request",
            "requestId=" + std::to_string(request->requestId));
        sendToRelevantClients(swFailReq, true);
        notePssSyntheticFaultBroadcast();
        sendToRelevantClients(*request, false);
        response->action = NO_ACTION_REQUIRED;
        return NVPSD_SUCCESS;
    }

    /*
     * The link is demonstrably healthy: this request was accepted in sequence and
     * PSS is not reporting ERROR. If a synthetic fault was broadcast earlier and
     * nothing has said so since, tell EVERY client -- including the ones whose
     * subscriptions mean no ordinary traffic will ever reach them.
     *
     * A GAP does not qualify: it is itself the fault. Only ACCEPT does.
     */
    if (pssObservation == NvPSDGatewayPssRequestObservation::ACCEPT)
    {
        bool owed = false;
        {
            std::lock_guard<std::mutex> lock(clientTableMutex);
            owed = recoveryOwedByAnyLocked();
        }
        if (owed)
        {
            DecisionRequest recoveredReq = {};
            /* Local output is non-null; the helper's false path exists only for
             * public null-output validation. */
            (void)NvPSDGatewayBuildPssCommRecoveredRequest(*request, &recoveredReq);
            g_recoverySent.clear();
            sendToRelevantClients(recoveredReq, true, &g_recoverySent);

            /* Pay each debt the transport accepted a record for -- unless a new
             * synthetic fault re-marked that client while the sends were in
             * flight, in which case the newer debt stands. */
            unsigned int paid = 0U;
            unsigned int stillOwed = 0U;
            {
                std::lock_guard<std::mutex> lock(clientTableMutex);
                for (const RecoverySent& r : g_recoverySent)
                {
                    ClientEntry& ce = clientTable[r.slot];
                    if (!ce.inUse || ce.peer != r.peer)
                        continue;
                    const uint64_t after = NvPSDGatewayRecoveryDebtAfterSend(
                        ce.recoveryOwedGen, r.recoveryOwedGen, true);
                    if (after == 0U && ce.recoveryOwedGen != 0U)
                        ++paid;
                    ce.recoveryOwedGen = after;
                }
                for (unsigned int c = 0; c < g_maxClients; ++c)
                {
                    if (clientTable[c].inUse && clientTable[c].recoveryOwedGen != 0U)
                        ++stillOwed;
                }
            }

            /* Logged only when a record went out, so a client parked in the
             * degraded tier does not produce a line per accepted request. */
            if (paid > 0U && stillOwed == 0U)
            {
                NvPSBWriteData(
                    NVPSB_LOG_INFO,
                    "PSD-Gateway: PSS link recovered; recovery record delivered to every client that was owed one",
                    "requestId=" + std::to_string(request->requestId) +
                        ", clients=" + std::to_string(paid));
            }
            else if (paid > 0U)
            {
                NvPSBWriteData(
                    NVPSB_LOG_WARNING,
                    "PSD-Gateway: recovery record delivered; some clients still owed one (degraded heartbeat, send failure, or a new fault); retried on the next accepted request",
                    "requestId=" + std::to_string(request->requestId) +
                        ", delivered=" + std::to_string(paid) +
                        ", stillOwed=" + std::to_string(stillOwed));
            }
        }
    }

    /* Normal / Degraded mode: forward all events (including STALE) to SDM */
    if (request->sensorDataSummarySize > 0)
    {
        const bool statusNoopOnly = NvPSDGatewayIsStatusNoopOnlyRequest(*request);
        sendToRelevantClients(*request, false);
        response->action = statusNoopOnly ? NO_ACTION_REQUIRED : IMPLEMENT_SAFETY_CONTROL;
    }
    else
    {
        NvPSBWriteData(
            NVPSB_LOG_INFO,
            "PSD-Gateway: no events in request – nothing to forward", "");
        response->action = NO_ACTION_REQUIRED;
    }

    return NVPSD_SUCCESS;
}

NvPSDErr onPSDControlStop()
{
    stopGateway.store(true);
    return NVPSD_SUCCESS;
}

/*  Init / Shutdown */
int launchPSDControl(unsigned int numClients,
                     NvPSFTransportKind transportKind, const std::string& unixSocketPath)
{
    if (numClients == 0 || numClients > MAX_DECISION_MAKER_CLIENTS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: numClients must be 1.." +
                        std::to_string(MAX_DECISION_MAKER_CLIENTS), "");
        return -1;
    }
    g_maxClients = numClients;

    gatewayClientEpochSeed.store(makeGatewayEpochSeed(), std::memory_order_relaxed);
    for (unsigned int i = 0; i < MAX_DECISION_MAKER_CLIENTS; ++i)
    {
        clientTable[i].inUse = false;
        clientTable[i].gatewayTxEpoch = 0U;
        clientTable[i].recoveryOwedGen = 0U;
    }
    {
        std::lock_guard<std::mutex> lock(clientTableMutex);
        pendingPeers.clear();
    }

    /*
     * The transport pool is deliberately larger than the clientTable. It has to
     * be: the two-stage admission means a peer holds a transport slot
     * from the moment it connects but only takes a clientTable slot once it has
     * sent a valid REGR, so unregistered peers need somewhere to sit that is not
     * the decision-maker capacity. Sizing it at twice the client count gives
     * every registered SDM room plus an equal pending headroom, and the
     * admission-timeout reaper is what keeps that headroom from filling up.
     */
    NvPSFTransportCfg cfg = {};
    cfg.kind            = transportKind;
    cfg.role            = NVPSF_ROLE_SERVER;
    cfg.link            = NVPSF_LINK_GATEWAY_TO_SDM;
    cfg.maxPeers        = numClients * 2U;
    cfg.maxMessageBytes = sizeof(NvPSDGatewayDecisionRequestPacket);
    /* The measured floor for this link. See NvPSFGatewaySdmMinQueueDepth() for
     * the derivation and the deployment sysctl it requires. A shortfall is a
     * fatal startup error. */
    cfg.minQueueDepth   = NvPSFGatewaySdmMinQueueDepth();
    cfg.recvTimeoutMs   = GATEWAY_POLL_TIMEOUT_MS;
    cfg.sendTimeoutMs   = 0U;
    cfg.unixPath        = unixSocketPath.c_str();
    cfg.unixMode        = NVPSD_GATEWAY_SOCKET_MODE;
    cfg.allowedGid      = getgid();
    /* UNIX_SOCKET only: the inet fields are unused and left zeroed. */

    NvPSFTransport* t = nullptr;
    const NvPSFTransportErr oerr = NvPSFTransportOpen(&cfg, &t);
    if (oerr != NVPSF_TRANSPORT_OK || t == nullptr)
    {
        /* Fatal at startup, never a downgrade to another kind. */
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: transport open failed",
                       std::string("kind=") + NvPSFTransportKindName(transportKind) +
                           ", err=" + NvPSFTransportErrName(oerr));
        return -1;
    }
    g_transport.store(t, std::memory_order_release);

    /*
     * The credential contract has to be legible from the gateway's own log or
     * the integrator cannot confirm it. Note the enforced check is on the
     * peer's PRIMARY gid: an SDM carrying the PSF group only as a supplementary
     * group is refused, and this line is what makes that diagnosable rather than
     * presenting as a generic connect failure. A GID is not a network address,
     * so this is not covered by the sensitive-data logging rule.
     */
    if (transportKind == NVPSF_TRANSPORT_UNIX_SOCKET)
    {
        NvPSBWriteData(NVPSB_LOG_INFO, "PSD-Gateway: peer credential allowlist",
                       "requiredPrimaryGid=" + std::to_string(static_cast<unsigned long>(cfg.allowedGid)));
    }

    stopGateway.store(false);
    hbSeqNo.store(1);  /* first sent HB will be seq 1; new clients use lastAckedSeq=HB_SEQ_NONE so first miss is counted */
    NvPSBWriteData(NVPSB_LOG_INFO,
                   "PSD-Gateway: listening for SDM clients | max " + std::to_string(numClients) + " clients | HB enabled", "");

    std::thread regThread(registrationListenerLoop);
    std::thread hbThread(heartbeatSenderLoop);

    while (!stopGateway.load())
    {
        if (g_signal_received)
        {
            shutdownPSDControl();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (hbThread.joinable())
        hbThread.join();
    if (regThread.joinable())
        regThread.join();

    /* Socket is closed in main() after NvPSDSocketExit() so callback never uses it after close. */
    return 0;
}

void shutdownPSDControl()
{
    stopGateway.store(true);
}

/*
 * Call only after the registration and heartbeat threads are joined, so no
 * thread can be inside a transport call. NvPSFTransportClose closes every
 * descriptor and unlinks a bound AF_UNIX path; the handle is cleared first so a
 * late caller sees nullptr rather than a freed object.
 */
void closePSDControlSocket()
{
    NvPSFTransport* const t = g_transport.exchange(nullptr, std::memory_order_acq_rel);
    if (t != nullptr)
        (void)NvPSFTransportClose(t);
}

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [--num_clients N] [--max_hb_failures N] [--regr_max_per_window N] [--regr_window_ms MS]\n\n"
              << "PSD Gateway — bridges NvPSS daemon events to decision-maker clients.\n\n"
              << "The gateway -> SDM link is AF_UNIX SOCK_SEQPACKET at "
              << NVPSF_SOCKET_PATH_PSD_GATEWAY << " and is NOT selectable.\n"
              << "Registration is authenticated by SO_PEERCRED against this process's\n"
              << "primary GID. --transport, --sdm_socket, --sdm_ip, --sdm_port and\n"
              << "--sdm_allow were retired with that decision.\n\n"
              << "Options:\n"
              << "  --num_clients <N>       Max decision-maker clients, 1-" << MAX_DECISION_MAKER_CLIENTS
              << " (default: " << MAX_DECISION_MAKER_CLIENTS << ").\n"
              << "  --max_hb_failures <N>   Heartbeat miss limit, 1-255 (default: 10).\n"
              << "  --regr_max_per_window <N>  REGRs accepted per peer per window, "
              << NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_MIN << "-" << NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_MAX
              << " (default: " << NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_DEFAULT << ").\n"
              << "  --regr_window_ms <MS>   The rate-limit window, " << NVPSD_GATEWAY_REGR_WINDOW_MS_MIN
              << "-" << NVPSD_GATEWAY_REGR_WINDOW_MS_MAX << " ms (default: "
              << NVPSD_GATEWAY_REGR_WINDOW_MS_DEFAULT << "). A REGR over the limit is\n"
              << "                          refused (RGNK reason=rate_limited) unread.\n"
              << "  -h, --help              Show this help message.\n";
}

/* Main entry point */
int main(int argc, char* argv[])
{
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const char* prog = (argc > 0 && argv[0] != nullptr) ? argv[0] : "nvpsd_gateway";
    unsigned int numClients = MAX_DECISION_MAKER_CLIENTS;
    uint32_t regrMaxPerWindow = NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_DEFAULT;
    uint32_t regrWindowMs = NVPSD_GATEWAY_REGR_WINDOW_MS_DEFAULT;

    /*
     * GATEWAY -> SDM IS AF_UNIX SOCK_SEQPACKET. NOT SELECTABLE.
     *
     * Deployment decision: this hop and the PSS RPC and PSS->PSD hops all run on
     * UNIX_SOCKET; only the out-of-scope SDM -> cmd_receiver (PLC) link stays
     * UDP, and that is a raw AF_INET datagram socket inside each SDM, not this
     * transport. With no selection there is no --transport, no --sdm_ip/--sdm_port
     * (a UNIX endpoint has no address pair) and no --sdm_allow: registration is
     * authenticated by SO_PEERCRED against the process primary GID, so the
     * source-address allowlist has nothing to guard.
     *
     * Hard-coded rather than defaulted, deliberately. A default is something an
     * operator can override; this is a property of the deployment, and a flag
     * that silently accepted UDP_SOCKET would reintroduce an unauthenticated
     * registration path that nothing else in the system expects.
     */
    const NvPSFTransportKind sdmTransport = NVPSF_TRANSPORT_UNIX_SOCKET;
    const std::string        sdmSocketPath = NVPSF_SOCKET_PATH_PSD_GATEWAY;

    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            printUsage(prog);
            return 0;
        }
        else if (std::strcmp(argv[i], "--num_clients") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --num_clients requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            long n = std::strtol(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' ||
                n < 1 || n > static_cast<long>(MAX_DECISION_MAKER_CLIENTS))
            {
                std::cerr << "error: --num_clients: invalid number (use 1.." << MAX_DECISION_MAKER_CLIENTS << ")\n";
                printUsage(prog);
                return 1;
            }
            numClients = static_cast<unsigned int>(n);
        }
        else if (std::strcmp(argv[i], "--max_hb_failures") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --max_hb_failures requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || v < 1UL || v > 255UL)
            {
                std::cerr << "error: --max_hb_failures: use 1..255\n";
                printUsage(prog);
                return 1;
            }
            g_maxHbFailures.store(static_cast<uint32_t>(v));
        }
        else if (std::strcmp(argv[i], "--regr_max_per_window") == 0 ||
                 std::strcmp(argv[i], "--regr_window_ms") == 0)
        {
            const bool isMax = (std::strcmp(argv[i], "--regr_max_per_window") == 0);
            const unsigned long lo = isMax ? NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_MIN : NVPSD_GATEWAY_REGR_WINDOW_MS_MIN;
            const unsigned long hi = isMax ? NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_MAX : NVPSD_GATEWAY_REGR_WINDOW_MS_MAX;
            if (i + 1 >= argc)
            {
                std::cerr << "error: " << argv[i] << " requires a value\n";
                printUsage(prog);
                return 1;
            }
            const char* opt = argv[i];
            char* end = nullptr;
            errno = 0;
            const unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || argv[i][0] == '-' || v < lo || v > hi)
            {
                std::cerr << "error: " << opt << ": use " << lo << ".." << hi << "\n";
                printUsage(prog);
                return 1;
            }
            if (isMax)
                regrMaxPerWindow = static_cast<uint32_t>(v);
            else
                regrWindowMs = static_cast<uint32_t>(v);
        }
        else if (argv[i][0] == '-')
        {
            std::cerr << "error: unknown option (see --help)\n";
            printUsage(prog);
            return 1;
        }
        else
        {
            std::cerr << "error: unexpected positional argument (see --help)\n";
            printUsage(prog);
            return 1;
        }
    }


    if (NvPSBInitialize("nvpsd_gateway", NVPSB_PSD_CLIENT) != NVPSB_SUCCESS)
    {
        std::cerr << "Failed to initialise PSB" << std::endl;
        return 1;
    }

    /* The authority on which transport a run actually used. The
     * safe-state injectors' --transport flag is an operator declaration and
     * cannot observe this; an evidence artifact is attributable only when it is
     * read against this line. Emitted through NvPSB rather than std::cerr so it
     * lands in the same log the rest of the startup record does. */
    NvPSBWriteData(NVPSB_LOG_INFO,
                   "Gateway->SDM transport selected",
                   std::string("transport=") + NvPSFTransportKindName(sdmTransport));
#ifdef NVPSF_DBG
    /* Endpoint detail is debug-only: addresses, ports and internal socket paths
     * are sensitive per the repo logging rule. The transport *kind* above is the
     * value needed for evidence attribution; the endpoint is not. */
    NvPSBWriteData(NVPSB_LOG_DEBUG,
                   "Gateway->SDM endpoint",
                   "socket=" + sdmSocketPath);
#endif

    /* Single NvPSD context: one AF_UNIX SOCK_SEQPACKET connection to the NvPSS
     * daemon, carrying all events. */
    NvPSDCtx* ctx = NvPSDCreateContext();
    if (!ctx)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to create NvPSD Control context", "");
        return 1;
    }

    /*
     * This was NvPSDInitialize() with four message-queue names,
     * and NvPSDInitialize hard-codes POSIX_MSG_QUE -- which is what made the
     * socket path unreachable in production no matter what anything else
     * selected. The gateway now takes the socket path, so the PSS -> PSD link
     * is UNIX_SOCKET end to end.
     *
     * There is no configuration here on purpose. The link is not
     * selectable: NvPSSDaemon rejects the retired PSSDToPSDComBackend key
     * outright, so no operator input can reach this decision.
     */
    /*
     * Retry the connect, count-bounded but generous.
     *
     * Under the message queues this was order-independent: mq_open(O_CREAT)
     * created the queue whichever process ran first. A socket connect is not --
     * it fails with ENOENT/ECONNREFUSED until nvpss_daemon has bound and
     * listened. Both shipped launchers start nvpsd_gateway BEFORE nvpss_daemon,
     * so without this loop the gateway dies at startup on every single boot,
     * no DecisionRequest ever reaches an SDM, and every SDM latches on decision
     * freshness. Failing closed on a genuine misconfiguration is correct;
     * failing closed on a startup race is an availability defect wearing a
     * safety argument's clothes.
     *
     * The launcher order is corrected too, but this loop is what makes the
     * gateway robust to it rather than dependent on it.
     */
    /*
     * Paired with NvPSDSocketExit(), and the pairing matters: NvPSDExit() is
     * the exit for a different backend, so calling it on a socket context
     * leaves the PSS -> PSD channel open on shutdown. It compiles either way,
     * and neither name says which transport it belongs to.
     */
    NvPSDErr err = NVPSD_FAIL;
    for (int attempt = 0; attempt < PSD_SOCKET_CONNECT_MAX_ATTEMPTS; ++attempt)
    {
        err = NvPSDSocketInitialize(ctx, NVPSD_CLIENT);
        if (err == NVPSD_SUCCESS)
        {
            break;
        }
        if (g_signal_received)
        {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(PSD_SOCKET_CONNECT_RETRY_DELAY_MS));
    }
    if (err != NVPSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
                       "Failed to connect to nvpss_daemon on the PSS->PSD socket",
                       "attempts=" + std::to_string(PSD_SOCKET_CONNECT_MAX_ATTEMPTS));
        NvPSDDestroyContext(ctx);
        return 1;
    }

    /*
     * Register the event types this gateway wants. Without this the daemon's
     * eventTypeToClient map stays empty and EVERY sendDecisionRequestToPSD
     * returns "No PSD client registered for event type" -- the PSS -> PSD link
     * would be connected and completely inert.
     *
     * This was invisible before C1c because the message-queue arm calls
     * NvPSDProcessDecisionRequest directly and needs no event-type routing at
     * all. Moving to the socket path made registration load-bearing.
     *
     * The gateway is the sole PSD client for the whole event range, so it
     * subscribes to everything rather than a subset: filtering to individual
     * SDMs happens downstream, on the gateway -> SDM link.
     */
    {
        /*
         * PSDRegistrationMsg carries at most 10 event types per message
         * (pss_daemon.h), and the daemon's handler is additive -- it inserts
         * each type into eventTypeToClient without clearing what came before --
         * so the full range is registered in batches rather than one call.
         *
         * PSS_STATUS_NOOP is the last enumerator, and it must be included: it is
         * the periodic status/no-op DecisionRequest that keeps SDM decision
         * freshness alive. Omitting it would leave the SDMs latching on
         * freshness during idle periods.
         */
        constexpr uint32_t kEventTypeCount =
            static_cast<uint32_t>(PSS_STATUS_NOOP) + 1U;
        constexpr uint32_t kPerMessage =
            static_cast<uint32_t>(sizeof(((PSDRegistrationMsg*)nullptr)->eventTypes) /
                                  sizeof(((PSDRegistrationMsg*)nullptr)->eventTypes[0]));

        for (uint32_t base = 0; base < kEventTypeCount; base += kPerMessage)
        {
            EventType batch[kPerMessage];
            const uint32_t n = std::min(kPerMessage, kEventTypeCount - base);
            for (uint32_t i = 0; i < n; ++i)
            {
                batch[i] = static_cast<EventType>(base + i);
            }
            err = NvPSDRegisterEventTypes(ctx, batch, n);
            if (err != NVPSD_SUCCESS)
            {
                NvPSBWriteData(NVPSB_LOG_ERR,
                               "Failed to register event types with nvpss_daemon",
                               "batchBase=" + std::to_string(base));
                NvPSDDestroyContext(ctx);
                return 1;
            }
        }
        NvPSBWriteData(NVPSB_LOG_INFO,
                       "Registered PSS->PSD event types",
                       "count=" + std::to_string(kEventTypeCount));
    }

    /* Single PSS client + HB for this process: gateway registers below; NvPSD library must not duplicate. */
    err = NvPSDSetPssHeartbeatExternallyManaged(ctx, 1);
    if (err != NVPSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
                       "NvPSDSetPssHeartbeatExternallyManaged failed (listener already started?)", "");
        NvPSDDestroyContext(ctx);
        return 1;
    }

    NvPSDCallbacks callbacks;
    callbacks.processDecisionRequest  = onEventNotificationReceive;
    callbacks.notifyShutdownRequest   = onPSDControlStop;
    callbacks.publishDecisionResponse = nullptr;

    NvPSBWriteData(NVPSB_LOG_INFO, "Register NvPSD Control callbacks", "");

    err = NvPSDRegisterCallbacks(ctx, &callbacks);
    if (err != NVPSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register callbacks", "");
        NvPSDSocketExit(ctx);
        NvPSDDestroyContext(ctx);
        return 1;
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD Gateway: daemon (AF_UNIX SOCK_SEQPACKET) + SDM decision-maker clients + HB", "");

    gatewayPssClientId.store(UINT32_MAX);
    uint32_t registeredPssId = UINT32_MAX;
    bool     pssRpcRegistered = false;

    for (int attempt = 1; attempt <= PSS_REGISTER_MAX_ATTEMPTS; ++attempt)
    {
        if (g_signal_received)
            break;

        registeredPssId = UINT32_MAX;
        if (NvPSSRegisterPSSClient(&registeredPssId, CLIENT_PSD_GATEWAY) == NVPSSD_SUCCESS)
        {
            gatewayPssClientId.store(registeredPssId);
            stopGatewayPssHb.store(false);
            gatewayPssHbThread = std::thread(gatewayPssHeartbeatLoop);
            NvPSBWriteData(NVPSB_LOG_INFO,
                           "PSD-Gateway: sole PSS client registration + heartbeat for this process, clientId=" +
                               std::to_string(registeredPssId) +
                               (attempt > 1 ? " (after " + std::to_string(attempt) + " attempt(s))" : ""),
                           "");
            pssRpcRegistered = true;
            break;
        }

        if (attempt < PSS_REGISTER_MAX_ATTEMPTS && !g_signal_received)
        {
            NvPSBWriteData(NVPSB_LOG_INFO,
                           "PSD-Gateway: NvPSSRegisterPSSClient failed; retry " + std::to_string(attempt) + "/" +
                               std::to_string(PSS_REGISTER_MAX_ATTEMPTS) + " in " +
                               std::to_string(PSS_REGISTER_RETRY_DELAY_MS) + " ms (PSS RPC socket not ready?)",
                           "");
            std::this_thread::sleep_for(std::chrono::milliseconds(PSS_REGISTER_RETRY_DELAY_MS));
        }
    }

    if (!pssRpcRegistered)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "PSD-Gateway: NvPSSRegisterPSSClient failed after " +
                           std::to_string(PSS_REGISTER_MAX_ATTEMPTS) +
                           " attempt(s) — gateway will not send heartbeats to PSS",
                       "");
    }

    g_regrLimiter.configure(regrMaxPerWindow, regrWindowMs);
    NvPSBWriteData(NVPSB_LOG_INFO, "PSD-Gateway: REGR rate limit",
                   "maxPerWindow=" + std::to_string(regrMaxPerWindow) +
                       ", windowMs=" + std::to_string(regrWindowMs) + ", scope=per peer");

    if (launchPSDControl(numClients, sdmTransport, sdmSocketPath) != 0)
    {
        cleanupGatewayPssClient();
        NvPSDSocketExit(ctx);
        closePSDControlSocket();
        NvPSDDestroyContext(ctx);
        return 1;
    }

    cleanupGatewayPssClient();

    NvPSBWriteData(NVPSB_LOG_INFO, "Exit NvPSD Control", "");
    NvPSDSocketExit(ctx);
    closePSDControlSocket();
    NvPSDDestroyContext(ctx);
    return 0;
}
