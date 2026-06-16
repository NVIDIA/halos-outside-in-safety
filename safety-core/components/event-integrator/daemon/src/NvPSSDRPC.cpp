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

#if defined(MSG_NOSIGNAL)
constexpr int kPssStreamSendFlags = MSG_NOSIGNAL;
#else
constexpr int kPssStreamSendFlags = 0;
#endif

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
        case CLIENT_MDX:            return "MDXClient";
        case CLIENT_SAFETY_MONITOR: return "SafetyAIMonitor";
        case CLIENT_PSD_GATEWAY:    return "PSDGateway";
        default:                    return "UnregisteredClient";
    }
}

/**
 * Max wall-clock time to assemble one full request struct so a slow/stalled peer cannot block the RPC thread forever.
 */
constexpr int kRpcRecvAllDeadlineMs = 500;

constexpr int kRpcSendAllDeadlineMs = 500;

static bool sendAll(int fd, const void* buf, size_t len, int flags)
{
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kRpcSendAllDeadlineMs);

    while (remaining > 0)
    {
        const ssize_t n = send(fd, p, remaining, flags);
        if (n > 0)
        {
            p += n;
            remaining -= static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
            return false;
        const int e = errno;
        if (e == EINTR)
            continue;
        if (e == EAGAIN || e == EWOULDBLOCK)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            int waitMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (waitMs < 1)
                waitMs = 1;

            struct pollfd pfd = {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int pr = poll(&pfd, 1, waitMs);
            if (pr < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (pr == 0)
                return false;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                return false;
            continue;
        }
        return false;
    }
    return true;
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

void sendReportSafetyEventResponse(int sd, uint32_t reqSeqNo, uint8_t reportStatus, unsigned int client)
{
    NvPSSDRPCMsgResp resp = {};
    resp.respSeqNo = reqSeqNo;
    resp.size = 1;
    memset(resp.respPayload, 0, sizeof(resp.respPayload));
    resp.respPayload[0] = reportStatus;
    if (!sendAll(sd, &resp, sizeof(NvPSSDRPCMsgResp), kPssStreamSendFlags))
        logPssSendErrToClient(client);
}

/**
 * Recv until len bytes, EOF (0), error (-1, errno set), or overall deadline exceeded (errno ETIMEDOUT).
 * Uses poll() between recv calls so the RPC server thread cannot stall indefinitely on one client.
 */
static ssize_t recvAll(int fd, void* buf, size_t len)
{
    auto* p = static_cast<uint8_t*>(buf);
    size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRpcRecvAllDeadlineMs);

    while (off < len)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        int waitMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (waitMs < 1)
            waitMs = 1;

        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, waitMs);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr == 0)
        {
            errno = ETIMEDOUT;
            return -1;
        }
        if ((pfd.revents & POLLNVAL) != 0)
        {
            errno = EINVAL;
            return -1;
        }
        if ((pfd.revents & POLLERR) != 0)
        {
            errno = EIO;
            return -1;
        }

        const ssize_t n = recv(fd, p + off, len - off, 0);
        if (n > 0)
        {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
    return static_cast<ssize_t>(len);
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

void NvPSSDRPC::processPendingDisconnects(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                          std::mutex& inputSafetyEventQueMutex)
{
    std::deque<std::pair<uint32_t, uint32_t>> batch;
    {
        std::lock_guard<std::mutex> pd(pendingDisconnectMutex);
        batch.swap(pendingDisconnectClients);
        pendingDisconnectSeen.clear();
    }

    for (const auto& entry : batch)
    {
        const uint32_t slot = entry.first;
        const uint32_t enqueueGen = entry.second;
        if (slot >= maxClients)
            continue;
        if (slotGeneration_[slot].load(std::memory_order_relaxed) != enqueueGen)
            continue;
        const int sd = clientSockets[slot];
        if (sd < 0)
            continue;

        const uint8_t slotType = getClientType(slot);
        if (slotType != 0)
        {
            SafetyEvent ev = {};
            ev.type = SW_FAIL;
            ev.severity = CRITICAL;
            ev.timestamp = monotonicNowNs();
            ev.confidenceLevel = 1.0f;
            ev.processed = false;
            {
                int n = snprintf(ev.sensorIdentifier, sizeof(ev.sensorIdentifier), "PSS_RPC");
                if (n < 0 || static_cast<size_t>(n) >= sizeof(ev.sensorIdentifier))
                    ev.sensorIdentifier[sizeof(ev.sensorIdentifier) - 1] = '\0';
            }
            {
                int n = snprintf(ev.ruleIdentifier, sizeof(ev.ruleIdentifier),
                                 "heartbeat_fault_client_%u", static_cast<unsigned int>(slot));
                if (n < 0 || static_cast<size_t>(n) >= sizeof(ev.ruleIdentifier))
                    ev.ruleIdentifier[sizeof(ev.ruleIdentifier) - 1] = '\0';
            }
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
                }
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
    serverSocket = -1;
    std::fill(std::begin(clientSockets), std::end(clientSockets), -1);
    for (auto& g : slotGeneration_)
        g.store(0U, std::memory_order_relaxed);
}

NvPSSDRPC::~NvPSSDRPC(){}

NvPSSDErr NvPSSDRPC::NvPSSDInitRPCServer()
{
    NvPSSDErr err = NVPSSD_SUCCESS;
    struct sockaddr_un address = {};

    serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if(serverSocket == -1)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error in initializing socket for PSS", "errno: " + std::to_string(errno));
        err = NVPSSD_FAIL;
        goto exit;
    }
    if(serverSocket >= FD_SETSIZE)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Server socket fd exceeds FD_SETSIZE limit",
                       "fd: " + std::to_string(serverSocket) + ", FD_SETSIZE: " + std::to_string(FD_SETSIZE));
        close(serverSocket);
        serverSocket = -1;
        err = NVPSSD_FAIL;
        goto exit;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, channel.c_str(), sizeof(address.sun_path) - 1);

    unlink(channel.c_str());

    if(bind(serverSocket, (struct sockaddr *)&address, sizeof(address)) == -1)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error in binding socket for PSS", "errno: " + std::to_string(errno));
        err = NVPSSD_FAIL;
        goto exit;
    }

    if(listen(serverSocket, maxPendingClients) == -1)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error in listening on socket for PSS", "errno: " + std::to_string(errno));
        err = NVPSSD_FAIL;
        goto exit;
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon listening on", "channel: " + channel);

exit:
    if (err != NVPSSD_SUCCESS && serverSocket >= 0)
    {
        close(serverSocket);
        serverSocket = -1;
    }
    return err;
}

NvPSSDErr NvPSSDRPC::NvPSSDStartRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex, const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb,
                                void* trustReportCtx)
{
    runRpcServer.store(true);
    rpcServerThread = std::thread(&NvPSSDRPC::NvPSSDRunRPCServer, this, std::ref(inputSafetyEventQueRef),
                        std::ref(inputSafetyEventQueMutex), thresholdConfidence, std::ref(rpcServerTerminationCV),
                        trustReportCb, trustReportCtx);
    return NVPSSD_SUCCESS;
}


NvPSSDErr NvPSSDRPC::NvPSSDRunRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex, const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb,
                                void* trustReportCtx)
{
    int maxSd = 0,sd = 0;
    unsigned int client = 0;
    SafetyEvent reportedEvent = {};

    while (runRpcServer.load())
    {
        processPendingDisconnects(inputSafetyEventQueRef, inputSafetyEventQueMutex);

        fd_set readFds;
        FD_ZERO(&readFds);
        const bool serverSocketValid = (serverSocket >= 0 && serverSocket < FD_SETSIZE);
        if (serverSocketValid)
        {
            FD_SET(serverSocket, &readFds);
        }
        maxSd = serverSocketValid ? serverSocket : -1;

        for (client = 0; client < maxClients; client++)
        {
            sd = clientSockets[client];
            if (sd >= 0 && sd < FD_SETSIZE)
            {
                FD_SET(sd, &readFds);
                if (sd > maxSd) maxSd = sd;
            }
        }

        // Add timeout to prevent blocking
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms

        int activity = select(maxSd + 1, &readFds, NULL, NULL, &timeout);

        if (activity < 0) {
            if (errno == EINTR) {
                // Interrupted by signal - check shutdown flag
                NvPSBWriteData(NVPSB_LOG_INFO, "select() interrupted by signal", "");
                continue;
            }
            NvPSBWriteData(NVPSB_LOG_ERR, "select() error", "errno: " + std::to_string(errno));
            continue;
        }

        if (activity == 0) {
            // Timeout occurred - loop back to check runRpcServer flag
            continue;
        }

        if (serverSocketValid && FD_ISSET(serverSocket, &readFds))
        {
            int newSocket = accept(serverSocket, NULL, NULL);
            if (newSocket < 0)
            {
                NvPSBWriteData(NVPSB_LOG_ERR, "accept() failed", "errno: " + std::to_string(errno));
            }
            else if (newSocket >= FD_SETSIZE)
            {
                NvPSBWriteData(NVPSB_LOG_ERR, "accept() fd exceeds FD_SETSIZE, closing",
                               "fd: " + std::to_string(newSocket) + ", FD_SETSIZE: " + std::to_string(FD_SETSIZE));
                close(newSocket);
            }
            else
            {
                struct timeval rcvTo = {0, kPssClientSocketRecvTimeoutUs};
                if (setsockopt(newSocket, SOL_SOCKET, SO_RCVTIMEO, &rcvTo, sizeof(rcvTo)) != 0)
                {
                    NvPSBWriteData(NVPSB_LOG_WARNING, "PSS: SO_RCVTIMEO on new client socket failed",
                                   "errno: " + std::to_string(errno));
                }
                for (client = 0; client < maxClients; client++)
                {
                    if (clientSockets[client] < 0)
                    {
                        clearClientState(static_cast<uint32_t>(client));
                        clientSockets[client] = newSocket;
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
                    NvPSBWriteData(NVPSB_LOG_ERR, "accept() dropped: no free client slot", "");
                    close(newSocket);
                }
            }
        }

        /* Thread safety: clientSockets is accessed only by this thread (rpcServerThread).
         * NvPSSDCloseRPCServer joins this thread before closing sockets. */
        for (client = 0; client < maxClients; client++)
        {
            sd = clientSockets[client];
            if (sd >= 0 && sd < FD_SETSIZE && FD_ISSET(sd, &readFds))
            {
                NvPSSDRPCMsgReq req = {};
                NvPSSDRPCMsgResp resp = {};
                const ssize_t nread = recvAll(sd, &req, sizeof(NvPSSDRPCMsgReq));
                if (nread <= 0)
                {
                    if (nread < 0)
                    {
                        if (errno == ETIMEDOUT)
                        {
                            NvPSBWriteData(NVPSB_LOG_WARNING,
                                           "PSS Daemon: timed out assembling request from client (slow/stalled peer)",
                                           "client: " + std::to_string(client));
                        }
                        else
                        {
                            NvPSBWriteData(NVPSB_LOG_WARNING, "PSS Daemon: recv on client socket failed",
                                           "client: " + std::to_string(client) + ", errno: " + std::to_string(errno));
                        }
                    }
                    disconnectClient(static_cast<uint32_t>(client));
                    continue;
                }
                /* nread == sizeof(NvPSSDRPCMsgReq): recvAll only returns full read, EOF, or error. */
                if (req.msg != SEND_HEARTBEAT)
                {
                    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon has received message",
                                  "Msg: " + std::to_string(req.msg) + ", SeqNo: " + std::to_string(req.reqSeqNo));
                }

                uint8_t reportStatus = REPORT_ACCEPTED;
                switch(req.msg)
                {
                        case REGISTER_CLIENT:
                            resp.respSeqNo = 0;
                            resp.size = 5; /* 4 bytes client id + 1 byte status */
                            memset(resp.respPayload, 0, sizeof(resp.respPayload));
                            {
                                const uint32_t gen = slotGeneration_[client].load(std::memory_order_relaxed);
                                const uint32_t encodedId = (gen << 8) | static_cast<uint32_t>(client);
                                const uint32_t wireId = htonl(encodedId);
                                memcpy(resp.respPayload, &wireId, sizeof(wireId));
                                const uint8_t regStatus = validateAndAcceptRegistration(
                                    static_cast<uint32_t>(client), req);
                                resp.respPayload[4] = regStatus;
                            }
                            if (!sendAll(sd, &resp, sizeof(NvPSSDRPCMsgResp), kPssStreamSendFlags))
                                logPssSendErrToClient(client);
                            if (resp.respPayload[4] != REGISTER_ACCEPTED)
                                disconnectClient(static_cast<uint32_t>(client));
                            break;

                        case REPORT_SAFETY_EVENT:
                            {
                                reportStatus = REPORT_ACCEPTED;
                                if (getClientType(static_cast<uint32_t>(client)) == 0)
                                {
                                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                                   "Dropping REPORT_SAFETY_EVENT: client not registered",
                                                   "client: " + std::to_string(client));
                                    reportStatus = REPORT_REJECTED_UNAUTHORIZED;
                                    sendReportSafetyEventResponse(sd, req.reqSeqNo, reportStatus, client);
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
                                    if (static_cast<size_t>(req.size) < sizeof(SafetyEvent))
                                    {
                                        NvPSBWriteData(NVPSB_LOG_WARNING, "Dropping REPORT_SAFETY_EVENT: payload size too small",
                                                      "size: " + std::to_string(req.size) + ", need: " + std::to_string(sizeof(SafetyEvent)));
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
                                                  ", Severity: " + std::to_string(reportedEvent.severity) +
                                                  ", Confidence: " + std::to_string(reportedEvent.confidenceLevel));
                                    }

                                    if (reportStatus == REPORT_ACCEPTED)
                                    {

                                    /* Only Safety Monitor may send the trust-report event types (SENSOR_/AI_PIPELINE_ VALID/INVALID) */
                                    const bool isTrustReportType = (reportedEvent.type == SENSOR_INVALID || reportedEvent.type == SENSOR_VALID ||
                                                                   reportedEvent.type == AI_PIPELINE_INVALID || reportedEvent.type == AI_PIPELINE_VALID);
                                    if (isTrustReportType && (reporterClientType != CLIENT_SAFETY_MONITOR || !hasRecentHeartbeat(clientId)))
                                    {
                                        NvPSBWriteData(NVPSB_LOG_WARNING, "Dropping trust report: only Safety Monitor may send SENSOR_/AI_PIPELINE_ VALID/INVALID",
                                                      "client: " + std::to_string(client) + ", reporterType: " + std::to_string(reporterClientType));
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
                                sendReportSafetyEventResponse(sd, req.reqSeqNo, reportStatus, client);
                            }
                            break;

                        case TERMINATE_CLIENT:
                            resp.respSeqNo = req.reqSeqNo;
                            resp.size = 1;
                            memset(resp.respPayload, 0, sizeof(resp.respPayload));
                            resp.respPayload[0] = TERMINATE_ACCEPTED;
                            if (!sendAll(sd, &resp, sizeof(NvPSSDRPCMsgResp), kPssStreamSendFlags))
                                logPssSendErrToClient(client);
                            disconnectClient(static_cast<uint32_t>(client));
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
                                    disconnectClient(static_cast<uint32_t>(client));
                                    break;
                                }
                                updateClientHeartbeat(static_cast<uint32_t>(client), registeredType);
                                {
                                    NvPSSDRPCMsgResp hbResp = {};
                                    hbResp.respSeqNo = req.reqSeqNo;
                                    hbResp.size = 1;
                                    hbResp.respPayload[0] = HEARTBEAT_ACK;
                                    if (!sendAll(sd, &hbResp, sizeof(NvPSSDRPCMsgResp), kPssStreamSendFlags))
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

uint8_t NvPSSDRPC::validateAndAcceptRegistration(uint32_t clientSlot, const NvPSSDRPCMsgReq& req)
{
    if (req.size < 1U)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "REGISTER_CLIENT: no clientType in payload", "");
        return REGISTER_REJECTED_INVALID_TYPE;
    }

    const uint8_t reqClientType = req.reqPayload[0];
    if (reqClientType != CLIENT_MDX &&
        reqClientType != CLIENT_SAFETY_MONITOR &&
        reqClientType != CLIENT_PSD_GATEWAY)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
                       "REGISTER_CLIENT: invalid clientType",
                       "type: " + std::to_string(reqClientType));
        return REGISTER_REJECTED_INVALID_TYPE;
    }

    const bool isSingletonType = (reqClientType == CLIENT_SAFETY_MONITOR ||
                                  reqClientType == CLIENT_PSD_GATEWAY);
    std::lock_guard<std::mutex> hbLock(heartbeatMutex);
    if (isSingletonType)
    {
        uint32_t staleSlot = UINT32_MAX;
        for (const auto& kv : clientTypeMap)
        {
            if (kv.second != reqClientType ||
                kv.first == clientSlot ||
                kv.first >= MAX_POSSIBLE_CLIENTS ||
                clientSockets[kv.first] < 0)
                continue;

            {
                const auto faultIt = hbFaultLatched.find(kv.first);
                const bool isLatched = (faultIt != hbFaultLatched.end() && faultIt->second);
                if (!isLatched)
                {
                    NvPSBWriteData(NVPSB_LOG_WARNING,
                                   "REGISTER_CLIENT: duplicate singleton client type rejected",
                                   "type: " + std::to_string(reqClientType));
                    return REGISTER_REJECTED_DUPLICATE_TYPE;
                }
            }
            staleSlot = kv.first;
        }
        if (staleSlot != UINT32_MAX)
        {
            disconnectClientLocked(staleSlot);
            NvPSBWriteData(NVPSB_LOG_WARNING,
                           "REGISTER_CLIENT: evicted fault-latched stale singleton",
                           "type: " + std::to_string(reqClientType));
        }
    }

    clientTypeMap[clientSlot] = reqClientType;

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
    return REGISTER_ACCEPTED;
}

void NvPSSDRPC::clearClientStateLocked(uint32_t clientId)
{
    const auto ctypeIt = clientTypeMap.find(clientId);
    const bool wasSm = (ctypeIt != clientTypeMap.end() && ctypeIt->second == CLIENT_SAFETY_MONITOR);

    clientLastHeartbeat.erase(clientId);
    clientHeartbeatCount.erase(clientId);
    clientTypeMap.erase(clientId);
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
}

void NvPSSDRPC::clearClientState(uint32_t clientId)
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    clearClientStateLocked(clientId);
}

void NvPSSDRPC::disconnectClient(uint32_t slot)
{
    std::lock_guard<std::mutex> lock(heartbeatMutex);
    disconnectClientLocked(slot);
}

void NvPSSDRPC::disconnectClientLocked(uint32_t slot)
{
    if (slot < MAX_POSSIBLE_CLIENTS && clientSockets[slot] >= 0)
        close(clientSockets[slot]);
    if (slot < MAX_POSSIBLE_CLIENTS)
        clientSockets[slot] = -1;
    clearClientStateLocked(slot);
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
        if (clientSockets[i] >= 0)
        {
            close(clientSockets[i]);
            clientSockets[i] = -1;
        }
    }

    // Close server socket
    if (serverSocket >= 0) {
        close(serverSocket);
        serverSocket = -1;
    }

    // Remove socket file
    unlink(channel.c_str());

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
                (clientType == CLIENT_SAFETY_MONITOR) ? "SafetyMonitor" : "PSDGateway";
            NvPSBWriteData(NVPSB_LOG_INFO,
                "Connection alive: " + clientName + " - " +
                std::to_string(clientHeartbeatCount[clientId]) + " heartbeats received", "");
        }
#endif
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

}
