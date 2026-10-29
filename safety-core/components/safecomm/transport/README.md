<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# `safecomm/transport` — PSF inter-process transport

`libnvpsf_transport` carries PSF messages over either of two transports behind a
single C API. It exists so that truncation detection, peer credential
validation, bounded blocking, and message-boundary preservation have exactly one
implementation to verify instead of one per call site.

## Why the API is message-oriented

Both transports preserve message boundaries: UDP by datagram, and
`AF_UNIX SOCK_SEQPACKET` by design. Two guarantees follow, and both remove
whole classes of caller-side bug:

- `NvPSFTransportRecv` returns exactly one whole message or an error, so no
  caller needs a `recvAll` loop and a short read is not a reachable state.
- `NvPSFTransportSend` transmits one whole message or reports
  `NVPSF_TRANSPORT_WOULD_BLOCK`, so no caller needs a `sendAll` loop and a
  partial send is not a reachable state.

`SOCK_STREAM` is deliberately absent. It does not preserve boundaries, which is
why the PSS RPC link migrates off it rather than being wrapped.

## Transport kinds

| Kind | Transport | Peer identity |
| --- | --- | --- |
| `NVPSF_TRANSPORT_UDP_SOCKET` | `AF_INET` `SOCK_DGRAM` | source address |
| `NVPSF_TRANSPORT_UNIX_SOCKET` | `AF_UNIX` `SOCK_SEQPACKET` | connection plus validated `SO_PEERCRED` |

## Per-link availability

A kind that a link does not support is rejected by `NvPSFTransportOpen` with
`NVPSF_TRANSPORT_INVALID_ARG`. There is no fallback: the caller is expected to
treat the failure as fatal at startup, because a silent downgrade would change
which path safety traffic takes without anyone being told.

| Link | `UDP_SOCKET` | `UNIX_SOCKET` |
| --- | --- | --- |
| `NVPSF_LINK_PSS_RPC` | no | yes |
| `NVPSF_LINK_PSS_TO_PSD` | no | yes |
| `NVPSF_LINK_GATEWAY_TO_SDM` | yes (default) | yes |

The matrix states what each link is *designed* to carry. What the tree actually
runs is narrower: every link from an MDX client through the PSS daemon and the
PSD gateway to an SDM is `UNIX_SOCKET`, selected in code and not configurable.
`UDP_SOCKET` remains supported by the library and carries the SDM egress to
`cmd_rx`.

## Selection

No binary exposes a transport flag. Each endpoint names its kind at the call to
`NvPSFTransportOpen`, so the two ends of a link cannot be configured apart, and
selection is startup-only and immutable afterwards.

`nvpsf::selectTransportKind()` remains the one place operator text becomes a
kind: it parses, checks the value against the availability matrix for the named
link, and reports. It is for tools where a transport name still arrives as text,
such as a test injector's `--transport` that labels an artifact rather than
configuring a link.

Rules it enforces:

- An invalid or unsupported-for-this-link value is **fatal**, never a fallback
  and never a downgrade. The diagnostic names the flag, the link, and the
  accepted values, and derives that list from the availability matrix so it can
  never advertise a value the matrix would refuse.
- A rejected value exits non-zero even where the value is only a label, because
  an artifact describing a configuration that cannot exist is worse than none.

## Socket paths

Each path string is defined exactly once. The two PSS daemon paths stay in
`components/event-integrator/daemon/include/pss_daemon.h`, which many
components already include; moving them here would force each of them to
acquire this header's include path just to learn a socket path.

| Path | Constant | Home |
| --- | --- | --- |
| `/run/nvpsf` | `NVPSF_RUNTIME_DIR` | `nvpsf_transport.h` |
| `/run/nvpsf/nvpssd` | `PSS_DAEMON_RPC_SOCKET_PATH` | `pss_daemon.h` |
| `/run/nvpsf/nvpssd_to_psd` | `PSS_DAEMON_SOCKET_PATH` | `pss_daemon.h` |
| `/run/nvpsf/nvpsd_gateway` | `NVPSF_SOCKET_PATH_PSD_GATEWAY` | `nvpsf_transport.h` |

`/run/nvpsf` is mode 1777. The sticky bit stops one endpoint from unlinking
another's socket, but it grants no confidentiality, so every server `chmod`s its
own socket after `bind()` and validates `SO_PEERCRED` on each peer. Directory
permissions alone are not a security boundary here.

## Caller lock-order obligation

The transport's internal descriptor mutex is a **leaf lock**, and this library
never invokes a caller callback while holding it. Keeping it a leaf is the
caller's responsibility:

> No function in `nvpsf_transport.h` may be called while the caller holds one of
> its own locks.

`NvPSFTransportDropPeer` is the easy one to get wrong, because reclaim logic
naturally sits inside a locked region. Follow the same shape as the send paths:
snapshot the peer set under your lock, release it, then call the transport.

## `minQueueDepth`

`minQueueDepth` states how many whole messages the transport buffer must hold.
`0` disables the check, which is the correct value on every link except
gateway → SDM.

The reason it is per-link rather than global: depth matters only where a failed
send **burns a sequence number**, because that is the only place a drop
escalates to a fault-safe latch requiring physical manual release. That is the
gateway → SDM envelope alone. On the other links a send failure surfaces as a
retried or failed call, so a floor there would impose a cost against no
corresponding failure mode.

Enforcement is deliberately one-sided, and callers need to understand why. On
`SOCK_SEQPACKET` and UDP a sender is throttled by the **receiver's**
`SO_RCVBUF`, which lives in the peer process and cannot be set or read from
here. Each endpoint therefore validates its own buffer with a `getsockopt`
read-back and fails fatally on a shortfall, rather than one endpoint asserting a
guarantee on the other's behalf. The kernel silently clamps to
`net.core.rmem_max` / `net.core.wmem_max`, so the read-back is the check that
matters, not the `setsockopt` return.

## Credentials

`UNIX_SOCKET` validates `SO_PEERCRED` in **both** directions against
`cfg->allowedGid`:

- A server checks each accepted peer before allocating a slot, so an
  unauthorised or rate-exceeded caller never consumes capacity. Doing it in the
  other order would let a local process exhaust every slot by connecting
  repeatedly and deny service to real SDMs.
- A client checks the server after `connect()`. This is not symmetry for its own
  sake: because `/run/nvpsf` is 1777, any local process can create a socket at
  an unclaimed path and impersonate a PSF server.

Credentials are captured once and bound to the slot for its lifetime.
Authorization must read the stored credential, never a field from a
client-controlled message payload. Credential values are never logged, since
uid, gid and pid identify local principals.

`UDP_SOCKET` carries no credential, and `NvPSFTransportPeerCred` reports
`NVPSF_TRANSPORT_DENIED` for it rather than returning a zeroed struct that could
be mistaken for a validated identity.

## Addressing a peer, and sending first

Every send names a peer. A server learns each peer from
`NVPSF_EVENT_PEER_CONNECTED` or from `Recv`'s `fromPeer`, but a client has no
accept event to learn from, and a peer id cannot be constructed by the caller
because it embeds an internally assigned generation counter.

`NvPSFTransportSolePeer` supplies it. Clients need this to send first, which every
PSF client does: an SDM registers with the gateway, the PSS daemon initiates to
PSD, and an RPC client issues a request. Without it a client could only learn its
own peer *after* receiving a message, leaving the send-first direction
unreachable.

Valid for a client of any kind. Refused for a server, which has many peers and
no single answer.

## Receive fairness

With several peers ready, `NvPSFTransportRecv` selects round-robin from the slot
after the one last serviced and returns at most one message per call. A
lowest-slot-first scan would be wrong rather than merely unfair: one chatty peer
inside the allowed group could starve the other slots indefinitely, and on the
gateway link that means registrations and heartbeat acknowledgements from healthy
SDMs go unserviced and those SDMs get reclaimed as dead. With a shared UDP socket
the kernel interleaved senders on our behalf; draining N connections in
application code makes fairness this library's problem.

## No severity lanes

There is deliberately no lane parameter on `NvPSFTransportSend`. Neither
transport can express a severity at the wire level, so a lane could only be
carried by giving a link more than one channel.

Severity handling lives above this library instead. The sending thread keeps
separate critical and operational queues and arbitrates between them before any
byte reaches the transport, which is why one socket suffices. That argument
depends on the PSS → PSD link staying single-request-in-flight; a pipelined
sender would make wire-level head-of-line blocking reachable and would require
lanes to be introduced.

A lane argument here could therefore only be accepted and ignored, and an
ignored severity argument on a safety path invites a caller to believe it did
something.

## Platform

Linux only. Depends on `SO_PEERCRED`, `struct ucred`, `AF_UNIX SOCK_SEQPACKET`,
`MSG_NOSIGNAL`, `SOCK_CLOEXEC`, and `accept4()`.

## Build

```text
components/safecomm/transport/
  include/nvpsf_transport.h        kinds, links, socket paths, the API
  src/nvpsf_transport.c            both backends
components/safecomm/nvpsf_transport.cmake
                                   libnvpsf_transport.so
```

Built when `SAFETY_CORE_BUILD_SAFECOMM_TRANSPORT` is `ON` (the default).
Consumers link the `nvpsf_transport` CMake target, which also carries the
public include directory; a consumer that only needs the transport-kind enum
can add `components/safecomm/transport/include` to its include path instead.
