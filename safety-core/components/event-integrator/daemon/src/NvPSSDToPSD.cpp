/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>
#include <cstring>
#include <unistd.h>

#include "NvPSSDToPSD.hpp"
#include "NvPSSDeliveryFailSafe.hpp"
#include "pss_message_validate.h"

namespace nvpss
{

/*
 * sendAll() and recvAll() are gone.
 *
 * They existed because SOCK_STREAM can split or coalesce a message, so every
 * caller had to loop until it had exactly the bytes it wanted. SOCK_SEQPACKET
 * preserves message boundaries: NvPSFTransportSend transmits one whole message
 * or reports WOULD_BLOCK, and NvPSFTransportRecv returns one whole message or
 * an error. A partial transfer is not a reachable outcome, so the loops have
 * nothing left to do.
 */

/*
 * The single-reader demux depends on these three sizes being distinct: the
 * server thread identifies a message by its length alone, with no MSG_PEEK and
 * no type field read ahead of the body. If a protocol change ever makes two of
 * them equal, this stops compiling rather than silently misrouting a
 * DecisionResponse into the registration path.
 */
static_assert(sizeof(PSDRegistrationMsg) != sizeof(DecisionResponse),
              "PSD control and response messages must be distinguishable by size");
static_assert(sizeof(DecisionRequest) != sizeof(DecisionResponse),
              "DecisionRequest and DecisionResponse must be distinguishable by size");
static_assert(sizeof(DecisionRequest) != sizeof(PSDRegistrationMsg),
              "DecisionRequest and PSDRegistrationMsg must be distinguishable by size");

/** Largest message this link carries, so the transport can size its buffers. */
static constexpr size_t kMaxPsdMessageBytes = sizeof(DecisionRequest);

/** Poll wake-up period, so the server loop notices runningPSSDServer going false. */
static constexpr uint32_t kPsdPollTimeoutMs = 1000U;

/** Mode applied to the AF_UNIX socket after bind; the 1777 parent grants nothing. */
static constexpr mode_t kPsdSocketMode = 0660;

/** Assignment message sent to a client the moment it is admitted. */
struct PsdClientIdAssignment
{
    uint32_t clientId;
    uint32_t status;    /* 0 = admitted, 1 = rejected */
};

NvPSSDToPSDClient::NvPSSDToPSDClient() : transport(nullptr), runningPSSDServer(false)
{
}

NvPSSDToPSDClient::~NvPSSDToPSDClient()
{
    stopPSSDServer();
}

// Setup PSS Daemon socket to listen for connections from PSD clients on
// PSS_DAEMON_SOCKET_PATH (/run/nvpsf/nvpssd_to_psd).
NvPSSDErr NvPSSDToPSDClient::initializePSSDServer()
{
    /*
     * UNIX_SOCKET unconditionally. There is no configuration surface for
     * this link any more: NvPSSDaemon rejects the retired PSSDToPSDComBackend
     * key outright, so nothing can select anything else. Passing the kind
     * explicitly rather than defaulting keeps the link's transport visible at
     * the point it is created.
     */
    NvPSFTransportCfg cfg = {};
    cfg.kind            = NVPSF_TRANSPORT_UNIX_SOCKET;
    cfg.role            = NVPSF_ROLE_SERVER;
    cfg.link            = NVPSF_LINK_PSS_TO_PSD;
    cfg.maxPeers        = MAX_PSD_CLIENTS;
    cfg.maxMessageBytes = kMaxPsdMessageBytes;
    /*
     * Zero, and this one is a conclusion rather than a deferral. Depth
     * enforcement exists for the gateway -> SDM link, where a dropped send burns
     * a sequence number and latches an SDM. Here a send failure surfaces to the
     * caller as a failed delivery that the retry budget handles, so a depth
     * floor would buy nothing and could abort startup on a correctly
     * provisioned system.
     */
    cfg.minQueueDepth   = 0U;
    cfg.recvTimeoutMs   = kPsdPollTimeoutMs;
    cfg.sendTimeoutMs   = 0U;
    cfg.unixPath        = PSS_DAEMON_SOCKET_PATH;
    cfg.unixMode        = kPsdSocketMode;
    cfg.allowedGid      = getgid();

    const NvPSFTransportErr err = NvPSFTransportOpen(&cfg, &transport);
    if (err != NVPSF_TRANSPORT_OK || transport == nullptr) {
        std::cerr << "Failed to open PSS->PSD transport: "
                  << NvPSFTransportErrName(err) << std::endl;
        transport = nullptr;
        return NVPSSD_FAIL;
    }

    std::cout << "PSD Server initialized on " << PSS_DAEMON_SOCKET_PATH
              << " (UNIX_SOCKET)" << std::endl;
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

    /* Release any sender still parked, before tearing the transport down. A
     * sender blocked on the mailbox would otherwise wait out its full timeout
     * against a server that is never going to answer. */
    {
        std::lock_guard<std::mutex> lock(mailboxMtx);
        for (uint32_t i = 0; i < MAX_PSD_CLIENTS; i++) {
            if (mailbox[i].armed) {
                mailbox[i].peerLost = true;
            }
        }
    }
    mailboxCv.notify_all();

    /* Safe only because the server thread is joined above: NvPSFTransportClose
     * requires that no thread is inside a transport call. It closes every
     * descriptor and unlinks the bound path. */
    if (transport != nullptr) {
        (void)NvPSFTransportClose(transport);
        transport = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(clientMapMtx);
        clientIdToPeer.clear();
        eventTypeToClient.clear();
    }

    return NVPSSD_SUCCESS;
}

bool NvPSSDToPSDClient::clientIdForPeerLocked(NvPSFPeerId peer,
                                              uint32_t* outClientId) const
{
    for (const auto& entry : clientIdToPeer) {
        if (entry.second == peer) {
            *outClientId = entry.first;
            return true;
        }
    }
    return false;
}

bool NvPSSDToPSDClient::admitPeer(NvPSFPeerId peer, uint32_t* outClientId)
{
    std::lock_guard<std::mutex> lock(clientMapMtx);
    for (uint32_t i = 0; i < MAX_PSD_CLIENTS; i++) {
        if (clientIdToPeer.find(i) == clientIdToPeer.end()) {
            clientIdToPeer[i] = peer;
            *outClientId = i;
            return true;
        }
    }
    return false;
}

void NvPSSDToPSDClient::deliverResponse(uint32_t clientId,
                                        const DecisionResponse& response)
{
    bool matched = false;
    {
        std::lock_guard<std::mutex> lock(mailboxMtx);
        ResponseMailbox& mb = mailbox[clientId];
        /*
         * Only a matching decisionId completes the wait. A late response from an
         * earlier request that already timed out can still arrive on this
         * socket, and accepting it would answer the current request with a
         * previous decision. Discarding it here preserves the drain-until-
         * deadline behaviour the stream implementation had, with the sender
         * simply continuing to wait.
         */
        if (mb.armed && !mb.have && response.decisionId == mb.requestId) {
            mb.response = response;
            mb.have = true;
            matched = true;
        }
    }
    if (matched) {
        mailboxCv.notify_all();
    } else {
        std::cerr << "Discarding unmatched DecisionResponse from PSD client "
                  << clientId << ": decisionId=" << response.decisionId
                  << std::endl;
    }
}

void NvPSSDToPSDClient::failWaiter(uint32_t clientId)
{
    bool woke = false;
    {
        std::lock_guard<std::mutex> lock(mailboxMtx);
        if (mailbox[clientId].armed && !mailbox[clientId].have) {
            mailbox[clientId].peerLost = true;
            woke = true;
        }
    }
    if (woke) {
        mailboxCv.notify_all();
    }
}

NvPSSDErr NvPSSDToPSDClient::runPSSDServer()
{
    /*
     * The single reader. Nothing else in this class calls NvPSFTransportRecv,
     * and that is the property this design requires -- not a lock around two
     * readers, but one reader.
     */
    NvPSFTransportEvent events[MAX_PSD_CLIENTS + 4U];
    std::vector<uint8_t> buf(kMaxPsdMessageBytes);

    while (runningPSSDServer.load()) {
        size_t eventCount = 0;
        const NvPSFTransportErr pollErr =
            NvPSFTransportPoll(transport, kPsdPollTimeoutMs, events,
                               sizeof(events) / sizeof(events[0]), &eventCount);
        if (pollErr != NVPSF_TRANSPORT_OK && pollErr != NVPSF_TRANSPORT_TIMEOUT) {
            /* Back off before retrying. A persistent poll failure would
             * otherwise peg a core and flood the log without bound. */
            std::cerr << "PSS->PSD transport poll error: "
                      << NvPSFTransportErrName(pollErr) << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        for (size_t e = 0; e < eventCount; e++) {
            const NvPSFTransportEvent& ev = events[e];

            if (ev.kind == NVPSF_EVENT_PEER_CONNECTED) {
                /* Credentials were validated by the transport before this
                 * event; admission here is only about capacity. */
                uint32_t clientId = 0;
                PsdClientIdAssignment assignment = {};
                if (admitPeer(ev.peer, &clientId)) {
                    assignment.clientId = clientId;
                    assignment.status = 0U;
                } else {
                    std::cerr << "Maximum PSD clients reached, rejecting connection"
                              << std::endl;
                    assignment.clientId = 0U;
                    assignment.status = 1U;
                }

                const NvPSFTransportErr serr =
                    NvPSFTransportSend(transport, ev.peer, &assignment,
                                       sizeof(assignment));
                if (serr != NVPSF_TRANSPORT_OK) {
                    std::cerr << "Failed to send client ID to new PSD client: "
                              << NvPSFTransportErrName(serr) << std::endl;
                    if (assignment.status == 0U) {
                        /* Already drops the peer; calling DropPeer again here
                         * would be a second reclaim whose only saving grace is
                         * that the generation check makes it a no-op. */
                        (void)handlePSDClientDisconnection(clientId);
                    } else {
                        (void)NvPSFTransportDropPeer(transport, ev.peer);
                    }
                } else if (assignment.status != 0U) {
                    (void)NvPSFTransportDropPeer(transport, ev.peer);
                }
                continue;
            }

            if (ev.kind == NVPSF_EVENT_PEER_GONE) {
                uint32_t clientId = 0;
                bool known = false;
                {
                    std::lock_guard<std::mutex> lock(clientMapMtx);
                    known = clientIdForPeerLocked(ev.peer, &clientId);
                }
                if (known) {
                    (void)handlePSDClientDisconnection(clientId);
                }
                continue;
            }

            if (ev.kind != NVPSF_EVENT_MESSAGE_READY) {
                continue;
            }

            NvPSFPeerId from = NVPSF_PEER_INVALID;
            size_t      n    = 0;
            const NvPSFTransportErr rerr =
                NvPSFTransportRecv(transport, &from, buf.data(), buf.size(), &n);

            if (rerr == NVPSF_TRANSPORT_WOULD_BLOCK) {
                continue;
            }

            /*
             * A disconnect can surface as a return code instead of an event,
             * and when it does the transport has already released the slot --
             * so no PEER_GONE event will follow and this is the only chance to
             * reclaim. Recv is round-robin over all peers, so the disconnect
             * consumed here may belong to a different client than the event
             * that prompted the read.
             */
            if (rerr == NVPSF_TRANSPORT_PEER_GONE) {
                uint32_t clientId = 0;
                bool known = false;
                {
                    std::lock_guard<std::mutex> lock(clientMapMtx);
                    known = (from != NVPSF_PEER_INVALID) &&
                            clientIdForPeerLocked(from, &clientId);
                }
                if (known) {
                    (void)handlePSDClientDisconnection(clientId);
                }
                continue;
            }

            if (rerr == NVPSF_TRANSPORT_TRUNCATED) {
                std::cerr << "Oversized message from a PSD client; discarding"
                          << std::endl;
                continue;
            }
            if (rerr != NVPSF_TRANSPORT_OK) {
                std::cerr << "PSS->PSD transport recv error: "
                          << NvPSFTransportErrName(rerr) << std::endl;
                continue;
            }

            uint32_t clientId = 0;
            {
                std::lock_guard<std::mutex> lock(clientMapMtx);
                if (!clientIdForPeerLocked(from, &clientId)) {
                    /* A message from a peer that was never admitted, or whose
                     * slot has already been reclaimed. Nothing to route it to. */
                    continue;
                }
            }

            /*
             * Demux on length. No MSG_PEEK, no read-ahead of a type field: with
             * message boundaries preserved, the size IS the discriminator, and
             * the static_asserts above guarantee the shapes stay distinguishable.
             */
            if (n == sizeof(DecisionResponse)) {
                DecisionResponse response = {};
                std::memcpy(&response, buf.data(), sizeof(response));
                deliverResponse(clientId, response);
                continue;
            }

            if (n == sizeof(PSDRegistrationMsg)) {
                PSDRegistrationMsg regMsg = {};
                std::memcpy(&regMsg, buf.data(), sizeof(regMsg));

                if (regMsg.msgType == REGISTER_EVENT_TYPES) {
                    (void)handlePSDEventTypeRegistration(clientId, regMsg);
                } else if (regMsg.msgType == UNREGISTER_PSD_CLIENT) {
                    (void)handlePSDClientDisconnection(clientId);
                } else {
                    std::cerr << "Unexpected msgType " << regMsg.msgType
                              << " from PSD client " << clientId
                              << ", disconnecting" << std::endl;
                    (void)handlePSDClientDisconnection(clientId);
                }
                continue;
            }

            std::cerr << "Unexpected " << n << "-byte message from PSD client "
                      << clientId << ", disconnecting" << std::endl;
            (void)handlePSDClientDisconnection(clientId);
        }
    }

    return NVPSSD_SUCCESS;
}

// Save the event types for that PSD Client in a map
NvPSSDErr NvPSSDToPSDClient::handlePSDEventTypeRegistration(uint32_t clientId,
                                                             const PSDRegistrationMsg& msg)
{
    /*
     * @p clientId is the SERVER-ESTABLISHED identity, resolved from the
     * authenticated transport peer the message actually arrived on. The
     * msg.clientId field is client-controlled and is deliberately NOT used for
     * routing: a client could otherwise send clientId=0 and bind another
     * client's event types to itself, so every subsequent DecisionRequest for
     * those types would be delivered to the wrong process while the sender
     * parked waiting for a response the real owner was never asked for.
     *
     * Authorization must derive from state established at authenticated
     * connection time, never from a field in a per-message payload.
     */
    if (msg.clientId != clientId) {
        std::cerr << "PSD client " << clientId << " claimed clientId "
                  << msg.clientId << " in its registration; using the "
                  << "authenticated identity and ignoring the claim" << std::endl;
    }

    // Register event types, clamped to the array capacity. Computed before the
    // critical section so no avoidable work happens under the lock.
    constexpr uint32_t maxEventSlots =
        static_cast<uint32_t>(sizeof(msg.eventTypes) / sizeof(msg.eventTypes[0]));
    uint32_t safeCount = std::min(msg.eventTypesCount, maxEventSlots);
    if (msg.eventTypesCount > maxEventSlots) {
        std::cerr << "PSD Client " << clientId << " eventTypesCount "
                  << msg.eventTypesCount << " exceeds capacity, truncating to "
                  << maxEventSlots << std::endl;
    }

    /*
     * Validate the client and publish its event types in one critical section,
     * copying the peer out rather than keeping the iterator: the map may rehash
     * once the lock is released. Diagnostics and the acknowledgment are emitted
     * afterwards so that no I/O of any kind, including stderr, happens under
     * this lock.
     */
    NvPSFPeerId peer = NVPSF_PEER_INVALID;
    {
        std::lock_guard<std::mutex> lock(clientMapMtx);

        auto peerIt = clientIdToPeer.find(clientId);
        if (peerIt != clientIdToPeer.end()) {
            peer = peerIt->second;

            for (uint32_t i = 0; i < safeCount; i++) {
                EventType eventType = msg.eventTypes[i];
                eventTypeToClient[eventType] = clientId;
            }
        }
    }

    if (peer == NVPSF_PEER_INVALID) {
        std::cerr << "Invalid client ID " << clientId << " for event registration" << std::endl;
        return NVPSSD_FAIL;
    }

    NvPSSDToPSDResp response;
    response.clientId = clientId;
    response.status = 0;

    // Send confirmation that event_types are registered successfully to PSD Client
    const NvPSFTransportErr serr =
        NvPSFTransportSend(transport, peer, &response, sizeof(response));
    if (serr != NVPSF_TRANSPORT_OK) {
        std::cerr << "Failed to send acknowledgment to PSD Client " << clientId
                  << ": " << NvPSFTransportErrName(serr) << std::endl;
        return NVPSSD_FAIL;
    }

    std::cout << "Sent acknowledgment to PSD Client " << clientId
              << " for " << safeCount << " event types" << std::endl;
    return NVPSSD_SUCCESS;
}

// Release a PSD client's registration and its transport peer.
NvPSSDErr NvPSSDToPSDClient::handlePSDClientDisconnection(uint32_t clientId)
{
    if (clientId >= MAX_PSD_CLIENTS) {
        std::cerr << "Invalid client ID " << clientId
                  << " in disconnection handler (max " << MAX_PSD_CLIENTS << ")" << std::endl;
        return NVPSSD_FAIL;
    }

    /*
     * Unpublish before dropping. A sender resolves a peer from these maps and
     * then transmits with the lock released, so the identity must be gone from
     * the maps before the peer is released -- otherwise a sender can be handed
     * an identity that is about to be freed. The NvPSFPeerId generation counter
     * makes the residual case detectable rather than dangerous: a send on a
     * stale identity returns PEER_GONE instead of reaching whichever client has
     * since been given that slot.
     */
    NvPSFPeerId peer = NVPSF_PEER_INVALID;
    {
        std::lock_guard<std::mutex> lock(clientMapMtx);

        auto peerIt = clientIdToPeer.find(clientId);
        if (peerIt != clientIdToPeer.end()) {
            peer = peerIt->second;
            clientIdToPeer.erase(peerIt);
        }

        // Remove event type mappings for this client
        auto it = eventTypeToClient.begin();
        while (it != eventTypeToClient.end()) {
            if (it->second == clientId) {
                it = eventTypeToClient.erase(it);
            } else {
                ++it;
            }
        }
    }

    /* Outside the lock: DropPeer is a transport call, and the transport's mutex
     * is a leaf that must not be taken under a caller lock. */
    if (peer != NVPSF_PEER_INVALID) {
        (void)NvPSFTransportDropPeer(transport, peer);
    }

    /* A sender parked on this client will never be answered now. Waking it here
     * means it fails immediately instead of burning its full response timeout
     * against a client that has gone. */
    failWaiter(clientId);

    std::cout << "PSD Client " << clientId << " disconnected" << std::endl;
    return NVPSSD_SUCCESS;
}

// Sending DecisionRequest and DecisionResponse to the appropriate PSD Client based on the Safety Event's type
NvPSSDErr NvPSSDToPSDClient::sendDecisionRequestToPSD(const DecisionRequest& request,
                                                      DecisionResponse* response,
                                                      uint32_t timeoutMs,
                                                      NvPSSDeliveryFailureReason* outReason)
{
    /*
     * Default to the refusal reason and overwrite it only on the two exits that
     * are genuinely a timeout. Defaulting this way means a future early return
     * added above those exits is attributed conservatively rather than being
     * reported as a timeout it never reached.
     */
    if (outReason != nullptr)
        *outReason = NvPSSDeliveryFailureReason::PROCESS_DECISION_REQUEST_FAILED;

    if (!response) {
        std::cerr << "ERROR: NULL response pointer" << std::endl;
        return NVPSSD_FAIL;
    }

    if (request.sensorDataSummarySize == 0) {
        std::cerr << "DecisionRequest has no sensor data" << std::endl;
        return NVPSSD_FAIL;
    }

    EventType eventType = request.sensorDataSummary[0].event.type;

    /*
     * Resolve event type -> client -> peer in ONE critical section. Two separate
     * acquisitions would let the client disconnect in between, so the peer could
     * belong to a different client than the one the event type resolved to.
     */
    uint32_t    targetPSDClientId = 0U;
    NvPSFPeerId targetPeer = NVPSF_PEER_INVALID;
    bool        eventTypeRegistered = false;
    {
        std::lock_guard<std::mutex> lock(clientMapMtx);

        auto PSDClientID = eventTypeToClient.find(eventType);
        if (PSDClientID != eventTypeToClient.end()) {
            eventTypeRegistered = true;
            targetPSDClientId = PSDClientID->second;

            auto peerIt = clientIdToPeer.find(targetPSDClientId);
            if (peerIt != clientIdToPeer.end()) {
                targetPeer = peerIt->second;
            }
        }
    }

    if (!eventTypeRegistered) {
        std::cerr << "No PSD client registered for event type: " << eventType << std::endl;
        return NVPSSD_FAIL;
    }
    if (targetPeer == NVPSF_PEER_INVALID) {
        std::cerr << "PSD client " << targetPSDClientId << " not connected" << std::endl;
        return NVPSSD_FAIL;
    }
    if (targetPSDClientId >= MAX_PSD_CLIENTS) {
        std::cerr << "PSD client id " << targetPSDClientId << " out of range" << std::endl;
        return NVPSSD_FAIL;
    }

    /*
     * Defence-in-depth: run the same full validation PSD applies on receipt so a
     * malformed request is caught at the source rather than silently forwarded.
     * Covers CRC, schema version, sensorDataSummarySize bounds, OperationalMode
     * range, and per-sensor fused-field checks -- symmetric with PSD's inbound
     * gate. Done before arming the mailbox so a rejected request never occupies
     * a slot.
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

    /*
     * Arm BEFORE sending, not after. The server thread can read the
     * DecisionResponse and try to deliver it before this thread gets back from
     * NvPSFTransportSend; if the mailbox were armed afterwards the response
     * would be discarded as unmatched and the request would time out despite
     * having been answered correctly.
     */
    const uint32_t myRequestId = request.requestId;
    {
        std::lock_guard<std::mutex> lock(mailboxMtx);
        ResponseMailbox& mb = mailbox[targetPSDClientId];
        /*
         * Refuse rather than overwrite. This link is single-request-in-flight
         * and SafetyEventManager enforces it by holding psdSendMtx_ across the
         * whole send-and-wait, so a second arm means that invariant has been
         * broken somewhere.
         *
         * Refusing turns the invariant from an assumption into something this
         * code detects. Overwriting would corrupt both senders: the second
         * replaces the first's requestId, and whichever response arrives wakes
         * both -- returning one request's RecommendedAction as the answer to
         * the other, silently, on the decision path.
         */
        if (mb.armed) {
            std::cerr << "REQUEST ALREADY IN FLIGHT: a second DecisionRequest is "
                         "already in "
                         "flight to PSD client " << targetPSDClientId
                      << "; refusing to overwrite it" << std::endl;
            return NVPSSD_FAIL;
        }
        mb.armed = true;
        mb.requestId = myRequestId;
        mb.have = false;
        mb.peerLost = false;
        mb.response = DecisionResponse{};
    }

    /* Disarm on every exit path, including the early returns below. */
    struct MailboxGuard
    {
        NvPSSDToPSDClient* self;
        uint32_t           id;
        ~MailboxGuard()
        {
            std::lock_guard<std::mutex> lock(self->mailboxMtx);
            self->mailbox[id].armed = false;
        }
    } guard{this, targetPSDClientId};

    const NvPSFTransportErr serr =
        NvPSFTransportSend(transport, targetPeer, &request, sizeof(DecisionRequest));
    if (serr != NVPSF_TRANSPORT_OK) {
        /* A partial send is not reachable on a message-oriented transport, so
         * there is no short-write case to distinguish here. */
        std::cerr << "Failed to send DecisionRequest to PSD client "
                  << targetPSDClientId << ": " << NvPSFTransportErrName(serr)
                  << std::endl;
        return NVPSSD_FAIL;
    }

    /*
     * Park until the server thread delivers the matching response. This thread
     * never touches the socket -- that is what makes the reader single. The
     * wait is bounded by the caller's timeout, so it cannot become an unbounded
     * block on a safety path.
     */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    std::unique_lock<std::mutex> lock(mailboxMtx);
    ResponseMailbox& mb = mailbox[targetPSDClientId];
    /*
     * The predicate checks the request id, not just `have`. Without it a
     * response delivered for a *different* requestId would satisfy the wait and
     * be returned as this request's decision. deliverResponse already matches on
     * decisionId before setting `have`, so this is belt-and-braces -- but it is
     * the belt that makes the guarantee local to the waiter rather than
     * depending on the deliverer being correct.
     */
    const bool delivered = mailboxCv.wait_until(
        lock, deadline,
        [&mb, myRequestId] {
            return (mb.have && mb.requestId == myRequestId) || mb.peerLost;
        });

    if (!delivered || !mb.have || mb.requestId != myRequestId) {
        const bool lost = mb.peerLost;
        lock.unlock();
        /*
         * RESPONSE_TIMEOUT, not a send refusal: the request left this process.
         * Peer-loss lands here too -- the send succeeded and no matching
         * DecisionResponse ever arrived, which is the same evidence gap as an
         * expired wait and must not be recorded as a failure to send.
         */
        if (outReason != nullptr)
            *outReason = NvPSSDeliveryFailureReason::RESPONSE_TIMEOUT;
        if (lost) {
            std::cerr << "PSD client " << targetPSDClientId
                      << " disconnected while awaiting DecisionResponse" << std::endl;
        } else {
            std::cerr << "Timeout receiving matching DecisionResponse from PSD client "
                      << targetPSDClientId << " (waited " << timeoutMs << " ms)"
                      << std::endl;
        }
        return NVPSSD_FAIL;
    }

    *response = mb.response;
    lock.unlock();

    if (outReason != nullptr)
        *outReason = NvPSSDeliveryFailureReason::NONE;

    std::cout << "Successfully routed DecisionRequest for event type " << eventType
              << " to PSD client " << targetPSDClientId << std::endl;

    return NVPSSD_SUCCESS;
}

}
