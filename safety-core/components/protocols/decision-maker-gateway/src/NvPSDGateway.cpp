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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "NvPSD.h"
#include "NvPSB.h"
#include "pss_daemon.h"
#include "pss_protocol.h"
#include "pss_message_validate.h"
#include "NvPSDGateway.h"
#include "NvPSDGatewayContract.hpp"

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
/* Cap sendto() EINTR retries; brief sleep between retries avoids busy-spin vs yield() alone. */
static constexpr int      SENDTO_EINTR_MAX_RETRIES     = 32;
static constexpr int      SENDTO_EINTR_RETRY_SLEEP_MS = 1;
/* PSS RPC socket may not listen at process start; retry NvPSSRegisterPSSClient so gateway HB to PSS is reliable. */
static constexpr int      PSS_REGISTER_MAX_ATTEMPTS    = 60;
static constexpr int      PSS_REGISTER_RETRY_DELAY_MS  = 500;

/* Signal-safe flag: set in signal handler (async-signal-safe); main loop checks it and calls shutdownPSDControl(). */
static volatile sig_atomic_t g_signal_received = 0;

/* static state */
static int gatewayUdpSock = -1;
static std::atomic<bool> stopGateway{false};
static std::mutex        clientTableMutex;
/* Protects assignment to gatewayUdpSock and close(); I/O uses dup() under this lock so each caller owns
 * an fd until close — safe if gatewayUdpSock is closed/replaced while another thread sendto/recvfroms. */
static std::mutex        gatewaySocketMutex;
static std::atomic<uint32_t> hbSeqNo{0};
static std::atomic<uint64_t> gatewayClientEpochSeed{1U};
static unsigned int      g_maxClients = MAX_DECISION_MAKER_CLIENTS;
static std::mutex        pssSequenceMutex;
static NvPSDGatewayPssSequenceState pssSequenceState;

namespace {

struct ScopedFd {
    int fd;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd()
    {
        if (fd < 0)
            return;
        /* Do not retry close() on EINTR: POSIX leaves fd state ambiguous; a retry could close a reused number. */
        if (::close(fd) < 0)
        {
            if (errno == EINTR)
                NvPSBWriteData(NVPSB_LOG_WARNING,
                               "PSD-Gateway: close(dup gateway fd) returned EINTR; assuming closed (no retry)", "");
            else
                NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: close(dup gateway fd) failed", "");
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

/* dup(gatewayUdpSock) while locked; caller closes the dup (e.g. ScopedFd). Returns -1 if no socket or dup fails. */
int dupGatewaySocketFd()
{
    std::lock_guard<std::mutex> sockLock(gatewaySocketMutex);
    if (gatewayUdpSock < 0)
        return -1;
    return dup(gatewayUdpSock);
}

} // namespace

/* Key for O(1) client lookup by address (HB ACK and re-registration). */
static inline uint64_t clientKey(const struct sockaddr_in* a)
{
    return (static_cast<uint64_t>(a->sin_addr.s_addr) << 16) | (a->sin_port & 0xFFFFU);
}

/* Wrap-safe: true when sentSeq is ahead of ackedSeq in the 32-bit sequence ring (ACK not yet received). */
static inline bool isSeqAhead(uint32_t sentSeq, uint32_t ackedSeq)
{
    uint32_t diff = sentSeq - ackedSeq;
    return (diff != 0 && diff <= 0x7FFFFFFFU);
}

struct ClientEntry
{
    struct sockaddr_in addr;
    socklen_t          addrLen;
    std::unordered_set<EventType> eventTypes;
    bool               inUse;
    uint32_t           lastAckedSeq{0};
    uint64_t           gatewayTxEpoch{0};
    uint32_t           nextGatewayTxSeq{NVPSD_GATEWAY_DECISION_TX_SEQ_MIN};
    int                missedCount{0};
};
static ClientEntry clientTable[MAX_DECISION_MAKER_CLIENTS];
/* O(1) lookup: client address key -> slot index. Updated on register and reclaim. */
static std::unordered_map<uint64_t, unsigned int> g_addrToSlot;

/* Fail-safe HB policy: WARN tier = max/2 (integer division). Configured via --max_hb_failures. */
static std::atomic<uint32_t> g_maxHbFailures{HB_MAX_MISSED_DEFAULT};

static std::atomic<uint32_t> gatewayPssClientId{UINT32_MAX};
static std::mutex            gatewayPssRpcMtx;
static std::atomic<bool>                            stopGatewayPssHb{true};
static std::thread                                  gatewayPssHbThread;

static void sendToRelevantClients(const DecisionRequest& req, bool sendFullRequest);

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
    struct sockaddr_in addr;
    socklen_t          addrLen;
    NvPSDGatewayDecisionRequestPacket packet;
    uint8_t            eventCount;
};

/* Reused in sendToRelevantClients / buildRequestFromIndex (single callback thread) to avoid per-request allocations. */
static thread_local std::vector<SendItem> g_toSendList;
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

/* Send full or filtered DecisionRequest to all registered clients. Build send list under lock; send outside lock. */
static void sendToRelevantClients(const DecisionRequest& req, bool sendFullRequest)
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

            SendItem item = {};
            DecisionRequest payload = {};
            item.addr = clientTable[c].addr;
            item.addrLen = clientTable[c].addrLen;
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

            const uint32_t gatewayTxSeq = clientTable[c].nextGatewayTxSeq;
            /* Payload may differ per client (filtering); recompute CRC before each packet is wrapped. */
            pssDecisionRequestSetCRC(&payload);
            if (!NvPSDGatewayBuildDecisionRequestPacket(payload,
                                                        clientTable[c].gatewayTxEpoch,
                                                        gatewayTxSeq,
                                                        &item.packet))
            {
                NvPSBWriteData(NVPSB_LOG_ERR,
                               "PSD-Gateway: failed to build Gateway DecisionRequest packet", "");
                continue;
            }
            clientTable[c].nextGatewayTxSeq = nextGatewayTxSeqAfter(gatewayTxSeq);
            g_toSendList.push_back(item);
        }
    }

    if (g_toSendList.empty())
        return;

    ScopedFd scoped(dupGatewaySocketFd());
    if (scoped.fd < 0)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING, "PSD-Gateway: dup(gatewayUdpSock) failed (DecisionRequest)", "");
        return;
    }

    for (const SendItem& item : g_toSendList)
    {
        ssize_t sent = -1;
        for (int eintrAttempt = 0; eintrAttempt < SENDTO_EINTR_MAX_RETRIES; ++eintrAttempt)
        {
            sent = sendto(scoped.fd, &item.packet, sizeof(item.packet), 0,
                          reinterpret_cast<const struct sockaddr*>(&item.addr), item.addrLen);
            if (sent >= 0)
                break;
            if (errno != EINTR)
            {
                NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: failed to send DecisionRequest", "");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SENDTO_EINTR_RETRY_SLEEP_MS));
        }
        const NvPSDGatewayPacketSendResult sendResult =
            NvPSDGatewayDecisionPacketSendResult(sent);
        if (sendResult == NvPSDGatewayPacketSendResult::FAILED_SYSCALL)
        {
            if (errno == EINTR)
            {
                NvPSBWriteData(NVPSB_LOG_ERR,
                               "PSD-Gateway: failed to send DecisionRequest (EINTR retries exhausted)", "");
            }
        }
        else if (sendResult == NvPSDGatewayPacketSendResult::FAILED_SHORT_SEND)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: short Gateway DecisionRequest packet send", "");
        }
#ifdef NVPSF_DBG
        if (sendResult == NvPSDGatewayPacketSendResult::SENT)
            NvPSBWriteData(NVPSB_LOG_INFO,
                           "PSD-Gateway: sent Gateway packet seq=" +
                               std::to_string(item.packet.gatewayTxSeq) +
                               " request id=" + std::to_string(item.packet.request.requestId) +
                               " with " + std::to_string(item.eventCount) + " events", "");
#endif
    }
}

/* Registration + HB ACK listener: recvfrom REGR (registration) or HBPC (heartbeat ACK). */
static void registrationListenerLoop()
{
    const size_t bufSize = 4 + 1 + MAX_EVENT_TYPES_PER_CLIENT * sizeof(uint32_t);
    std::vector<char> buf(bufSize);
    struct sockaddr_in sender = {};
    socklen_t slen;

    while (!stopGateway.load())
    {
        slen = sizeof(sender);
        ScopedFd scoped(dupGatewaySocketFd());
        if (scoped.fd < 0)
        {
            if (stopGateway.load())
                break;
            {
                std::lock_guard<std::mutex> g(gatewaySocketMutex);
                if (gatewayUdpSock < 0)
                    break;
            }
            NvPSBWriteData(NVPSB_LOG_WARNING, "PSD-Gateway: dup(gatewayUdpSock) failed", "");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        ssize_t n = recvfrom(scoped.fd, buf.data(), bufSize, 0,
                             reinterpret_cast<struct sockaddr*>(&sender), &slen);
        if (n < 0)
        {
            const int err = errno;
            if (err == EINTR)
                continue;
            if (err != EAGAIN && err != EWOULDBLOCK)
                NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: recvfrom error", "");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (n == NVPSD_GATEWAY_HB_MSG_SIZE && std::memcmp(buf.data(), NVPSD_GATEWAY_HB_MAGIC_CLIENT, 4) == 0)
        {
            uint32_t netSeq;
            std::memcpy(&netSeq, buf.data() + 4, 4);
            uint32_t ackSeq = ntohl(netSeq);
            uint64_t key = clientKey(&sender);
            std::lock_guard<std::mutex> lock(clientTableMutex);
            auto it = g_addrToSlot.find(key);
            if (it != g_addrToSlot.end())
            {
                unsigned int c = it->second;
                if (clientTable[c].inUse)
                {
                    /* Only advance lastAckedSeq and clear missedCount when ackSeq is ahead; ignore stale/duplicate ACKs so dead clients can still be reclaimed. */
                    if (isSeqAhead(ackSeq, clientTable[c].lastAckedSeq))
                    {
                        clientTable[c].lastAckedSeq = ackSeq;
                        clientTable[c].missedCount = 0;
                    }
                }
            }
            continue;
        }
        if (n < 5 || std::memcmp(buf.data(), NVPSD_GATEWAY_REG_MAGIC, 4) != 0)
            continue;

        /* REGR is unauthenticated; deploy in trusted network. TODO: add auth/allowlist/rate limit before accepting. */
        uint8_t count = static_cast<uint8_t>(buf[4]);
        if (count == 0 || count > MAX_EVENT_TYPES_PER_CLIENT ||
            (size_t)n < 5 + count * sizeof(uint32_t))
            continue;

        /* Validate each event type from external input. Mandatory system/safety events are added only after
         * at least one client-requested type is valid, so empty/all-invalid REGR packets stay rejected. */
        uint32_t rawEventTypes[MAX_EVENT_TYPES_PER_CLIENT] = {};
        for (uint8_t i = 0; i < count; ++i)
        {
            uint32_t val;
            std::memcpy(&val, buf.data() + 5 + i * sizeof(uint32_t), sizeof(uint32_t));
            rawEventTypes[i] = ntohl(val);
        }
        std::unordered_set<EventType> types;
        if (!NvPSDGatewayBuildRegisteredEventSet(rawEventTypes, count, &types))
        {
            NvPSBWriteData(NVPSB_LOG_WARNING, "PSD-Gateway: ignoring REGR with no valid event types", "");
            continue;
        }

        const uint64_t peerKey = clientKey(&sender);
        {
            std::lock_guard<std::mutex> lock(clientTableMutex);
            int slot = -1;
            auto it = g_addrToSlot.find(peerKey);
            if (it != g_addrToSlot.end() && clientTable[it->second].inUse)
                slot = static_cast<int>(it->second);
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
                const bool existingSameClient =
                    clientTable[slot].inUse &&
                    clientKey(&clientTable[slot].addr) == peerKey;
                if (clientTable[slot].inUse)
                    g_addrToSlot.erase(clientKey(&clientTable[slot].addr));
                clientTable[slot].addr = sender;
                clientTable[slot].addrLen = slen;
                clientTable[slot].eventTypes = std::move(types);
                clientTable[slot].inUse = true;
                clientTable[slot].lastAckedSeq = HB_SEQ_NONE;  /* first HB will be counted as missed until ACK */
                clientTable[slot].missedCount = 0;
                if (!existingSameClient)
                {
                    clientTable[slot].gatewayTxEpoch = allocateGatewayClientEpoch();
                    clientTable[slot].nextGatewayTxSeq = NVPSD_GATEWAY_DECISION_TX_SEQ_MIN;
                }
                g_addrToSlot[peerKey] = static_cast<unsigned int>(slot);
                NvPSBWriteData(NVPSB_LOG_INFO,
                               "PSD-Gateway: registered client " + std::to_string(slot) +
                                   " (" + std::to_string(clientTable[slot].eventTypes.size()) + " event types)", "");
            }
            else
            {
                NvPSBWriteData(NVPSB_LOG_WARNING,
                               "PSD-Gateway: registration rejected (max clients reached)", "");
            }
        }
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

        ScopedFd scoped(dupGatewaySocketFd());
        if (scoped.fd < 0)
        {
            {
                std::lock_guard<std::mutex> g(gatewaySocketMutex);
                if (gatewayUdpSock < 0)
                    break;
            }
            NvPSBWriteData(NVPSB_LOG_WARNING, "PSD-Gateway: dup(gatewayUdpSock) failed (HB)", "");
            continue;
        }

        /* Start at 1 so first HB is always ahead of new clients' lastAckedSeq (HB_SEQ_NONE); first miss is counted. */
        uint32_t seq = hbSeqNo.fetch_add(1);
        char pkt[NVPSD_GATEWAY_HB_MSG_SIZE];
        std::memcpy(pkt, NVPSD_GATEWAY_HB_MAGIC_GATEWAY, 4);
        uint32_t netSeq = htonl(seq);
        std::memcpy(pkt + 4, &netSeq, 4);

        std::vector<std::pair<struct sockaddr_in, socklen_t>> addrs;
        {
            std::lock_guard<std::mutex> lock(clientTableMutex);
            for (unsigned int c = 0; c < g_maxClients; ++c)
            {
                if (!clientTable[c].inUse)
                    continue;
                addrs.push_back({ clientTable[c].addr, clientTable[c].addrLen });
            }
        }
        for (const auto& a : addrs)
        {
            ssize_t ss = -1;
            for (int eintrAttempt = 0; eintrAttempt < SENDTO_EINTR_MAX_RETRIES; ++eintrAttempt)
            {
                ss = sendto(scoped.fd, pkt, sizeof(pkt), 0,
                              reinterpret_cast<const struct sockaddr*>(&a.first), a.second);
                if (ss >= 0)
                    break;
                if (errno != EINTR)
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: HB send failed", "");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(SENDTO_EINTR_RETRY_SLEEP_MS));
            }
            if (ss < 0 && errno == EINTR)
                NvPSBWriteData(NVPSB_LOG_ERR,
                               "PSD-Gateway: HB send failed (EINTR retries exhausted)", "");
        }

        /* Wait for ACKs; add grace to avoid false misses during transient send/recv delays. */
        int waitMs = (HB_ACK_TIMEOUT_MS > HB_SEND_INTERVAL_MS)
            ? (HB_ACK_TIMEOUT_MS - HB_SEND_INTERVAL_MS + HB_ACK_GRACE_MS)
            : (HB_FALLBACK_SLEEP_MS + HB_ACK_GRACE_MS);
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));

        if (stopGateway.load())
            break;

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
                    const uint64_t bkey = clientKey(&clientTable[c].addr);
                    g_addrToSlot.erase(bkey);
                    clientTable[c].inUse = false;
                    clientTable[c].gatewayTxEpoch = 0U;
                    clientTable[c].nextGatewayTxSeq = NVPSD_GATEWAY_DECISION_TX_SEQ_MIN;
                    NvPSBWriteData(NVPSB_LOG_ERR,
                                   "PSD-Gateway: client " + std::to_string(c) +
                                       " HB failure miss=" + std::to_string(m) + "/" + std::to_string(maxF) +
                                       " seq=" + std::to_string(seq) +
                                       " – slot reclaimed; SDM may REGR again",
                                   "");
                }
                else
                {
                    std::string tier;
                    if (mu >= 1U && mu <= warnW)
                        tier = " [warn tier]";
                    else if (mu > warnW && mu < maxF)
                        tier = " [degraded: DecisionRequest forwarding stopped]";
                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                   "PSD-Gateway: no HB ACK from client " + std::to_string(c) +
                                       " seq=" + std::to_string(seq) +
                                       " miss=" + std::to_string(m) + "/" + std::to_string(maxF) + tier,
                                   "");
                }
            }
        }
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
        sendToRelevantClients(*request, false);
        response->action = NO_ACTION_REQUIRED;
        return NVPSD_SUCCESS;
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
int launchPSDControl(const std::string& sdmIP, unsigned int sdmPort, unsigned int numClients)
{
    if (numClients == 0 || numClients > MAX_DECISION_MAKER_CLIENTS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: numClients must be 1.." +
                        std::to_string(MAX_DECISION_MAKER_CLIENTS), "");
        return -1;
    }
    if (sdmPort == 0 || sdmPort > 65535)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: sdmPort must be 1..65535", "");
        return -1;
    }
    g_maxClients = numClients;

    gatewayClientEpochSeed.store(makeGatewayEpochSeed(), std::memory_order_relaxed);
    for (unsigned int i = 0; i < MAX_DECISION_MAKER_CLIENTS; ++i)
    {
        clientTable[i].inUse = false;
        clientTable[i].gatewayTxEpoch = 0U;
        clientTable[i].nextGatewayTxSeq = NVPSD_GATEWAY_DECISION_TX_SEQ_MIN;
    }
    g_addrToSlot.clear();

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: socket() failed", "");
        return -1;
    }

    struct sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port   = htons(static_cast<uint16_t>(sdmPort));
    if (inet_pton(AF_INET, sdmIP.c_str(), &bindAddr.sin_addr) <= 0)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: invalid bind address", "");
        close(sock);
        return -1;
    }
    if (bind(sock, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: bind failed", "");
        close(sock);
        return -1;
    }

    /* Timeout so registration thread can check stopGateway periodically */
    struct timeval tv = {};
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "PSD-Gateway: setsockopt(SO_RCVTIMEO) failed", "");
        close(sock);
        return -1;
    }

    {
        std::lock_guard<std::mutex> sockLock(gatewaySocketMutex);
        gatewayUdpSock = sock;
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

    /* Socket is closed in main() after NvPSDExit() so callback never uses it after close. */
    return 0;
}

void shutdownPSDControl()
{
    stopGateway.store(true);
}

/* Call after reg/HB threads are joined so no thread is in recvfrom/sendto. */
void closePSDControlSocket()
{
    std::lock_guard<std::mutex> sockLock(gatewaySocketMutex);
    if (gatewayUdpSock >= 0)
    {
        close(gatewayUdpSock);
        gatewayUdpSock = -1;
    }
}

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [--sdm_ip IP] [--sdm_port PORT] [--num_clients N] [--max_hb_failures N]\n\n"
              << "PSD Gateway — bridges NvPSS daemon events to UDP decision-maker clients.\n\n"
              << "Options:\n"
              << "  --sdm_ip <IP>           SDM listen IP address (default: 127.0.0.1).\n"
              << "  --sdm_port <PORT>       SDM listen port, 1-65535 (default: 50000).\n"
              << "  --num_clients <N>       Max decision-maker clients, 1-" << MAX_DECISION_MAKER_CLIENTS
              << " (default: " << MAX_DECISION_MAKER_CLIENTS << ").\n"
              << "  --max_hb_failures <N>   Heartbeat miss limit, 1-255 (default: 10).\n"
              << "  -h, --help              Show this help message.\n";
}

/* Main entry point */
int main(int argc, char* argv[])
{
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const char* prog = (argc > 0 && argv[0] != nullptr) ? argv[0] : "nvpsd_gateway";
    std::string  sdmIP   = "127.0.0.1";
    unsigned int sdmPort = 50000;
    unsigned int numClients = MAX_DECISION_MAKER_CLIENTS;

    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            printUsage(prog);
            return 0;
        }
        else if (std::strcmp(argv[i], "--sdm_ip") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --sdm_ip requires a value\n";
                printUsage(prog);
                return 1;
            }
            sdmIP = argv[++i];
        }
        else if (std::strcmp(argv[i], "--sdm_port") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --sdm_port requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            long p = std::strtol(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || p <= 0 || p > 65535)
            {
                std::cerr << "error: --sdm_port: invalid number (use 1..65535)\n";
                printUsage(prog);
                return 1;
            }
            sdmPort = static_cast<unsigned int>(p);
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

    /* Single NvPSD context: one connection to NvPSS daemon via msgq (receives all events). */
    NvPSDCtx* ctx = NvPSDCreateContext();
    if (!ctx)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to create NvPSD Control context", "");
        return 1;
    }

    NvPSDErr err = NvPSDInitialize(ctx,
                                   "/client_to_pss", "/pss_to_client",
                                   "/client_to_pss_critical", "/pss_to_client_critical",
                                   NVPSD_CLIENT);
    if (err != NVPSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to initialise NvPSD Control", "");
        NvPSDDestroyContext(ctx);
        return 1;
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
        NvPSDExit(ctx);
        NvPSDDestroyContext(ctx);
        return 1;
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD Gateway: daemon (msgq) + UDP decision-maker clients + HB", "");

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

    if (launchPSDControl(sdmIP, sdmPort, numClients) != 0)
    {
        cleanupGatewayPssClient();
        NvPSDExit(ctx);
        closePSDControlSocket();
        NvPSDDestroyContext(ctx);
        return 1;
    }

    cleanupGatewayPssClient();

    NvPSBWriteData(NVPSB_LOG_INFO, "Exit NvPSD Control", "");
    NvPSDExit(ctx);
    closePSDControlSocket();
    NvPSDDestroyContext(ctx);
    return 0;
}
