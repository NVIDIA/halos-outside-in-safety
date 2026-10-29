/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <iostream>
 #include <string>
 #include <cstring>
 #include <algorithm>
#include <chrono>
 #include <thread>
 #include <errno.h>
 #include <unistd.h>
 #include <pthread.h>
 #include <sys/socket.h>
 #include <sys/un.h>

 #include "NvPSD.hpp"
 #include "NvPSDInternal.hpp"
 #include "NvPSB.h"
 #include "pss_message_validate.h"

 namespace nvpsd
 {

/*
 * Client-side receive wake-up period, so a listener loop notices shutdown
 * promptly instead of waiting out a long block.
 *
 * sendAll()/recvAll() used to live in this file. They existed only because
 * SOCK_STREAM can split or coalesce a message, so every caller had to loop
 * until it had exactly the bytes it wanted. SOCK_SEQPACKET preserves message
 * boundaries and the transport transmits or receives one whole message or
 * reports an error, so a partial transfer is not a reachable outcome and the
 * loops had no work left.
 */
static constexpr uint32_t kPsdClientRecvTimeoutMs = 1000U;

/** Bound on the connect and registration handshakes. */
static constexpr uint32_t kPsdHandshakeTimeoutMs = 5000U;

/* Reconnect cadence: fixed, no exponential backoff. */
static constexpr uint32_t kPsdReconnectIntervalMs = 500U;

/**
 * Receive one whole message, waiting up to @p timeoutMs for it to arrive.
 *
 * NvPSFTransportRecv NEVER waits. It issues recvmsg() with MSG_DONTWAIT on a
 * non-blocking descriptor, and `NvPSFTransportCfg::recvTimeoutMs` is stored by
 * Open and then never read again -- there is no SO_RCVTIMEO and no internal
 * poll. The only call in the API that waits is NvPSFTransportPoll.
 *
 * Calling Recv directly for a handshake does not work. Since AF_UNIX connect()
 * completes as soon as the connection is queued in the backlog -- long before the daemon has been woken from poll(),
 * run accept and sent its reply -- the client lost that race in essentially
 * every execution, got WOULD_BLOCK, and reported "failed to receive client ID".
 * The PSS -> PSD link would never have come up. Every client-side receive must
 * go through this helper.
 *
 * @param[out] outLen Bytes written to @p buf.
 * @return NVPSF_TRANSPORT_OK, NVPSF_TRANSPORT_TIMEOUT if nothing arrived in
 *         time, or the underlying error.
 */
static NvPSFTransportErr psdRecvWithDeadline(NvPSFTransport* t, void* buf,
                                             size_t bufLen, size_t* outLen,
                                             uint32_t timeoutMs)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;)
    {
        const NvPSFTransportErr err =
            NvPSFTransportRecv(t, nullptr, buf, bufLen, outLen);
        if (err != NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            return err;   /* OK, PEER_GONE, TRUNCATED or a hard error. */
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return NVPSF_TRANSPORT_TIMEOUT;
        }

        /* Wait for readability rather than spinning. Poll is the only call in
         * this API that blocks, and it is bounded by the remaining budget so
         * the whole helper stays within timeoutMs (LT-3). */
        const auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        NvPSFTransportEvent events[4];
        size_t              count = 0;
        (void)NvPSFTransportPoll(t, static_cast<uint32_t>(remainingMs), events,
                                 sizeof(events) / sizeof(events[0]), &count);

        /*
         * THE EVENTS MUST BE INSPECTED, not discarded. Poll is not a passive
         * sleep: on POLLHUP it emits PEER_GONE *and releases the peer slot*.
         *
         * The first version of this helper used Poll purely to wait and threw
         * the array away, which lost the disconnect entirely. What followed was
         * silent and total: with the slot gone, Recv finds nothing in use and
         * returns WOULD_BLOCK rather than PEER_GONE, and Poll builds zero
         * pollfds and returns TIMEOUT *immediately without sleeping*. So the
         * loop span at 100% CPU for the whole budget, the caller saw TIMEOUT
         * and treated it as "nothing arrived", and reconnectPssToPsd() was
         * never entered. The link stayed down and a core
         * stayed pinned.
         *
         * Because the listener spends essentially all its idle time inside
         * Poll, this is the path a daemon restart almost always takes; the
         * Recv-side PEER_GONE is the rarer case.
         */
        for (size_t i = 0; i < count; ++i)
        {
            if (events[i].kind == NVPSF_EVENT_PEER_GONE)
            {
                return NVPSF_TRANSPORT_PEER_GONE;
            }
        }
    }
}


 /*
  * Constructor of NvPSD.
  *
  * One AF_UNIX SOCK_SEQPACKET connection carries CRITICAL and OPERATIONAL
  * both, so the endpoint role is the only thing this object needs to be told.
  */
 NvPSD::NvPSD(NvPSDEndpoint endpt) : endpt(endpt)
 {
     switch (endpt)
     {
     case NVPSD_PSS:
         pssState.store(UNINITIALIZED);
         clientState.store(NA);
         break;
     case NVPSD_CLIENT:
         pssState.store(NA);
         clientState.store(UNINITIALIZED);
     }

     if (endpt == NVPSD_CLIENT)
     {
         if (NvPSBInitialize("NVPSB_PSD_CLIENT", NVPSB_PSD_CLIENT) != NVPSB_SUCCESS)
         {
             std::cerr << "Failed to initialize PSB.\n";
         }
     }

     transport.store(nullptr);
     serverPeer = NVPSF_PEER_INVALID;
     PSSDaemonSocketPath = PSS_DAEMON_SOCKET_PATH;
     clientId = 0;
     pssClientId = UINT32_MAX;

     callbacks = {nullptr, nullptr};
     listenOnMsgChannelBackend.store(false);
     response_ready = false;
     decisionResponseTransmission = false;
     decisionResponseTimeoutMs.store(NVPSD_DECISION_RESPONSE_TIMEOUT_MS_DEFAULT);
 }

/*
Destructor of NvPSD
Safety net: releases any resources that NvPSDChannelClose did not (or was
never called for).  All operations are guarded to tolerate double-close.
*/
NvPSD::~NvPSD()
{
    // 1. Signal every loop / thread to stop.
    listenOnMsgChannelBackend.store(false);
    psdHeartbeatRunning.store(false);

    // Wake the decision-response transmission thread, which may be
    // blocked on cv.wait_for().
    {
        std::lock_guard<std::mutex> lk(mtx);
        response_ready = true;
    }
    cv.notify_one();

    // 2. Close the socket early so any thread blocked in recv/send
    //    returns immediately with an error.
    /*
     * The transport is NOT closed here -- it is closed after the joins below.
     *
     * The earlier version closed first, reasoning that it would unblock any
     * thread parked inside the transport. That is precisely what the transport
     * header forbids ("Call only after every thread that uses t has been
     * joined"), and exchange(nullptr) does not help: msgHandlerThread caches the
     * raw pointer for a whole iteration and holds it across Recv, across the
     * user callback, and across Send. Closing under it frees the handle while
     * that cached copy is still in use.
     *
     * Termination is driven by listenOnMsgChannelBackend instead, and every
     * client receive is now bounded by psdRecvWithDeadline, so the joins are
     * bounded without needing the close to interrupt them.
     */

    // 3. Join all threads that were ever started.
    if (msgHandlerThread.joinable())
    {
        msgHandlerThread.join();
    }
    if (decisionResponseTrasmitThread.joinable())
    {
        decisionResponseTrasmitThread.join();
    }
    if (psdHeartbeatThread.joinable())
    {
        psdHeartbeatThread.join();
    }


    // 5. Terminate PSS client registration if still active.
    if (pssClientId != UINT32_MAX)
    {
        NvPSSTerminatePSSClient(pssClientId);
        pssClientId = UINT32_MAX;
    }

    /*
     * 6. Close the transport LAST, now that every thread that could hold a
     *    cached NvPSFTransport* has been joined above. The transport header
     *    requires exactly this ordering.
     */
    {
        NvPSFTransport* t = transport.exchange(nullptr);
        if (t != nullptr)
        {
            (void)NvPSFTransportClose(t);
        }
    }
}

#if defined(MSG_NOSIGNAL)
#else
static constexpr int kStreamSendFlags = 0;
#endif

/**
 * Read exactly @p len bytes from a stream socket into @p buf.
 *
 * Handles partial reads and EINTR.  Returns:
 *   len  – full frame received
 *   0    – peer closed the connection (possibly after a partial frame)
 *  -1    – unrecoverable error (errno is set)
 */
/**
 * Initialize the communication channels.
  *
  * NvPSD supports two IPC backends, POSIX message que and NvSciIPC.
  * Currently only POSIX message que is supported.
  *
  * POSIX Message Que:
  * For a given endpoint, two message queues are created.
  * They both are opened in NON_BLOCKING mode.
  * At the user API level, asynchronous messaging is exposed by managing
  * separate threads for send and receive over created queues.
  *
  * NvSciIPC:
  * <TBD>
  *
  */
/*
 * Open the AF_UNIX SOCK_SEQPACKET endpoint and complete the client-ID
 * handshake. Factored out of NvPSDChannelCreate so the reconnect path below can
 * reuse it verbatim -- a reconnect that diverged from the initial connect is
 * exactly how a link comes back up subtly wrong.
 */
NvPSDErr NvPSD::openPssToPsdTransport()
{
    NvPSFTransportCfg cfg = {};
    cfg.kind            = NVPSF_TRANSPORT_UNIX_SOCKET;
    cfg.role            = NVPSF_ROLE_CLIENT;
    cfg.link            = NVPSF_LINK_PSS_TO_PSD;
    cfg.maxPeers        = 1U;    /* forced to 1 for a client regardless */
    cfg.maxMessageBytes = sizeof(DecisionRequest);
    cfg.minQueueDepth   = 0U;    /* floor applies to the gateway link only */
    cfg.recvTimeoutMs   = kPsdClientRecvTimeoutMs;
    cfg.sendTimeoutMs   = 0U;
    cfg.unixPath        = PSSDaemonSocketPath.c_str();
    cfg.unixMode        = 0660;
    cfg.allowedGid      = getgid();

    NvPSFTransport* t = nullptr;
    const NvPSFTransportErr oerr = NvPSFTransportOpen(&cfg, &t);
    if (oerr != NVPSF_TRANSPORT_OK || t == nullptr)
    {
        return NVPSD_FAIL;
    }

    /* A client must be able to send first, and its peer id is assigned
     * internally, so it cannot be constructed here. */
    if (NvPSFTransportSolePeer(t, &serverPeer) != NVPSF_TRANSPORT_OK)
    {
        (void)NvPSFTransportClose(t);
        return NVPSD_FAIL;
    }

    struct { uint32_t clientId; uint32_t status; } response = {};
    size_t got = 0;
    const NvPSFTransportErr rerr =
        psdRecvWithDeadline(t, &response, sizeof(response), &got,
                            kPsdHandshakeTimeoutMs);
    if (rerr != NVPSF_TRANSPORT_OK || got != sizeof(response) || response.status != 0U)
    {
        /* status != 0 means the daemon refused us for capacity; a timeout or
         * short read means it went away mid-handshake. Both are fatal to this
         * attempt; the caller decides whether to retry. */
        (void)NvPSFTransportClose(t);
        return NVPSD_FAIL;
    }

    clientId = response.clientId;
    transport.store(t);
    return NVPSD_SUCCESS;
}

/*
 * Re-establish the link after the daemon has gone.
 *
 * Count-unbounded at a fixed 500 ms cadence with no exponential backoff, per
 * Unbounded because a daemon restart must not require restarting
 * this process. A bounded retry would leave the link permanently down after a
 * long enough outage, which is the failure this loop exists to prevent. Fixed cadence because backoff would make the worst-case
 * recovery time depend on how long the outage had already lasted, which is
 * precisely when recovery matters most.
 *
 * The outage is still visible: it is logged on entry and on success, so a
 * reconnect never masks a restart (LT-9). It is also still bounded in effect --
 * the SDMs latch on decision freshness long before this loop gives up, because
 * it never gives up.
 */
NvPSDErr NvPSD::reconnectPssToPsd()
{
    NvPSBWriteData(NVPSB_LOG_ERR,
                   "PSS->PSD link lost; reconnecting", "");

    for (uint32_t attempt = 1U; listenOnMsgChannelBackend.load(); ++attempt)
    {
        if (openPssToPsdTransport() == NVPSD_SUCCESS)
        {
            /*
             * Re-register before returning. A restored connection with no
             * event-type routing is worse than no connection: the daemon would
             * accept it, report a healthy client, and still deliver nothing.
             */
            if (!registeredEventTypes.empty())
            {
                /*
                 * Replay in batches, because PSDRegistrationMsg carries only
                 * ten event types per message -- a single call would silently
                 * restore the first ten and leave the rest unrouted, which
                 * looks exactly like a healthy reconnect.
                 *
                 * The vector is moved out first and repopulated by the normal
                 * registration path, rather than iterated in place:
                 * NvPSDRegisterEventTypes appends to it, so replaying from the
                 * live member would both duplicate every entry and grow it
                 * without bound across successive reconnects.
                 */
                std::vector<EventType> replay;
                replay.swap(registeredEventTypes);

                constexpr uint32_t kPerMessage =
                    static_cast<uint32_t>(sizeof(((PSDRegistrationMsg*)nullptr)->eventTypes) /
                                          sizeof(((PSDRegistrationMsg*)nullptr)->eventTypes[0]));
                NvPSDErr rerr = NVPSD_SUCCESS;
                for (size_t base = 0; base < replay.size() && rerr == NVPSD_SUCCESS;
                     base += kPerMessage)
                {
                    const uint32_t n = static_cast<uint32_t>(
                        std::min(static_cast<size_t>(kPerMessage), replay.size() - base));
                    rerr = NvPSDRegisterEventTypes(&replay[base], n);
                }

                if (rerr != NVPSD_SUCCESS)
                {
                    /*
                     * Restore the FULL pre-reconnect set unconditionally.
                     *
                     * NvPSDRegisterEventTypes appends before it sends and does
                     * not undo that on failure, so after a partial replay the
                     * member is non-empty but SHORT. The earlier
                     * `if (empty())` guard therefore did not restore, `replay`
                     * -- the only complete copy -- was destroyed at scope exit,
                     * and the lost types were gone for good: every later
                     * reconnect replayed the truncated set, leaving the link
                     * connected, reporting healthy, and silently unroutable for
                     * those events. That is the exact "worse than no
                     * connection" case this block exists to prevent.
                     */
                    registeredEventTypes = std::move(replay);
                    NvPSBWriteData(NVPSB_LOG_ERR,
                                   "PSS->PSD reconnected but re-registration failed; retrying", "");
                    NvPSFTransport* t = transport.exchange(nullptr);
                    if (t != nullptr)
                    {
                        (void)NvPSFTransportClose(t);
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kPsdReconnectIntervalMs));
                    continue;
                }
            }

            NvPSBWriteData(NVPSB_LOG_INFO,
                           "PSS->PSD link re-established",
                           "attempts=" + std::to_string(attempt));
            return NVPSD_SUCCESS;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPsdReconnectIntervalMs));
    }

    /* Only reachable when shutdown was requested while reconnecting. */
    return NVPSD_FAIL;
}

  NvPSDErr NvPSD::NvPSDChannelCreate(NvPSFTransportKind backend)
  {
      NvPSDErr err = NVPSD_SUCCESS;
      this->backend = backend;
      /* An unsupported kind falls into the else below and fails loudly. */
      if(backend == NVPSF_TRANSPORT_UNIX_SOCKET)
      {
         err = openPssToPsdTransport();
         if (err != NVPSD_SUCCESS)
         {
             goto exit;
         }
      }
      else
      {
          std::cerr<<"IPC backend other than POSIX Message Que and SOCKET is not yet supported. Exiting\n";
  #ifdef NVPSF_DBG
          NvPSBWriteData(NVPSB_LOG_ERR, "Messaging backend other than posix message que is not yet supported", "");
  #endif
          err = NVPSD_FAIL;
          goto exit;
      }
  exit:
      return err;
  }

 /**
  * Register callbacks
  *
  * The client endpoint of NvPSD is required to register two callbacks
  * processDecisionRequest, notifyShutdownRequest.
  * notifyShutdownRequest : When PSS requests to stop the messaging and thereby terminating
  * the ongoing session
  * processDecisionRequest : When PSD is required to generate decision response messages
  * publishDecisionResponse: When PSD is required to send decision response
  *
  * When these callbacks are registered client state shifts from UNINITIALIZED => INITIALIZED
  */

 NvPSDErr NvPSD::NvPSDSetCbs(NvPSDCbInternal callbacks)
 {
     this->callbacks.processDecisionRequest = callbacks.processDecisionRequest;
     if (callbacks.publishDecisionResponse)
     {
         this->callbacks.publishDecisionResponse = callbacks.publishDecisionResponse;
         decisionResponseTransmission = true;
     }
     this->callbacks.notifyShutdownRequest = callbacks.notifyShutdownRequest;

     clientState = INITIALIZED;

     return NVPSD_SUCCESS;
 }

 /**
  * Get the endpoint for given instance of NvPSD
  *
  * This is a helper method to identify whether the given
  * instance of NvPSD is connected to PSS or Client.
  *
  * This is helpful in access controlling certain methods which
  * are restricted to particular endpoint.
  */

 NvPSDEndpoint NvPSD::NvPSDGetChannelEndpt()
 {
     return endpt;
 }

 /**
  * Launcher of listener+handler thread
  *
  * This helper function launches a thread that perfoms the following :
  *
  * It listens on the message que for messages from the other endpoint.
  * Once the message is received, depending on the
  * command, it takes the subsequent action i.e. invoke certain callback or
  * send the acknowledgement etc.
  *
  *
  */

 NvPSDErr NvPSD::setPssHeartbeatExternallyManaged(bool external)
 {
     if (listenOnMsgChannelBackend.load())
         return NVPSD_FAIL;
     pssHeartbeatExternallyManaged.store(external);
     return NVPSD_SUCCESS;
 }

 NvPSDErr NvPSD::NvPSDSetDecisionResponseTimeoutMs(uint32_t timeoutMs)
 {
     if (!NvPSDIsDecisionResponseTimeoutMsValid(timeoutMs))
         return NVPSD_FAIL;
     decisionResponseTimeoutMs.store(timeoutMs, std::memory_order_relaxed);
     return NVPSD_SUCCESS;
 }

 NvPSDErr NvPSD::NvPSDChannelListenerStart()
 {
 NvPSDErr err = NVPSD_SUCCESS;
     if (backend == NVPSF_TRANSPORT_UNIX_SOCKET)
     {
         listenOnMsgChannelBackend.store(true);

         /* ONE handler thread, for one socket carrying both severities. */
         msgHandlerThread = std::thread(&NvPSD::handleMsgsOnMsgQueClientEndpt, this);

         if (decisionResponseTransmission)
         {
             decisionResponseTrasmitThread = std::thread(&NvPSD::handleDecisionResponseTransmission, this);
         }

         if (!pssHeartbeatExternallyManaged.load())
             NvPSDStartHeartbeat();
     }
     else
     {
         err = NVPSD_FAIL;
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que and posix socket is not yet supported", "");
 #endif
     }

     return err;
 }

 /**
  * Send the request to NvPSD about staring the messaging session
  *
  * This method would be only accessed by PSS endpoint.
  * Access to this method is controlled at NvPSD_interface.
  *
  */

NvPSDErr NvPSD::NvPSDRequestStart()
{
    pssState.store(INITIALIZED);
    listenOnMsgChannelBackend.store(true);

    return NVPSD_SUCCESS;
}

 /**
  *  Decision Request to PSD Client
  *
  * This funcion is supposed to be called by NVPSD_PSS endpoint.
  *
  */

  NvPSDErr NvPSD::NvPSDGenerateDecision(const DecisionRequest* request, DecisionResponse* response)
  {
      NvPSDErr err = NVPSD_SUCCESS;
    if(pssState != INITIALIZED)
    {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"PSS endpoint is not in INITIALIZED state", "");
#endif
            std::cerr<<"PSS endpoint is not in INITIALIZED state";
            err = NVPSD_FAIL;
            goto exit;
    }

     /* Reject zero-size requests that somehow bypassed validation;
      * reading sensorDataSummary[0] with no entries is undefined. */
      if (request->sensorDataSummarySize == 0)
      {
          std::cerr << "NvPSDGenerateDecision: sensorDataSummarySize is 0, cannot route" << std::endl;
          err = NVPSD_FAIL;
          goto exit;
      }

     /* One socket carries both severities, so there is no channel to select. */
     if (backend == NVPSF_TRANSPORT_UNIX_SOCKET)
     {
         NvPSFTransport* const t = transport.load();
         if (t == nullptr)
         {
 #ifdef NVPSD_DBG
         syslog(LOG_ERR, "Transport not connected");
 #endif
             std::cerr << "Transport not connected";
             err = NVPSD_FAIL;
             goto exit;
         }
        // Send Decision Request to PSD Client called from PSS Daemon
        const NvPSFTransportErr serr =
            NvPSFTransportSend(t, serverPeer, request, sizeof(DecisionRequest));
        if (serr != NVPSF_TRANSPORT_OK)
        {
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in sending data to client (%s)", NvPSFTransportErrName(serr));
#endif
            std::cerr << "Error in sending data to client ("
                      << NvPSFTransportErrName(serr) << "), disconnecting" << std::endl;
            /*
             * The out-of-frame-alignment reasoning that justified disconnecting
             * on a partial write no longer applies: a message-oriented transport
             * transmits a whole message or nothing, so framing cannot desync
             *. Disconnecting is retained anyway because every remaining
             * failure here -- PEER_GONE, WOULD_BLOCK on a link with no
             * backpressure budget, a hard error -- means this connection is not
             * usable, and failing fast beats retrying against a dead peer.
             */
            NvPSFTransport* closeT = transport.exchange(nullptr);
            if (closeT != nullptr)
            {
                (void)NvPSFTransportClose(closeT);
            }
            err = NVPSD_FAIL;
            goto exit;
        }
        // Wait for Decision Response from PSD Client
        {
            size_t got = 0;
            const NvPSFTransportErr rerr =
                NvPSFTransportRecv(t, nullptr, response, sizeof(DecisionResponse), &got);
            if (rerr != NVPSF_TRANSPORT_OK || got != sizeof(DecisionResponse))
            {
#ifdef NVPSD_DBG
                syslog(LOG_ERR, "Error in receiving data from client (%s)", NvPSFTransportErrName(rerr));
#endif
                std::cerr << "Error in receiving data from client ("
                          << NvPSFTransportErrName(rerr) << ", " << got
                          << " bytes), disconnecting" << std::endl;
                NvPSFTransport* closeT = transport.exchange(nullptr);
                if (closeT != nullptr)
                {
                    (void)NvPSFTransportClose(closeT);
                }
                err = NVPSD_FAIL;
                goto exit;
            }
        }
      }
      else
      {
  #ifdef NVPSF_DBG
          NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que is not yet supported", "");
  #endif
          std::cerr<<"messaging backend other than msg que is not yet supported \n";
          err = NVPSD_FAIL;
          goto exit;
      }
  exit:
      return err;
  }

 /**
  * Terminate the ongoing session
  *
  * Function to terminate the ongoing messaging session
  * This can only be invoked by PSS endpoint.
  *
  */

 NvPSDErr NvPSD::NvPSDStop()
 {
     NvPSDErr err = NVPSD_SUCCESS;
     DecisionRequest request = {};

     if (pssState != INITIALIZED)
     {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"PSS endpoint is not in INITIALIZED stated", "");
 #endif
         std::cerr << "PSS endpoint is not in INITIALIZED stated";
         err = NVPSD_FAIL;
         goto exit;
     }

    request.requestId = stopID;
    request.sensorDataSummarySize = 0;  // STOP request has no sensor data
    request.pssStatus.mode = stopMode;
    pssDecisionRequestSetCRC(&request);

    /* One socket, one send. */
    if (backend == NVPSF_TRANSPORT_UNIX_SOCKET)
     {
         // Send STOP Request to PSD Client
         NvPSFTransport* const t = transport.load();
         if (t == nullptr)
         {
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Transport not connected");
 #endif
             std::cerr << "Transport not connected";
             err = NVPSD_FAIL;
             goto exit;
         }
         const NvPSFTransportErr serr =
             NvPSFTransportSend(t, serverPeer, &request, sizeof(DecisionRequest));
         if (serr != NVPSF_TRANSPORT_OK)
         {
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Error in sending STOP request to client (%s)", NvPSFTransportErrName(serr));
 #endif
             std::cerr << "Error in sending STOP request to client ("
                       << NvPSFTransportErrName(serr) << ")" << std::endl;
             err = NVPSD_FAIL;
             goto exit;
         }

 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "STOP request sent to client via socket");
 #endif
         std::cout << "STOP request sent to client via socket" << std::endl;
     }
     else
     {
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que is not yet supported", "");
 #endif
         std::cerr<<"messaging backend other than msg que is not yet supported \n";
         err = NVPSD_FAIL;
         goto exit;
     }

 exit:
     return err;
 }

 /**
  * Close the Message queues
  *
  * Close and delelte/unlink the open message queues
  * This is the complete closure of messaging between PSS
  * and Client endpoints of NvPSD
  *
  */

 NvPSDErr NvPSD::NvPSDChannelClose(NvPSFTransportKind backend)
 {
     NvPSDErr err = NVPSD_SUCCESS;

     // Signal threads to stop
     listenOnMsgChannelBackend.store(false);
     std::this_thread::sleep_for(std::chrono::milliseconds(200));

     /*Before closing the communication channel, lets ensure all the forked threads are joined
     and no thread remain running while channels are closing*/
     if (endpt == NVPSD_CLIENT)
     {
         if (msgHandlerThread.joinable())
         {
             msgHandlerThread.join();
         }

         if (decisionResponseTrasmitThread.joinable())
         {
             decisionResponseTrasmitThread.join();
         }
     }

     /* One transport to close; NvPSFTransportClose unlinks the bound path. */
     if (backend == NVPSF_TRANSPORT_UNIX_SOCKET)
     {
         // Stop heartbeat thread before closing
         NvPSDStopHeartbeat();

         // Close the PSD Client endpoint
         NvPSFTransport* t = transport.exchange(nullptr);
         if (t != nullptr)
         {
             // Send unregister message to PSS Daemon before closing
             PSDRegistrationMsg msg = {};
             msg.msgType = UNREGISTER_PSD_CLIENT;
             msg.clientId = clientId;
             msg.eventTypesCount = 0;
             const NvPSFTransportErr serr =
                 NvPSFTransportSend(t, serverPeer, &msg, sizeof(msg));
             if (serr != NVPSF_TRANSPORT_OK)
             {
 #ifdef NVPSD_DBG
                 syslog(LOG_ERR, "Failed to send unregister message (%s)", NvPSFTransportErrName(serr));
 #endif
                 std::cerr << "Failed to send unregister message ("
                           << NvPSFTransportErrName(serr) << ")" << std::endl;
             }

             (void)NvPSFTransportClose(t);

 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "Socket connection closed");
 #endif
             std::cout << "Disconnected from PSD server" << std::endl;
         }
     }
     else
     {
         std::cerr << "messaging backend other than socket is not yet supported" << std::endl;
         err = NVPSD_FAIL;
         goto exit;
     }

 exit:
     return err;
 }

/**
 * Listener on the message queue  and Message handler thread for client
 *
 * Entry point of the listener thread.
 *
 * This thread would keep on listening on the message que for messages from the PSS endpoint.
 * This thread also handles the received messages.
 *
 */

 NvPSDErr NvPSD::handleMsgsOnMsgQueClientEndpt()
  {
      NvPSDErr err = NVPSD_SUCCESS;
      DecisionRequest request = {};
      DecisionResponse response = {};

     /* One receive loop, handling CRITICAL and OPERATIONAL both. */
    if(backend == NVPSF_TRANSPORT_UNIX_SOCKET)
    {
        while (listenOnMsgChannelBackend.load())
        {
            NvPSFTransport* const t = transport.load();
            if (t == nullptr)
            {
                break;
            }

            /*
             * The client's single reader. Recv returns one whole DecisionRequest
             * or an error -- never a fragment -- so the old "short read means the
             * stream desynchronized" case has no producer and the loop no longer
             * has to treat a partial frame as fatal.
             */
            size_t bytesReceived = 0;
            const NvPSFTransportErr rerr =
                psdRecvWithDeadline(t, &request, sizeof(DecisionRequest),
                                    &bytesReceived, kPsdClientRecvTimeoutMs);

            if (rerr == NVPSF_TRANSPORT_TIMEOUT)
            {
                /* Nothing arrived within the wake-up period. Loop so
                 * listenOnMsgChannelBackend is re-checked promptly.
                 *
                 * Going through the helper is what keeps this from being a
                 * 100%-CPU spin: a bare Recv returns immediately because it
                 * never waits, so a loop built on a bare Recv pins a core
                 * permanently on a safety target. */
                continue;
            }

           if (rerr == NVPSF_TRANSPORT_OK && bytesReceived == sizeof(DecisionRequest))
           {
                /* Verify CRC + fields before acting on the message. */
                {
                    uint32_t vErr = validateDecisionRequest(&request);
                    if (vErr != PSS_VALID)
                    {
                        std::cerr << "handleMsgsOnMsgQueClientEndpt(socket): DecisionRequest validation failed (0x"
                                  << std::hex << vErr << std::dec << "), dropping" << std::endl;
                        continue;
                    }
                }

                // Handle STOP request (special case)
                if (request.requestId == stopID && request.pssStatus.mode == stopMode)
                {
                     listenOnMsgChannelBackend.store(false);
                     if (callbacks.notifyShutdownRequest)
                     {
                         callbacks.notifyShutdownRequest();
                     }
                     else
                     {
                         std::cerr << "ERROR: notifyShutdownRequest callback not registered" << std::endl;
                     }
                     if (NvPSBExit() != NVPSB_SUCCESS)
                     {
                         std::cerr << "Failed to exit NvPSB.\n";
                     }
                     break;
                 }

                 // Generate Decision using callback
                 if (callbacks.processDecisionRequest)
                 {
                     callbacks.processDecisionRequest(&request, &response);
                 }
                 else
                 {
                     std::cerr << "ERROR: processDecisionRequest callback not registered" << std::endl;
                     // Fill default response
                     response.decisionId = NVPSD_NO_RSP;
                     response.action = NO_ACTION_REQUIRED;
                     response.confidenceLevel = 0.0f;
                 }

                 // Report Decision
                 if (decisionResponseTransmission && callbacks.publishDecisionResponse)
                 {
                     std::lock_guard<std::mutex> lock(mtx);
                     decisionResponseQueue.push(response);
                     response_ready = true;
                     cv.notify_one();
                 }

                 // Send DecisionResponse to PSS Daemon
                 const NvPSFTransportErr serr =
                     NvPSFTransportSend(t, serverPeer, &response, sizeof(DecisionResponse));
                 if (serr == NVPSF_TRANSPORT_WOULD_BLOCK)
                 {
                     /*
                     * Backpressure, not a broken link. Tearing down here would
                     * convert a momentarily full socket buffer into a permanent
                     * outage of the path carrying PSS ERROR and SW_FAIL. The
                     * daemon's own retry budget covers a lost response.
                     */
                     std::cerr << "DecisionResponse dropped (peer buffer full); "
                                  "link left up" << std::endl;
                 }
                 else if (serr != NVPSF_TRANSPORT_OK)
                 {
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in sending DecisionResponse (%s)", NvPSFTransportErrName(serr));
#endif
                     std::cerr << "Error sending DecisionResponse ("
                               << NvPSFTransportErrName(serr) << "), reconnecting" << std::endl;
                     NvPSFTransport* closeT = transport.exchange(nullptr);
                     if (closeT != nullptr)
                     {
                         (void)NvPSFTransportClose(closeT);
                     }
                     if (reconnectPssToPsd() != NVPSD_SUCCESS)
                     {
                         break;
                     }
                 }
            }
            else if (rerr == NVPSF_TRANSPORT_PEER_GONE)
            {
                /*
                 * The daemon has gone. Reconnect rather than tearing the link
                 * down for the lifetime of this process: a daemon restart
                 * must not require restarting this one.
                 */
                NvPSFTransport* closeT = transport.exchange(nullptr);
                if (closeT != nullptr)
                {
                    (void)NvPSFTransportClose(closeT);
                }
                if (reconnectPssToPsd() != NVPSD_SUCCESS)
                {
                    /* Only fails when shutdown was requested mid-reconnect. */
                    break;
                }
                continue;
            }
            else
            {
                /* TRUNCATED, or a hard transport error. A truncated message is a
                 * protocol error rather than a transient: the message is already
                 * discarded by the kernel and cannot be re-read, so continuing
                 * would silently skip a DecisionRequest. */
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in receiving DecisionRequest (%s), disconnecting",
                   NvPSFTransportErrName(rerr));
#endif
                std::cerr << "Error receiving DecisionRequest ("
                          << NvPSFTransportErrName(rerr) << ", " << bytesReceived
                          << " bytes), reconnecting" << std::endl;
                NvPSFTransport* closeT = transport.exchange(nullptr);
                if (closeT != nullptr)
                {
                    (void)NvPSFTransportClose(closeT);
                }
                if (reconnectPssToPsd() != NVPSD_SUCCESS)
                {
                    break;
                }
                continue;
            }
        }
    }
      return err;
  }


 /* All messages are handled by handleMsgsOnMsgQueClientEndpt(): one socket
  * carries CRITICAL and OPERATIONAL both, so there is no second listener. */


 /**
  * Listener on the decision response for client
  *
  * Entry point of the listener thread.
  *
  * This thread would keep on listening on the decision response genrated by client.
  * This thread also handles transmission of generated decision response.
  *
  */
 NvPSDErr NvPSD::handleDecisionResponseTransmission()
 {
     NvPSDErr err = NVPSD_SUCCESS;

     while (listenOnMsgChannelBackend.load())
     {
         std::unique_lock<std::mutex> lck(mtx);
         if (cv.wait_for(lck, std::chrono::seconds(timeout),
                         [&]
                         { return response_ready; }))
         {
             // Condition met
             response_ready = false;
             if (!decisionResponseQueue.empty())
             {
                 // Copy response before releasing lock
                 auto response = decisionResponseQueue.front();
                 decisionResponseQueue.pop();
                 // Release lock before calling callback
                 lck.unlock();

                 if (callbacks.publishDecisionResponse) {
                     callbacks.publishDecisionResponse(&response);
                 }
             }
             else
             {
                 lck.unlock();
                 std::cerr << "No Decision Response found to report\n";
             }
         }
         else
         {
             // Timeout
             continue;
         }
     }

     return err;
 }

 NvPSDErr NvPSD::NvPSDRegisterEventTypes(const EventType *eventTypes, uint32_t count)
 {
     NvPSDErr err = NVPSD_SUCCESS;

     NvPSFTransport* const t = transport.load();
     if (t == nullptr)
     {
         std::cerr << "Transport not connected. Call NvPSDChannelCreate first." << std::endl;
         return NVPSD_FAIL;
     }

     /* Remembered so reconnectPssToPsd() can re-issue them. Additive, matching
      * the daemon's handler, so batched registrations accumulate here too. */
     for (uint32_t i = 0; i < count; ++i)
     {
         registeredEventTypes.push_back(eventTypes[i]);
     }

     // Prepare registration message
     PSDRegistrationMsg msg = {};
     msg.msgType = REGISTER_EVENT_TYPES;
     msg.clientId = clientId;
     msg.eventTypesCount = count;

     for (uint32_t i = 0; i < count && i < 10; i++)
     {
         msg.eventTypes[i] = eventTypes[i];
     }

     // Send registration request to PSS Daemon
     const NvPSFTransportErr serr =
         NvPSFTransportSend(t, serverPeer, &msg, sizeof(msg));
     if (serr != NVPSF_TRANSPORT_OK)
     {
         std::cerr << "Failed to send event type registration ("
                   << NvPSFTransportErrName(serr) << ")" << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Failed to send event type registration (%s)", NvPSFTransportErrName(serr));
 #endif
         return NVPSD_FAIL;
     }

     // Wait for confirmation from PSS Daemon
     struct
     {
         uint32_t clientId;
         uint32_t status;
     } response = {};
     size_t got = 0;
     if (psdRecvWithDeadline(t, &response, sizeof(response), &got,
                             kPsdHandshakeTimeoutMs) == NVPSF_TRANSPORT_OK &&
         got == sizeof(response))
     {
         if (response.status == 0)
         {
             std::cout << "Successfully registered " << count << " event types" << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "Successfully registered %d event types", count);
 #endif
         }
         else
         {
             std::cerr << "Failed to register event types - server response: " << response.status << std::endl;
             err = NVPSD_FAIL;
         }
     }
     else
     {
         std::cerr << "Failed to receive registration response (errno="
                   << errno << ")" << std::endl;
         err = NVPSD_FAIL;
     }

     return err;
 }

 NvPSDErr NvPSD::NvPSDStartHeartbeat()
 {
     NvPSDErr err = NVPSD_SUCCESS;

     if (psdHeartbeatRunning.load())
     {
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_INFO, "Heartbeat already running", "");
 #endif
         return NVPSD_SUCCESS;
     }

     if (NvPSSRegisterPSSClient(&pssClientId, CLIENT_PSD_GATEWAY) != NVPSSD_SUCCESS)
     {
         std::cerr << "Failed to register with PSS daemon for heartbeat" << std::endl;
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register with PSS daemon for heartbeat", "");
 #endif
         return NVPSD_FAIL;
     }

 #ifdef NVPSF_DBG
     NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD registered with PSS daemon, clientId: " + std::to_string(pssClientId), "");
 #endif

     psdHeartbeatRunning.store(true);
     psdHeartbeatThread = std::thread(&NvPSD::psdHeartbeatLoop, this);

 #ifdef NVPSF_DBG
     NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD heartbeat thread started", "");
 #endif

     return err;
 }

 NvPSDErr NvPSD::NvPSDStopHeartbeat()
 {
     NvPSDErr err = NVPSD_SUCCESS;

     psdHeartbeatRunning.store(false);

     if (psdHeartbeatThread.joinable())
     {
         psdHeartbeatThread.join();
     }

     if (pssClientId != UINT32_MAX)
     {
         NvPSSTerminatePSSClient(pssClientId);
         pssClientId = UINT32_MAX;
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD terminated PSS client registration", "");
 #endif
     }

     return err;
 }

 NvPSDErr NvPSD::psdHeartbeatLoop()
 {
     NvPSDErr err = NVPSD_SUCCESS;
     NvPSSDErr hbStatus = NVPSSD_SUCCESS;
     int consecutiveFailures = 0;
     const int maxConsecutiveFailures = 10;

     while (psdHeartbeatRunning.load() && listenOnMsgChannelBackend.load())
     {
         if (pssClientId != UINT32_MAX)
         {
             hbStatus = NvPSSSendHeartbeat(pssClientId, CLIENT_PSD_GATEWAY);
             if (hbStatus != NVPSSD_SUCCESS)
             {
                 consecutiveFailures++;
                 if (consecutiveFailures >= maxConsecutiveFailures)
                 {
 #ifdef NVPSF_DBG
                     NvPSBWriteData(NVPSB_LOG_ERR, "NvPSD heartbeat failed " +
                         std::to_string(maxConsecutiveFailures) + " consecutive times, exiting", "");
 #endif
                     listenOnMsgChannelBackend.store(false);
                     break;
                 }
             }
             else
             {
                 consecutiveFailures = 0;
             }
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(HB_INTERVAL_MS));
     }

     return err;
 }

 }
