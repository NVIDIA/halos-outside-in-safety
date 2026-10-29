<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->
<!--- Directory structure of psd --->

| Path | What it is |
| --- | --- |
| `src/NvPSDGateway.cpp` | The `nvpsd_gateway` binary. Fans `DecisionRequest`s out to registered SDMs, heartbeats them, and reports client loss. |
| `src/NvPSD.cpp` | `libnvpsd`. The PSD side of the PSS → PSD link, and the client API SDM-side code builds against. |
| `src/NvPSD_interface.cpp` | C interface shim over `NvPSD`. |
| `include/NvPSDGatewayProtocol.h` | Gateway ↔ SDM wire format: `REGR`, the `RGNK` registration refusal, heartbeat, and the decision envelope. |
| `include/NvPSDGatewayContract.hpp` | Envelope construction and validation shared by the gateway and every SDM. |

## Links and transports

The gateway sits between two links with different rules, and conflating them is
the most common way to get a change here wrong.

| Link | Transport | Selectable? |
| --- | --- | --- |
| PSS Daemon → PSD Gateway | `UNIX_SOCKET` (`AF_UNIX SOCK_SEQPACKET`) | **No.** Mandatory. POSIX message queues were retired from the PSF links, and an `nvpss.conf` still carrying `PSSDToPSDComBackend` is rejected at startup rather than honoured. |
| PSD Gateway → SDM | `UNIX_SOCKET` (`AF_UNIX SOCK_SEQPACKET` at `/run/nvpsf/nvpsd_gateway`) | **No.** Fixed in the gateway and in every client. `--transport`, `--sdm_socket`, `--sdm_ip`, `--sdm_port` and `--sdm_allow` were retired; registration is authenticated by `SO_PEERCRED` against the gateway's primary GID, so there is no address allowlist to configure. |
| SDM → `cmd_receiver` (PLC) | UDP | Out of scope; unchanged. |

An unusable transport is a **fatal startup error**, never a fallback. Delivering
a different transport from the one the link is built for would be a silent
downgrade on a safety link (IV-7, TR-2).

Each binary logs the transport it actually opened at startup. That log line — not
a command-line flag recorded elsewhere — is the authority when reading evidence.

## Admission model (two-stage)

A connection does **not** get a `clientTable` slot. Only a validated `REGR`
does.

1. **Accept.** The transport holds the accepted-but-unregistered peer in its own
   bounded pool, with a mandatory admission-timeout reaper. On `UNIX_SOCKET` the
   peer's credentials are checked against the allowed GID at this point
   (`SO_PEERCRED`); a peer outside the group never reaches stage 2.
2. **Register.** A `REGR` that parses, carries at least one valid event type, and
   passes range checks allocates the `clientTable` slot.

This is why a peer that connects and then says nothing cannot exhaust the client
table: it occupies a pool entry until the reaper takes it back, and legitimate
SDMs continue to register throughout.

**A refused `REGR` is answered, never silent.** The gateway replies with `RGNK`, five bytes:
the magic, then one reason byte.

| Reason | When |
| --- | --- |
| `malformed` | Shorter than its header, or an event-type count of 0 or over 32 |
| `no_valid_event_types` | No requested type is subscribable |
| `table_full` | Every client slot (`--num_clients`) is taken; the peer stays pending, so the reaper still owns it |
| `rate_limited` | Over the per-peer limit below; refused unread |

- **The SDM side.** The SDM logs the reason at error level, once per
  reason until a gateway heartbeat shows it registered:
  `the gateway refused this SDM's registration; reason=<name>`. Nothing else changes: an
  unregistered SDM receives no decisions and latches on its gateway-heartbeat watchdog, as before.
- **Older builds.** An SDM built before `RGNK` logs `received partial packet (5 bytes, …)` and
  ignores it.
- **The gateway side.** It records every refusal:
  `PSD-Gateway: registration refused reason=<name>, peer=<id>, replySent=yes|<error>`.

**`REGR` is rate limited per peer.**
- At most `--regr_max_per_window` (default 4, 1–64) per `--regr_window_ms` (default 10000,
  1000–600000) are read. Both options are bounded; an out-of-range value is a startup error.
- Beyond that, a `REGR` is refused unread. Only the first refusal in a window is answered, so a
  flooding peer costs one 5-byte reply per window. `RGNK` is smaller than any `REGR`, so it cannot
  amplify.
- An SDM sends one `REGR` at launch or reconnect, then one every 30 s, far below any allowed
  setting.
- The gateway logs its settings at startup (`PSD-Gateway: REGR rate limit`).

The gateway auto-adds `PSS_STATUS_NOOP`, `SW_FAIL`, `SENSOR_INVALID` and
`SENSOR_VALID` to every valid registration, so an SDM does not have to advertise
the no-op or sensor-health types to keep its freshness contract alive.

## Two rules that are load-bearing

**No transport call may be made while holding `clientTableMutex`** — including
`DropPeer`. The fan-out is two-phase on purpose: build the send list under the
lock, release it, then send. `clientTableMutex` is a leaf and must not be held
across any I/O, `std::cerr` included.

**Descriptor and peer arrays initialise to their invalid sentinel, never 0.**
Descriptor 0 is a valid descriptor and a zeroed `NvPSFPeerId` is not the same as
an invalid one. Every cleanup path must restore the sentinel.

## Backpressure

Non-blocking send. On `WOULD_BLOCK` the message is **dropped, counted, and
surfaced as fault evidence** — never retried inline and never blocking. This is
closest to the UDP loss semantics the link had before and keeps the safety path
non-blocking.

The consequence is worth stating plainly: on the gateway → SDM link a failed
send burns an already-allocated sequence number, and the SDM sees that as a
sequence gap and latches into fault-safe lockout needing a validated manual
release. That is why the receive-buffer floor (`minQueueDepth`) on this link is
not allowed to be guessed — it is derived from measured `T_service_worst` and
`T_emit_min`, and until that measurement lands the floor stays `0` and the
`UNIX_SOCKET` option on this link stays behind the carriage guard
(`nvpsf::transportKindIsCarried`).

## Reconnect

Both ends of the PSS → PSD link and both SDM upstream clients reconnect on
`PEER_GONE`: count-unbounded, fixed 500 ms cadence, no exponential backoff. A
restart of the peer must not require restarting this process, and backoff would
make worst-case recovery depend on how long the outage had already lasted —
which is exactly when recovery matters most.

Re-registration is replayed on every reconnect. A restored connection with no
event-type routing is worse than no connection: the peer accepts it, reports a
healthy client, and delivers nothing.

The outage is logged on entry and on success, so a reconnect never masks a
restart.

### One trap, for anyone writing a wait helper here

`NvPSFTransportPoll` is **not** a passive sleep. On `POLLHUP` it emits
`PEER_GONE` *and releases the peer slot*. A helper that uses `Poll` only to wait
and discards the event array consumes the disconnect — after which `Recv`
returns `WOULD_BLOCK` rather than `PEER_GONE`, and the next `Poll` builds zero
pollfds and returns `TIMEOUT` immediately without sleeping. The result is a core
pinned at 100% on a link that never reconnects, and it looks quiet from outside.
Every wait helper must inspect the events it polls.
