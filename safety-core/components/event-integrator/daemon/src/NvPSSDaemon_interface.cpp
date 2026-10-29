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

/*
 * "Stream" in these names is now historical: the RPC socket is
 * AF_UNIX SOCK_SEQPACKET, not a byte stream. The names are left alone
 * deliberately -- renaming them and the ~47 related identifiers would be a
 * large mechanical diff across a safety path for no behavioural gain, and the change
 * is already the highest-regression-risk change in the programme. Read
 * "stream" here as "the RPC connection".
 */
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);

    for (;;)
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

        /* One send, one whole message. SOCK_SEQPACKET is all-or-nothing, so a
         * short write has no remainder to resume -- see the note above the
         * socket() call. The deadline and EAGAIN handling stay: a daemon that
         * is not reading still applies backpressure. */
        const ssize_t n = send(fd, p, len, flags | MSG_DONTWAIT);
        if (n == static_cast<ssize_t>(len))
        {
            return true;
        }
        if (n >= 0)
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);

    for (;;)
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
        /*
         * DRAIN BEFORE REPORTING A HANGUP.
         *
         * POLLHUP together with POLLIN means the peer closed AND data is still
         * queued for us; the data does not vanish because the writer went away.
         * This used to test the hangup bits first and return -1, throwing away a
         * response that was already sitting in the socket buffer.
         *
         * It broke TERMINATE_CLIENT every time, not intermittently: the daemon
         * answers the terminate and then disconnects the client immediately, so
         * the reply and the hangup always arrive in the same poll. The caller
         * reported "Failed to terminate PSS client" while the daemon had done
         * exactly what it was asked. Reproduced from two directions --
         * pss_rpc_seqpacket_driver's "client terminates cleanly" and the x86
         * stack's cleanup step.
         *
         * A hangup treated as "nothing to read" is the same defect shape.
         */
        if ((pfd.revents & POLLIN) == 0 &&
            (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            errno = (pfd.revents & POLLNVAL) ? EINVAL : EIO;
            return -1;
        }

        /*
         * One recvmsg, one whole message.
         *
         * RECVMSG RATHER THAN RECV, because only recvmsg reports MSG_TRUNC. On
         * SOCK_SEQPACKET an oversized message is copied up to `len` and the
         * remainder is DISCARDED, so recv() returns exactly `len` and the
         * `n == len` test below could not tell a truncated message from a
         * correct one -- the caller would then read a partial NvPSSDRPCMsgResp
         * as though it were whole. Measured: 512 bytes sent, 64 requested,
         * recv() returns 64 and reports nothing; recvmsg() returns 64 with
         * MSG_TRUNC set.
         *
         * The realistic trigger is version skew, not attack. Six of the seven
         * RPC clients ship inside the PSF package and cannot skew;
         * `mdx-client` deploys with the metropolis application, so it is the
         * one binary that can
         * meet a daemon whose response struct has grown -- and it would have
         * misread the reply silently.
         *
         * A short return still means the daemon sent something that is not an
         * NvPSSDRPCMsgResp; the kernel has already discarded the rest of that
         * message, so accumulating towards `len` would splice two different
         * messages together. NvPSFTransport's recvOnFd() makes the same two
         * checks; this is the client half of the same link.
         */
        struct iovec  iov = {};
        struct msghdr msg = {};
        iov.iov_base   = p;
        iov.iov_len    = len;
        msg.msg_iov    = &iov;
        msg.msg_iovlen = 1;

        const ssize_t n = recvmsg(fd, &msg, 0);
        if (n > 0 && (msg.msg_flags & MSG_TRUNC) != 0)
        {
            /* Oversized for this buffer. The excess is already gone and cannot
             * be re-read, so this is a protocol error, not a retry. */
            errno = EPROTO;
            return -1;
        }
        if (n == static_cast<ssize_t>(len))
        {
            return static_cast<ssize_t>(len);
        }
        if (n == 0)
        {
            return 0;                 /* daemon closed the connection */
        }
        if (n > 0)
        {
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            continue;
        }
        return -1;
    }
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
const std::string socketPath = PSS_DAEMON_RPC_SOCKET_PATH;

/**
 * Client to socket mapping
 * Mapping from client to its corresponding socket
 * Maintained as stdlib unordered_map
 */
std::unordered_map<uint32_t,int>clientIdToSocketMap;

/**
 * Forward declarations of internal functions
 */
NvPSSDErr NvPSSRegisterPSSClient_Socket(uint32_t* clientId, uint8_t clientType, uint8_t instanceId);
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
    return NvPSSRegisterPSSClientInstance(clientId, clientType, 0U);
}

NvPSSDErr NvPSSRegisterPSSClientInstance(uint32_t* clientId, uint8_t clientType, uint8_t instanceId)
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    switch(rpcBackend)
    {
        case SOCKET:
          err = NvPSSRegisterPSSClient_Socket(clientId, clientType, instanceId);
          break;

        /* NVSCI is enumerated explicitly, not swept into `default:` (repo enum-audit rule). With every enumerator named, -Wswitch turns a
         * future backend into a COMPILE error at all four of these sites --
         * which is the only reliable way to be told about a dispatch site that
         * needs a new arm. A bare `default:` would silently accept it and fail
         * at run time on a safety path instead. */
        case NVSCI:
        default:
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "Unsupported PSS RPC backend; only AF_UNIX SOCK_SEQPACKET "
                           "socket transport is implemented",
                           "backend: " + std::to_string(static_cast<int>(rpcBackend)));
            err = NVPSSD_FAIL;
            break;
    }

    return err;
}

NvPSSDErr NvPSSRegisterPSSClient_Socket(uint32_t* clientId, uint8_t clientType, uint8_t instanceId)
{
    NvPSSDErr err = NVPSSD_SUCCESS;
    if (clientId == nullptr)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "NvPSSRegisterPSSClient_Socket: clientId output parameter is null", "");
        return NVPSSD_FAIL;
    }
    if (clientType != CLIENT_MDX &&
        clientType != CLIENT_SAFETY_MONITOR &&
        clientType != CLIENT_PSD_GATEWAY &&
        clientType != CLIENT_PERCEPTION_MONITOR)
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
                       "NvPSSRegisterPSSClient_Socket: invalid clientType",
                       "clientType: " + std::to_string(clientType));
        return NVPSSD_FAIL;
    }
    if (instanceId > PSS_MDX_INSTANCE_MAX || (instanceId != 0U && clientType != CLIENT_MDX))
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
                       "NvPSSRegisterPSSClient_Socket: invalid instance id (MDX only, 1.." +
                           std::to_string(PSS_MDX_INSTANCE_MAX) + ")",
                       "clientType: " + std::to_string(clientType) +
                           ", instanceId: " + std::to_string(instanceId));
        return NVPSSD_FAIL;
    }
    struct sockaddr_un addr = {};
    NvPSSDRPCMsgResp msgResp = {};
    NvPSSDRPCMsgReq msgReq = {};
    int clientSocket = -1;
    int previousSocket = -1;
    ssize_t recvResult = -1;

    /* SOCK_SEQPACKET, matching the daemon. See the note on the
     * connect() failure path below: this is not interchangeable with the old
     * SOCK_STREAM, and the mismatch has a dedicated diagnostic. */
    clientSocket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
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
        /*
         * EPROTOTYPE gets its own diagnostic, and it is NOT debug-only.
         *
         * The PSS RPC uses SOCK_SEQPACKET, not SOCK_STREAM, and
         * there is no wire compatibility: a client built before that change
         * cannot connect to a daemon built after it, or vice versa. The kernel
         * reports that as EPROTOTYPE, which as a bare "connection failed" is
         * indistinguishable from "the daemon is not running" -- and the two
         * have completely different fixes. Naming it, and naming the version
         * constraint, is the difference between a five-minute packaging fix and
         * an afternoon spent looking at the wrong process.
         */
        if (errno == EPROTOTYPE)
        {
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "PSS RPC socket type mismatch: this client uses "
                           "AF_UNIX SOCK_SEQPACKET but the daemon is listening "
                           "on SOCK_STREAM. Client and nvpss_daemon must come "
                           "from the same PSF package: the RPC socket type "
                           "changed and the two types cannot interoperate.",
                           "socket: " + socketPath);
        }
#ifdef NVPSF_DBG
        else
        {
            NvPSBWriteData(NVPSB_LOG_INFO, "Connection to PSS failed while registering new client",
                           "errno: " + std::to_string(errno));
        }
#endif
        err = NVPSSD_FAIL;
        goto exit;
    }

#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "A new client is connected to PSS daemon", "");
#endif

    msgReq.msg = REGISTER_CLIENT;
    msgReq.reqSeqNo = msgCounter.fetch_add(1U, std::memory_order_relaxed);
    /* Instance 0 sends exactly what every client sent before instance ids
     * existed, so an anonymous registration is byte-identical on the wire. */
    msgReq.size = static_cast<uint16_t>((instanceId != 0U) ? 2U : 1U);
    memset(&msgReq.reqPayload, 0, sizeof(msgReq.reqPayload));
    msgReq.reqPayload[0] = clientType;
    msgReq.reqPayload[1] = instanceId;
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
                    ", instanceId: " + std::to_string(instanceId) +
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

        /* NVSCI is enumerated explicitly, not swept into `default:` (repo enum-audit rule). With every enumerator named, -Wswitch turns a
         * future backend into a COMPILE error at all four of these sites --
         * which is the only reliable way to be told about a dispatch site that
         * needs a new arm. A bare `default:` would silently accept it and fail
         * at run time on a safety path instead. */
        case NVSCI:
        default:
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "Unsupported PSS RPC backend; only AF_UNIX SOCK_SEQPACKET "
                           "socket transport is implemented",
                           "backend: " + std::to_string(static_cast<int>(rpcBackend)));
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
        /* Reported unconditionally, not under NVPSF_DBG: a broken descriptor for a
           client that IS registered is library-internal state corruption. Callers log
           only that the call failed, so without this record a release build gives the
           operator no way to tell it from a daemon-side refusal. */
        NvPSBWriteData(NVPSB_LOG_ERR, "TerminatePSSClient: invalid socket descriptor for client",
                       " clientId: " + std::to_string(clientId) +
                       " fd: " + std::to_string(storedSocket));
        err = NVPSSD_FAIL;
        goto exit;
    }

    {
        std::lock_guard<std::mutex> streamIoLock(*streamIoMutex);
        streamFd = storedSocket;

        if (!(streamFd >= 0 && streamFd < FD_SETSIZE))
        {
            /* Reported unconditionally, not under NVPSF_DBG: an fd outside
               [0, FD_SETSIZE) means this process is out of descriptors, not that the
               daemon is unwell. The two demand opposite responses and the caller's
               NVPSSD_FAIL does not distinguish them. */
            NvPSBWriteData(NVPSB_LOG_ERR, "TerminatePSSClient: socket fd outside [0, FD_SETSIZE) for select",
                           " clientId: " + std::to_string(clientId) +
                           " fd: " + std::to_string(streamFd) +
                           " FD_SETSIZE: " + std::to_string(static_cast<int>(FD_SETSIZE)));
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

        /* NVSCI is enumerated explicitly, not swept into `default:` (repo enum-audit rule). With every enumerator named, -Wswitch turns a
         * future backend into a COMPILE error at all four of these sites --
         * which is the only reliable way to be told about a dispatch site that
         * needs a new arm. A bare `default:` would silently accept it and fail
         * at run time on a safety path instead. */
        case NVSCI:
        default:
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "Unsupported PSS RPC backend; only AF_UNIX SOCK_SEQPACKET "
                           "socket transport is implemented",
                           "backend: " + std::to_string(static_cast<int>(rpcBackend)));
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
        /* Reported unconditionally, not under NVPSF_DBG: a broken descriptor for a
           client that IS registered is library-internal state corruption. Callers log
           only that the call failed, so without this record a release build gives the
           operator no way to tell it from a daemon-side refusal. */
        NvPSBWriteData(NVPSB_LOG_ERR, "ReportSafetyEvent: invalid socket descriptor for client",
                       " clientId: " + std::to_string(clientId) +
                       " fd: " + std::to_string(storedSocket));
        err = NVPSSD_FAIL;
        goto exit;
    }

    {
        std::lock_guard<std::mutex> streamIoLock(*streamIoMutex);
        streamFd = storedSocket;

        if (!(streamFd >= 0 && streamFd < FD_SETSIZE))
        {
            /* Reported unconditionally, not under NVPSF_DBG: an fd outside
               [0, FD_SETSIZE) means this process is out of descriptors, not that the
               daemon is unwell. The two demand opposite responses and the caller's
               NVPSSD_FAIL does not distinguish them. */
            NvPSBWriteData(NVPSB_LOG_ERR, "ReportSafetyEvent: socket fd outside [0, FD_SETSIZE) for select",
                           " clientId: " + std::to_string(clientId) +
                           " fd: " + std::to_string(streamFd) +
                           " FD_SETSIZE: " + std::to_string(static_cast<int>(FD_SETSIZE)));
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

        /* NVSCI is enumerated explicitly, not swept into `default:` (repo enum-audit rule). With every enumerator named, -Wswitch turns a
         * future backend into a COMPILE error at all four of these sites --
         * which is the only reliable way to be told about a dispatch site that
         * needs a new arm. A bare `default:` would silently accept it and fail
         * at run time on a safety path instead. */
        case NVSCI:
        default:
            NvPSBWriteData(NVPSB_LOG_ERR,
                           "Unsupported PSS RPC backend; only AF_UNIX SOCK_SEQPACKET "
                           "socket transport is implemented",
                           "backend: " + std::to_string(static_cast<int>(rpcBackend)));
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
        /* Reported unconditionally, not under NVPSF_DBG: a broken descriptor for a
           client that IS registered is library-internal state corruption. Callers log
           only that the call failed, so without this record a release build gives the
           operator no way to tell it from a daemon-side refusal. */
        NvPSBWriteData(NVPSB_LOG_ERR, "SendHeartbeat: invalid socket descriptor for client",
                       " clientId: " + std::to_string(clientId) +
                       " fd: " + std::to_string(storedSocket));
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