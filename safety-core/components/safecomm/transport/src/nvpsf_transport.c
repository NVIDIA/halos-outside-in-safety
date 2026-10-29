/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nvpsf_transport.c
 * @brief Message-oriented transport over UDP and AF_UNIX SOCK_SEQPACKET.
 *
 * _GNU_SOURCE must precede every include: SO_PEERCRED, struct ucred, and
 * accept4() are GNU extensions.
 *
 * @par Peer model per kind
 * - UNIX_SOCKET server: one listener plus one descriptor per accepted peer.
 *   Identity comes from the connection and its validated SO_PEERCRED.
 * - UNIX_SOCKET client: one connected descriptor in slot 0.
 * - UDP_SOCKET: one shared datagram socket. A server learns each peer's address
 *   from the first datagram it sends and fans out with sendto(), because that is
 *   the only identity UDP offers. Address-derived identity is weaker than a
 *   validated credential, which is why UDP registration stays a documented
 *   accepted risk rather than something this layer pretends to solve.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "nvpsf_transport.h"

/** Invalid descriptor sentinel. Never 0: descriptor 0 is valid. */
#define NVPSF_FD_INVALID (-1)

/** Slot index occupies the low 16 bits of a peer id, generation the high 16. */
#define NVPSF_ID_FIELD_MASK 0xFFFFU

/** Generation values start here; 0 is skipped so a zeroed peer id is invalid. */
#define NVPSF_GEN_FIRST 1U

/**
 * Accept-rate window. Enforced per endpoint rather than per credential:
 * every admitted peer shares the allowlisted GID, so a credential does not
 * distinguish one caller from another, and a per-credential counter would
 * either rate-limit all peers as one or never trip. What actually needs
 * bounding is slot churn on the listener, which is what this caps.
 */
/** Maximum entries in a UDP source allowlist. */
#define NVPSF_ALLOWLIST_MAX 8U

#define NVPSF_ACCEPT_WINDOW_MS 1000U
#define NVPSF_ACCEPT_PER_WINDOW_PER_PEER 4U

/** Accepts examined in one Poll call, so one Poll cannot run unbounded. */
#define NVPSF_ACCEPTS_PER_POLL 8U

/**
 * Linux charges socket buffer accounting overhead on top of payload and reports
 * the doubled figure back from getsockopt, so sizing compares against twice the
 * payload requirement.
 */
#define NVPSF_SO_BUF_ACCOUNTING_FACTOR 2

/** Bound on the poll() array: every peer, plus the listener or the UDP socket. */
#define NVPSF_POLL_SLOTS (NVPSF_TRANSPORT_MAX_PEERS_LIMIT + 1U)

typedef struct
{
    int           fd;          /**< Per-peer descriptor; NVPSF_FD_INVALID when unused. */
    uint32_t      generation;  /**< Bumped on every allocation. */
    int           inUse;
    int           credValid;
    NvPSFPeerCred cred;
    uint64_t      dropCount;
    /* UDP only: the peer's address, learned on first receipt or configured. */
    struct sockaddr_in addr;
    int                addrValid;
} NvPSFPeerSlot;

struct NvPSFTransport
{
    NvPSFTransportKind kind;
    NvPSFTransportRole role;
    NvPSFTransportLink link;

    uint32_t maxPeers;
    size_t   maxMessageBytes;
    uint32_t minQueueDepth;
    uint32_t recvTimeoutMs;
    uint32_t sendTimeoutMs;

    pthread_mutex_t mtx;   /**< Leaf lock. No caller callback runs under it. */
    int             mtxReady;

    NvPSFPeerSlot *peers;
    uint32_t       rrCursor;   /**< Round-robin receive cursor. */

    /* UNIX_SOCKET */
    int   listenFd;
    char  unixPath[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int   pathBound;      /**< This endpoint bound the path and must unlink it. */
    gid_t allowedGid;

    /* UDP_SOCKET: one socket shared by every peer. */
    int udpFd;

    /* UDP source allowlist. Server + UDP only; empty otherwise. */
    struct { uint32_t netBE; uint32_t maskBE; } allow[NVPSF_ALLOWLIST_MAX];
    uint32_t allowCount;
    uint8_t  allowDenyReported;   /* one diagnostic per accept window */

    /* Endpoint-wide accept rate window. */
    uint64_t acceptWindowStartMs;
    uint32_t acceptWindowCount;
    /* One admission-denial diagnostic per window; see admissionDenyFirstInWindow. */
    uint8_t  acceptWindowDenyReported;
};

const char *NvPSFTransportErrName(NvPSFTransportErr err)
{
    switch (err)
    {
        case NVPSF_TRANSPORT_OK:          return "OK";
        case NVPSF_TRANSPORT_WOULD_BLOCK: return "WOULD_BLOCK";
        case NVPSF_TRANSPORT_PEER_GONE:   return "PEER_GONE";
        case NVPSF_TRANSPORT_TRUNCATED:   return "TRUNCATED";
        case NVPSF_TRANSPORT_TIMEOUT:     return "TIMEOUT";
        case NVPSF_TRANSPORT_DENIED:      return "DENIED";
        case NVPSF_TRANSPORT_INVALID_ARG: return "INVALID_ARG";
        case NVPSF_TRANSPORT_FAIL:        break;
        default:                          break;
    }
    return "FAIL";
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static uint64_t monotonicMs(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0U;
    }
    return ((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U);
}

static NvPSFPeerId peerIdMake(uint32_t slot, uint32_t generation)
{
    return (NvPSFPeerId)(((generation & NVPSF_ID_FIELD_MASK) << 16) |
                         (slot & NVPSF_ID_FIELD_MASK));
}

static uint32_t peerIdSlot(NvPSFPeerId id)
{
    return (uint32_t)(id & NVPSF_ID_FIELD_MASK);
}

static uint32_t peerIdGeneration(NvPSFPeerId id)
{
    return (uint32_t)((id >> 16) & NVPSF_ID_FIELD_MASK);
}

/**
 * Resolve a peer id to a live slot. Caller must hold t->mtx.
 *
 * A generation mismatch means the identity is stale and the slot has since been
 * recycled; returning NULL keeps a deferred disconnect from acting on the new
 * occupant instead of the peer the caller meant.
 */
static NvPSFPeerSlot *slotFor(NvPSFTransport *t, NvPSFPeerId id)
{
    uint32_t slot;

    if (id == NVPSF_PEER_INVALID)
    {
        return NULL;
    }
    slot = peerIdSlot(id);
    if (slot >= t->maxPeers)
    {
        return NULL;
    }
    if (!t->peers[slot].inUse)
    {
        return NULL;
    }
    if (t->peers[slot].generation != peerIdGeneration(id))
    {
        return NULL;
    }
    return &t->peers[slot];
}

/** Close a slot's descriptor and mark it free. Caller must hold t->mtx. */
static void slotRelease(NvPSFPeerSlot *s)
{
    if (s->fd != NVPSF_FD_INVALID)
    {
        (void)close(s->fd);
        s->fd = NVPSF_FD_INVALID;
    }
    s->inUse = 0;
    s->credValid = 0;
    s->addrValid = 0;
    memset(&s->cred, 0, sizeof(s->cred));
    memset(&s->addr, 0, sizeof(s->addr));
    /* dropCount survives deliberately, so per-peer backpressure history is not
     * erased by a reconnect. */
}

/** Allocate a free slot. Caller must hold t->mtx. Returns NULL when full. */
static NvPSFPeerSlot *slotAcquire(NvPSFTransport *t, uint32_t *outSlot)
{
    uint32_t i;

    for (i = 0U; i < t->maxPeers; i++)
    {
        if (!t->peers[i].inUse)
        {
            t->peers[i].generation++;
            if ((t->peers[i].generation & NVPSF_ID_FIELD_MASK) == 0U)
            {
                t->peers[i].generation = NVPSF_GEN_FIRST;
            }
            t->peers[i].inUse = 1;
            t->peers[i].fd = NVPSF_FD_INVALID;
            *outSlot = i;
            return &t->peers[i];
        }
    }
    return NULL;
}

/** Find a UDP peer by address. Caller must hold t->mtx. */
static NvPSFPeerSlot *slotFindByAddr(NvPSFTransport *t,
                                     const struct sockaddr_in *addr,
                                     uint32_t *outSlot)
{
    uint32_t i;

    for (i = 0U; i < t->maxPeers; i++)
    {
        if (t->peers[i].inUse && t->peers[i].addrValid &&
            t->peers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            t->peers[i].addr.sin_port == addr->sin_port)
        {
            *outSlot = i;
            return &t->peers[i];
        }
    }
    return NULL;
}

/**
 * Endpoint-wide accept/admission rate check. Caller must hold t->mtx.
 * @return 1 when within budget, 0 when the window is exhausted.
 */
static int admissionRateAllows(NvPSFTransport *t)
{
    uint64_t nowMs = monotonicMs();
    uint32_t budget;

    if ((nowMs - t->acceptWindowStartMs) >= (uint64_t)NVPSF_ACCEPT_WINDOW_MS)
    {
        t->acceptWindowStartMs = nowMs;
        t->acceptWindowCount = 0U;
        t->acceptWindowDenyReported = 0U;
        t->allowDenyReported = 0U;
    }
    budget = t->maxPeers * NVPSF_ACCEPT_PER_WINDOW_PER_PEER;
    if (t->acceptWindowCount >= budget)
    {
        return 0;
    }
    t->acceptWindowCount++;
    return 1;
}

/**
 * Parse a comma-separated IPv4 allowlist ("a.b.c.d" or "a.b.c.d/len") into t.
 *
 * @return 0 on success, -1 on a malformed entry or too many entries. Every
 *         failure names the offending text: an operator who mistypes a CIDR on
 *         a safety link must not have to guess which entry was wrong.
 */
static int allowListParse(NvPSFTransport *t, const char *spec)
{
    const char *p = spec;

    t->allowCount = 0U;
    while (*p != '\0')
    {
        char     entry[64];
        size_t   n = 0U;
        char    *slash;
        uint32_t prefix = 32U;
        struct in_addr a;

        while (*p == ' ' || *p == ',') { p++; }
        if (*p == '\0') { break; }
        while (*p != '\0' && *p != ',' && n < (sizeof(entry) - 1U))
        {
            entry[n++] = *p++;
        }
        entry[n] = '\0';
        if (*p != '\0' && *p != ',')
        {
            fprintf(stderr, "nvpsf_transport: allowlist entry too long: %s...\n", entry);
            return -1;
        }
        while (n > 0U && entry[n - 1U] == ' ') { entry[--n] = '\0'; }
        if (n == 0U) { continue; }

        if (t->allowCount >= NVPSF_ALLOWLIST_MAX)
        {
            fprintf(stderr, "nvpsf_transport: allowlist has more than %u entries\n",
                    (unsigned)NVPSF_ALLOWLIST_MAX);
            return -1;
        }

        slash = strchr(entry, '/');
        if (slash != NULL)
        {
            char *end = NULL;
            long  v;
            *slash = '\0';
            v = strtol(slash + 1, &end, 10);
            if (end == slash + 1 || end == NULL || *end != '\0' || v < 0 || v > 32)
            {
                fprintf(stderr, "nvpsf_transport: allowlist prefix length must be "
                                "0..32: %s/%s\n", entry, slash + 1);
                return -1;
            }
            prefix = (uint32_t)v;
        }
        if (inet_pton(AF_INET, entry, &a) != 1)
        {
            fprintf(stderr, "nvpsf_transport: allowlist entry is not an IPv4 "
                            "address: %s\n", entry);
            return -1;
        }
        /* Host-order mask, then back to network order, so /0 and /32 are both
         * expressible without a shift by the type width (which is UB). */
        {
            const uint32_t maskHost = (prefix == 0U)
                                          ? 0U
                                          : (0xFFFFFFFFU << (32U - prefix));
            t->allow[t->allowCount].maskBE = htonl(maskHost);
            t->allow[t->allowCount].netBE  = a.s_addr & htonl(maskHost);
            t->allowCount++;
        }
    }

    if (t->allowCount == 0U)
    {
        fprintf(stderr, "nvpsf_transport: allowlist is empty after parsing \"%s\"\n",
                spec);
        return -1;
    }
    return 0;
}

/**
 * Whether a datagram source is allowed to register. Caller holds t->mtx.
 *
 * Only consulted for a UDP server; every other configuration has a stronger
 * check (SO_PEERCRED) or no unknown senders at all.
 */
static int allowListPermits(const NvPSFTransport *t, const struct sockaddr_in *src)
{
    uint32_t i;

    for (i = 0U; i < t->allowCount; i++)
    {
        if ((src->sin_addr.s_addr & t->allow[i].maskBE) == t->allow[i].netBE)
        {
            return 1;
        }
    }
    return 0;
}

/**
 * Answer whether an admission denial should emit a diagnostic. Returns 1 for the
 * first denial in each rate window and 0 thereafter. Caller must hold t->mtx and
 * must have just had admissionRateAllows() return 0, so the window state is
 * current.
 *
 * Counting denials would not work as the limiter: admissionRateAllows() stops
 * incrementing once the budget is reached, so acceptWindowCount sits at exactly
 * the budget for the rest of the window and every denial looks like the first
 * one. A first attempt at this rate limit tested acceptWindowCount == budget and
 * therefore printed on every denial while claiming the opposite -- worse than no
 * limit, because the claim would have been believed.
 */
static int admissionDenyFirstInWindow(NvPSFTransport *t)
{
    if (t->acceptWindowDenyReported != 0U)
    {
        return 0;
    }
    t->acceptWindowDenyReported = 1U;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Buffer provisioning                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Enforce @p depth whole messages on one socket buffer option.
 *
 * Only half the contract is locally enforceable: on SOCK_SEQPACKET and UDP a
 * sender is throttled by the *receiver's* SO_RCVBUF, which lives in the peer
 * process and cannot be set or read from here. Each endpoint therefore
 * validates its own buffer, and a shortfall is fatal rather than a warning,
 * because on the gateway link an unprovisioned buffer makes WOULD_BLOCK
 * reachable under ordinary load, and every occurrence burns a sequence number
 * and latches the SDM into lockout that needs physical manual release.
 *
 * @return 0 on success, -1 on shortfall or syscall failure.
 */
static int provisionSocketBuffer(int fd, int optName, const char *optLabel,
                                 uint32_t depth, size_t msgBytes)
{
    const size_t maxPayload = (size_t)(INT_MAX / NVPSF_SO_BUF_ACCOUNTING_FACTOR);
    size_t       requiredPayload;
    int          requested;
    int          granted = 0;
    socklen_t    len = (socklen_t)sizeof(granted);

    if (depth == 0U)
    {
        return 0;
    }
    if (msgBytes == 0U || (size_t)depth > (maxPayload / msgBytes))
    {
        fprintf(stderr,
                "nvpsf_transport: %s sizing for %u messages of %zu bytes exceeds "
                "the representable range\n", optLabel, depth, msgBytes);
        return -1;
    }
    requiredPayload = (size_t)depth * msgBytes;
    requested = (int)requiredPayload * NVPSF_SO_BUF_ACCOUNTING_FACTOR;

    if (setsockopt(fd, SOL_SOCKET, optName, &requested, sizeof(requested)) != 0)
    {
        fprintf(stderr, "nvpsf_transport: setsockopt(%s, %d) failed: %s\n",
                optLabel, requested, strerror(errno));
        return -1;
    }
    /* Read back rather than trust the set: the kernel silently clamps to
     * net.core.{r,w}mem_max, so a granted value below the requirement means the
     * depth guarantee does not hold no matter what was requested. */
    if (getsockopt(fd, SOL_SOCKET, optName, &granted, &len) != 0)
    {
        fprintf(stderr, "nvpsf_transport: getsockopt(%s) failed: %s\n",
                optLabel, strerror(errno));
        return -1;
    }
    if (granted < requested)
    {
        fprintf(stderr,
                "nvpsf_transport: %s granted %d bytes but %d are required for %u "
                "messages of %zu bytes; raise net.core.%smem_max\n",
                optLabel, granted, requested, depth, msgBytes,
                (optName == SO_RCVBUF) ? "r" : "w");
        return -1;
    }
    return 0;
}

static int provisionBuffers(int fd, const NvPSFTransportCfg *cfg)
{
    if (provisionSocketBuffer(fd, SO_RCVBUF, "SO_RCVBUF", cfg->minQueueDepth,
                              cfg->maxMessageBytes) != 0)
    {
        return -1;
    }
    return provisionSocketBuffer(fd, SO_SNDBUF, "SO_SNDBUF", cfg->minQueueDepth,
                                 cfg->maxMessageBytes);
}

/* ------------------------------------------------------------------------- */
/* Credentials                                                               */
/* ------------------------------------------------------------------------- */

/**
 * Read the connected peer's credential and check it against the allowlist.
 *
 * Used in both directions: a server validates each accepted client, and a
 * client validates the server after connect. The client-side check closes a
 * real hole rather than being symmetry for its own sake -- /run/nvpsf stays
 * mode 1777, so any local process can create a socket at an unclaimed path and
 * impersonate a PSF server.
 *
 * LOGGING RULE, narrowed rather than broken.
 *
 * uid and pid are still never logged: they identify a local principal and
 * naming them in a denial leaks which process was probing. GID is different and
 * is now reported on a mismatch, because it is not an identity -- it is the
 * CONFIGURED CONTRACT value, the thing the integrator sets, and the expected
 * side of it is simply this process's own getgid().
 *
 * Without it a container GID mapping error is indistinguishable from any other
 * connect failure, and the integrator has nothing to debug from: "not in the
 * allowed group" says a mismatch happened but not which mapping is wrong. That
 * is exactly what must be diagnosable, and reporting two group
 * numbers is a far smaller disclosure than leaving a deployment unable to tell
 * a misconfiguration from a dead socket.
 *
 * @return 0 when allowed, -1 when refused or unreadable.
 */
static int checkPeerCred(int fd, gid_t allowedGid, NvPSFPeerCred *out)
{
    struct ucred uc;
    socklen_t    len = (socklen_t)sizeof(uc);

    memset(&uc, 0, sizeof(uc));
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) != 0)
    {
        return -1;
    }
    if (len != (socklen_t)sizeof(uc))
    {
        return -1;
    }
    if (uc.gid != allowedGid)
    {
        /* gid only -- never uid or pid. See the logging rule above. */
        fprintf(stderr,
                "nvpsf_transport: peer GID %u does not match the required PSF "
                "group %u. Every PSF process must share one GID; check the "
                "container UID/GID mapping across the boundary this link "
                "crosses.\n",
                (unsigned)uc.gid, (unsigned)allowedGid);
        return -1;
    }
    out->pid = uc.pid;
    out->uid = uc.uid;
    out->gid = uc.gid;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Configuration validation                                                  */
/* ------------------------------------------------------------------------- */

static NvPSFTransportErr validateCfg(const NvPSFTransportCfg *cfg)
{
    if (cfg == NULL)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if ((int)cfg->kind < 0 || (int)cfg->kind >= (int)NVPSF_TRANSPORT_KIND_COUNT)
    {
        fprintf(stderr, "nvpsf_transport: invalid transport kind %d\n", (int)cfg->kind);
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    /*
     * A UDP server must say who may register.
     *
     * UDP carries no credential, so the SO_PEERCRED check that authenticates
     * registration on the UNIX_SOCKET path has nothing to inspect, and a
     * datagram's source address is the only identity available. Refusing to
     * start is the same fail-closed shape as a minQueueDepth shortfall: an
     * endpoint that cannot state its own admission policy does not run.
     *
     * Clients are exempt -- a client connect()s to one peer and never admits
     * anyone.
     */
    if (cfg->kind == NVPSF_TRANSPORT_UDP_SOCKET &&
        cfg->role == NVPSF_ROLE_SERVER &&
        (cfg->inetAllowList == NULL || cfg->inetAllowList[0] == '\0'))
    {
        fprintf(stderr,
                "nvpsf_transport: a UDP server requires inetAllowList. UDP "
                "registration is unauthenticated -- there is no peer credential to "
                "check -- so the set of source addresses permitted to register must "
                "be stated explicitly. Example: \"127.0.0.1/32\" for a loopback "
                "test, or the SDM subnet in deployment. Prefer UNIX_SOCKET, which "
                "authenticates registration with SO_PEERCRED.\n");
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if ((int)cfg->link < 0 || (int)cfg->link >= (int)NVPSF_LINK_COUNT)
    {
        fprintf(stderr, "nvpsf_transport: invalid link id %d\n", (int)cfg->link);
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (cfg->role != NVPSF_ROLE_SERVER && cfg->role != NVPSF_ROLE_CLIENT)
    {
        fprintf(stderr, "nvpsf_transport: invalid role %d\n", (int)cfg->role);
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (!NvPSFTransportLinkSupportsKind(cfg->link, cfg->kind))
    {
        fprintf(stderr,
                "nvpsf_transport: transport %s is not supported on link %s; "
                "refusing to start\n",
                NvPSFTransportKindName(cfg->kind),
                NvPSFTransportLinkName(cfg->link));
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (cfg->maxMessageBytes == 0U ||
        cfg->maxMessageBytes > (size_t)NVPSF_TRANSPORT_MAX_MESSAGE_BYTES)
    {
        fprintf(stderr, "nvpsf_transport: maxMessageBytes %zu out of range (1..%u)\n",
                cfg->maxMessageBytes, NVPSF_TRANSPORT_MAX_MESSAGE_BYTES);
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (cfg->maxPeers == 0U || cfg->maxPeers > NVPSF_TRANSPORT_MAX_PEERS_LIMIT)
    {
        fprintf(stderr, "nvpsf_transport: maxPeers %u out of range (1..%u)\n",
                cfg->maxPeers, NVPSF_TRANSPORT_MAX_PEERS_LIMIT);
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    return NVPSF_TRANSPORT_OK;
}

/** Copy a required path/name into a fixed buffer, refusing to truncate. */
static int copyBounded(char *dst, size_t dstSize, const char *src,
                       const char *label)
{
    size_t n;

    if (src == NULL)
    {
        fprintf(stderr, "nvpsf_transport: %s is required\n", label);
        return -1;
    }
    n = strlen(src);
    if (n == 0U || n >= dstSize)
    {
        fprintf(stderr, "nvpsf_transport: %s length %zu does not fit in %zu bytes\n",
                label, n, dstSize);
        return -1;
    }
    memcpy(dst, src, n + 1U);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Open helpers                                                              */
/* ------------------------------------------------------------------------- */

/**
 * Bind an AF_UNIX SOCK_SEQPACKET listener.
 *
 * Order is fixed: validate length, unlink stale, bind, chmod, listen. chmod is
 * required rather than cosmetic -- the 1777 parent directory grants no
 * protection, so the socket's own mode is the only thing narrowing who may
 * reach it before SO_PEERCRED is consulted.
 */
static NvPSFTransportErr openUnixServer(NvPSFTransport *t,
                                        const NvPSFTransportCfg *cfg)
{
    struct sockaddr_un addr;
    int                fd;
    int                backlog;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* t->unixPath is sized to sun_path and copyBounded already refused an
     * overlong path, so this cannot truncate. */
    memcpy(addr.sun_path, t->unixPath, strlen(t->unixPath) + 1U);

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        fprintf(stderr, "nvpsf_transport: socket(AF_UNIX, SOCK_SEQPACKET) failed: %s\n",
                strerror(errno));
        return NVPSF_TRANSPORT_FAIL;
    }

    /*
     * PROBE BEFORE UNLINKING.
     *
     * This used to unlink(path) unconditionally and then bind, which recovers a
     * stale socket after an unclean exit -- and also lets a SECOND server
     * silently STEAL a live path from a running one. Every client connecting
     * afterwards reached the impostor while the original sat holding a socket
     * nobody could route to, and both endpoints looked healthy from their own
     * side. design.md section 14 says a second instance "fails at bind rather
     * than stealing the path"; the code did the opposite, and
     * v3_failure_modes_driver caught it.
     *
     * Bind first. Only on EADDRINUSE ask whether anyone is actually there:
     *   connect succeeds     -> a LIVE server owns it. Refuse to start.
     *   connect ECONNREFUSED -> stale socket from an unclean exit; unlink and
     *                           retry, which is the recovery case.
     *   anything else        -> refuse. An unknown state on a safety path is
     *                           not something to resolve by force.
     */
    if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
    {
        int bindErrno = errno;

        if (bindErrno != EADDRINUSE)
        {
            /* Naming the path is permitted here under the fatal-startup
             * carve-out: without it, a missing /run/nvpsf is
             * indistinguishable from a permissions problem. */
            fprintf(stderr, "nvpsf_transport: bind(%s) failed: %s\n",
                    t->unixPath, strerror(bindErrno));
            (void)close(fd);
            return NVPSF_TRANSPORT_FAIL;
        }

        {
            const int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            int       live = 0;
            int       stale = 0;

            if (probe >= 0)
            {
                if (connect(probe, (const struct sockaddr *)&addr,
                            (socklen_t)sizeof(addr)) == 0)
                {
                    live = 1;
                }
                else if (errno == ECONNREFUSED)
                {
                    stale = 1;
                }
                (void)close(probe);
            }

            if (live)
            {
                fprintf(stderr,
                        "nvpsf_transport: bind(%s) refused -- a LIVE server is "
                        "already listening on this path. Refusing to start "
                        "rather than unlinking it: taking the path would "
                        "silently redirect every future client to this process "
                        "and strand the running one. Stop the other "
                        "instance first.\n", t->unixPath);
                (void)close(fd);
                return NVPSF_TRANSPORT_FAIL;
            }
            if (!stale)
            {
                fprintf(stderr,
                        "nvpsf_transport: bind(%s) failed with EADDRINUSE and "
                        "the path could not be probed; refusing to force it\n",
                        t->unixPath);
                (void)close(fd);
                return NVPSF_TRANSPORT_FAIL;
            }
        }

        /* Stale socket from an unclean exit -- the recovery case. */
        if (unlink(t->unixPath) != 0 && errno != ENOENT)
        {
            fprintf(stderr, "nvpsf_transport: unlink(%s) failed: %s\n",
                    t->unixPath, strerror(errno));
            (void)close(fd);
            return NVPSF_TRANSPORT_FAIL;
        }
        if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
        {
            fprintf(stderr,
                    "nvpsf_transport: bind(%s) failed after clearing a stale "
                    "socket: %s\n", t->unixPath, strerror(errno));
            (void)close(fd);
            return NVPSF_TRANSPORT_FAIL;
        }
    }
    t->pathBound = 1;

    if (chmod(t->unixPath, cfg->unixMode) != 0)
    {
        fprintf(stderr, "nvpsf_transport: chmod(%s, %o) failed: %s\n",
                t->unixPath, (unsigned)cfg->unixMode, strerror(errno));
        (void)close(fd);
        return NVPSF_TRANSPORT_FAIL;
    }
    if (provisionBuffers(fd, cfg) != 0)
    {
        (void)close(fd);
        return NVPSF_TRANSPORT_FAIL;
    }

    /* Backlog covers every peer, so a simultaneous cold start of all of them
     * cannot overflow the queue and turn startup into a retry storm. */
    backlog = (int)cfg->maxPeers;
    if (listen(fd, backlog) != 0)
    {
        fprintf(stderr, "nvpsf_transport: listen(backlog=%d) failed: %s\n",
                backlog, strerror(errno));
        (void)close(fd);
        return NVPSF_TRANSPORT_FAIL;
    }

    t->listenFd = fd;
    return NVPSF_TRANSPORT_OK;
}

/**
 * Connect an AF_UNIX SOCK_SEQPACKET client and validate the server credential.
 *
 * A non-blocking AF_UNIX connect does not behave like TCP: when the listener's
 * backlog is full it fails immediately with EAGAIN rather than returning
 * EINPROGRESS and completing later. There is nothing to wait for writability
 * on, so EAGAIN is reported as WOULD_BLOCK for the caller's reconnect cadence
 * to retry. A poll-for-writability retry written against the TCP idiom would
 * wait forever here.
 */
static NvPSFTransportErr openUnixClient(NvPSFTransport *t,
                                        const NvPSFTransportCfg *cfg)
{
    struct sockaddr_un addr;
    NvPSFPeerCred      cred;
    NvPSFPeerSlot     *s;
    uint32_t           slot = 0U;
    int                fd;

    memset(&addr, 0, sizeof(addr));
    memset(&cred, 0, sizeof(cred));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, t->unixPath, strlen(t->unixPath) + 1U);

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        fprintf(stderr, "nvpsf_transport: socket(AF_UNIX, SOCK_SEQPACKET) failed: %s\n",
                strerror(errno));
        return NVPSF_TRANSPORT_FAIL;
    }
    if (provisionBuffers(fd, cfg) != 0)
    {
        (void)close(fd);
        return NVPSF_TRANSPORT_FAIL;
    }

    if (connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
    {
        int err = errno;

        (void)close(fd);
        if (err == EAGAIN || err == EWOULDBLOCK)
        {
            return NVPSF_TRANSPORT_WOULD_BLOCK;
        }
        if (err == EPROTOTYPE)
        {
            /* Name the socket-type mismatch: otherwise this presents as a
             * generic connect failure and gets misdiagnosed as a missing
             * daemon rather than a version skew. */
            fprintf(stderr,
                    "nvpsf_transport: connect(%s) refused with EPROTOTYPE -- the "
                    "server listens with a different socket type; the PSF package "
                    "and application versions must match\n", t->unixPath);
            return NVPSF_TRANSPORT_FAIL;
        }
        if (err == ENOENT || err == ECONNREFUSED)
        {
            return NVPSF_TRANSPORT_PEER_GONE;
        }
        fprintf(stderr, "nvpsf_transport: connect(%s) failed: %s\n",
                t->unixPath, strerror(err));
        return NVPSF_TRANSPORT_FAIL;
    }

    if (checkPeerCred(fd, t->allowedGid, &cred) != 0)
    {
        fprintf(stderr, "nvpsf_transport: server credential rejected; refusing to "
                        "treat the connection as trusted\n");
        (void)close(fd);
        return NVPSF_TRANSPORT_DENIED;
    }

    s = slotAcquire(t, &slot);
    if (s == NULL)
    {
        (void)close(fd);
        return NVPSF_TRANSPORT_FAIL;
    }
    s->fd = fd;
    s->cred = cred;
    s->credValid = 1;
    return NVPSF_TRANSPORT_OK;
}

/**
 * Open a UDP endpoint.
 *
 * A client connect()s even though the socket is a datagram socket, so the
 * kernel drops datagrams from anyone but the expected peer; that is cheaper and
 * harder to get wrong than checking the source address on every receive. A
 * server binds instead, and learns each peer's address as it arrives.
 */
static NvPSFTransportErr openUdp(NvPSFTransport *t, const NvPSFTransportCfg *cfg)
{
    struct sockaddr_in addr;
    int                fd;

    if (t->role == NVPSF_ROLE_SERVER &&
        allowListParse(t, cfg->inetAllowList) != 0)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (cfg->inetAddr == NULL)
    {
        fprintf(stderr, "nvpsf_transport: inetAddr is required for UDP_SOCKET\n");
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (cfg->inetPort == 0U)
    {
        fprintf(stderr, "nvpsf_transport: inetPort 0 is not a valid port\n");
        return NVPSF_TRANSPORT_INVALID_ARG;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->inetPort);
    if (inet_pton(AF_INET, cfg->inetAddr, &addr.sin_addr) != 1)
    {
        fprintf(stderr, "nvpsf_transport: inetAddr is not a valid IPv4 address\n");
        return NVPSF_TRANSPORT_INVALID_ARG;
    }

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        fprintf(stderr, "nvpsf_transport: socket(AF_INET, SOCK_DGRAM) failed: %s\n",
                strerror(errno));
        return NVPSF_TRANSPORT_FAIL;
    }
    if (provisionBuffers(fd, cfg) != 0)
    {
        (void)close(fd);
        return NVPSF_TRANSPORT_FAIL;
    }

    if (cfg->role == NVPSF_ROLE_SERVER)
    {
        if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
        {
            fprintf(stderr, "nvpsf_transport: UDP bind failed: %s\n", strerror(errno));
            (void)close(fd);
            return NVPSF_TRANSPORT_FAIL;
        }
    }
    else
    {
        NvPSFPeerSlot *s;
        uint32_t       slot = 0U;

        if (connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
        {
            fprintf(stderr, "nvpsf_transport: UDP connect failed: %s\n",
                    strerror(errno));
            (void)close(fd);
            return NVPSF_TRANSPORT_FAIL;
        }
        s = slotAcquire(t, &slot);
        if (s == NULL)
        {
            (void)close(fd);
            return NVPSF_TRANSPORT_FAIL;
        }
        s->addr = addr;
        s->addrValid = 1;
        /* UDP carries no credential. Left invalid so PeerCred reports the
         * absence rather than handing back a zeroed struct that could be
         * mistaken for a validated identity. */
        s->credValid = 0;
    }

    t->udpFd = fd;
    return NVPSF_TRANSPORT_OK;
}

/* ------------------------------------------------------------------------- */
/* Open / Close                                                              */
/* ------------------------------------------------------------------------- */

NvPSFTransportErr NvPSFTransportOpen(const NvPSFTransportCfg *cfg,
                                     NvPSFTransport **out)
{
    NvPSFTransport   *t;
    NvPSFTransportErr err;
    uint32_t          i;

    if (out == NULL)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    *out = NULL;

    err = validateCfg(cfg);
    if (err != NVPSF_TRANSPORT_OK)
    {
        return err;
    }

    t = (NvPSFTransport *)calloc(1U, sizeof(*t));
    if (t == NULL)
    {
        return NVPSF_TRANSPORT_FAIL;
    }

    t->kind = cfg->kind;
    t->role = cfg->role;
    t->link = cfg->link;
    t->maxPeers = cfg->maxPeers;
    if (cfg->role == NVPSF_ROLE_CLIENT)
    {
        /* A client holds exactly one connection, so the slot table cannot be
         * oversized into a false capacity. A UDP server keeps its configured
         * capacity, because it does fan out to many SDMs. */
        t->maxPeers = 1U;
    }
    t->maxMessageBytes = cfg->maxMessageBytes;
    t->minQueueDepth = cfg->minQueueDepth;
    t->recvTimeoutMs = cfg->recvTimeoutMs;
    t->sendTimeoutMs = cfg->sendTimeoutMs;
    t->allowedGid = cfg->allowedGid;
    t->listenFd = NVPSF_FD_INVALID;
    t->udpFd = NVPSF_FD_INVALID;
    t->acceptWindowStartMs = monotonicMs();

    t->peers = (NvPSFPeerSlot *)calloc((size_t)t->maxPeers, sizeof(*t->peers));
    if (t->peers == NULL)
    {
        free(t);
        return NVPSF_TRANSPORT_FAIL;
    }
    for (i = 0U; i < t->maxPeers; i++)
    {
        t->peers[i].fd = NVPSF_FD_INVALID;
    }

    if (pthread_mutex_init(&t->mtx, NULL) != 0)
    {
        free(t->peers);
        free(t);
        return NVPSF_TRANSPORT_FAIL;
    }
    t->mtxReady = 1;

    switch (cfg->kind)
    {
        case NVPSF_TRANSPORT_UNIX_SOCKET:
            if (copyBounded(t->unixPath, sizeof(t->unixPath), cfg->unixPath,
                            "unixPath") != 0)
            {
                err = NVPSF_TRANSPORT_INVALID_ARG;
                break;
            }
            err = (cfg->role == NVPSF_ROLE_SERVER) ? openUnixServer(t, cfg)
                                                   : openUnixClient(t, cfg);
            break;
        case NVPSF_TRANSPORT_UDP_SOCKET:
            err = openUdp(t, cfg);
            break;
        case NVPSF_TRANSPORT_KIND_COUNT:
        default:
            err = NVPSF_TRANSPORT_INVALID_ARG;
            break;
    }

    if (err != NVPSF_TRANSPORT_OK)
    {
        (void)NvPSFTransportClose(t);
        return err;
    }

    *out = t;
    return NVPSF_TRANSPORT_OK;
}

NvPSFTransportErr NvPSFTransportClose(NvPSFTransport *t)
{
    uint32_t i;

    if (t == NULL)
    {
        return NVPSF_TRANSPORT_OK;
    }

    if (t->peers != NULL)
    {
        for (i = 0U; i < t->maxPeers; i++)
        {
            if (t->peers[i].fd != NVPSF_FD_INVALID)
            {
                (void)close(t->peers[i].fd);
                t->peers[i].fd = NVPSF_FD_INVALID;
            }
        }
        free(t->peers);
        t->peers = NULL;
    }

    if (t->listenFd != NVPSF_FD_INVALID)
    {
        (void)close(t->listenFd);
        t->listenFd = NVPSF_FD_INVALID;
    }
    if (t->udpFd != NVPSF_FD_INVALID)
    {
        (void)close(t->udpFd);
        t->udpFd = NVPSF_FD_INVALID;
    }
    /* Unlink only if this endpoint bound the path, so a failed client open can
     * never remove a live server's socket. */
    if (t->pathBound)
    {
        (void)unlink(t->unixPath);
        t->pathBound = 0;
    }

    if (t->mtxReady)
    {
        (void)pthread_mutex_destroy(&t->mtx);
        t->mtxReady = 0;
    }
    free(t);
    return NVPSF_TRANSPORT_OK;
}

/* ------------------------------------------------------------------------- */
/* Accept                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Accept one pending connection. Caller must hold t->mtx.
 *
 * A refused connection is accepted and then closed rather than left queued:
 * leaving it pending would keep poll() reporting the listener readable forever
 * and spin the caller's loop.
 *
 * Order matters. The rate check and the credential check both run before a slot
 * is acquired, because an unauthorised or spinning peer must never consume
 * capacity -- otherwise a local process could occupy every slot by connecting
 * repeatedly and deny service to real SDMs.
 *
 * @return 1 when @p outEvent was filled, 0 when nothing more is pending.
 */
static int acceptOne(NvPSFTransport *t, NvPSFTransportEvent *outEvent)
{
    NvPSFPeerCred  cred;
    NvPSFPeerSlot *s;
    uint32_t       slot = 0U;
    int            fd;

    memset(&cred, 0, sizeof(cred));

    fd = accept4(t->listenFd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
    {
        return 0;
    }

    if (!admissionRateAllows(t))
    {
        /* Rate-limited for the same reason as the UDP path below: a client
         * retrying connect() in a loop would otherwise emit one line per
         * attempt, and the flood is what buries the first line that mattered. */
        if (admissionDenyFirstInWindow(t))
        {
            fprintf(stderr, "nvpsf_transport: rejected connection, endpoint "
                            "connect rate exceeded (%u per %u ms window); further "
                            "rejections in this window are not reported\n",
                            (unsigned)(t->maxPeers * NVPSF_ACCEPT_PER_WINDOW_PER_PEER),
                            (unsigned)NVPSF_ACCEPT_WINDOW_MS);
        }
        (void)close(fd);
        return 0;
    }
    if (checkPeerCred(fd, t->allowedGid, &cred) != 0)
    {
        fprintf(stderr, "nvpsf_transport: rejected connection, peer credential is "
                        "not in the allowed group\n");
        (void)close(fd);
        return 0;
    }

    s = slotAcquire(t, &slot);
    if (s == NULL)
    {
        fprintf(stderr, "nvpsf_transport: rejected connection, all %u peer slots "
                        "are occupied\n", t->maxPeers);
        (void)close(fd);
        return 0;
    }

    s->fd = fd;
    s->cred = cred;
    s->credValid = 1;

    outEvent->kind = NVPSF_EVENT_PEER_CONNECTED;
    outEvent->peer = peerIdMake(slot, s->generation);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Poll                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Is there still a whole message queued on @p fd?
 *
 * Needed because revents CANNOT answer it. AF_UNIX sets POLLIN whenever the
 * peer has shut down -- unix_poll() ORs in EPOLLIN for RCV_SHUTDOWN, not only
 * for a non-empty receive queue -- so "closed with data pending" and "closed
 * with nothing left" both arrive as exactly POLLIN|POLLHUP. Measured, both
 * cases, same bits.
 *
 * MSG_PEEK does not consume, so the message the caller is about to be told
 * about is still there for Recv. One byte is enough: SOCK_SEQPACKET preserves
 * boundaries, so a short peek reports the presence of a message without
 * disturbing it, and the discarded remainder is not a truncation of anything.
 *
 * Returns 0 on error, which includes EBADF and a shut-down empty queue (0
 * bytes). Failing towards PEER_GONE is right: the alternative is holding a slot
 * for a peer we cannot read.
 */
static int peerHasQueuedMessage(int fd)
{
    char    probe;
    ssize_t got;

    do
    {
        got = recv(fd, &probe, sizeof(probe), MSG_PEEK | MSG_DONTWAIT);
    } while (got < 0 && errno == EINTR);

    return (got > 0) ? 1 : 0;
}

NvPSFTransportErr NvPSFTransportPoll(NvPSFTransport *t, uint32_t timeoutMs,
                                     NvPSFTransportEvent *events,
                                     size_t maxEvents, size_t *outCount)
{
    struct pollfd pfds[NVPSF_POLL_SLOTS];
    uint32_t      pfdSlot[NVPSF_POLL_SLOTS];
    uint32_t      pfdGeneration[NVPSF_POLL_SLOTS];
    uint32_t      n = 0U;
    size_t        produced = 0U;
    uint32_t      i;
    int           ready;
    int           listenIndex = -1;
    int           sharedIndex = -1;   /* The shared UDP socket. */
    int           sharedError = 0;    /* Set when the shared socket faulted. */

    if (t == NULL || events == NULL || outCount == NULL || maxEvents == 0U)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    *outCount = 0U;
    if (timeoutMs > (uint32_t)INT_MAX)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }

    /* Snapshot descriptors under the lock, then poll with the lock released.
     * Polling while holding it would stall every concurrent Send for the whole
     * timeout, which is exactly the blocking-under-a-shared-lock pattern the
     * leaf-lock rule exists to prevent. */
    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }

    if (t->listenFd != NVPSF_FD_INVALID && n < NVPSF_POLL_SLOTS)
    {
        pfds[n].fd = t->listenFd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        pfdSlot[n] = 0U;
        pfdGeneration[n] = 0U;
        listenIndex = (int)n;
        n++;
    }
    if (t->udpFd != NVPSF_FD_INVALID && n < NVPSF_POLL_SLOTS)
    {
        pfds[n].fd = t->udpFd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        pfdSlot[n] = 0U;
        pfdGeneration[n] = 0U;
        sharedIndex = (int)n;
        n++;
    }
    if (t->kind == NVPSF_TRANSPORT_UNIX_SOCKET)
    {
        for (i = 0U; i < t->maxPeers && n < NVPSF_POLL_SLOTS; i++)
        {
            if (t->peers[i].inUse && t->peers[i].fd != NVPSF_FD_INVALID)
            {
                pfds[n].fd = t->peers[i].fd;
                pfds[n].events = POLLIN;
                pfds[n].revents = 0;
                pfdSlot[n] = i;
                pfdGeneration[n] = t->peers[i].generation;
                        n++;
            }
        }
    }
    (void)pthread_mutex_unlock(&t->mtx);

    if (n == 0U)
    {
        return NVPSF_TRANSPORT_TIMEOUT;
    }

    ready = poll(pfds, (nfds_t)n, (int)timeoutMs);
    if (ready < 0)
    {
        /* EINTR is reported as a timeout so callers keep one retry path. */
        return (errno == EINTR) ? NVPSF_TRANSPORT_TIMEOUT : NVPSF_TRANSPORT_FAIL;
    }
    if (ready == 0)
    {
        return NVPSF_TRANSPORT_TIMEOUT;
    }

    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }

    for (i = 0U; i < n && produced < maxEvents; i++)
    {
        if (pfds[i].revents == 0)
        {
            continue;
        }

        if (listenIndex >= 0 && i == (uint32_t)listenIndex)
        {
            uint32_t accepted = 0U;

            while (produced < maxEvents && accepted < NVPSF_ACCEPTS_PER_POLL)
            {
                if (!acceptOne(t, &events[produced]))
                {
                    break;
                }
                produced++;
                accepted++;
            }
            continue;
        }

        if (sharedIndex >= 0 && i == (uint32_t)sharedIndex)
        {
            if ((pfds[i].revents & POLLIN) != 0)
            {
                /* The shared UDP socket. A server cannot know the sender until
                 * the datagram is read, so the peer is reported as invalid and
                 * the caller learns it from Recv's fromPeer. A client has only
                 * its one counterpart, so it can be named here.
                 *
                 * Readable wins over a pending error, for the same reason the
                 * per-peer branch drains before reporting a hangup: a datagram
                 * already in the buffer is still valid. The error stays pending
                 * and is reported by the next Poll. */
                events[produced].kind = NVPSF_EVENT_MESSAGE_READY;
                events[produced].peer = (t->role == NVPSF_ROLE_CLIENT)
                                            ? peerIdMake(0U, t->peers[0].generation)
                                            : NVPSF_PEER_INVALID;
                produced++;
                continue;
            }
            if ((pfds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)
            {
                /*
                 * AN ERROR IS NOT A TIMEOUT.
                 *
                 * This branch used to fall through to `continue`, so a socket in
                 * an error state produced no events and Poll returned TIMEOUT --
                 * measured at 0.0 ms against a 500 ms budget, because poll()
                 * returns immediately while an error is pending. A caller that
                 * polls and does nothing else therefore spins at full speed
                 * while being told nothing happened.
                 *
                 * The provoking case is ordinary, not exotic: a UDP client
                 * connect()s (see udpOpen), so a gateway that is not listening
                 * yields ICMP port-unreachable and sk_err = ECONNREFUSED.
                 *
                 * getsockopt(SO_ERROR) is what CONSUMES the pending error. It is
                 * not diagnostic decoration -- without it the condition is
                 * level-triggered and would be re-reported forever. In-tree this
                 * was masked because every product caller calls Recv even when
                 * Poll says TIMEOUT, and that Recv cleared it; the contract
                 * never promised that, and relying on it made the diagnostic
                 * come out of the wrong function.
                 *
                 * Reported as FAIL rather than PEER_GONE deliberately. PEER_GONE
                 * reads better for ECONNREFUSED, but Recv already maps that
                 * errno to FAIL, and having the two entry points disagree about
                 * one condition is worse than either choice alone. PEER_GONE
                 * would also make an SDM tear down and reopen its transport
                 * whenever the gateway blinks, which is churn this link does not
                 * have today. If that mapping is ever wanted it belongs in
                 * recvOnFd() and here together, as one change with its own
                 * evidence.
                 */
                int       soErr = 0;
                socklen_t soLen = (socklen_t)sizeof(soErr);

                if (getsockopt(t->udpFd, SOL_SOCKET, SO_ERROR, &soErr, &soLen) != 0)
                {
                    soErr = errno;
                }
                fprintf(stderr,
                        "nvpsf_transport: shared UDP socket error (revents=0x%x, "
                        "SO_ERROR=%d: %s)\n",
                        (unsigned)pfds[i].revents, soErr,
                        (soErr != 0) ? strerror(soErr) : "none");
                sharedError = 1;
            }
            continue;
        }

        /* Per-peer descriptor. The snapshot was taken with the lock released, so
         * re-verify slot, descriptor and generation together: without the check,
         * a slot closed and reused in the interim could be reported under a
         * recycled identity. */
        {
            uint32_t slot = pfdSlot[i];

            if (slot >= t->maxPeers || !t->peers[slot].inUse ||
                t->peers[slot].fd != pfds[i].fd ||
                t->peers[slot].generation != pfdGeneration[i])
            {
                continue;
            }
            if ((pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            {
                /*
                 * DRAIN BEFORE REPORTING A HANGUP.
                 *
                 * A peer that sends its last message and closes puts both the
                 * data and the hangup in one revents. Releasing the slot here
                 * closes the descriptor, and the queued message goes with it --
                 * measured at three sent, zero readable. On the gateway link
                 * that message is a DecisionResponse; on the RPC link it is
                 * whatever the client asked for last.
                 *
                 * POLLNVAL is exempt: the descriptor is not valid, so there is
                 * nothing to drain and nothing to peek at.
                 *
                 * This is the same defect, and the same fix, as recvAll() in
                 * NvPSSDaemon_interface.cpp -- which was corrected first and
                 * whose reasoning was never carried back here.
                 */
                if ((pfds[i].revents & POLLNVAL) == 0 &&
                    peerHasQueuedMessage(t->peers[slot].fd))
                {
                    events[produced].kind = NVPSF_EVENT_MESSAGE_READY;
                    events[produced].peer =
                        peerIdMake(slot, t->peers[slot].generation);
                    produced++;
                    continue;
                }
                events[produced].kind = NVPSF_EVENT_PEER_GONE;
                events[produced].peer = peerIdMake(slot, t->peers[slot].generation);
                produced++;
                slotRelease(&t->peers[slot]);
                continue;
            }
            if ((pfds[i].revents & POLLIN) != 0)
            {
                events[produced].kind = NVPSF_EVENT_MESSAGE_READY;
                events[produced].peer = peerIdMake(slot, t->peers[slot].generation);
                produced++;
            }
        }
    }

    (void)pthread_mutex_unlock(&t->mtx);

    *outCount = produced;
    if (produced > 0U)
    {
        /* Events first. The shared-socket error branch only runs when that
         * socket had nothing readable, so this cannot discard a fault that has
         * no other way to reach the caller. */
        return NVPSF_TRANSPORT_OK;
    }
    return (sharedError != 0) ? NVPSF_TRANSPORT_FAIL : NVPSF_TRANSPORT_TIMEOUT;
}

/* ------------------------------------------------------------------------- */
/* Receive                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * Read one message from @p fd, optionally reporting the source address.
 *
 * recvmsg rather than recv, because only recvmsg reports MSG_TRUNC. That
 * matters: SOCK_SEQPACKET and UDP both discard the remainder of an oversized
 * message instead of returning it on the next read, so an unnoticed truncation
 * would corrupt the stream silently rather than merely delay it.
 */
static NvPSFTransportErr recvOnFd(int fd, void *buf, size_t bufLen,
                                  struct sockaddr_in *src, size_t *outLen,
                                  int isSeqPacket)
{
    struct msghdr msg;
    struct iovec  iov;
    ssize_t       got;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = bufLen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1U;
    if (src != NULL)
    {
        memset(src, 0, sizeof(*src));
        msg.msg_name = src;
        msg.msg_namelen = (socklen_t)sizeof(*src);
    }

    got = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (got < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return NVPSF_TRANSPORT_WOULD_BLOCK;
        }
        if (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE)
        {
            return NVPSF_TRANSPORT_PEER_GONE;
        }
        return NVPSF_TRANSPORT_FAIL;
    }
    if ((msg.msg_flags & MSG_TRUNC) != 0)
    {
        return NVPSF_TRANSPORT_TRUNCATED;
    }
    if (got == 0)
    {
        /* On SOCK_SEQPACKET a zero-length read means the peer shut down; PSF
         * never sends an empty message, so there is no ambiguity. On UDP a
         * zero-length datagram is malformed input, not a disconnect. */
        return isSeqPacket ? NVPSF_TRANSPORT_PEER_GONE : NVPSF_TRANSPORT_TRUNCATED;
    }
    if ((size_t)got > bufLen)
    {
        return NVPSF_TRANSPORT_TRUNCATED;
    }

    *outLen = (size_t)got;
    return NVPSF_TRANSPORT_OK;
}

/**
 * Read one datagram from the shared UDP socket and attribute it to a peer.
 * Caller must hold t->mtx.
 *
 * A server admits an unknown sender by allocating a slot, because an address is
 * the only identity UDP offers. This is the unauthenticated-registration risk
 * that the UDP path carries by decision; it is bounded here by the peer table
 * size and the admission rate window so it cannot exhaust capacity for free.
 */
static NvPSFTransportErr recvFromUdp(NvPSFTransport *t, NvPSFPeerId *fromPeer,
                                     void *buf, size_t bufLen, size_t *outLen)
{
    struct sockaddr_in src;
    NvPSFTransportErr  err;
    NvPSFPeerSlot     *s;
    uint32_t           slot = 0U;

    err = recvOnFd(t->udpFd, buf, bufLen, &src, outLen, 0);
    if (err != NVPSF_TRANSPORT_OK)
    {
        return err;
    }

    s = slotFindByAddr(t, &src, &slot);
    if (s == NULL)
    {
        if (t->role == NVPSF_ROLE_CLIENT)
        {
            /* connect() already filters foreign senders, so this is only
             * reachable if the peer address changed. Treat as unattributable. */
            return NVPSF_TRANSPORT_DENIED;
        }
        /*
         * The source must be allowlisted before anything else considers
         * admitting it. Checked BEFORE the rate limiter deliberately -- an
         * off-allowlist flood must not consume the admission budget that a
         * legitimate SDM's REGR needs, or the allowlist would turn a
         * registration attack into a denial of service against the peers it was
         * added to protect.
         */
        if (!allowListPermits(t, &src))
        {
            if (t->allowDenyReported == 0U)
            {
                char who[INET_ADDRSTRLEN] = {0};
                (void)inet_ntop(AF_INET, &src.sin_addr, who, sizeof(who));
                /* The source address IS the credential on this path, so naming
                 * it is reporting the rejected contract value -- the same
                 * narrowing already applied to the GID diagnostic. */
                fprintf(stderr,
                        "nvpsf_transport: dropped datagram from %s, source is not "
                        "in the UDP registration allowlist; further "
                        "rejections in this window are not reported\n", who);
                t->allowDenyReported = 1U;
            }
            return NVPSF_TRANSPORT_DENIED;
        }
        if (!admissionRateAllows(t))
        {
            /*
             * This rejection used to be SILENT, and it was the only one that
             * was. Its two siblings both report: the accept path prints
             * "endpoint connect rate exceeded", and the slot-exhaustion branch
             * immediately below prints "all %u peer slots are occupied". A new
             * UDP peer denied here produced nothing at all -- no stderr, no
             * syslog, and the datagram dropped -- so a legitimate SDM that
             * could not register was indistinguishable from one whose REGR
             * never arrived. That is exactly the state a harness debugging
             * session was left in with no way to tell the two apart.
             *
             * Rate-limited rather than per-message: the counter is per window,
             * so a peer retrying its REGR every 500 ms would otherwise emit a
             * line every 500 ms for as long as it kept trying.
             */
            if (admissionDenyFirstInWindow(t))
            {
                fprintf(stderr,
                        "nvpsf_transport: dropped datagram from an unknown peer, "
                        "admission rate exceeded (%u per %u ms window); further "
                        "drops in this window are not reported\n",
                        (unsigned)(t->maxPeers * NVPSF_ACCEPT_PER_WINDOW_PER_PEER),
                        (unsigned)NVPSF_ACCEPT_WINDOW_MS);
            }
            return NVPSF_TRANSPORT_DENIED;
        }
        s = slotAcquire(t, &slot);
        if (s == NULL)
        {
            fprintf(stderr, "nvpsf_transport: dropped datagram, all %u peer slots "
                            "are occupied\n", t->maxPeers);
            return NVPSF_TRANSPORT_DENIED;
        }
        s->addr = src;
        s->addrValid = 1;
        s->credValid = 0;
    }

    if (fromPeer != NULL)
    {
        *fromPeer = peerIdMake(slot, s->generation);
    }
    return NVPSF_TRANSPORT_OK;
}

NvPSFTransportErr NvPSFTransportRecv(NvPSFTransport *t, NvPSFPeerId *fromPeer,
                                     void *buf, size_t bufLen, size_t *outLen)
{
    NvPSFTransportErr err;
    uint32_t          examined;

    if (t == NULL || buf == NULL || outLen == NULL || bufLen == 0U)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    *outLen = 0U;
    if (fromPeer != NULL)
    {
        *fromPeer = NVPSF_PEER_INVALID;
    }

    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }

    if (t->kind == NVPSF_TRANSPORT_UDP_SOCKET)
    {
        err = recvFromUdp(t, fromPeer, buf, bufLen, outLen);
        (void)pthread_mutex_unlock(&t->mtx);
        return err;
    }

    /* Round-robin from the slot after the one last serviced. A lowest-slot-first
     * scan would let one chatty peer inside the allowed group starve the rest;
     * on the gateway link that means registrations and heartbeat
     * acknowledgements from healthy SDMs go unserviced and those SDMs are
     * reclaimed as dead. At most one message is returned per call, so no single
     * peer can hold the receive path. */
    for (examined = 0U; examined < t->maxPeers; examined++)
    {
        uint32_t       slot = (t->rrCursor + 1U + examined) % t->maxPeers;
        NvPSFPeerSlot *s = &t->peers[slot];

        if (!s->inUse || s->fd == NVPSF_FD_INVALID)
        {
            continue;
        }

        err = recvOnFd(s->fd, buf, bufLen, NULL, outLen, 1);
        if (err == NVPSF_TRANSPORT_WOULD_BLOCK)
        {
            continue;   /* Nothing on this peer; try the next. */
        }

        t->rrCursor = slot;
        if (fromPeer != NULL)
        {
            *fromPeer = peerIdMake(slot, s->generation);
        }
        if (err == NVPSF_TRANSPORT_PEER_GONE)
        {
            slotRelease(s);
        }
        (void)pthread_mutex_unlock(&t->mtx);
        return err;
    }

    (void)pthread_mutex_unlock(&t->mtx);
    return NVPSF_TRANSPORT_WOULD_BLOCK;
}

/* ------------------------------------------------------------------------- */
/* Send                                                                      */
/* ------------------------------------------------------------------------- */

/** Map a send errno to a transport result. Caller must hold t->mtx. */
static NvPSFTransportErr sendErrnoToResult(int err, NvPSFPeerSlot *s)
{
    if (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS || err == EINTR)
    {
        if (err != EINTR)
        {
            s->dropCount++;
        }
        return NVPSF_TRANSPORT_WOULD_BLOCK;
    }
    if (err == EPIPE || err == ECONNRESET || err == ENOTCONN ||
        err == ECONNREFUSED)
    {
        return NVPSF_TRANSPORT_PEER_GONE;
    }
    if (err == EMSGSIZE)
    {
        return NVPSF_TRANSPORT_TRUNCATED;
    }
    return NVPSF_TRANSPORT_FAIL;
}

NvPSFTransportErr NvPSFTransportSend(NvPSFTransport *t, NvPSFPeerId toPeer,
                                     const void *buf, size_t len)
{
    NvPSFTransportErr err = NVPSF_TRANSPORT_OK;
    NvPSFPeerSlot    *s;
    ssize_t           sent;

    if (t == NULL || buf == NULL || len == 0U)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (len > t->maxMessageBytes)
    {
        /* Refuse rather than let the kernel truncate: the excess is discarded,
         * so a truncated send would look successful here and arrive corrupt. */
        return NVPSF_TRANSPORT_TRUNCATED;
    }

    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }

    s = slotFor(t, toPeer);
    if (s == NULL)
    {
        (void)pthread_mutex_unlock(&t->mtx);
        return NVPSF_TRANSPORT_PEER_GONE;
    }

    /* MSG_NOSIGNAL so a vanished peer cannot kill the process with SIGPIPE;
     * MSG_DONTWAIT so the safety path never blocks regardless of how the
     * descriptor happens to be configured. */
    if (t->kind == NVPSF_TRANSPORT_UDP_SOCKET)
    {
        if (t->udpFd == NVPSF_FD_INVALID || !s->addrValid)
        {
            (void)pthread_mutex_unlock(&t->mtx);
            return NVPSF_TRANSPORT_PEER_GONE;
        }
        sent = sendto(t->udpFd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT,
                      (const struct sockaddr *)&s->addr,
                      (socklen_t)sizeof(s->addr));
    }
    else
    {
        if (s->fd == NVPSF_FD_INVALID)
        {
            (void)pthread_mutex_unlock(&t->mtx);
            return NVPSF_TRANSPORT_PEER_GONE;
        }
        sent = send(s->fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    }

    if (sent < 0)
    {
        err = sendErrnoToResult(errno, s);
    }
    else if ((size_t)sent != len)
    {
        /* Datagram and seqpacket sockets send all or nothing, so this is
         * unexpected rather than routine. Reporting success would be silent data
         * loss, so it surfaces as a protocol error. */
        err = NVPSF_TRANSPORT_TRUNCATED;
    }

    (void)pthread_mutex_unlock(&t->mtx);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Peer queries                                                              */
/* ------------------------------------------------------------------------- */

NvPSFTransportErr NvPSFTransportSolePeer(NvPSFTransport *t,
                                         NvPSFPeerId *outPeer)
{
    NvPSFTransportErr err;

    if (t == NULL || outPeer == NULL)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    *outPeer = NVPSF_PEER_INVALID;

    /* A server has many peers and learns each from an accept event or from Recv,
     * so there is no single answer to return. */
    if (t->role == NVPSF_ROLE_SERVER)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }

    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }
    if (t->peers[0].inUse)
    {
        *outPeer = peerIdMake(0U, t->peers[0].generation);
        err = NVPSF_TRANSPORT_OK;
    }
    else
    {
        err = NVPSF_TRANSPORT_PEER_GONE;
    }
    (void)pthread_mutex_unlock(&t->mtx);
    return err;
}

NvPSFTransportErr NvPSFTransportPeerCred(NvPSFTransport *t, NvPSFPeerId peer,
                                         NvPSFPeerCred *outCred)
{
    NvPSFTransportErr err;
    NvPSFPeerSlot    *s;

    if (t == NULL || outCred == NULL)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    memset(outCred, 0, sizeof(*outCred));

    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }
    s = slotFor(t, peer);
    if (s == NULL)
    {
        err = NVPSF_TRANSPORT_PEER_GONE;
    }
    else if (!s->credValid)
    {
        /* UDP carries no credential. Reported as unavailable so a caller cannot
         * mistake a zeroed struct for a validated identity. */
        err = NVPSF_TRANSPORT_DENIED;
    }
    else
    {
        *outCred = s->cred;
        err = NVPSF_TRANSPORT_OK;
    }
    (void)pthread_mutex_unlock(&t->mtx);
    return err;
}

NvPSFTransportErr NvPSFTransportDropPeer(NvPSFTransport *t, NvPSFPeerId peer)
{
    NvPSFTransportErr err;
    NvPSFPeerSlot    *s;

    if (t == NULL)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }
    s = slotFor(t, peer);
    if (s == NULL)
    {
        err = NVPSF_TRANSPORT_PEER_GONE;
    }
    else
    {
        slotRelease(s);
        err = NVPSF_TRANSPORT_OK;
    }
    (void)pthread_mutex_unlock(&t->mtx);
    return err;
}

NvPSFTransportErr NvPSFTransportPeerDropCount(NvPSFTransport *t,
                                              NvPSFPeerId peer,
                                              uint64_t *outDrops)
{
    NvPSFTransportErr err;
    NvPSFPeerSlot    *s;

    if (t == NULL || outDrops == NULL)
    {
        return NVPSF_TRANSPORT_INVALID_ARG;
    }
    *outDrops = 0U;

    if (pthread_mutex_lock(&t->mtx) != 0)
    {
        return NVPSF_TRANSPORT_FAIL;
    }
    s = slotFor(t, peer);
    if (s == NULL)
    {
        err = NVPSF_TRANSPORT_PEER_GONE;
    }
    else
    {
        *outDrops = s->dropCount;
        err = NVPSF_TRANSPORT_OK;
    }
    (void)pthread_mutex_unlock(&t->mtx);
    return err;
}
