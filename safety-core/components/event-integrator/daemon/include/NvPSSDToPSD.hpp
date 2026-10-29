/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSSD_TO_PSD_HPP
#define NVPSSD_TO_PSD_HPP

#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <errno.h>
#include <string.h>

#include "nvpsf_transport.h"
#include "pss_daemon.h"
#include "NvPSSDeliveryFailSafe.hpp"

namespace nvpss
{

class NvPSSDToPSDClient
{
private:
    /*
     * AF_UNIX SOCK_SEQPACKET endpoint replacing the raw SOCK_STREAM listener and
     * its per-client descriptor array. Owned entirely by this object; published
     * before PSSDServerThread starts and torn down only after it is joined.
     */
    NvPSFTransport* transport;
    std::atomic<bool> runningPSSDServer;
    std::thread PSSDServerThread;

    /*
     * ONE READER PER SOCKET -- this is the whole point of the class's shape, and
     * a two-reader race.
     *
     * Two threads must never read the same PSD client socket: the server's
     * select() loop and the caller's thread inside sendDecisionRequestToPSD.
     * Whichever won the race consumed the bytes. When the loop won, it read the
     * first four bytes of a DecisionResponse as a control msgType, failed to
     * match REGISTER_EVENT_TYPES or UNREGISTER_PSD_CLIENT, and disconnected a
     * perfectly healthy client -- closing the descriptor while the sender was
     * still inside recv() on it.
     *
     * Now PSSDServerThread is the only reader of anything. A sender transmits
     * its DecisionRequest and then parks on this mailbox; the server thread
     * reads the DecisionResponse and hands it over. No second reader exists, so
     * the race has no participants rather than being synchronised.
     *
     * This is also why the demux no longer needs MSG_PEEK. SOCK_SEQPACKET
     * preserves message boundaries, so one Recv yields exactly one whole
     * message and its length identifies it: PSDRegistrationMsg and
     * DecisionResponse are different sizes, checked by static_assert in the
     * implementation. Peeking at a type field to decide how much to read was an
     * artifact of the byte-stream socket and is gone with it.
     */
    struct ResponseMailbox
    {
        bool             armed{false};      /* a sender is parked on this slot */
        uint32_t         requestId{0};      /* what it is waiting for */
        bool             have{false};       /* response delivered */
        bool             peerLost{false};   /* client vanished; stop waiting */
        DecisionResponse response{};
    };
    ResponseMailbox mailbox[MAX_PSD_CLIENTS];

    /*
     * Guards mailbox[] only.
     *
     * A LEAF, and it must never be held together with clientMapMtx. The two
     * protect disjoint state and are always taken one at a time: the sender
     * resolves its target under clientMapMtx, releases, then arms its mailbox;
     * the server thread updates one or the other but never both at once. Nesting
     * them would create a second lock order to reason about on a safety path
     * for no benefit.
     */
    mutable std::mutex mailboxMtx;
    std::condition_variable mailboxCv;

    /*
     * Guards clientIdToSocket and eventTypeToClient, and nothing else.
     *
     * Both maps are mutated by PSSDServerThread (accept, registration,
     * disconnect) and read by the PSS sender thread inside
     * sendDecisionRequestToPSD. Without this lock an insert or erase can rehash
     * while the sender is mid-find, which on a safety path can misroute a
     * DecisionRequest to the wrong PSD client or fault on a dangling bucket.
     *
     * Lock order: this is a LEAF. sendDecisionRequestToPSD is called with
     * SafetyEventManager::psdSendMtx_ already held, so the order is
     * psdSendMtx_ -> clientMapMtx. Never acquire another lock while holding
     * this one, and never hold it across blocking I/O: the sender blocks in
     * send/recv for up to pss_to_psd_response_timeout_ms, and blocking there
     * would stall accepts and registrations for the same duration. Copy the
     * descriptor out under the lock, release, then do I/O. Iterators must not
     * outlive the critical section either, for the same rehash reason.
     *
     * PSDclientSockets is deliberately NOT covered: it is touched only by
     * PSSDServerThread, and by stopPSSDServer after that thread is joined.
     */
    mutable std::mutex clientMapMtx;

    /* clientId -> transport peer. Replaces clientIdToSocket; NvPSFPeerId carries
     * a generation counter, so an identity naming a recycled slot is detected
     * rather than acted on. */
    std::unordered_map<uint32_t, NvPSFPeerId> clientIdToPeer;

    // Map which maps each event_type to the PSD Client's clientId
    std::unordered_map<EventType, uint32_t> eventTypeToClient;

    NvPSSDErr runPSSDServer();
    /**
     * @param clientId Server-established identity of the sender, resolved from
     *        the authenticated transport peer. Routing must never use
     *        msg.clientId, which the client controls.
     */
    NvPSSDErr handlePSDEventTypeRegistration(uint32_t clientId,
                                            const PSDRegistrationMsg& msg);
    NvPSSDErr handlePSDClientDisconnection(uint32_t clientId);

    /** Allocate a clientId for a newly connected peer. Returns false when full. */
    bool admitPeer(NvPSFPeerId peer, uint32_t* outClientId);
    /** Resolve a peer back to its clientId. Caller holds clientMapMtx. */
    bool clientIdForPeerLocked(NvPSFPeerId peer, uint32_t* outClientId) const;
    /** Hand a received DecisionResponse to whichever sender is waiting for it. */
    void deliverResponse(uint32_t clientId, const DecisionResponse& response);
    /** Wake any sender parked on @p clientId because its client is gone. */
    void failWaiter(uint32_t clientId);

public:
    NvPSSDToPSDClient();
    ~NvPSSDToPSDClient();

    NvPSSDErr initializePSSDServer();
    NvPSSDErr startPSSDServer();
    NvPSSDErr stopPSSDServer();

    /*
     * @param outReason  Optional. On failure, receives WHY the send failed, so
     *                   the audit record can tell a response timeout apart from
     *                   a refusal to send. Every failure exit sets it.
     *
     * The distinction exists here and nowhere else: both outcomes collapse to
     * NVPSSD_FAIL at the return, and only this function knows which happened.
     * Widening NvPSSDErr instead was rejected -- it is shared across the daemon,
     * so one link's vocabulary would leak into every other caller.
     */
    NvPSSDErr sendDecisionRequestToPSD(const DecisionRequest& request,
                                       DecisionResponse* response,
                                       uint32_t timeoutMs,
                                       NvPSSDeliveryFailureReason* outReason = nullptr);
};

}

#endif
