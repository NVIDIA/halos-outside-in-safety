/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/select.h>

#include "NvPSSDToPSD.hpp"
#include "pss_message_validate.h"

namespace nvpss
{

static constexpr int kMaxTransientRetries = 3;

/**
 * Write exactly @p len bytes to a stream socket.
 * Handles EINTR (unlimited) and EAGAIN/EWOULDBLOCK/zero-byte sends (bounded).
 * Returns: len on success, -1 on error (errno set).
 */
static ssize_t sendAll(int fd, const void* buf, size_t len)
{
    const auto* src = static_cast<const uint8_t*>(buf);
    size_t totalSent = 0;
    int transientRetries = 0;

    while (totalSent < len) {
        ssize_t n = send(fd, src + totalSent, len - totalSent, 0);
        if (n > 0) {
            totalSent += static_cast<size_t>(n);
            transientRetries = 0;
        } else if (n == 0) {
            if (++transientRetries > kMaxTransientRetries) {
                errno = EAGAIN;
                return -1;
            }
            continue;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                ++transientRetries <= kMaxTransientRetries) {
                continue;
            }
            return -1;
        }
    }
    return static_cast<ssize_t>(totalSent);
}

/**
 * Read exactly @p len bytes from a stream socket.
 * Handles EINTR (unlimited) and EAGAIN/EWOULDBLOCK (bounded).
 * Returns: len on success, 0 on peer close, -1 on error (errno set).
 */
static ssize_t recvAll(int fd, void* buf, size_t len)
{
    auto* dst = static_cast<uint8_t*>(buf);
    size_t totalRecvd = 0;
    int transientRetries = 0;

    while (totalRecvd < len) {
        ssize_t n = recv(fd, dst + totalRecvd, len - totalRecvd, 0);
        if (n > 0) {
            totalRecvd += static_cast<size_t>(n);
            transientRetries = 0;
        } else if (n == 0) {
            return 0;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                ++transientRetries <= kMaxTransientRetries) {
                continue;
            }
            return -1;
        }
    }
    return static_cast<ssize_t>(totalRecvd);
}

NvPSSDToPSDClient::NvPSSDToPSDClient() : PSSDaemonSocket(-1), runningPSSDServer(false)
{
    std::fill(std::begin(PSDclientSockets), std::end(PSDclientSockets), -1);
}

NvPSSDToPSDClient::~NvPSSDToPSDClient()
{
    stopPSSDServer();
}

// Setup PSS Daemon socket to listen for connections from PSD clients on
// PSS_DAEMON_SOCKET_PATH (/run/nvpsf/nvpssd_to_psd).
NvPSSDErr NvPSSDToPSDClient::initializePSSDServer()
{
    struct sockaddr_un addr;

    PSSDaemonSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (PSSDaemonSocket == -1) {
        std::cerr << "Failed to create PSD Client Socket" << std::endl;
        return NVPSSD_FAIL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PSS_DAEMON_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(PSS_DAEMON_SOCKET_PATH);

    if (bind(PSSDaemonSocket, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "Failed to bind PSD Client Socket" << std::endl;
        close(PSSDaemonSocket);
        return NVPSSD_FAIL;
    }

    if (listen(PSSDaemonSocket, 5) == -1) {
        std::cerr << "Failed to listen on PSD Client Socket" << std::endl;
        close(PSSDaemonSocket);
        return NVPSSD_FAIL;
    }

    std::cout << "PSD Server initialized on " << PSS_DAEMON_SOCKET_PATH << std::endl;
    return NVPSSD_SUCCESS;
}

// Create a thread to accept and handle PSD Client connections to PSS Daemon
NvPSSDErr NvPSSDToPSDClient::startPSSDServer()
{
    runningPSSDServer.store(true);
    PSSDServerThread = std::thread(&NvPSSDToPSDClient::runPSSDServer, this);
    return NVPSSD_SUCCESS;
}

// Stop and close all sockets to the PSD Client
NvPSSDErr NvPSSDToPSDClient::stopPSSDServer()
{
    runningPSSDServer.store(false);
    if (PSSDServerThread.joinable()) {
        PSSDServerThread.join();
    }

    // Close all PSD client sockets accpted by PSS Daemon
    for (int i = 0; i < MAX_PSD_CLIENTS; i++) {
        if (PSDclientSockets[i] >= 0) {
            close(PSDclientSockets[i]);
            PSDclientSockets[i] = -1;
        }
    }

    // Close PSS Daemon socket
    if (PSSDaemonSocket != -1) {
        close(PSSDaemonSocket);
        unlink(PSS_DAEMON_SOCKET_PATH);
        PSSDaemonSocket = -1;
    }

    return NVPSSD_SUCCESS;
}

NvPSSDErr NvPSSDToPSDClient::runPSSDServer()
{
    fd_set readFds;
    int maxSd, newSocket;

    while (runningPSSDServer.load()) {
        FD_ZERO(&readFds);
        FD_SET(PSSDaemonSocket, &readFds);
        maxSd = PSSDaemonSocket;

        // Add all active client sockets to the set
        for (uint32_t i = 0; i < MAX_PSD_CLIENTS; i++) {
            int sd = PSDclientSockets[i];
            if (sd >= 0) {
                FD_SET(sd, &readFds);
                if (sd > maxSd) maxSd = sd;
            }
        }

        // Wait for activity with timeout
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int selectRet = select(maxSd + 1, &readFds, NULL, NULL, &timeout);
        if (selectRet < 0) {
            if (errno != EINTR) {
                std::cerr << "select() failed: errno=" << errno
                          << " (" << strerror(errno) << ")" << std::endl;
            }
            continue;
        }

        // Handle new PSD Client connections to PSS Daemon
        if (FD_ISSET(PSSDaemonSocket, &readFds)) {
            newSocket = accept(PSSDaemonSocket, NULL, NULL);
            if (newSocket >= 0) {
                struct timeval tv;
                tv.tv_sec = 5;   // 5 second timeout
                tv.tv_usec = 0;

                setsockopt(newSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(newSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                bool PSDclientAdded = false;
                for (uint32_t i = 0; i < MAX_PSD_CLIENTS; i++) {
                    if (PSDclientSockets[i] == -1) {
                        PSDclientSockets[i] = newSocket;
                        clientIdToSocket[i] = newSocket;

                        struct { uint32_t clientId; uint32_t status; } response;
                        response.clientId = i;
                        response.status = 0;

                        if (sendAll(newSocket, &response, sizeof(response))
                            == static_cast<ssize_t>(sizeof(response))) {
                            PSDclientAdded = true;
                        } else {
                            std::cerr << "Failed to send client ID to new PSD client: "
                                      << strerror(errno) << std::endl;
                            close(newSocket);
                            PSDclientSockets[i] = -1;
                            clientIdToSocket.erase(i);
                        }
                        break;
                    }
                }

                if (!PSDclientAdded) {
                    std::cerr << "Maximum PSD clients reached, rejecting connection" << std::endl;
                    struct { uint32_t clientId; uint32_t status; } response;
                    response.clientId = 0;
                    response.status = 1;
                    if (sendAll(newSocket, &response, sizeof(response))
                        != static_cast<ssize_t>(sizeof(response))) {
                        std::cerr << "Failed to send rejection to PSD client: "
                                  << strerror(errno) << std::endl;
                    }
                    close(newSocket);
                }
            }
        }

        // Handle PSD client setup messages
        for (uint32_t i = 0; i < MAX_PSD_CLIENTS; i++) {
            int sd = PSDclientSockets[i];
            if (sd >= 0 && FD_ISSET(sd, &readFds)) {
                struct { uint32_t msgType; } msgHeader;
                int result = recv(sd, &msgHeader, sizeof(msgHeader), MSG_PEEK);

                if (result == sizeof(msgHeader)) {
                    // PSD Client sending event_types it needs to accept
                    if (msgHeader.msgType == REGISTER_EVENT_TYPES) {
                        PSDRegistrationMsg regMsg;
                        ssize_t nread = recvAll(sd, &regMsg, sizeof(PSDRegistrationMsg));
                        if (nread == static_cast<ssize_t>(sizeof(PSDRegistrationMsg))) {
                            handlePSDEventTypeRegistration(regMsg);
                        } else {
                            if (nread == 0) {
                                std::cerr << "Connection closed while receiving "
                                          << "PSDRegistrationMsg from client " << i << std::endl;
                            } else {
                                std::cerr << "Failed to receive PSDRegistrationMsg from client "
                                          << i << ": " << strerror(errno) << std::endl;
                            }
                            handlePSDClientDisconnection(i);
                        }
                    }
                    // PSD Client terminating its session
                    else if (msgHeader.msgType == UNREGISTER_PSD_CLIENT) {
                        handlePSDClientDisconnection(i);
                    } else {
                        std::cerr << "Unexpected msgType " << msgHeader.msgType
                                  << " from PSD client " << i << ", disconnecting" << std::endl;
                        handlePSDClientDisconnection(i);
                    }
                } else if (result == 0) {
                    handlePSDClientDisconnection(i);
                }
            }
        }
    }

    return NVPSSD_SUCCESS;
}

// Save the event types for that PSD Client in a map
NvPSSDErr NvPSSDToPSDClient::handlePSDEventTypeRegistration(const PSDRegistrationMsg& msg)
{
    uint32_t clientId = msg.clientId;

    // Validate client ID and socket connection
    auto socketIt = clientIdToSocket.find(clientId);
    if (socketIt == clientIdToSocket.end()) {
        std::cerr << "Invalid client ID " << clientId << " for event registration" << std::endl;
        return NVPSSD_FAIL;
    }

    // Register event types, clamped to the array capacity
    constexpr uint32_t maxEventSlots =
        static_cast<uint32_t>(sizeof(msg.eventTypes) / sizeof(msg.eventTypes[0]));
    uint32_t safeCount = std::min(msg.eventTypesCount, maxEventSlots);
    if (msg.eventTypesCount > maxEventSlots) {
        std::cerr << "PSD Client " << clientId << " eventTypesCount "
                  << msg.eventTypesCount << " exceeds capacity, truncating to "
                  << maxEventSlots << std::endl;
    }
    for (uint32_t i = 0; i < safeCount; i++) {
        EventType eventType = msg.eventTypes[i];
        eventTypeToClient[eventType] = clientId;
    }

    NvPSSDToPSDResp response;
    response.clientId = clientId;
    response.status = 0;

    // Send confirmation that event_types are registered successfully to PSD Client
    int clientSocket = socketIt->second;
    ssize_t sent = sendAll(clientSocket, &response, sizeof(response));
    if (sent != static_cast<ssize_t>(sizeof(response))) {
        if (sent == 0) {
            std::cerr << "Failed to send acknowledgment to PSD Client " << clientId
                      << ": connection closed" << std::endl;
        } else {
            std::cerr << "Failed to send acknowledgment to PSD Client " << clientId
                      << ": " << strerror(errno) << std::endl;
        }
        return NVPSSD_FAIL;
    }

    std::cout << "Sent acknowledgment to PSD Client " << clientId
              << " for " << safeCount << " event types" << std::endl;
    return NVPSSD_SUCCESS;
}

// To close all the PSD Client sockets
NvPSSDErr NvPSSDToPSDClient::handlePSDClientDisconnection(uint32_t clientId)
{
    if (clientId >= MAX_PSD_CLIENTS) {
        std::cerr << "Invalid client ID " << clientId
                  << " in disconnection handler (max " << MAX_PSD_CLIENTS << ")" << std::endl;
        return NVPSSD_FAIL;
    }

    clientIdToSocket.erase(clientId);

    // Remove event type mappings for this client
    auto it = eventTypeToClient.begin();
    while (it != eventTypeToClient.end()) {
        if (it->second == clientId) {
            it = eventTypeToClient.erase(it);
        } else {
            ++it;
        }
    }

    // Close socket
    if (PSDclientSockets[clientId] >= 0) {
        close(PSDclientSockets[clientId]);
        PSDclientSockets[clientId] = -1;
    }

    std::cout << "PSD Client " << clientId << " disconnected" << std::endl;
    return NVPSSD_SUCCESS;
}

// Sending DecisionRequest and DecisionResponse to the appropriate PSD Client based on the Safety Event's type
NvPSSDErr NvPSSDToPSDClient::sendDecisionRequestToPSD(const DecisionRequest& request, DecisionResponse* response)
{
    if (!response) {
        std::cerr << "ERROR: NULL response pointer" << std::endl;
        return NVPSSD_FAIL;
    }

    if (request.sensorDataSummarySize == 0) {
        std::cerr << "DecisionRequest has no sensor data" << std::endl;
        return NVPSSD_FAIL;
    }

    EventType eventType = request.sensorDataSummary[0].event.type;

    auto PSDClientID = eventTypeToClient.find(eventType);
    if (PSDClientID == eventTypeToClient.end()) {
        std::cerr << "No PSD client registered for event type: " << eventType << std::endl;
        return NVPSSD_FAIL;
    }

    uint32_t targetPSDClientId = PSDClientID->second;
    auto PSSDSocket = clientIdToSocket.find(targetPSDClientId);
    if (PSSDSocket == clientIdToSocket.end()) {
        std::cerr << "PSD client " << targetPSDClientId << " not connected" << std::endl;
        return NVPSSD_FAIL;
    }

    int targetPSSDSocket = PSSDSocket->second;
    // Set 2-second timeout
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(targetPSSDSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        std::cerr << "Warning: Failed to set socket receive timeout: " << strerror(errno) << std::endl;
    }

    if (setsockopt(targetPSSDSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        std::cerr << "Warning: Failed to set socket send timeout: " << strerror(errno) << std::endl;
    }

    /*
     * Defence-in-depth: run the same full validation PSD applies on
     * receipt so a malformed request is caught at the source rather
     * than silently forwarded.  This covers CRC, schema version,
     * sensorDataSummarySize bounds, OperationalMode range, and
     * per-sensor fused-field checks — symmetric with PSD's inbound gate.
     */
    {
        uint32_t vErr = validateDecisionRequest(&request);
        if (vErr != PSS_VALID) {
            std::cerr << "DecisionRequest validation failed (0x" << std::hex << vErr
                      << std::dec << ") before sending to PSD client "
                      << targetPSDClientId << std::endl;
            return NVPSSD_FAIL;
        }
    }

    ssize_t bytesSent = send(targetPSSDSocket, &request, sizeof(DecisionRequest), 0);
    if (bytesSent < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            std::cerr << "Timeout sending DecisionRequest to PSD client " << targetPSDClientId << std::endl;
        } else {
            std::cerr << "Failed to send DecisionRequest to PSD client " << targetPSDClientId
                      << ": " << strerror(errno) << std::endl;
        }
        return NVPSSD_FAIL;
    }
    if (bytesSent != sizeof(DecisionRequest)) {
        std::cerr << "Partial send to PSD client " << targetPSDClientId << std::endl;
        return NVPSSD_FAIL;
    }

    // Receive DecisionResponse from PSD client
    ssize_t bytesReceived = recv(targetPSSDSocket, response, sizeof(DecisionResponse), 0);
    if (bytesReceived < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            std::cerr << "Timeout receiving DecisionResponse from PSD client " << targetPSDClientId
                      << " (waited 2 seconds)" << std::endl;
        } else {
            std::cerr << "Failed to receive DecisionResponse from PSD client " << targetPSDClientId
                      << ": " << strerror(errno) << std::endl;
        }
        return NVPSSD_FAIL;
    }

    if (bytesReceived == 0) {
        std::cerr << "PSD client " << targetPSDClientId << " closed connection" << std::endl;
        return NVPSSD_FAIL;
    }

    if (bytesReceived != sizeof(DecisionResponse)) {
        std::cerr << "Partial receive from PSD client " << targetPSDClientId << std::endl;
        return NVPSSD_FAIL;
    }

    std::cout << "Successfully routed DecisionRequest for event type " << eventType
              << " to PSD client " << targetPSDClientId << std::endl;

    return NVPSSD_SUCCESS;
}

}
