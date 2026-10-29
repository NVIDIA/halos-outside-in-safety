/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nvpsf_transport.h
 * @brief Message-oriented inter-process transport for PSF links.
 *
 * One API over two transports: AF_UNIX SOCK_SEQPACKET and UDP. The API is
 * message-oriented rather than stream-oriented, because both preserve message
 * boundaries. Two consequences the caller can rely on:
 *
 *  - Recv returns exactly one whole message or an error. A partial message is
 *    not a reachable outcome, so no caller needs a recvAll loop.
 *  - Send transmits one whole message or reports WOULD_BLOCK. A partial send is
 *    not a reachable outcome, so no caller needs a sendAll loop.
 *
 * @par Bounded blocking
 * Send is always non-blocking at the syscall level. Waiting happens only in
 * NvPSFTransportPoll, which takes an explicit timeout. There is no call in this
 * API through which a caller can block indefinitely.
 *
 * @par Caller lock-order obligation
 * The transport's internal descriptor mutex is a leaf lock: this library never
 * invokes a caller callback while holding it, so it can never acquire a caller's
 * lock. The obligation is therefore **per entry point**, not blanket.
 *
 * A blanket "never call this API under your own lock" rule would be unsafe, not
 * merely conservative: obeying it literally means dropping your lock before
 * Send, which opens a window for a concurrent Close to free the handle
 * mid-send. Consult the table instead.
 *
 * | Call | Under a caller lock? |
 * | --- | --- |
 * | `NvPSFTransportSend` | **Permitted.** Non-blocking at the syscall level (`MSG_DONTWAIT`, and every socket is `SOCK_NONBLOCK`), acquires only the transport's own leaf mutex, and makes no callback. Holding a caller lock across it is in fact the simplest way to keep the handle alive against a concurrent Close. |
 * | `NvPSFTransportRecv` | **Permitted**, for the same reasons. `recvmsg` uses `MSG_DONTWAIT`. |
 * | `NvPSFTransportPoll` | **Not permitted** while holding a lock any send path also takes. Poll is the ONLY call in this API that blocks, for up to its timeout, and it would stall every concurrent Send for that whole period. |
 * | `NvPSFTransportDropPeer` | **Not permitted.** It tears down peer state, and reclaim logic naturally sits inside a locked region -- which is what makes this the easy one to get wrong. Snapshot the peer set under your lock, release it, then call DropPeer. |
 * | `NvPSFTransportClose` | **Not permitted.** It frees the handle. No other thread may be inside any call on it, which is a stronger requirement than a lock can express. |
 *
 * The distinction that matters: Send and Recv are bounded and side-effect-free
 * with respect to the caller's state; Poll blocks; DropPeer and Close destroy
 * state other callers may be using. Treat the last three as the rule and the
 * first two as safe.
 *
 * @par Descriptors
 * Descriptors stay private to the transport object. No function returns or
 * accepts a raw descriptor, so a caller cannot outlive-use one.
 *
 * @par Portability
 * Linux only. Depends on SO_PEERCRED, struct ucred, AF_UNIX SOCK_SEQPACKET,
 * MSG_NOSIGNAL, and SOCK_CLOEXEC.
 */

#ifndef NVPSF_TRANSPORT_H
#define NVPSF_TRANSPORT_H

/*
 * This header declares the complete public surface of the PSF transport: the
 * transport-kind vocabulary and socket paths, the endpoint API, and a small set
 * of operator-facing helpers for parsing a transport name from a command line.
 *
 * The endpoint API is C, and is usable from both C and C++. The operator-facing
 * helpers return std::string and are therefore declared only when the header is
 * compiled as C++; a C consumer sees everything else unchanged.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>   /* fprintf, for the min-queue-depth override warning */
#include <stdlib.h>  /* getenv, strtol */
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Transport kinds, socket paths and link vocabulary                          */
/* ------------------------------------------------------------------------- */


/**
 * @brief Transport used to carry PSF messages over one link.
 *
 * Values are explicit and stable. Not every kind is valid on every link; each
 * link declares the kinds it supports and rejects the rest at startup.
 *
 * These enumerators must not be placed on the wire. Convert to and from a
 * fixed-width integer at any serialization boundary and range-check against
 * NVPSF_TRANSPORT_KIND_COUNT before casting back.
 */
typedef enum NvPSFTransportKind_t
{
    /**
     * AF_UNIX SOCK_SEQPACKET. The default and the only kind the two upstream
     * links accept.
     */
    NVPSF_TRANSPORT_UNIX_SOCKET = 0,
    /** AF_INET SOCK_DGRAM. Gateway -> SDM link only. */
    NVPSF_TRANSPORT_UDP_SOCKET  = 1,
    /** One past the highest kind. Sizes dispatch tables; not a selectable value. */
    NVPSF_TRANSPORT_KIND_COUNT  = 2
} NvPSFTransportKind;

/**
 * @brief Operator-facing name of @a kind, for logs and diagnostics.
 * @return A static string; "INVALID" when @a kind is out of range. Never NULL,
 *         so callers may log the result without a null check.
 */
static inline const char *NvPSFTransportKindName(NvPSFTransportKind kind)
{
    switch (kind)
    {
        case NVPSF_TRANSPORT_UNIX_SOCKET:   return "UNIX_SOCKET";
        case NVPSF_TRANSPORT_UDP_SOCKET:    return "UDP_SOCKET";
        case NVPSF_TRANSPORT_KIND_COUNT:    break;
        default:                            break;
    }
    return "INVALID";
}

/**
 * @brief Which PSF link an endpoint serves.
 *
 * Lives here beside the kind enum rather than in nvpsf_transport.h because the
 * two are one vocabulary: a transport selection is only meaningful paired with
 * the link it applies to, and configuration parsers need both before any
 * endpoint exists.
 */
typedef enum NvPSFTransportLink_t
{
    /** mdx-client and other libnvpss clients to the PSS daemon. */
    NVPSF_LINK_PSS_RPC = 0,
    /** PSS daemon to the PSD gateway. */
    NVPSF_LINK_PSS_TO_PSD = 1,
    /** PSD gateway to SDM. */
    NVPSF_LINK_GATEWAY_TO_SDM = 2,
    /** Number of links. Sizes dispatch tables; not a selectable value. */
    NVPSF_LINK_COUNT = 3
} NvPSFTransportLink;

/**
 * @brief Whether @a link can carry @a kind.
 *
 * The single definition of the availability matrix. A selection outside it is a
 * fatal startup error everywhere, never a silent downgrade, because a downgrade
 * would move safety traffic onto a different path than the operator configured
 * with no indication.
 *
 * Deliberately a header-only inline so configuration parsers and test tooling
 * can validate a selection without linking libnvpsf_transport. Duplicating the
 * matrix in those callers instead would let the copies drift apart, which is the
 * failure this placement prevents.
 *
 * @return 1 when supported, 0 when not or when either argument is out of range.
 */
static inline int NvPSFTransportLinkSupportsKind(NvPSFTransportLink link,
                                                 NvPSFTransportKind kind)
{
    /*
     * Only the gateway link offers a choice; the two upstream links are
     * UNIX_SOCKET unconditionally.
     *
     * UNIX_SOCKET is 0: it is the default, and the only kind two of the three
     * links accept.
     *
     * ABI NOTE. These enumerator values are not stable across PSF releases.
     * They are never placed on the wire, never read as a number from
     * configuration, and are parsed by name on the command line, so the only
     * exposure is a binary built against one release linked against another.
     * Build and ship all PSF components together.
     */
    /*                                    UNIX  UDP */
    static const unsigned char table[NVPSF_LINK_COUNT][NVPSF_TRANSPORT_KIND_COUNT] = {
        /* NVPSF_LINK_PSS_RPC        */ {  1,   0  },
        /* NVPSF_LINK_PSS_TO_PSD     */ {  1,   0  },
        /* NVPSF_LINK_GATEWAY_TO_SDM */ {  1,   1  }
    };

    if ((int)link < 0 || (int)link >= (int)NVPSF_LINK_COUNT)
    {
        return 0;
    }
    if ((int)kind < 0 || (int)kind >= (int)NVPSF_TRANSPORT_KIND_COUNT)
    {
        return 0;
    }
    return (table[link][kind] != 0U) ? 1 : 0;
}

/**
 * @brief Operator-facing name of @a link, for diagnostics.
 * @return A static string; "INVALID" when out of range. Never NULL.
 */
static inline const char *NvPSFTransportLinkName(NvPSFTransportLink link)
{
    switch (link)
    {
        case NVPSF_LINK_PSS_RPC:        return "PSS_RPC";
        case NVPSF_LINK_PSS_TO_PSD:     return "PSS_TO_PSD";
        case NVPSF_LINK_GATEWAY_TO_SDM: return "GATEWAY_TO_SDM";
        case NVPSF_LINK_COUNT:          break;
        default:                        break;
    }
    return "INVALID";
}

/**
 * Runtime directory holding every PSF AF_UNIX socket. Created by tmpfiles.d
 * (see pkg/deb/create_debian_package.sh) and bind-mounted into containers.
 * Mode 1777: the sticky bit stops one endpoint from
 * unlinking another's socket, but grants no confidentiality, so each socket
 * sets its own mode after bind and validates SO_PEERCRED.
 */
#define NVPSF_RUNTIME_DIR "/run/nvpsf"

/**
 * PSD gateway to SDM endpoint. Server: nvpsd_gateway.
 * Defined here so packaging, launcher, and both endpoints agree on one string;
 * the gateway and its SDM clients begin using it when UNIX_SOCKET support
 * lands on that link.
 *
 * The PSS daemon's two socket paths are deliberately not here. They live in
 * components/event-integrator/daemon/include/pss_daemon.h, which many
 * components already include; moving them would force every one of those
 * components to acquire this header's include path for no benefit. Each path
 * string is still defined exactly once.
 */
#define NVPSF_SOCKET_PATH_PSD_GATEWAY NVPSF_RUNTIME_DIR "/nvpsd_gateway"

/**
 * @brief Receive-buffer floor for the gateway -> SDM link, in whole messages.
 *
 * DERIVED, NOT CHOSEN:
 *
 *     depth = ceil((MAIN_LOOP_POLL_TIMEOUT_MS + T_service_worst) / T_emit_min)
 *           = ceil((50 + 0.146) / 0.1460) = 344, x1.5 margin = 516
 *
 * with T_service_worst and T_emit_min measured on x86-lin64 against the real
 * SDM decision path and the real gateway respectively.
 *
 * This link and no other. On PSS -> PSD and the PSS RPC the depth stays 0:
 * a failed send there surfaces as a retried or failed call, whereas
 * here it burns an already-allocated sequence number, which the SDM cannot
 * distinguish from a lost message -- so it latches into fault-safe lockout
 * needing a validated manual release. That asymmetry is the whole reason the
 * floor exists.
 *
 * The byte requirement is 516 x sizeof(NvPSDGatewayDecisionRequestPacket),
 * computed from the real message size at runtime rather than hardcoded. That
 * size fell from 2125 to 1621 bytes when SafetyEvent's 64-byte ruleIdentifier
 * was replaced by a 1-byte cause, so the requirement dropped from 1096500 to
 * 836436. Deployments already provisioning 1096500 remain correct and
 * over-provisioned; there is no need to lower a working sysctl.
 *
 * A shortfall is a FATAL startup error, not a warning, and the transport names
 * the granted value, the required value and the sysctl to change.
 */
#define NVPSF_GATEWAY_SDM_MIN_QUEUE_DEPTH 516U

/**
 * @brief Provisioned depth for the gateway -> SDM link, honouring a
 *        deployment override.
 *
 * Returns NVPSF_GATEWAY_SDM_MIN_QUEUE_DEPTH unless NVPSF_MIN_QUEUE_DEPTH is set
 * in the environment, in which case that value is used and a warning naming the
 * risk is printed.
 *
 * WHY AN OVERRIDE EXISTS AT ALL, since a lower value re-opens exactly the
 * spurious-lockout path this floor closes:
 *
 * The floor is a statement about what the HOST has been provisioned for, not a
 * safety dial. A host whose net.core.rmem_max cannot reach 1096500 -- every
 * stock distribution default is far below it -- cannot start the gateway or
 * either SDM at all once this lands, and that includes every build and test
 * machine. Without an explicit, loud override the only way to run any
 * integration test would be root access to change a kernel parameter, which
 * turns an unprovisioned CI box into a hard stop rather than a warning.
 *
 * It is deliberately an environment variable and not a config key or CLI flag:
 * it must be conspicuous, it must not look like normal operator tuning, and it
 * must not be reachable from nvpss.conf. Startup-only and read once, so it
 * is therefore immutable once Open returns.
 *
 * Production must NOT set it. Release documentation states the sysctl instead.
 */
static inline uint32_t NvPSFGatewaySdmMinQueueDepth(void)
{
    const char *override = getenv("NVPSF_MIN_QUEUE_DEPTH");
    if (override != NULL && override[0] != '\0')
    {
        /*
         * endptr, not a discarded NULL. strtol("abc") returns 0 and sets no
         * error the caller can see without it, so the range test below passed
         * and NVPSF_MIN_QUEUE_DEPTH=abc silently selected depth 0 -- no floor
         * at all on the one link where a dropped DecisionRequest latches an SDM
         * into lockout. The "not a usable depth" branch was unreachable for
         * anything non-numeric, which is the input most likely to be a typo.
         *
         * Requires the whole string to be consumed and non-empty, so trailing
         * junk ("64x") is rejected rather than truncated to 64.
         */
        char       *end = NULL;
        const long  v   = strtol(override, &end, 10);
        if (end != override && end != NULL && *end == '\0' &&
            v >= 0 && v <= 1000000L)
        {
            fprintf(stderr,
                    "nvpsf_transport: WARNING gateway->SDM minQueueDepth overridden "
                    "to %ld by NVPSF_MIN_QUEUE_DEPTH (derived value is %u). A depth "
                    "below the derived value re-opens the backpressure path that "
                    "latches an SDM into fault-safe lockout on a dropped "
                    "DecisionRequest. This must not be set in production.\n",
                    v, (unsigned)NVPSF_GATEWAY_SDM_MIN_QUEUE_DEPTH);
            return (uint32_t)v;
        }
        fprintf(stderr,
                "nvpsf_transport: NVPSF_MIN_QUEUE_DEPTH=\"%s\" is not a usable "
                "depth; using the derived value %u\n",
                override, (unsigned)NVPSF_GATEWAY_SDM_MIN_QUEUE_DEPTH);
    }
    return NVPSF_GATEWAY_SDM_MIN_QUEUE_DEPTH;
}

/* ------------------------------------------------------------------------- */
/* Endpoint API                                                               */
/* ------------------------------------------------------------------------- */


/** Opaque endpoint handle. */
typedef struct NvPSFTransport NvPSFTransport;

/**
 * @brief Opaque peer identity: slot index in the low 16 bits, generation
 *        counter in the high 16 bits.
 *
 * The generation makes a stale identity detectable rather than dangerous: a
 * deferred disconnect that names a slot which has since been recycled fails the
 * generation check and is ignored, instead of acting on the new occupant.
 * Callers must treat the value as opaque and compare only for equality.
 */
typedef uint32_t NvPSFPeerId;

/**
 * No peer. Not a collidable value: slot indices are capped well below 0xFFFF
 * by NVPSF_TRANSPORT_MAX_PEERS_LIMIT, so no live identity can equal it.
 */
#define NVPSF_PEER_INVALID ((NvPSFPeerId)UINT32_MAX)

/** Hard upper bound on peers per endpoint, independent of configuration. */
#define NVPSF_TRANSPORT_MAX_PEERS_LIMIT 64U

/** Largest message any link may carry. Bounds every internal buffer. */
#define NVPSF_TRANSPORT_MAX_MESSAGE_BYTES 65536U

typedef enum NvPSFTransportRole_t
{
    NVPSF_ROLE_SERVER = 0,
    NVPSF_ROLE_CLIENT = 1
} NvPSFTransportRole;

typedef enum NvPSFTransportErr_t
{
    NVPSF_TRANSPORT_OK = 0,
    /** Backpressure. The caller applies its own drop policy; nothing was sent. */
    NVPSF_TRANSPORT_WOULD_BLOCK,
    /** Peer disconnected or was never connected. Fault evidence, per LT-7. */
    NVPSF_TRANSPORT_PEER_GONE,
    /** Message exceeded the buffer or MSG_TRUNC was set. A protocol error. */
    NVPSF_TRANSPORT_TRUNCATED,
    NVPSF_TRANSPORT_TIMEOUT,
    /** Peer credential rejected. No slot was allocated. */
    NVPSF_TRANSPORT_DENIED,
    NVPSF_TRANSPORT_INVALID_ARG,
    NVPSF_TRANSPORT_FAIL
} NvPSFTransportErr;

/*
 * There is deliberately no severity lane in this API. No supported transport
 * can express a severity. Priority is arbitrated
 * entirely above the transport, in the sending thread's own critical and
 * operational queues, so a lane argument here could only be accepted and
 * ignored -- and an ignored severity argument on a safety path invites a caller
 * to believe it did something.
 */

/** Validated peer credential, captured once at accept/connect time. */
typedef struct NvPSFPeerCred_t
{
    pid_t pid;
    uid_t uid;
    gid_t gid;
} NvPSFPeerCred;

typedef enum NvPSFTransportEventKind_t
{
    NVPSF_EVENT_PEER_CONNECTED = 0,
    NVPSF_EVENT_PEER_GONE = 1,
    NVPSF_EVENT_MESSAGE_READY = 2
} NvPSFTransportEventKind;

typedef struct NvPSFTransportEvent_t
{
    NvPSFTransportEventKind kind;
    NvPSFPeerId             peer;
} NvPSFTransportEvent;

/**
 * @brief Endpoint configuration. Startup-only; nothing here is mutable after
 *        NvPSFTransportOpen returns.
 *
 * Only the fields belonging to @c kind are read. String fields are copied
 * during Open, so the caller may free them afterwards.
 */
typedef struct NvPSFTransportCfg_t
{
    NvPSFTransportKind kind;
    NvPSFTransportRole role;
    NvPSFTransportLink link;
    /** Peers this endpoint may hold. Forced to 1 for every client role. */
    uint32_t           maxPeers;
    /** Largest frame on this link. Bounds every internal buffer. */
    size_t             maxMessageBytes;
    /**
     * Whole messages the transport buffer must hold. 0 disables the check.
     *
     * Non-zero only on the gateway -> SDM link, because that is the only link
     * where a failed send burns a sequence number and therefore latches the
     * SDM into fault-safe lockout. Elsewhere a send failure surfaces as a
     * retried or failed call, so a depth floor buys nothing.
     */
    uint32_t           minQueueDepth;
    uint32_t           recvTimeoutMs;
    uint32_t           sendTimeoutMs;

    /* NVPSF_TRANSPORT_UNIX_SOCKET */
    const char *unixPath;
    /** Applied with chmod() after bind(); the 1777 parent grants no protection. */
    mode_t      unixMode;
    /** SO_PEERCRED allowlist. Peers outside this group are refused. */
    gid_t       allowedGid;

    /* NVPSF_TRANSPORT_UDP_SOCKET */
    const char *inetAddr;
    uint16_t    inetPort;
    /**
     * Source-address allowlist for UDP registration. **Required for a UDP
     * server**; ignored for every other kind and role.
     *
     * Comma-separated IPv4 addresses or CIDR blocks, e.g.
     * `"127.0.0.1/32"` or `"10.0.4.0/24,10.0.5.7"`. A bare address means /32.
     * At most NVPSF_ALLOWLIST_MAX entries.
     *
     * @par Why this is mandatory rather than optional
     * UDP carries no credential, so SO_PEERCRED -- the check that authenticates
     * registration on the UNIX_SOCKET path -- has nothing to inspect. A
     * datagram's source address is the only identity the kind offers. Without a
     * bound on which sources may register, any peer that can route to this port
     * can occupy a slot and be sent DecisionRequests.
     *
     * Making it optional would have closed the gap only for operators who
     * happened to set it, which is the same as not closing it. An endpoint that
     * cannot say who is allowed to register cannot be started; the message names
     * the flag. Same fail-closed shape as the minQueueDepth shortfall and the
     * retired-config-key rejection.
     *
     * This bounds *who* may register. It is not authentication: a source address
     * on a routed network can be spoofed, and an allowlisted host that is itself
     * compromised is still allowlisted. UNIX_SOCKET remains the authenticated
     * option and the recommended deployment.
     */
    const char *inetAllowList;
} NvPSFTransportCfg;

/**
 * @brief Create an endpoint.
 *
 * Fails rather than degrades. An unsupported kind for @c cfg->link, a path too
 * long for sun_path, or a buffer the kernel will not grant all return an error
 * with a diagnostic on stderr; the caller is expected to treat any failure here
 * as fatal at startup.
 *
 * @param[in]  cfg Configuration. Not retained.
 * @param[out] out Receives the handle on success; set to NULL on failure.
 */
NvPSFTransportErr NvPSFTransportOpen(const NvPSFTransportCfg *cfg,
                                     NvPSFTransport **out);

/**
 * @brief Destroy an endpoint, closing every descriptor and unlinking a bound
 *        AF_UNIX path.
 *
 * Call only after every thread that uses @p t has been joined. NULL is a no-op.
 */
NvPSFTransportErr NvPSFTransportClose(NvPSFTransport *t);

/**
 * @brief Wait up to @p timeoutMs for peer or message activity.
 *
 * Accepts pending connections, validating credentials, and reports the results
 * as events. Returns NVPSF_TRANSPORT_TIMEOUT with @c *outCount == 0 when
 * nothing happened.
 *
 * @par A faulted UDP socket returns FAIL, not TIMEOUT
 * When the shared datagram socket is in an error state -- most often ICMP
 * port-unreachable from a peer that is not listening -- Poll consumes the
 * pending error, names it on stderr and returns NVPSF_TRANSPORT_FAIL. It does
 * not report TIMEOUT: poll() does not block while an error is pending, so a
 * caller told "nothing happened" would spin.
 *
 * @par A disconnected peer with data still queued reports MESSAGE_READY first
 * A peer that sends and then closes delivers both in one wakeup. Poll reports
 * the readable data and holds the slot; PEER_GONE follows on a later Poll, or
 * as Recv's return code, once the queue is empty. Draining the peer is
 * therefore what surfaces the disconnect: a caller that acts on events but
 * never calls Recv never observes PEER_GONE.
 *
 * @param[out] events    Caller-provided array.
 * @param[in]  maxEvents Capacity of @p events.
 * @param[out] outCount  Events written.
 */
NvPSFTransportErr NvPSFTransportPoll(NvPSFTransport *t, uint32_t timeoutMs,
                                     NvPSFTransportEvent *events,
                                     size_t maxEvents, size_t *outCount);

/**
 * @brief Receive exactly one whole message.
 *
 * With several peers ready, selection is round-robin from the slot after the
 * one last serviced, and one call yields at most one message. Lowest-slot-first
 * would let a chatty peer inside the allowed group starve the others; on the
 * gateway link that would mean registrations and heartbeat acknowledgements
 * from healthy SDMs go unserviced and those SDMs get reclaimed as dead.
 *
 * @param[out] fromPeer Sender. May be NULL if the caller does not need it.
 * @param[out] outLen   Bytes written to @p buf.
 * @retval NVPSF_TRANSPORT_TRUNCATED @p bufLen was too small; the message has
 *         been discarded by the kernel and cannot be re-read.
 * @retval NVPSF_TRANSPORT_WOULD_BLOCK No message was ready.
 */
NvPSFTransportErr NvPSFTransportRecv(NvPSFTransport *t, NvPSFPeerId *fromPeer,
                                     void *buf, size_t bufLen, size_t *outLen);

/**
 * @brief Send one whole message. Never blocks.
 *
 * @retval NVPSF_TRANSPORT_WOULD_BLOCK Buffer full; nothing was sent. On the
 *         gateway link the caller must treat this as fault evidence, not a
 *         transient, because the buffer is provisioned for the worst legitimate
 *         burst.
 */
NvPSFTransportErr NvPSFTransportSend(NvPSFTransport *t, NvPSFPeerId toPeer,
                                     const void *buf, size_t len);

/**
 * @brief Identity of the single peer held by a non-server endpoint.
 *
 * Required for a client to send first, which every PSF client does: an SDM
 * registers with the gateway, the PSS daemon initiates to PSD, and an RPC client
 * issues a request. A peer id embeds a generation counter that is assigned
 * internally, so it cannot be constructed by the caller, and without this
 * accessor a client could only learn its own peer after *receiving* a message --
 * making the send-first direction unreachable.
 *
 * Valid for a client of any kind. A server has many peers and learns each from
 * NVPSF_EVENT_PEER_CONNECTED or from Recv, so the question does not apply and
 * the call is refused rather than answered arbitrarily.
 *
 * @param[out] outPeer Receives the peer id; NVPSF_PEER_INVALID on failure.
 * @retval NVPSF_TRANSPORT_INVALID_ARG @p t is a socket server.
 * @retval NVPSF_TRANSPORT_PEER_GONE   The connection has since dropped.
 */
NvPSFTransportErr NvPSFTransportSolePeer(NvPSFTransport *t,
                                         NvPSFPeerId *outPeer);

/**
 * @brief Read the credential captured when @p peer was admitted.
 *
 * Authorization must use this, never a field from a client-controlled message
 * payload.
 */
NvPSFTransportErr NvPSFTransportPeerCred(NvPSFTransport *t, NvPSFPeerId peer,
                                         NvPSFPeerCred *outCred);

/**
 * @brief Disconnect @p peer and free its slot.
 *
 * Must be called outside any caller lock. A stale identity whose generation no
 * longer matches returns NVPSF_TRANSPORT_PEER_GONE without touching the slot's
 * current occupant.
 */
NvPSFTransportErr NvPSFTransportDropPeer(NvPSFTransport *t, NvPSFPeerId peer);

/**
 * @brief Messages dropped for @p peer because the buffer was full.
 *
 * Exists so a caller can log backpressure as fault evidence with a count
 * attached, rather than reporting each WOULD_BLOCK in isolation.
 */
NvPSFTransportErr NvPSFTransportPeerDropCount(NvPSFTransport *t,
                                              NvPSFPeerId peer,
                                              uint64_t *outDrops);

/** @brief Human-readable name for @p err. Never NULL. */
const char *NvPSFTransportErrName(NvPSFTransportErr err);

#ifdef __cplusplus
}   /* extern "C" */

/* ------------------------------------------------------------------------- */
/* Operator-facing helpers (C++ only)                                         */
/* ------------------------------------------------------------------------- */

#include <iostream>
#include <string>

namespace nvpsf {

/** Outcome of reading a transport kind from operator text. */
enum class TransportParse
{
    /** Recognised and current: UNIX_SOCKET or UDP_SOCKET. */
    Ok,
    /** Anything else. */
    Unknown
};

/**
 * @brief Read a transport kind by its operator-facing name.
 *
 * Deliberately silent: it classifies and leaves the diagnostic to the caller, so
 * the same classification can be reported to std::cerr by a CLI, to NvPSB by the
 * daemon, or asserted by a test without a spurious message. selectTransportKind()
 * is the wrapper that does report, and is what CLI code should call.
 *
 * @param[in]  text Operator-supplied value; compared exactly, no case folding.
 * @param[out] out  Written only when the result is @c Ok.
 */
inline TransportParse parseTransportKind(const std::string& text,
                                         NvPSFTransportKind& out)
{
    if (text == "UDP_SOCKET") {
        out = NVPSF_TRANSPORT_UDP_SOCKET;
        return TransportParse::Ok;
    }
    if (text == "UNIX_SOCKET") {
        out = NVPSF_TRANSPORT_UNIX_SOCKET;
        return TransportParse::Ok;
    }
    return TransportParse::Unknown;
}

/**
 * @brief The kinds @p link is *designed* to carry, comma-separated.
 *
 * Derived from the availability matrix on every call rather than written out,
 * so it can never drift from the matrix.
 *
 * This is the design statement. For anything an operator reads, use
 * selectableKindsFor() instead — see the note there.
 */
inline std::string supportedKindsFor(NvPSFTransportLink link)
{
    std::string list;

    for (int k = 0; k < static_cast<int>(NVPSF_TRANSPORT_KIND_COUNT); ++k) {
        const NvPSFTransportKind kind = static_cast<NvPSFTransportKind>(k);
        if (NvPSFTransportLinkSupportsKind(link, kind) != 0) {
            if (!list.empty()) {
                list += ", ";
            }
            list += NvPSFTransportKindName(kind);
        }
    }
    return list;
}

/**
 * @brief The kinds an operator can select on @p link.
 *
 * Identical to supportedKindsFor() while every designed kind is also offered to
 * operators. It stays a separate name because it answers a different question:
 * usage text and the "accepted values are ..." tail of each refusal call this
 * one, so should selection ever narrow below the design matrix, it narrows in
 * one place and every operator-facing string follows.
 */
inline std::string selectableKindsFor(NvPSFTransportLink link)
{
    std::string list;

    for (int k = 0; k < static_cast<int>(NVPSF_TRANSPORT_KIND_COUNT); ++k) {
        const NvPSFTransportKind kind = static_cast<NvPSFTransportKind>(k);
        if (NvPSFTransportLinkSupportsKind(link, kind) != 0) {
            if (!list.empty()) {
                list += ", ";
            }
            list += NvPSFTransportKindName(kind);
        }
    }
    return list;
}

/**
 * @brief Handle one `--<flag> KIND` option end to end.
 *
 * Parses, reports the deprecation alias, refuses a retired or unrecognised kind,
 * and refuses a kind the link cannot carry. Every refusal is fatal to the
 * caller and never a downgrade.
 *
 * @param[in]  flagName Flag as the operator typed it, e.g. `--gateway_transport`,
 *                      so the diagnostic names the option they actually used.
 * @param[in]  text     The value argument.
 * @param[in]  link     Link the flag configures.
 * @param[out] out      Written only on success.
 * @return true when @p out holds a usable selection.
 */
inline bool selectTransportKind(const char* flagName,
                                const std::string& text,
                                NvPSFTransportLink link,
                                NvPSFTransportKind& out)
{
    NvPSFTransportKind kind = NVPSF_TRANSPORT_UDP_SOCKET;
    const TransportParse parsed = parseTransportKind(text, kind);

    switch (parsed) {
        case TransportParse::Ok:
            break;
        case TransportParse::Unknown:
            std::cerr << "error: " << flagName << ": unknown transport '" << text
                      << "'; accepted values are " << selectableKindsFor(link)
                      << "\n";
            return false;
    }

    if (NvPSFTransportLinkSupportsKind(link, kind) == 0) {
        std::cerr << "error: " << flagName << ": "
                  << NvPSFTransportKindName(kind) << " is not available on link "
                  << NvPSFTransportLinkName(link) << "; accepted values are "
                  << selectableKindsFor(link) << "\n";
        return false;
    }


    out = kind;
    return true;
}

}   // namespace nvpsf
#endif /* __cplusplus */

#endif /* NVPSF_TRANSPORT_H */
