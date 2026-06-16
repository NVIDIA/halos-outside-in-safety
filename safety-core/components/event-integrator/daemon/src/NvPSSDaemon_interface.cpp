/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <errno.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <poll.h>

#include "pss_daemon.h"
#include "NvPSSDRPCMsg.h"
#include "NvPSB.h"
#include "pss_message_validate.h"

#define SOCKET_VALIDITY_CHECK_TIMEOUT_US 1000

#define RPC_RECV_DEADLINE_MS   500

/**
 * Message counter
 * Track all the message being sent
 * form the invocation of this library instance
 */
static std::atomic<uint32_t> msgCounter{0};

/**
 * Protects clientIdToSocketMap and clientIdToStreamIoMutexMap.
 * msgCounter is atomic to avoid contending with high-frequency heartbeat/report sends.
 */
static std::mutex g_pssRpcSocketMapMutex;

/**
 * Per registered client: serializes all I/O on that client's single UNIX stream socket.
 * dup() does not isolate recv(); concurrent recv() on the same stream still steals bytes.
 */
static std::unordered_map<uint32_t, std::shared_ptr<std::mutex>> clientIdToStreamIoMutexMap;

#if defined(MSG_NOSIGNAL)
constexpr int kClientStreamSendFlags = MSG_NOSIGNAL;
#else
constexpr int kClientStreamSendFlags = 0;
#endif

/**
 * Send exactly len bytes, looping on partial writes.
 * Uses poll(POLLOUT) with a deadline so send-side backpressure cannot block the caller forever.
 * Returns true on success, false on error.
 */
static bool sendAll(int fd, const void* buf, size_t len, int flags, int deadlineMs)
{
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);

    while (off < len)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            errno = ETIMEDOUT;
            return false;
        }
        int waitMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (waitMs < 1)
            waitMs = 1;

        const ssize_t n = send(fd, p + off, len - off, flags | MSG_DONTWAIT);
        if (n > 0)
        {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
        {
            errno = EIO;
            return false;
        }
        const int e = errno;
        if (e == EINTR)
            continue;
        if (e == EAGAIN || e == EWOULDBLOCK)
        {
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
            {
                errno = ETIMEDOUT;
                return false;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                errno = (pfd.revents & POLLNVAL) ? EINVAL : EIO;
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

/**
 * Recv exactly len bytes, looping on partial reads.
 * Uses poll() with a deadline so a stalled daemon cannot block the caller forever.
 * Returns: len on success, 0 on peer close, -1 on error (errno set; ETIMEDOUT on deadline).
 */
static ssize_t recvAll(int fd, void* buf, size_t len, int deadlineMs)
{
    auto* p = static_cast<uint8_t*>(buf);
    size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);

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
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            errno = (pfd.revents & POLLNVAL) ? EINVAL : EIO;
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
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;
    }
    return static_cast<ssize_t>(len);
}

/**
 * Backend for rpc communication with PSS daemon
 * Only unix socket backend is supported for now
 * NvSciIpc based backend support to follow
 */
const NvPSSDRPCBackend rpcBackend = SOCKET;

/**
 * Socket path
 * UNIX domain socket path on which the
 * PSS daemon listens for client connections
 */
/* Must match the server path in NvPSSDaemon.cpp. Keep these two strings in sync. */
const std::string socketPath = "/run/nvpsf/nvpssd";

/**
 * Client to socket mapping
 * Mapping from client to its corresponding socket
 * Maintained as stdlib unordered_map
 */
std::unordered_map<uint32_t,int>clientIdToSocketMap;

/**
 * Forward declarations of internal functions
 */
NvPSSDErr NvPSSRegisterPSSClient_Socket(uint32_t* clientId, uint8_t clientType);
NvPSSDErr NvPSSTerminatePSSClient_Socket(const uint32_t clientId);
NvPSSDErr NvPSSReportSafetyEvent_Socket(const uint32_t clientId, const SafetyEvent* event);

/**
 * Register PSS Clinet
 *
 * Registration of PSS client with PSS daemon involves three steps
 * 1. Connect to PSS daemon over specifid UNIX domain socket
 * 2. Send message REGISTER_CLIENT and wait for client ID from PSS daemon.
 * This client ID is used as reference as in subsequent communication with
 * PSS daemon
 * 3. Internally, maintain a map of client id returned by PSS daemon and the
 * corresponding socket. While performing subsequent communication with PSS
 * daemon, extract the socket id for given client id and then continue
 *
 */
NvPSSDErr NvPSSRegisterPSSClient(uint32_t* clientId, uint8_t clientType)
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    switch(rpcBackend)
    {
        case SOCKET:
          err = NvPSSRegisterPSSClient_Socket(clientId, clientType);
          break;

        default:
            NvPSBWriteData(NVPSB_LOG_INFO, "Only Socket based communication with PSS daemon is supported", "");
            err = NVPSSD_FAIL;
            break;
    }

    return err;
}

NvPSSDErr NvPSSRegisterPSSClient_Socket(uint32_t* clientId, uint8_t clientType)
{
    NvPSSDErr err = NVPSSD_SUCCESS;
    if (clientId == nullptr)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "NvPSSRegisterPSSClient_Socket: clientId output parameter is null", "");
        return NVPSSD_FAIL;
    }
    if (clientType != CLIENT_MDX &&
        clientType != CLIENT_SAFETY_MONITOR &&
        clientType != CLIENT_PSD_GATEWAY)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
                       "NvPSSRegisterPSSClient_Socket: invalid clientType",
                       "clientType: " + std::to_string(clientType));
        return NVPSSD_FAIL;
    }
    struct sockaddr_un addr = {};
    NvPSSDRPCMsgResp msgResp = {};
    NvPSSDRPCMsgReq msgReq = {};
    int clientSocket = -1;
    int previousSocket = -1;
    ssize_t recvResult = -1;

    clientSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if(clientSocket == -1)
    {
        #ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to create socket", "errno: " + std::to_string(errno));
        #endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    if(connect(clientSocket, (struct sockaddr*)&addr, sizeof(addr))!=0)
    {

#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "Connection to PSS failed while registering new client", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "A new client is connected to PSS daemon", "");
#endif

    msgReq.msg = REGISTER_CLIENT;
    msgReq.reqSeqNo = msgCounter.fetch_add(1U, std::memory_order_relaxed);
    msgReq.size = 1;
    memset(&msgReq.reqPayload, 0, sizeof(msgReq.reqPayload));
    msgReq.reqPayload[0] = clientType;
    if (!sendAll(clientSocket, &msgReq, sizeof(msgReq), kClientStreamSendFlags, RPC_RECV_DEADLINE_MS))
    {
        const int savedErr = errno;
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "Error in sending REGISTER_CLIENT msg to PSS Daemon: " + std::to_string(savedErr), "");
#endif
        (void)savedErr;
        err = NVPSSD_FAIL;
        goto exit;
    }

    recvResult = recvAll(clientSocket, &msgResp, sizeof(NvPSSDRPCMsgResp), RPC_RECV_DEADLINE_MS);
    if(recvResult != static_cast<ssize_t>(sizeof(NvPSSDRPCMsgResp)))
    {
#ifdef NVPSF_DBG
        if (recvResult == 0)
            NvPSBWriteData(NVPSB_LOG_INFO, "Connection closed by PSS daemon during registration", "");
        else
            NvPSBWriteData(NVPSB_LOG_INFO, "Error receiving registration response from PSS daemon: errno=" + std::to_string(errno), "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    if (msgResp.size < 5U)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "PSS registration response too short",
            "size: " + std::to_string(msgResp.size));
        err = NVPSSD_FAIL;
        goto exit;
    }
    {
        const uint8_t regStatus = msgResp.respPayload[4];
        if (regStatus != REGISTER_ACCEPTED)
        {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "PSS registration rejected",
                "clientType: " + std::to_string(clientType) +
                    ", status: " + std::to_string(regStatus));
            err = NVPSSD_FAIL;
            goto exit;
        }
        uint32_t wireId = 0;
        memcpy(&wireId, msgResp.respPayload, sizeof(wireId));
        *clientId = ntohl(wireId);
    }

    /* Replace map entry and close any previous fd under the per-client stream mutex so no send/recv races close(). */
    {
        std::shared_ptr<std::mutex> streamMux;
        {
            std::lock_guard<std::mutex> mapLock(g_pssRpcSocketMapMutex);
            if (clientIdToStreamIoMutexMap.find(*clientId) == clientIdToStreamIoMutexMap.end())
                clientIdToStreamIoMutexMap[*clientId] = std::make_shared<std::mutex>();
            streamMux = clientIdToStreamIoMutexMap[*clientId];
        }
        std::lock_guard<std::mutex> streamIoLock(*streamMux);
        {
            std::lock_guard<std::mutex> mapLock(g_pssRpcSocketMapMutex);
            const auto it = clientIdToSocketMap.find(*clientId);
            if (it != clientIdToSocketMap.end())
                previousSocket = it->second;
            clientIdToSocketMap[*clientId] = clientSocket;
        }
        if (previousSocket >= 0 && previousSocket != clientSocket)
            close(previousSocket);
    }

exit:
    if (err != NVPSSD_SUCCESS && clientSocket >= 0)
    {
        close(clientSocket);
        clientSocket = -1;
    }
    return err;
}

/**
 * Terminate PSS clinet
 *
 * It is 6 step process as follows
 * 1. Extract socket id from the provided client id
 * 2. Check whether the socket is in valid, writable state
 * 3. Send TERMINATE_CLIENT message to PSS daemon
 * 4. Wait for the response or till timeout occurs
 * 5. Check the response packet to inspect what PSS daemon
 *    has to say about this
 * 6. If it is okay, close the socket and erase the entry from the map
 */
NvPSSDErr NvPSSTerminatePSSClient(const uint32_t clientId)
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    switch(rpcBackend)
    {
        case SOCKET:
          err = NvPSSTerminatePSSClient_Socket(clientId);
          break;

        default:
            NvPSBWriteData(NVPSB_LOG_INFO, "Only Socket based communication with PSS daemon is supported", "");
            err = NVPSSD_FAIL;
            break;
    }

    return err;
}

/*------------------------------------------------------------------------------------------------*/

NvPSSDErr NvPSSTerminatePSSClient_Socket(const uint32_t clientId)
{
    NvPSSDErr err = NVPSSD_SUCCESS;
    int streamFd = -1;
    int storedSocket = -1;
    std::shared_ptr<std::mutex> streamIoMutex;
    fd_set writeFds;
    FD_ZERO(&writeFds);
    struct timeval socketValidityCheckTimeout = {0,SOCKET_VALIDITY_CHECK_TIMEOUT_US};
    int ready = 0;
    NvPSSDRPCMsgReq msgReq = {};
    NvPSSDRPCMsgResp msgResp = {};

    bool foundClient = false;
    {
        std::lock_guard<std::mutex> lock(g_pssRpcSocketMapMutex);
        const auto it = clientIdToSocketMap.find(clientId);
        if (it != clientIdToSocketMap.end())
        {
            storedSocket = it->second;
            foundClient = true;
            if (clientIdToStreamIoMutexMap.find(clientId) == clientIdToStreamIoMutexMap.end())
                clientIdToStreamIoMutexMap[clientId] = std::make_shared<std::mutex>();
            streamIoMutex = clientIdToStreamIoMutexMap[clientId];
        }
    }
    if (!foundClient)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "Client Id is invalid", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    if (storedSocket < 0)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid socket descriptor for client", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    {
        std::lock_guard<std::mutex> streamIoLock(*streamIoMutex);
        streamFd = storedSocket;

        if (!(streamFd >= 0 && streamFd < FD_SETSIZE))
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR, "Invalid socket fd for select (must be in [0, FD_SETSIZE))",
                           "fd: " + std::to_string(streamFd));
#endif
            err = NVPSSD_FAIL;
            goto exit;
        }
        FD_SET(streamFd, &writeFds);
        ready = select(streamFd + 1, NULL, &writeFds, NULL, &socketValidityCheckTimeout);
        if (ready == -1) {
            NvPSBWriteData(NVPSB_LOG_ERR, "select() failed", "");
            err = NVPSSD_FAIL;
            goto exit;
        }
        if (ready > 0 && FD_ISSET(streamFd, &writeFds))
        {
            const uint32_t seqNo = msgCounter.fetch_add(1U, std::memory_order_relaxed);
            msgReq.msg = TERMINATE_CLIENT;
            msgReq.reqSeqNo = seqNo;
            msgReq.size = 0;
            memset(&msgReq.reqPayload, 0,sizeof(msgReq.reqPayload));
            if (!sendAll(streamFd, &msgReq, sizeof(msgReq), kClientStreamSendFlags, RPC_RECV_DEADLINE_MS))
            {
                const int savedErr = errno;
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO, "Error in sending TERMINATE_CLIENT msg to PSS Daemon: " + std::to_string(savedErr), "");
#endif
                (void)savedErr;
                err = NVPSSD_FAIL;
                goto exit;
            }
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "Socket for Client Id " + std::to_string(clientId) + " is invalid", "");
#endif
            err = NVPSSD_FAIL;
            goto exit;
        }

        {
            const ssize_t recvResult = recvAll(streamFd, &msgResp, sizeof(NvPSSDRPCMsgResp), RPC_RECV_DEADLINE_MS);
            if (recvResult != static_cast<ssize_t>(sizeof(NvPSSDRPCMsgResp)))
            {
#ifdef NVPSF_DBG
                if (recvResult == 0)
                    NvPSBWriteData(NVPSB_LOG_INFO, "Connection closed by peer while awaiting terminate response", "");
                else
                    NvPSBWriteData(NVPSB_LOG_INFO, "Error receiving terminate response from PSS daemon: errno=" + std::to_string(errno), "");
#endif
                err = NVPSSD_FAIL;
                goto exit;
            }
            if (msgResp.respSeqNo != msgReq.reqSeqNo || msgResp.size != 1)
            {
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO, "Terminate response mismatch: expected seq=" + std::to_string(msgReq.reqSeqNo) +
                    " got seq=" + std::to_string(msgResp.respSeqNo) + " size=" + std::to_string(msgResp.size), "");
#endif
                err = NVPSSD_FAIL;
                goto exit;
            }
        }
        if((uint32_t)msgResp.respPayload[0] == TERMINATE_ACCEPTED)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "termination request for client id: " + std::to_string(clientId) + " successful", "");
#endif
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "Termination request for client id: " + std::to_string(clientId) + " failed", "");
#endif
            err = NVPSSD_FAIL;
            goto exit;
        }

        /* streamIoLock still held: no concurrent send/recv on this fd. Tear down maps and close under g_pssRpcSocketMapMutex. */
        {
            std::lock_guard<std::mutex> lock(g_pssRpcSocketMapMutex);
            const auto it = clientIdToSocketMap.find(clientId);
            if (it != clientIdToSocketMap.end())
            {
                const int s = it->second;
                clientIdToSocketMap.erase(clientId);
                clientIdToStreamIoMutexMap.erase(clientId);
                close(s);
            }
        }
    }

exit:
    if (err != NVPSSD_SUCCESS && streamIoMutex)
    {
        std::lock_guard<std::mutex> streamIoLock(*streamIoMutex);
        std::lock_guard<std::mutex> mapLock(g_pssRpcSocketMapMutex);
        const auto it = clientIdToSocketMap.find(clientId);
        if (it != clientIdToSocketMap.end())
        {
            const int fd = it->second;
            clientIdToSocketMap.erase(it);
            clientIdToStreamIoMutexMap.erase(clientId);
            if (fd >= 0)
                close(fd);
        }
    }
    return err;
}

/**
 * Report safety event
 *
 * It is 5 step process as follows
 *
 * 1. Extract socket id from the provided client id
 * 2. Check whether the socket is in valid, writable state
 * 3. Send REPORT_SAFETY_EVENT message to PSS daemon
 * 4. Wait for the response or till timeout occurs
 * 5. Check the response packet to inspect what PSS daemon
 *    has to say about this and return appropriate error code to caller
 */
NvPSSDErr NvPSSReportSafetyEvent(const uint32_t clientId, const SafetyEvent* event)
{
    if (event == nullptr)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "NvPSSReportSafetyEvent: event pointer is null", "");
        return NVPSSD_FAIL;
    }

    NvPSSDErr err = NVPSSD_SUCCESS;

    switch(rpcBackend)
    {
        case SOCKET:
          err = NvPSSReportSafetyEvent_Socket(clientId, event);
          break;

        default:
            NvPSBWriteData(NVPSB_LOG_INFO, "Only Socket based communication with PSS daemon is supported", "");
            err = NVPSSD_FAIL;
            break;
    }

    return err;
}

NvPSSDErr NvPSSReportSafetyEvent_Socket(const uint32_t clientId, const SafetyEvent* event)
{
    NvPSSDErr err = NVPSSD_SUCCESS;
    if (event == nullptr)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "NvPSSReportSafetyEvent_Socket: event pointer is null", "");
        return NVPSSD_FAIL;
    }
    int streamFd = -1;
    int storedSocket = -1;
    std::shared_ptr<std::mutex> streamIoMutex;
    fd_set writeFds;
    FD_ZERO(&writeFds);
    struct timeval socketValidityCheckTimeout = {0,SOCKET_VALIDITY_CHECK_TIMEOUT_US};
    int ready = 0;
    NvPSSDRPCMsgReq msgReq = {};
    NvPSSDRPCMsgResp msgResp = {};

    bool foundClient = false;
    {
        std::lock_guard<std::mutex> lock(g_pssRpcSocketMapMutex);
        const auto it = clientIdToSocketMap.find(clientId);
        if (it != clientIdToSocketMap.end())
        {
            storedSocket = it->second;
            foundClient = true;
            if (clientIdToStreamIoMutexMap.find(clientId) == clientIdToStreamIoMutexMap.end())
                clientIdToStreamIoMutexMap[clientId] = std::make_shared<std::mutex>();
            streamIoMutex = clientIdToStreamIoMutexMap[clientId];
        }
    }
    if (!foundClient)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "clientId is invalid", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    if (storedSocket < 0)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid socket descriptor for client", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    {
        std::lock_guard<std::mutex> streamIoLock(*streamIoMutex);
        streamFd = storedSocket;

        if (!(streamFd >= 0 && streamFd < FD_SETSIZE))
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR, "Invalid socket fd for select (must be in [0, FD_SETSIZE))",
                           "fd: " + std::to_string(streamFd));
#endif
            err = NVPSSD_FAIL;
            goto exit;
        }
        /*Do a socket validity check before actually calling send*/
        FD_SET(streamFd, &writeFds);
        ready = select(streamFd + 1, NULL, &writeFds, NULL, &socketValidityCheckTimeout);
        if (ready == -1) {
            NvPSBWriteData(NVPSB_LOG_ERR, "select() failed", "");
            err = NVPSSD_FAIL;
            goto exit;
        }
        if (ready > 0 && FD_ISSET(streamFd, &writeFds))
        {
            /*Socket is valid and writable, likely safe to call send().
            Preapre the packet and then send*/
            const uint32_t seqNo = msgCounter.fetch_add(1U, std::memory_order_relaxed);
            msgReq.msg = REPORT_SAFETY_EVENT;
            msgReq.reqSeqNo = seqNo;
            msgReq.size = sizeof(SafetyEvent);
            memset(&msgReq.reqPayload, 0,sizeof(msgReq.reqPayload));
            {
                SafetyEvent crcEvent;
                memcpy(&crcEvent, event, sizeof(SafetyEvent));
                pssSafetyEventSetCRC(&crcEvent);
                memcpy(&msgReq.reqPayload, &crcEvent, sizeof(SafetyEvent));
            }
            if (!sendAll(streamFd, &msgReq, sizeof(msgReq), kClientStreamSendFlags, RPC_RECV_DEADLINE_MS))
            {
                const int savedErr = errno;
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO, "Error in sending REPORT_SAFETY_EVENT msg to PSS Daemon: " + std::to_string(savedErr), "");
#endif
                (void)savedErr;
                err = NVPSSD_FAIL;
                goto exit;
            }
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "Socket for Client Id: " + std::to_string(clientId) + " is invalid", "");
#endif
            err = NVPSSD_FAIL;
            goto exit;
        }

        {
            const ssize_t rc = recvAll(streamFd, &msgResp, sizeof(NvPSSDRPCMsgResp), RPC_RECV_DEADLINE_MS);
            if (rc != static_cast<ssize_t>(sizeof(NvPSSDRPCMsgResp)))
            {
#ifdef NVPSF_DBG
                if (rc == 0)
                    NvPSBWriteData(NVPSB_LOG_INFO, "Connection closed by peer while awaiting report response", "");
                else
                    NvPSBWriteData(NVPSB_LOG_INFO, "Error receiving report response from PSS daemon: errno=" + std::to_string(errno), "");
#endif
                err = NVPSSD_FAIL;
                goto exit;
            }
            if (msgResp.respSeqNo != msgReq.reqSeqNo || msgResp.size != 1)
            {
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO, "Report response mismatch: expected seq=" + std::to_string(msgReq.reqSeqNo) +
                    " got seq=" + std::to_string(msgResp.respSeqNo) + " size=" + std::to_string(msgResp.size), "");
#endif
                err = NVPSSD_FAIL;
                goto exit;
            }
        }

        if (msgResp.respPayload[0] == REPORT_ACCEPTED)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "safety event reporting successful", "");
#endif
        }
        else
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "safety event reporting by client " + std::to_string(clientId) + " rejected",
                "status: " + std::to_string(msgResp.respPayload[0]));
            err = NVPSSD_FAIL;
            goto exit;
        }
    }

exit:
    return err;
}

/**
 * Send Heartbeat to PSS daemon
 */

static NvPSSDErr NvPSSSendHeartbeat_Socket(const uint32_t clientId, const uint8_t clientType);

NvPSSDErr NvPSSSendHeartbeat(const uint32_t clientId, const uint8_t clientType)
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    switch(rpcBackend)
    {
        case SOCKET:
            err = NvPSSSendHeartbeat_Socket(clientId, clientType);
            break;

        default:
            NvPSBWriteData(NVPSB_LOG_INFO, "Only Socket based communication with PSS daemon is supported", "");
            err = NVPSSD_FAIL;
            break;
    }

    return err;
}

static NvPSSDErr NvPSSSendHeartbeat_Socket(const uint32_t clientId, const uint8_t clientType)
{
    NvPSSDErr err = NVPSSD_SUCCESS;
    int streamFd = -1;
    int storedSocket = -1;
    std::shared_ptr<std::mutex> streamIoMutex;
    NvPSSDRPCMsgReq msgReq = {};
    uint32_t seqNo = 0;

    bool foundClient = false;
    {
        std::lock_guard<std::mutex> lock(g_pssRpcSocketMapMutex);
        const auto it = clientIdToSocketMap.find(clientId);
        if (it != clientIdToSocketMap.end())
        {
            storedSocket = it->second;
            foundClient = true;
            if (clientIdToStreamIoMutexMap.find(clientId) == clientIdToStreamIoMutexMap.end())
                clientIdToStreamIoMutexMap[clientId] = std::make_shared<std::mutex>();
            streamIoMutex = clientIdToStreamIoMutexMap[clientId];
        }
    }
    if (!foundClient)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "Client Id is invalid", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    if (storedSocket < 0)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid socket descriptor for client", "");
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

    {
        std::lock_guard<std::mutex> streamIoLock(*streamIoMutex);
        streamFd = storedSocket;

        seqNo = msgCounter.fetch_add(1U, std::memory_order_relaxed);

        msgReq.msg = SEND_HEARTBEAT;
        msgReq.reqSeqNo = seqNo;
        msgReq.size = 2;
        memset(msgReq.reqPayload, 0, sizeof(msgReq.reqPayload));
        msgReq.reqPayload[0] = HB_MSG;
        msgReq.reqPayload[1] = clientType;

        if (!sendAll(streamFd, &msgReq, sizeof(msgReq), kClientStreamSendFlags, RPC_RECV_DEADLINE_MS))
        {
            const int savedErr = errno;
            err = NVPSSD_FAIL;
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "Error in sending heartbeat to PSS Daemon: " + std::to_string(savedErr), "");
#endif
            (void)savedErr;
        }

        if (err == NVPSSD_SUCCESS)
        {
            NvPSSDRPCMsgResp hbResp = {};
            const ssize_t rc = recvAll(streamFd, &hbResp, sizeof(hbResp), RPC_RECV_DEADLINE_MS);
            if (rc != static_cast<ssize_t>(sizeof(hbResp))
                || hbResp.respSeqNo != seqNo
                || hbResp.size != 1
                || hbResp.respPayload[0] != HEARTBEAT_ACK)
            {
                err = NVPSSD_FAIL;
            }
        }
    }

exit:
    return err;
}