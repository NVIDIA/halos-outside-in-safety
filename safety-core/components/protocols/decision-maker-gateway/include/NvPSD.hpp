/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <cstdint>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <vector>

#include "nvpsf_transport.h"
#include "NvPSD.h"
#include "nvpsf_transport.h"
#include "pss_daemon.h"


namespace nvpsd
{

typedef struct NvPSDCbInternal_t
{
    /**< Callback for decision request. */
    NvPSDErr (*processDecisionRequest)(const DecisionRequest* request, DecisionResponse* response);
    /**< Callback for decision reporting. */
    NvPSDErr (*publishDecisionResponse)(const DecisionResponse* response);
    /**< Callback for termination request. */
    NvPSDErr (*notifyShutdownRequest)(void);
} NvPSDCbInternal;

typedef enum NvPSDState_t
{
    UNINITIALIZED=0,
    INITIALIZED,
    TERMINATED,
    NA=-1
}NvPSDState;


class NvPSD
{

private:
    /* One AF_UNIX SOCK_SEQPACKET connection carries both severities; there is
     * no per-severity channel to name or track. */
    const NvPSDEndpoint endpt;

    /*
     * AF_UNIX SOCK_SEQPACKET client endpoint, replacing the raw SOCK_STREAM
     * descriptor. Atomic for the same reason the descriptor was: teardown uses
     * exchange(nullptr) so exactly one caller wins the right to close, and a
     * concurrent user sees nullptr rather than a freed object.
     *
     * The socket type is not cosmetic. The PSS daemon's server side is
     * SOCK_SEQPACKET, and a SOCK_STREAM client cannot connect to it --
     * connect() fails with EPROTOTYPE. Both ends must come from the same
     * release.
     */
    std::atomic<NvPSFTransport*> transport;
    /* The daemon, re-learned on every (re)connect. */
    NvPSFPeerId serverPeer;

    /*
     * The event types this client registered, kept so they can be re-issued
     * after a reconnect. Restoring the connection without them would leave the
     * daemon's eventTypeToClient map empty for this client: connected, and
     * silently unable to receive a single DecisionRequest.
     *
     * Written by NvPSDRegisterEventTypes before the listener thread starts, and
     * read only by the listener's reconnect path afterwards.
     */
    std::vector<EventType> registeredEventTypes;
    std::string PSSDaemonSocketPath;
    uint32_t clientId;

    NvPSDCbInternal callbacks;
    NvPSFTransportKind backend;

    std::atomic<NvPSDState> pssState;
    std::atomic<NvPSDState> clientState;
    bool decisionResponseTransmission;

    // DecisionRequest signals to trigger PSS_CLIENT stop
    // requestID = UNIT32_MAX
    // pssStatus/mode = ERROR
    static constexpr uint stopID = std::numeric_limits<uint32_t>::max();
    static constexpr OperationalMode stopMode = ERROR;

    std::queue<DecisionResponse> decisionResponseQueue;
    std::mutex mtx;
    std::condition_variable cv;
    const uint8_t timeout = 5;
    bool response_ready;
    std::atomic<uint32_t> decisionResponseTimeoutMs;

    std::atomic<bool> listenOnMsgChannelBackend;
    std::thread msgHandlerThread;
    std::thread decisionResponseTrasmitThread;
    /* One handler for one socket: both severities share the connection. */
    NvPSDErr handleMsgsOnMsgQueClientEndpt();
    NvPSDErr handleDecisionResponseTransmission();

    std::thread psdHeartbeatThread;
    std::atomic<bool> psdHeartbeatRunning{false};
    uint32_t pssClientId;
    /** When true, ChannelListenerStart skips PSS register + internal HB (process owns it, e.g. PSD Gateway). */
    std::atomic<bool> pssHeartbeatExternallyManaged{false};
    NvPSDErr psdHeartbeatLoop();
    NvPSDErr NvPSDStartHeartbeat();
    NvPSDErr NvPSDStopHeartbeat();

public:
    explicit NvPSD(NvPSDEndpoint endPt);
    ~NvPSD();
    NvPSDErr NvPSDChannelCreate(NvPSFTransportKind backend);

    /** Open the AF_UNIX endpoint and complete the client-ID handshake. */
    NvPSDErr openPssToPsdTransport();
    /**
     * Re-establish the PSS -> PSD link after the daemon has gone.
     *
     * Count-unbounded at a fixed cadence: a daemon restart must
     * not require restarting this process. Re-issues the stored event-type
     * registration, because a connection without routing is indistinguishable
     * from a dead link from the SDMs' point of view.
     */
    NvPSDErr reconnectPssToPsd();
    NvPSDErr NvPSDSetCbs(NvPSDCbInternal callbacks);
    NvPSDEndpoint NvPSDGetChannelEndpt();
    NvPSDErr NvPSDChannelListenerStart();
    /** Call after NvPSDChannelCreate, before NvPSDChannelListenerStart. If @p external is true, no internal PSS client/HB. */
    NvPSDErr setPssHeartbeatExternallyManaged(bool external);
    NvPSDErr NvPSDSetDecisionResponseTimeoutMs(uint32_t timeoutMs);
    NvPSDErr NvPSDRequestStart();
    NvPSDErr NvPSDGenerateDecision(const DecisionRequest* request, DecisionResponse* response);
    NvPSDErr NvPSDStop();
    NvPSDErr NvPSDChannelClose(NvPSFTransportKind backend);

    NvPSDErr NvPSDRegisterEventTypes(const EventType* eventTypes, uint32_t count);
};

}

