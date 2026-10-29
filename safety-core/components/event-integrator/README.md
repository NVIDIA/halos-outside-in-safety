<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->
<!--- Directory structure of pss --->

| Directory | Contents |
| --- | --- |
| `daemon/` | `nvpss_daemon`: event ingest, fusion, decision dispatch, the PSS RPC server, and the PSS → PSD server (`NvPSSDToPSD`) |
| `psscom/` | `libnvpsscom`, the PSS communication helpers |

## Transports

Two links originate here, and **neither is selectable**.

| Link | Transport | Selectable? |
| --- | --- | --- |
| PSS clients → PSS daemon (PSS RPC) | `AF_UNIX` at `/run/nvpsf/nvpssd` — **still `SOCK_STREAM`; migrates to `SOCK_SEQPACKET` in task D1** | No |
| PSS daemon → PSD gateway | `AF_UNIX SOCK_SEQPACKET` at `/run/nvpsf/nvpssd_to_psd` | No |

### Why there is no selection

POSIX message queues are retired from the PSF links. All PSF components
run on one host, so a configurable transport bought nothing except a way for the
two ends of a link to disagree — and a disagreement here is not a connection
failure but a *silent* one: an operator believing they had configured one
transport while the daemon used another, on the link that carries PSS `ERROR`
and `SW_FAIL`. That is the class of defect invariants IV-7 and SI-10 exist to
prevent.

So the selection surface was removed rather than defaulted:

## Configuration: `enableFusion`

`nvpss.conf` must set `enableFusion` to `true` or `false`. The key is mandatory:
the daemon refuses to start if it is missing, repeated, or set to anything else,
and refuses to start if `nvpss.conf` itself cannot be read. It will not select an
event pipeline that the configuration does not state.

| Value | Behaviour |
| --- | --- |
| `true` | Detections of one object seen by several cameras are correlated into a single event before the safety decision maker sees them. |
| `false` | Each camera's report is delivered separately. No correlation. |

The value is read once at startup and is immutable afterwards; changing it needs
a daemon restart. The selected mode is logged unconditionally
(`Event pipeline selected`), so the log — not the file — is the record of what
actually ran.

### Which keys apply in which mode

| Key | `true` | `false` |
| --- | --- | --- |
| `enableFusion`, `sensorConfig` | required | required |
| `timeWindowSize`, `maxPipelines` | required | **required** |
| `fusionThreshold`, `alpha`, `beta`, `gamma`, `temporalTolerance` | required | not evaluated |
| `trajectoryCount`, `bypassFusionEvents` | optional | not evaluated |

`timeWindowSize` and `maxPipelines` read as fusion tuning but are not.
`timeWindowSize` also sets the age past which an event is marked `STALE`, and both
SDMs drop `STALE` events; `maxPipelines` bounds which pipeline IDs are accepted at
all. Both checks run in either mode, so neither key is optional under `false`.

Under `false`, any fusion-only key that is present is ignored and named once in an
informational log line, so a malformed value is visible immediately rather than
surfacing later when the key is flipped back to `true`.

### Event multiplicity, and the one-camera rule for counting events

Turning fusion off is not a performance switch. It changes what the safety
decision maker receives: one event per reporting camera instead of one per
detection. Whether that changes a decision depends on the event type.

| Event semantics | Example | Effect of N events instead of one |
| --- | --- | --- |
| Sets a state | forklift entered the trailer | none — setting the same state N times equals setting it once |
| Last event wins | proximity severity | none — the final command is the same |
| **Accumulates** | person count | **the count moves by N instead of 1** |

Measured with three cameras reporting one person entering the trailer: the count
moved by **3** with fusion off and by **1** with fusion on.

> **A counting event type must be mapped to exactly one camera.**

This holds in **both** modes and is not a consequence of turning fusion off. With
fusion on the collapse factor depends on fusion timing and is not guaranteed: the
same three-camera batch has been measured collapsing to 2 under one timing and to
1 under another. A count that moves by an amount you cannot predict is no safer
than one that moves by a known wrong amount. The constraint belongs to the
event mapping, not to the fusion setting; making the setting operator-selectable
is simply what turns it into something an integrator has to be told.

- **`PSSDToPSDComBackend` is a rejected key.** Its presence in `nvpss.conf` is a
  fatal startup error naming the file, the key and the remedy — detected on the
  key itself, so a trailing inline comment cannot evade it. Operators upgrading
  with a customised config must delete the line.
- **There is no CLI flag** for either link, nor for the gateway → SDM hop, which
  is fixed to `UNIX_SOCKET`
  ([transport guide](../safecomm/transport/README.md)).

The daemon logs the transport it is actually using at startup
(`PSS->PSD transport selected`). That line, not any configuration file, is the
authority when attributing a safe-state evidence artifact (G-30).

## Sensor identity: what `pipelineID` 0 means

`fusionMetadata.pipelineID` names the sensor an event came from, and **0 is not a
sensor**. It is the *unknown sensor* channel, and it is valid only for events that
genuinely name no sensor:

| Class | Types | `pipelineID = 0` | Producer |
| --- | --- | --- | --- |
| Pipeline-wide | `AI_PIPELINE_VALID`, `AI_PIPELINE_INVALID` | **valid** | Perception Monitor — trust is pipeline-wide, not per camera |
| Not sensor-bound | `SW_FAIL` | **valid** | daemon heartbeat fault; client faults such as `kafka_connection` |
| Not sensor-bound | `PSS_STATUS_NOOP` | **valid** | daemon-built |
| **Per-sensor** | `SENSOR_*` and every application event (`EVENT_*`, `ROI_*`, `TW_*`) | **rejected** | — |

A per-sensor event carrying 0 means the producer could not resolve its sensor name
against `sensor_config`. PSS rejects it at the RPC boundary with
`REPORT_REJECTED_UNRESOLVED_SENSOR`, and mdx-client drops it before sending,
logging the name that failed to resolve.

**Why both ends.** Only the producer knows the offending sensor *name*, which is
what an operator needs in order to fix the configuration; only the daemon can
guarantee the property regardless of which client is at fault. Neither alone is
sufficient.

**Why not simply reject every `pipelineID = 0`.** That would silently disable the
`AI_PIPELINE_INVALID` latch — one of the three independent safe-state latch
causes — and the daemon's own heartbeat `SW_FAIL`. The rule is driven by event
type, not by the value alone.

## Safety event wire contract

`daemon/include/pss_protocol.h` is compiled by the PSS daemon, the PSD gateway
and every SDM, so all of them must be rebuilt and redeployed together.

`PSS_SCHEMA_VERSION` is **5**. Version 3 carried a `char ruleIdentifier[64]` on
`SafetyEvent` and `FusedSafetyEvent`; version 4 replaced it with a one-byte
`EventCause`. Version 5 dropped the ROI and tripwire-crossing event types and
added `EVENT_23`, which shifts every later `EventType` value. No two versions can
interoperate, and they do not try to: upgrade every component in lockstep.

### `EventCause`

`EventCause` says *why* a sensor changed state. The verdict itself stays in
`EventType` (`SENSOR_INVALID` / `SENSOR_VALID`); the cause never overrides it.

Each cause belongs to one `EventType`. The grouping is not cosmetic: only the
`SENSOR_INVALID` causes reach the failed-sensor mask, so only they can affect the
all-safety-relevant-sensors-failed latch.

**`SENSOR_INVALID` causes** — these feed the failed-sensor mask:

| Value | Name | Emitted by | Failed bit |
| --- | --- | --- | --- |
| 0 | `NONE` | zero-initialised default, any event type | **set** |
| 1 | `SAIM_INIT` | SAIM, at first registration before any frame | **suppressed** |
| 2 | `SAIM_INPUT_DEGRADED` | SAIM — frame-quality below threshold | set |
| 3 | `SAIM_INTERNAL_ERROR` | SAIM — analyzer init or per-frame compute failure | set |
| 4 | `SAIM_STREAM_DISCONNECT` | SAIM — connect-retry exhaustion or reconnect | set |
| 5 | `SAIM_FRAME_DROP` | SAIM — FU-A NAL drops | set |
| 6 | `SAIM_UNKNOWN` | SAIM — defensive fallback for a missing or contradictory cause | set |

**`SENSOR_VALID` cause:**

| Value | Name | Emitted by |
| --- | --- | --- |
| 7 | `SAIM_SENSOR_HEALTHY` | SAIM, on the recovery edge after a sustained good streak |

**`SW_FAIL` causes** — these do not touch the sensor mask; a `SW_FAIL` drives the
safe state on its own path:

| Value | Name | Emitted by |
| --- | --- | --- |
| 8 | `HEARTBEAT_FAULT` | PSS daemon, on RPC heartbeat budget exhaustion (`NvPSSDRPC.cpp`). The client is in `fusionMetadata.clientID` |
| 9 | `GATEWAY_DECISION_SEQUENCE_GAP` | Reserved for an SDM-side bridge that detects a gateway decision-sequence discontinuity; not emitted by the components in this repository |
| 10 | `PSS_REQUEST_SEQUENCE_GAP` | PSD gateway — PSS `DecisionRequest` sequence discontinuity |
| 11 | `PSS_REQUEST_ID_INVALID` | PSD gateway — PSS request id outside the accepted range |
| 12 | `PSS_ERROR` | PSD gateway — PSS reported `ERROR`; the gateway broadcasts `SW_FAIL` |
| 13 | `CLIENT_INPUT_UNUSABLE` | PSS daemon — a required source's reports were rejected consecutively past `client_reject_error_threshold`, so its input is live but unusable. The client is in `fusionMetadata.clientID`, its input identity in `objectID[0]` (below). Sent once per episode; the episode is also reported as a level on every presence report, and the SDMs clear it only when that report shows the same source confirmed usable again |
| 15 | `SAFETY_MONITOR_LOST` | PSS daemon — the registered Safety AI Monitor stopped being one, **however** it happened: it hung (heartbeat budget exhausted), crashed (connection dropped), unregistered itself, or was dropped for a protocol error. The SDMs clear it **only** on a presence report (16) naming the monitor — never on ordinary traffic. The client is in `fusionMetadata.clientID` |
| 17 | `PERCEPTION_MONITOR_LOST` | PSS daemon — the same, for the Perception Monitor, the only source of AI-pipeline verdicts |
| 18 | `MDX_CLIENT_LOST` | PSS daemon — the same, for a **numbered** MDX client instance (see "Identifying an MDX client" below), **or** an instance `required_mdx_instances` requires that never registered. Other MDX clients' traffic does not clear it. Anonymous MDX clients keep cause 8 |

Causes 15, 17 and 18 carry the lost source's `PSS_PRESENCE_*` bit in
`fusionMetadata.objectID[0]`, so an SDM that never saw the source present still
knows which one has to come back.

Two causes ride a `PSS_STATUS_NOOP` rather than a fault report. Neither can latch
anything by itself arriving; the presence report can *raise* a loss by what it
**omits** (below):

| Value | Name | Emitted by |
| --- | --- | --- |
| 14 | `PSS_COMM_RECOVERED` | PSD gateway — its all-client recovery record after a synthetic fault (causes 10–12) cleared. Delivered to **every** SDM that received the fault — including one that was in the gateway's degraded heartbeat tier at the time, once it is back — so an SDM that receives no ordinary events still learns the fault recovered |
| 16 | `SOURCE_PRESENCE` | PSS daemon — the **presence report**, on every periodic status no-op. `fusionMetadata.objectID[0]` holds the `PSS_PRESENCE_*` set of sources that are registered, have heartbeated since registering, and are not heartbeat-faulted; `objectID[1]` the set PSS expects (below). Bits 16–31 of each carry the **input-state report** (below). All sampled when PSS builds the request |

`PSS_PRESENCE_*` bits: `0x1` Safety Monitor, `0x2` Perception Monitor,
`0x100 << (k-1)` MDX instance `k` (1..8, bits 8–15). Positions are wire contract —
append only.

**Input identities** use the same bits plus `PSS_INPUT_ANONYMOUS_CLIENT` (`0x4`),
which stands for every anonymous MDX client together; it appears only in the
input-state sets.

Causes 10–12 matter more than they look. Those three gateway events are otherwise
**identical on the wire** — same type, severity, status, `clientID`, and
`sensorIdentifier` `"PSD_GATEWAY"` — so the cause is their only discriminator.
Setting any of them to `CAUSE_NONE` makes three distinct faults indistinguishable
during triage.

Cause 13 has the same property against cause 8: both are daemon-originated
`SW_FAIL`s carrying `sensorIdentifier` `"PSS_RPC"` and a `clientID`, and they
differ only in whether the client stopped talking (`HEARTBEAT_FAULT`) or kept
talking unusably (`CLIENT_INPUT_UNUSABLE`). Those call for different operator
action, so they must not be collapsed.

Causes 15 and 17 split the two monitors out of cause 8 for a reason that goes
beyond triage: cause 8 clears on any later usable request, and ordinary MDX
traffic says nothing about whether sensor health or AI-pipeline trust is being
watched. Folded back into cause 8, a dead monitor's fault would clear on the next
MDX batch and a release would be accepted with no monitor running — which is what
happened before they existed.

**The report carries two sets.** `objectID[0]` is what is present now.
`objectID[1]` is what PSS **expects**: every tracked source that has heartbeated
since this PSS started, plus the sources `nvpss.conf` requires once the heartbeat
budget since PSS start has run out.

**What it does in the SDMs.** Each SDM keeps, for the life of its process, the set
of sources it knows of: every source it has seen present, every source named by a
loss `SW_FAIL`, and the report's expected set. A source named present clears its
lost sub-cause. A known source **missing** from a report raises it — latch and
safe hold — even though no `SW_FAIL` came. Each side covers the other's restart:

- **PSS restarts** while a monitor is dead: the new PSS has no record of it, but
  the SDMs do, and its reports no longer name it.
- **An SDM restarts** while a monitor is dead: the SDM has no record of it, but PSS
  does, in the expected set of its first report.
- **Both restart**: only a *required* source comes back into the expected set —
  once its start-up budget has run out — and a required source that never
  registered is also reported lost, `reason=never_registered`.

A source that is neither known nor required is not a loss, so a site that does not
run the Perception Monitor is unaffected. A no-op without cause 16 (the gateway's
14) says nothing about presence. A missed heartbeat inside the budget does **not**
remove a monitor from the report, so one late heartbeat cannot latch a site.

**Every loss `SW_FAIL` raises**, wherever it sits in a request and whatever
`pssStatus.mode` says. The source-lost rule is applied to every request, before the
PSS-ERROR return.

Clearing a sub-cause is never clearing the latch: the safe-state latch still
requires a validated manual release.

**A source is expected from its first heartbeat, not from its registration** —
the moment it can also be present. A report built between the two would otherwise
raise a loss on a healthy start.

**Every `SW_FAIL` is latched**, whatever its position and in an ERROR-mode request
too: each SDM raises the sub-cause of every `SW_FAIL` a request carries.

### The input-state report: a rejected source clears only on its own recovery

Cause 13 says a source's reports are being rejected. Ordinary traffic from **other**
sources says nothing about whether that source recovered, and PSS sends cause 13
once per episode, so a cause cleared by other traffic would never come back while
the source kept failing. The episode is therefore also reported as a **level**, in
bits 16–31 of the presence report:

| Word | Bits 16–31 |
| --- | --- |
| `objectID[0]` | **unusable**: identities with a cause-13 episode open now |
| `objectID[1]` | **confirmed usable**: identities from which PSS has accepted a report since the last cause-13 escalation for that identity |

Each SDM holds the `input_unusable` sub-cause for an identity from its cause-13
`SW_FAIL`, or from the unusable set, until a report — with `mode != ERROR` — shows
it confirmed usable and not unusable:

- **PSS restarts:** both sets start empty, so nothing is cleared. The SDM holds until
  that source's next accepted report.
- **An SDM restarts:** an episode that is still open is relearned from the unusable
  set.
- **The source leaves mid-episode:** it drops out of the unusable set but stays
  unconfirmed. The SDM holds until a client with that identity has a report accepted.

**A recovered source that sends no reports leaves the site release-denied** until
it does. An accepted report from that source is the only evidence of recovery that
exists.

**Which sources escalate.** A source escalates if `nvpss.conf` requires its
registered identity (a numbered MDX instance covered by `required_mdx_instances`, or
a monitor whose `required_*` key is 1), even if none of its reports was ever
accepted. Otherwise it escalates only once it has an established pipeline (an
earlier accepted report) in the reviewed set. An anonymous client that never had a
report accepted cannot escalate, which is what stops a rogue peer latching every SDM.
The record names the trigger: `requiredBy=identity` or `requiredBy=pipeline`.

`CAUSE_COUNT` is the range-check bound and is never sent.

### Adding a cause is a coordinated-deployment change

An unrecognised cause latches, so SDM decision logic is forward-safe. **Validation
is not.** `validateCommonEventFields()` rejects `cause >= CAUSE_COUNT` against the
*receiving* build's bound, and a rejected `DecisionRequest` is dropped before
freshness is refreshed — so a peer built before the value existed misses the fault
the event carried and, if the condition repeats, latches on a spurious freshness
timeout instead.

Every validating runtime must therefore be rebuilt together. That is already the
rule for a release.

### The rule that matters: only `SAIM_INIT` suppresses

On a `SENSOR_INVALID`, `SAIM_INIT` is the **only** value that stops a sensor's
failed bit being set. It exists so a camera that has registered but not yet
delivered a frame does not count toward the all-safety-relevant-sensors-failed
latch at boot. Every other value — including one a given SDM does not recognise
— sets the bit.

That asymmetry is deliberate and is why the cause is an enum field rather than a
set of new `EVENT_*` types. An unrecognised `EVENT_*` reaches `default: break;`
in the SDM dispatch and is silently dropped, so a newly added cause would have
failed **open** on a latch path. An unrecognised *cause* falls through to the
failure branch and fails **closed**. Adding a value is therefore safe by
construction on every receiver, including one that has not been rebuilt.

A value **outside** the enum is different again: `validateSafetyEvent` refuses
any value at or above `CAUSE_COUNT`, so the whole event is rejected at the
boundary and never reaches an SDM. Out-of-range is "not interpretable at all",
which is not the same as "in range and unrecognised", and the two must not be
collapsed into one rule.

### A sensor group's health is derived, and may not be reported

In a 3D deployment the BEV group is a configured identity like any camera — same
shape on the wire, its own `pipelineID`, its own name — but its health is
obtained differently. The daemon computes the group's verdict from its **member
cameras** and synthesises the group's own `SENSOR_INVALID` / `SENSOR_VALID` from
that. No client may report it.

A trust report whose `pipelineID` names a configured group is therefore rejected,
whatever its type and whichever authorised client sends it:

    Trust report rejected: pipelineID names a sensor group, whose health is
    derived from its members and cannot be reported directly

This is not only about a forged recovery. The direction that actually bites is a
forged `SENSOR_INVALID` while every member is healthy: in 3D the group alone is
the SDM's reviewed sensor set, so one such report is the **entire** reviewed set
failing, and the SDM enters the all-safety-relevant-sensors-failed safe hold —
which then needs a manual release. Without the rejection any client permitted to
send trust reports could halt the line.

The daemon's own synthesised verdict is unaffected: it is queued directly and
never passes through the trust-report path.

One related asymmetry in source validation. An omitted `sensorIdentifier` is
accepted for a camera and refused for a group. For a camera the `pipelineID` is
the safety key on its own, so naming nothing asserts nothing. For a group it is
not sufficient — a group is indistinguishable from a camera by id alone, and the
group is the one identity whose semantics cannot be inferred from the id, so it
must be named explicitly.

### Reading a cause in a log

**Every runtime logs the cause as a number**. There is no
enum-to-string helper in the tree, so the table above is the way to resolve one:

```text
[ATL] safety-sensor health change pipelineID=2 state=FAILED cause=4
                                                             ^ SAIM_STREAM_DISCONNECT
```

This is deliberate. A name table that lives beside the enum is a second
vocabulary that can drift from it, and FreeRTOS firmware carries no such table
anyway — so one format read the same way on all four runtimes beats two formats
that agree only while someone maintains them.

Operators upgrading from wire schema 3 see a real change here: the old
`ruleIdentifier` string appeared in the log directly.

### Cause on a fused event

Fusion combines contributors that may disagree. `FusedSafetyEvent.cause` carries
the contributors' cause only when they **all** agree; otherwise it is
`CAUSE_NONE`. A single-contributor passthrough keeps its cause unchanged. The
fused cause is diagnostic — it is not an input to any latch decision, which is
taken per contributing sensor.

## Diagnosing a client that stops reporting

When `NvPSSReportSafetyEvent()` returns `NVPSSD_FAIL`, callers log only that the
report failed. The **reason** lives in the interface library's own records, which
go to `syslog` — `NvPSBWriteData()` writes there unconditionally and mirrors to
stdout only under `NVPSF_DBG`, so on a release build the client's stdout shows
nothing and `journalctl` shows everything:

```bash
journalctl --since "10 min ago" | grep -E 'ReportSafetyEvent|SendHeartbeat|TerminatePSSClient'
```

| Record | What it means | What to do |
| --- | --- | --- |
| `socket fd outside [0, FD_SETSIZE) for select` | This **client process** is out of descriptors; its PSS socket landed above 1024 and `select()` cannot represent it | Raise the client's `RLIMIT_NOFILE` or reduce the descriptors it holds. The daemon is fine. |
| `invalid socket descriptor for client` | Library-internal state corruption — should not occur | Escalate; this is a defensive check that is unreachable through the public API. |
| nothing from the library, caller reports failure | The daemon refused or stopped answering | Check `nvpss_daemon`. |

The first case is the one to know about, because **registration still succeeds**
when it happens: `NvPSSRegisterPSSClient()` uses `sendAll`/`recvAll` and never
calls `select()`. A client in this state comes up looking healthy and silently
cannot report anything.

## Trust verdicts are re-asserted while they stand

SAIM and the Perception Monitor report on change, so an SDM that restarts after a change never
hears it. **On every status interval PSS re-sends each INVALID verdict that stands**, as an
ordinary daemon-originated trust report:

| Standing verdict | Re-sent as |
| --- | --- |
| AI pipeline INVALID | `AI_PIPELINE_INVALID`, pipeline 0 |
| 2D: a camera INVALID | `SENSOR_INVALID` for it, with its configured name and the cause last recorded for it |
| 3D: the BEV group last reported Untrusted | the group's `SENSOR_INVALID` (members are not re-sent) |

- A restarted SDM relearns within one status interval.
- **VALID is never re-sent**, because a restarted SDM starts with nothing failed.
- Re-assertion changes no trust state at PSS.
- `PSS_TRUST_VERDICTS_REASSERTED` records what is being re-sent, once each time that set changes.
- A PSS that restarts forgets every verdict. The monitors' own re-sends rebuild them within 30 s:
  SAIM, and the Perception Monitor ([perception-monitor](../perception-monitor/README.md)).

## The critical lane does not drop a fault report silently

The PSS-to-PSD critical lane carries the **must-deliver reports** (`SW_FAIL`,
`SENSOR_INVALID/VALID`, `AI_PIPELINE_INVALID/VALID`). It also carries ordinary events
that are there only because their source was distrusted, or because the mode was ERROR.

**An event keeps the trust it was queued with.** PSS stamps `isHealthy` and
`isTrustedSource` when it builds a `DecisionRequest`. The stamp is trusted only if the source
is trusted *then* **and** was trusted when the event was queued. An event observed while its
source was declared invalid is therefore delivered distrusted, and skipped by every SDM, even if
the source recovers before the event is sent. A recovery report does not vouch for what was
observed before it.

**A full lane makes room for a must-deliver report.** It evicts the newest event that was
queued from a distrusted source, or is STALE. Every SDM skips such an event whatever happens
to trust before it is sent, so evicting it loses nothing a decision uses. This holds for the
report that restores trust too: PSS applies the recovery before that report reaches the lane,
yet the events queued before it can still be evicted. The first 8 slots are never touched,
because a batch, or its retry, is copied from there.

**If nothing is evictable, the report is refused, and PSS stamps every
`DecisionRequest` `ERROR`.** Every SDM latches `mode_error`, and the gateway fans out its
PSS-ERROR fault.

**The refused report is kept, not lost.**
- It goes into a bounded pending list (32 entries).
- A trust verdict states the current state of one slot, a sensor or an AI pipeline, so a newer
  verdict for that slot replaces the pending one. Delivering the older one afterwards would roll
  the SDMs back.
- The slot is what the verdict is about, as PSS's trust state and every SDM key it: a sensor
  verdict by its `pipelineID`, an AI-pipeline verdict by its type alone. It is not keyed on who
  sent it. A monitor that reconnects gets a new client id, and PSS's own re-assertions carry 0;
  either still replaces the pending verdict for that sensor or that pipeline.
- A `SW_FAIL` is never coalesced.
- Pending reports are re-admitted to the lane, oldest first, as it drains.

The `ERROR` stamp stays until **all** of these hold:

- the lane has drained to half its depth;
- every refused report has been re-admitted **and delivered**;
- the gateway has accepted a request stamped `ERROR` *after the latest refusal*.

A refusal is therefore never withdrawn before it has reached the SDMs, even if the lane drains
within one sender cycle or the stamped send fails. If the pending list is itself full, the
report is lost: it is recorded and counted, and `ERROR` holds until the list drains.

**A request id keeps the stamp it was first sent with.** A retry re-sends the same request, its
`pssStatus` included. The gateway answers a request id it has already seen as a duplicate:
acknowledged, but not forwarded. A retry re-stamped `ERROR`, whose earlier `NORMAL` copy had
been forwarded, would therefore be acknowledged with no SDM having seen `ERROR`.
- **Overflow credit.** A refusal's `ERROR` is credited only when an acknowledged request id
  was first sent stamped with it.
- **Delivery fail-safe.** It returns to `NORMAL` only on an acknowledged request whose id was
  first sent stamped `ERROR`.
- **A newer ERROR.** One raised while a retry is in flight goes out on the next new request id. Routing is not affected, so the lane can
drain. A trusted event, or one queued only because the mode was ERROR, is
never evicted: it may still be acted on.

| Record | Written |
| --- | --- |
| `PSD_CRITICAL_LANE_EVICTION` | At most once per second, with the evicted and admitted event types and an exact running count |
| `PSS_CRITICAL_LANE_OVERFLOW` | Once, when a must-deliver report is refused (ERR); PSS reports ERROR from here |
| `PSS_CRITICAL_LANE_READMITTED` | When refused reports are re-admitted as the lane drains (`readmitted`, `stillPending`) |
| `PSS_CRITICAL_LANE_REFUSED_LOST` | ERR, per report the full pending list could not keep (`lostTotal`); PSS stays `ERROR` |
| `PSS_CRITICAL_LANE_RECOVERED` | Once, when the lane has drained to half, every refused report has been delivered, and an `ERROR`-stamped request sent after the latest refusal has been accepted (`errorStampDelivered=yes, refusedReportsDelivered=yes`) |

## An abandoned batch can be reconstructed, by request id

PSS retries an unacknowledged batch. After `3` failed delivery attempts, each a full retry
budget, it abandons the batch, and the delivery fail-safe has already raised PSS `ERROR` for
every SDM. Two records make the loss reconstructible.

| Record | Written |
| --- | --- |
| `PSD_STAGING_BATCH_UNDELIVERABLE` | Per abandoned batch. It gives the request id and the attempts, and lists the events no decision will see: `events(id:type:pipelineID:clientID)=[...]` |
| `PSD_STAGING_DELIVERY_RESUMED` | Once, on the first request PSD acknowledges afterwards. It gives that request's `requestId`, `abandonedBatches`, `abandonedEvents`, and the abandoned request ids (the last 8) |

Every SDM logs the id of each request it processes (`processing DecisionRequest id=N`), and the
gateway passes PSS's id through. So `PSD_STAGING_DELIVERY_RESUMED requestId=N` joins this log
to every SDM's at request `N`, without matching timestamps.

A batch that grows while it waits is a new batch, and its attempts restart. On a quiet site the
status no-ops alone grow the operational batch every 5 s. Abandonment therefore comes once the
batch holds 8 events, about a minute into an outage.

## Records that summarise instead of repeating

Two daemon records describe a condition that can persist at loop or frame rate.
Both are written as **episodes** so they stay bounded, and both close with an
exact count — nothing is sampled away.

| Record | Written | Closing record |
| --- | --- | --- |
| `PSS_FUSION_STAGE_FAILURE` | `episode=started` on the first failure; then at most one `episode=ongoing` per second with `failuresInEpisode` and `consecutiveFailures` | `PSS_FUSION_STAGE_RECOVERED … failuresInEpisode=N, episodeMs=…` after **one full second with no failure** |
| `PSS_CLIENT_REJECT_STREAK` (source not required) / `PSS_CLIENT_INPUT_UNUSABLE` (required, `requiredBy=identity` or `pipeline`) | Once, when the consecutive-rejection threshold is crossed | `PSS_CLIENT_REJECT_STREAK_ENDED … consecutiveRejections=N, escalated=yes/no, endedBy=…` when an accepted report arrives or the client goes away |

Why it matters: one record per failure on the 2 ms fusion loop is ~500 records/s,
which exhausts journald's default per-service budget (10 000 per 30 s) in about
20 s. journald then drops **every** record from the daemon for the rest of the
window, including unrelated ones. A record meant to make a failure visible was
able to hide the others.

The fusion episode closes only after a quiet second, so a stage that flaps
(fail, succeed, fail…) is one episode, not hundreds. Two consequences when
reading a log:

- `PSS_FUSION_HEALTH_RECOVERED` (escalation cleared) normally appears about one
  second **before** the stage's `PSS_FUSION_STAGE_RECOVERED` (episode closed).
  They answer different questions.
- For the input-queue stage, "a quiet second" is measured at the next successful
  event, so on a site with no traffic the closing record waits for the next event.

The escalation itself is unaffected: it still counts consecutive failures per
iteration and fires `PSS_FUSION_HEALTH_ERROR` at `fusionFailureErrorMs`.

## When a monitor goes away

Every way a registered Safety Monitor, Perception Monitor or numbered MDX instance
stops being one writes one ERR record and raises its `SW_FAIL` (cause 15, 17 or 18):

```text
Data: PSS_SAFETY_MONITOR_LOSTclient=<slot>, reason=<reason>, action=SW_FAIL, clears=only when a SafetyAIMonitor registers and heartbeats again
Data: PSS_PERCEPTION_MONITOR_LOSTclient=<slot>, reason=<reason>, action=SW_FAIL, clears=only when a PerceptionMonitor registers and heartbeats again
Data: PSS_MDX_CLIENT_LOSTclient=<slot|none>, instance=<k>, reason=<reason>, action=SW_FAIL, clears=only when MDX instance <k> registers and heartbeats again
```

PSB joins a record's name and data with no separator, as for every PSS record, so
match on the name as a prefix:
`journalctl | grep -E 'PSS_(SAFETY_MONITOR|PERCEPTION_MONITOR|MDX_CLIENT)_LOST'`.

| `reason` | What happened |
| --- | --- |
| `heartbeat_budget_exhausted` | Hung: `max_hb_failures` heartbeat ticks (5 s each) missed — **4 min 12 s** at the shipped 50. Also written when a new monitor evicts a hung one before PSS had dropped it |
| `connection_dropped` | Crashed: its socket closed without a terminate |
| `unregistered` | Orderly `NvPSSTerminatePSSClient()`. Treated as a loss like the others: the site is exactly as unmonitored |
| `protocol_error_*` | PSS dropped it for a malformed request, a failed receive, a rejected re-registration, or a heartbeat with the wrong client type |
| `never_registered` | `client=none`: a source `nvpss.conf` requires (below) had not registered once the heartbeat budget had run out since PSS started. Raised once per PSS lifetime |

Restarting a monitor therefore latches every SDM, and a validated manual release is
needed once it is back. A PSS restart forgets the monitor without writing this
record; the SDMs catch that case from the presence report instead (cause 16,
above).

## Identifying an MDX client

`CLIENT_MDX` is the one client type that may have several clients at once, and
the type is all a plain registration carries — the slot is reused, the pid
changes on restart, and uid/gid are shared. So PSS cannot tell one anonymous MDX
client from another, and cannot say that "the" MDX client is gone.

A client makes itself identifiable by registering with an **instance id**:

```c
NvPSSRegisterPSSClientInstance(&clientId, CLIENT_MDX, 1);   /* mdx_client --instance-id 1 */
```

On the wire that is `REGISTER_CLIENT` with `size = 2` and the id in `reqPayload[1]`;
an anonymous registration is unchanged (`size = 1`), so older clients need no
change. PSS then treats `(CLIENT_MDX, k)` like the Safety Monitor: one client per
instance (a second claim is refused with `REGISTER_REJECTED_DUPLICATE_TYPE`), its
presence in every report, and cause 18 when it goes. An id above 8, or a non-zero
id on another client type, is refused with `REGISTER_REJECTED_INVALID_INSTANCE` (4).

**Anonymous MDX clients (instance 0) behave exactly as before** — test injectors,
the aisle-monitor sim, and anything not yet updated. Their hang is cause 8, which
ordinary traffic clears; their crash or unregister raises nothing.

**An anonymous MDX client is not a tracked safety source, by design.** Its slot id
is reused across clients, so it is no identity a loss could wait on; another
client's traffic can stand in for it. Losing it is still reported (cause 8), but
nothing holds the site until **it** is back. A deployment that relies on an MDX
client for safety evidence must give it a numbered instance, as the shipped
launchers do. Then its loss, its absence at start-up (`required_mdx_instances`)
and its rejected input all clear only on that instance's own evidence.

**`required_mdx_instances`** (see "Required safety sources" below) covers the one
case the "once seen" rule cannot: an instance that never came up. The launchers
start one `mdx_client` as instance 1 and the shipped file sets `1`; a deployment
that runs no `mdx_client` must set `0`, or every SDM latches about 4 min after
start.

**A hung instance holds its identity until the budget runs out.** Like the
monitors, a numbered instance can be evicted only once it is heartbeat-faulted. An
`mdx_client --instance-id 1` restarted while the old process is hung (socket open,
no heartbeats) is refused as a duplicate and exits; a supervisor retrying it
succeeds once the old one is faulted — about 4 min at `max_hb_failures = 50`. A
crashed or stopped process frees the identity at once.

## Required safety sources

Three **mandatory** `nvpss.conf` keys say which safety sources a deployment runs:

| Key | Values | Source |
| --- | --- | --- |
| `required_safety_monitor` | 0 or 1 | Safety AI Monitor |
| `required_perception_monitor` | 0 or 1 | Perception Monitor |
| `required_mdx_instances` | 0..8 | numbered `mdx_client` instances 1..N |

**Strict.** Missing, repeated, not a plain decimal number, or out of range: the
daemon refuses to start, naming the key. None is ever defaulted or clamped — a
fallback of 0 would silently switch off the detection the key exists for. The
daemon logs what it will enforce at startup (`PSS required sources`).

**One required-source policy.** For control-plane launches, `launch_psf.sh`
checks the monitor keys against the independent `--saim-mode` and `--pcm-mode`
selections: `1` for ACTIVE, `0` for LEARN or skip. A mismatch fails before
services start; the launcher never rewrites the configuration.
Learning-only jobs do not start PSS and do not require these keys.

**What "required" adds.** Every registered monitor and every numbered MDX instance
is tracked whether required or not. Requiring one adds that it must *come up*:
once the heartbeat budget since PSS start has run out, a required source joins the
expected set of every presence report, and one that never registered is reported
lost, `reason=never_registered`. Not before: the budget is the start-up grace, so a
normal boot does not latch the SDMs.

**Which SDMs act on it.** All of them apply the presence report and hold a lost
source until it is back: ATL, Proximity, combined ATL+Proximity, and
Crossing-Junction-Safety.

## Why `SOCK_SEQPACKET` rather than `SOCK_STREAM`

This section describes the **PSS → PSD** link, which has migrated. The PSS RPC
link is still `SOCK_STREAM` until task D1 moves all seven of its clients in one
change set; until then the two links differ in socket type despite both being
AF_UNIX, and only the one below carries the guarantees described here.

Message boundaries are preserved, so `Recv` returns exactly one whole message or
an error. Two consequences that shaped the code:

- **No `sendAll`/`recvAll` loops.** A partial transfer is not a reachable
  outcome (SI-6), so the framing logic those loops existed to protect is gone.
- **No `MSG_PEEK` demux.** The PSS → PSD server identifies an inbound message by
  its *length*; `static_assert`s in `NvPSSDToPSD.cpp` keep the shapes distinct,
  so a protocol change that made two collide breaks the build rather than
  misrouting a `DecisionResponse` into the registration path.

That second point is not a tidiness argument. Peeking at a type field to decide
how much to read is what allowed the daemon's `select()` loop and its sender to
both read one socket — gap **G-33**, where the loop could consume a
`DecisionResponse`, misparse it as a control message, and disconnect a healthy
client mid-`recv`.

## One reader per socket

`PSSDServerThread` is the **only** caller of `NvPSFTransportRecv` in
`NvPSSDToPSD`. A sender transmits its `DecisionRequest` and then parks on a
per-client mailbox; the server thread reads the response and hands it over.

This is ownership, not locking. A mutex around two readers would have made the
interleaving safe and left the behaviour wrong — the loop would still sometimes
consume a response addressed to someone else.

**The mailbox is one slot per client, which makes TR-8 load-bearing.** TR-8 says
this link is single-request-in-flight, and `SafetyEventManager` enforces it by
holding `psdSendMtx_` across the whole send-and-wait. A second concurrent sender
to the same client is refused with an explicit `REQUEST ALREADY IN FLIGHT`
diagnostic rather
than silently overwriting the first sender's slot.

## Credentials

Both links validate `SO_PEERCRED` against an allowlisted GID, so **every PSF
process must share a primary group**. A peer whose primary GID does not match is
refused at accept. Note the check is on the *primary* GID: carrying the PSF group
as a supplementary group is not sufficient. See residual risk 3 in the design —
GID granularity cannot separate PSF components from each other, so any
compromised in-group process can impersonate an endpoint while `/run/nvpsf`
remains mode 1777.
