<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Config-driven client for capturing messages from perception

This client consumes Kafka topics **mdx-events** and **mdx-frames**, parses Behavior and FrameMessage protobufs, and maps incoming alerts to PSS safety events using **user-defined rules** from a protobuf config file. No code changes are needed for different use-cases—only the config file.

## Usage

```bash
# 2D
mdx_client -c <event_mapping_config> -s <sensor_config.conf>           [-i <1..8>] [-d|--debug] [-b <broker>]
# 3D / BEV
mdx_client -c <event_mapping_config> -g <sensor_pipelines_config.conf> [-i <1..8>] [-d|--debug] [-b <broker>]
```

### Required arguments

- **`-c, --config <path>`**: Event mapping config (protobuf text or binary).
- **Exactly one identity file**, `-s` or `-g`. Both, or neither, is a fatal argument error.
  - **`-s, --sensor-config <path>`**: **2D.** CSV: `pipelineId, sensorName, rtspUrl` (one per line; `#` comments). Shared with SAI and the PSS daemon. Replaces the old `stream.txt`.
  - **`-g, --sensor-pipelines-config <path>`**: **3D / BEV.** CSV: `pipelineId, sensorName`. Names the **BEV group**, not the cameras.

#### Why exactly one, and never both

This is a safety constraint, not tidiness.

The map answers one question — *which pipelineID does this Kafka `sensorId` mean?* — and the answer differs by deployment topology. In 2D the cameras are the event sources. In 3D they are not: the cameras are calibrated into a BEV group, Kafka reports under the group, and only the group is a decision-driving identity.

Load both maps and both answers exist at once. A stray per-camera event in a 3D deployment would then resolve to that camera's pipelineID and be reported as though it were decision-driving — gated against **one camera's** health instead of the group's. It would be accepted silently, and wrong in the direction that matters.

With one map per mode a topology mismatch is simply **unresolvable**, and an unresolvable event is dropped (see below). Neither is refused for the same reason in reverse: falling back to an empty map would drop every event, which looks identical to a quiet deployment.

The client logs the file it loaded and the identities it resolved at startup. In 3D that list should hold the group alone; cameras appearing there mean a 2D file was supplied to a 3D deployment, and every group-addressed event would be dropped.

Handing `-g` a three-column `sensor_config.conf` is refused, with a diagnostic naming the two-column shape — not silently accepted with the `rtspUrl` ignored.

### Optional arguments

- **`-d, --debug`**: Do not register with PSS or report events. Matched events are printed to stdout instead.
- **`-b, --broker <addr>`**: Kafka broker address (default: `localhost:9092` or `MDX_MSGBUS_BROKERS` env var).
- **`-i, --instance-id <1..8>`**: PSS instance id. **Deployments pass it; the launchers pass `1`.** See below.

#### Why `--instance-id` matters

PSS cannot otherwise tell one MDX client from another, so without it PSS cannot say
this client has stopped: a crash or an orderly exit raises nothing, and a hang raises
a fault that the next usable request — from any client — clears. The SDMs would then
hold their last decision with nothing feeding it.

With `--instance-id k` the client registers as MDX instance `k`, which PSS tracks like
the Safety Monitor:

- **One client per instance.** A second `mdx_client` with the same id is refused at
  registration and exits.
- **Any loss latches every SDM** — hang, crash, orderly exit — and no PLC release is
  accepted until instance `k` itself is back. Traffic from other MDX clients does not
  count. A restarted `mdx_client` therefore needs a manual release afterwards.
- **`required_mdx_instances` in `nvpss.conf`** (mandatory; shipped as `1`) also
  reports an instance that never registers at all, about 4 min after the daemon
  starts.
- **A hung instance holds its identity** until its heartbeat budget runs out (about
  4 min): a restarted `mdx_client` with the same id is refused until then. A crashed
  or stopped one frees it at once.

The id is logged at startup (`PSS instance: 1`) and at registration. Omitting it
registers anonymously, which is only right for test tools. An anonymous client is not
a tracked safety source:
- its loss is reported but clears on any client's traffic;
- a missing one at start-up is not detected;
- its rejected input is tracked under one identity shared by every anonymous client.

Give every `mdx_client` that supplies safety evidence a numbered instance.

### Sensor config format (`sensor_config.conf`, 2D)

```
# pipelineId, sensorName, rtspUrl
1, Camera_primary,   rtsp://192.168.1.10/Camera_primary
2, Camera_secondary, rtsp://192.168.1.11/Camera_secondary
3, Camera_dock_north, rtsp://192.168.1.12/Camera_dock_north
```

### Sensor pipelines format (`sensor_pipelines_config.conf`, 3D / BEV)

```
# pipelineId, sensorName
4, bev-sensor-1
```

No `rtspUrl` column, deliberately: a BEV group has no stream. That is exactly why it cannot be expressed in `sensor_config.conf` and why these are two files rather than one with an extra column. The cameras do **not** appear here — SAI still watches every camera through `sensor_config.conf`, but they are not what events arrive under.

A Kafka `sensorId` absent from whichever file was loaded has no pipelineID, and its event is **dropped** — see the section below.

The config file can be:
- **Protobuf text format** (e.g. `safety-core/adapters/vss/event-mappings/`)
- **Protobuf binary** (e.g. produced by `protoc --encode=...`)

## Event mapping config (proto)

Rules are defined in `safety-core/adapters/vss/mdx-msg-codec/proto/event_mapping.proto`:

- **EventMappingConfig**: repeated **EventMappingRule**
- **EventMappingRule**:
  - **Conditions** (empty string = match any):
    `message_source`, `alert_type`, `event_type`, `object_type`, `rule_id` (exact match, case-insensitive, except that a proximity-pair `rule_id` is its required PSS output identifier)
    A non-empty condition must be present on the candidate and match. In particular, an asserted `object_type` rule does not match when the current-frame object type is absent.
  - **mdx-frames violation filters** (values: `"any"` / empty, `"true"`, `"false"`):
    - **restricted_area_violation** – from `FrameMessage.rois` (TypeCount): restricted area
    - **confined_area_violation** – from `FrameMessage.rois` (TypeCount): confined area
    - **social_distancing_violation** – from `FrameMessage.socialDistancing` (SD.proximityDetections)
  - **Output**: `output_event`

Invalid violation filters are rejected when configuration loads. Any non-empty restricted/confined filter must explicitly use `message_source: "mdx-frames"` and `alert_type: "roi"`; any non-empty generic social-distance filter must use `message_source: "mdx-frames"` and `alert_type: "social_distancing"`. Violation filters cannot be combined in one rule. Both asserted and false ROI rules may constrain `object_type`; it is evaluated only against a current-frame object proven by the ROI, except for the decoded-empty-ROI clear behavior below, which uses the configured rule identity and type.

**First matching rule wins.** Rule order in the config matters.

### A sensor absent from the identity file now drops the event

If a Kafka `sensorId` does not match any entry in the identity file that was
loaded — `sensor_config.conf` in 2D, `sensor_pipelines_config.conf` in 3D — the
event is **dropped** and an error names the id. It is no longer sent with
`pipelineID = 0`.

This is also the mechanism that makes the one-file-per-mode rule safe: a
per-camera event in a 3D deployment, or a group-named event in a 2D one, simply
has no pipelineID and never reaches PSS.

That fallback was unsafe: `0` is the PSS daemon's *unknown sensor* passthrough, so
the event skipped identity validation and was stamped healthy permanently —
driving ATL and PXC decisions with no perception-health gating at all. The daemon
now refuses such events too (`REPORT_REJECTED_UNRESOLVED_SENSOR`); dropping here as
well is what puts the offending **name** in the log, which is what you need to fix
the configuration.

**If events stop flowing after an upgrade, check this log first** — the usual
cause is a sensor name in the Kafka stream that is missing from
`sensor_config.conf`, which previously failed silently.

In `--debug` mode an unresolvable sensor is dropped before the event is printed,
so it no longer appears in the debug dump. That is deliberate: printing an event
as though it would be sent, when it is being discarded, is misleading.

### `output_event` must be claimed by at most one rule — across *every* client

**Nothing checks this.** mdx-client does not reject two rules sharing an
`output_event`, within one file or across files, and the PSS daemon does not
detect two clients claiming the same event type. `EventMapping.cpp` reads
`output_event` into the rule and never compares it against another rule's. The
uniqueness checks that do exist (`insertUniqueProximityPairRule`,
`insertUniqueProximityPairRuleId`) cover proximity-pair identity, not the output
event.

**This is yours to enforce.** Every `EVENT_*` value must be claimed by at most
one rule across all mapping files deployed against one PSS daemon. The shipped
mappings are disjoint by construction (`EVENT_0`–`EVENT_5` against
`EVENT_8`/`EVENT_9`/`EVENT_10`); a third mapping file, or a second instance of an
existing one, is where this gets violated.

**What a collision costs.** The PSS daemon partitions fusion candidates by event
type. Two unrelated detections sharing a type become eligible to fuse with each
other. For a state-setting or last-event-wins type the effect is nil. For an
**accumulating** type it under-counts: two distinct physical events collapse into
one. Nothing detects this at runtime — the symptom is a count that is too low,
which is the direction that matters on a safety path.

There is no diagnostic to watch for. Verify disjointness when you author or
deploy a mapping, not afterwards.

### mdx-frames: how the three violations are derived (../mdx-msg-codec/proto/gen/mdx-messages)

| Violation                   | Source in FrameMessage                          |
|----------------------------|-------------------------------------------------|
| restricted_area_violation  | Each readable ROI with `id`, `type`, exact `info["restrictedAreaViolation"]`, and a unique `objectIds` member that resolves to one current object of the same type. The value is exactly `"true"` or `"false"`. A successfully decoded frame with `roisCount == 0` also creates a false candidate for each valid configured restricted false rule. |
| confined_area_violation   | The same current-ROI/current-object proof, using exact `info["confinedAreaViolation"]`. A successfully decoded frame with `roisCount == 0` also creates a false candidate for each valid configured confined false rule. |
| social_distancing_violation | Generic frame-social candidate: `proximityDetections > 0` is `true`; zero or an absent optional `socialDistancing` submessage is `false`; negative creates no generic candidate. |

Alerts from social distancing use `alert_type: "social_distancing"`.

### Per-frame assertion and clear rules

Every valid current-frame candidate is evaluated independently; there are no transition-only alert types. A missing/unreadable ROI, missing ID/type/object ID, invalid ROI value, ambiguous ID, unresolved current object, or ROI/current-object type mismatch produces no restricted/confined candidate and never infers a false value. The sole exception is a successfully decoded frame with an explicitly empty `rois` list: the client creates one false restricted or confined candidate per valid configured false rule, using that rule's `rule_id` and `object_type`. Decode failures and non-empty ROI frames do not take this clear path. Both asserted and false rules may constrain `object_type`; normal ROI candidates carry the proven current object type, while the decoded-empty-ROI clear candidate carries the configured type.

For ATL restricted-area rules, the approved application-event mapping is:

```text
rules {
  name: "Person restricted area ROI violation"
  message_source: "mdx-frames"
  alert_type: "roi"
  object_type: "person"
  rule_id: "roi-id-1"
  restricted_area_violation: "true"
  output_event: "EVENT_4"
}
rules {
  name: "Person restricted area ROI violation cleared"
  message_source: "mdx-frames"
  alert_type: "roi"
  object_type: "person"
  rule_id: "roi-id-1"
  restricted_area_violation: "false"
  output_event: "EVENT_5"
}
```

`EVENT_5` clears only the receiving application-event state. It is not proof of physical safety and does not release a downstream fault latch or manual-release requirement.

### Configuration-defined proximity pairs

A proximity mapping describes an unordered current-frame type pair using `object_type_primary` and `object_type_secondary`; `object_type` is the primary fallback, and an omitted secondary type means a same-type pair. Both current objects require unique numeric IDs, non-empty types, and finite coordinates. Invalid or ambiguous data creates no pair candidate.

Every pair rule uses `alert_type: "social_distancing"` and an explicitly present `proximity_violation` field. `true` requires a finite positive `distance_threshold_meters`; `false` requires that the threshold be omitted. The client selects the smallest configured true threshold at or above the pair’s current Euclidean distance. Every true and false pair rule requires a unique, non-empty `rule_id` shorter than `IDENTIFIER_NAME_LENGTH`. **The `rule_id` is not sent to PSS.** Wire schema 4 removed `SafetyEvent.ruleIdentifier`; rule identity on the wire is the `output_event` value alone. The `rule_id` remains required because it is the pair rule's identity client-side — an empty or malformed one still rejects the alert — but it stays in this process. Pair rules cannot set `social_distancing_violation`.

For a successful frame decode, absent `socialDistancing` is false. A present submessage is false only when `proximityDetections: 0` and `info["proximityViolation"]: "false"`; every other non-negative form is true, and a negative count creates no pair candidate. A false pair, or a true pair outside all configured thresholds, can match only `proximity_violation: false`. The configured thresholds for a true group must not exceed the producer’s SD threshold; otherwise that group produces no candidate.

Every matching physical pair is evaluated independently, so multiple pairs in one frame can emit multiple configured events. The producer contract must report SD only for the configured type pair; cluster data does not associate a violation with a specific pair of object IDs.

## Example mappings

| User intent                     | Rule conditions (example)                                   | Output   |
|---------------------------------|-------------------------------------------------------------|----------|
| Person crossed tripwire-1       | mdx-events, tripwire, IN, person, rule_id=tripwire-1        | EVENT_0  |
| Person enters restricted area   | mdx-frames, roi, person, roi-id-1, restricted_area_violation=true | EVENT_4 |
| Person restricted-area false value | mdx-frames, roi, person, roi-id-1, restricted_area_violation=false | EVENT_5 |
| Person in confined area         | mdx-frames, roi, current object type, confined_area_violation=true | Configuration-owned |
| Generic frame-social candidate  | mdx-frames, social_distancing, social_distancing_violation=true | Configuration-owned |
| Configured pair false           | mdx-frames, social_distancing, proximity_violation=false, primary/secondary pair, unique rule_id | Configuration-owned |
| Person–person within 1 m        | mdx-frames, social_distancing, proximity_violation=true, distance_threshold_meters=1.0, unique rule_id | Configuration-owned |

See `safety-core/adapters/vss/event-mappings` for a full text-format example.

## Build

- **With CMake**: Build the `mdx_client` target from the top-level `safety-core` build.
- **Regenerate protos** (protos live in the sibling `mdx-msg-codec/` — `cd` there first, then if you change `event_mapping.proto`):
  ```bash
  protoc -I proto --cpp_out=proto/gen/event-mapping proto/event_mapping.proto
  ```
  Use the protobuf 3.21.12 toolchain that the codec is built against rather than
  an arbitrary host `protoc`. The source lives in
  `safety-core/adapters/vss/mdx-msg-codec/proto/`, and generated files belong
  under `proto/gen/event-mapping/`.

## Dependencies

PSS daemon (NvPSS*), PSB (NvPSB), MsgBus (NvPSFMsgBus), MsgCodec (NvPSFMsgCodec).

## Directory layout

```
safety-core/adapters/vss/mdx-client/
├── include/
│   ├── common.hpp             # AlertMessage struct (pure C)
│   ├── MDXClient.hpp          # Entry point
│   ├── EventsParser.hpp       # mdx-events parser (uses NvPSFMsgCodec)
│   ├── FramesParser.hpp       # mdx-frames parser (uses NvPSFMsgCodec)
│   ├── SafetyEventReporter.hpp # Rule matching + PSS reporting (uses NvPSFMsgCodec)
│   ├── FrameOrdinalTracker.hpp # Parser-owned per-sensor frame ordinals
│   ├── FrameViolationUtils.hpp # ROI violation/object-ID validation
│   ├── FrameReportingUtils.hpp # Stateless frame ordinal/scale selection
│   ├── ProximityPairUtils.hpp  # Pair gate, threshold, and coordinate validation
│   └── EventMappingValidation.hpp # Mapping validation and strict conditions
├── src/
│   ├── MDXClient.cpp          # Config-driven client logic
│   ├── EventsParser.cpp
│   ├── FramesParser.cpp
│   ├── SafetyEventReporter.cpp
│   └── main.cpp
├── CMakeLists.txt
├── mdx_client.cmake
└── README.md
```

Event-mapping samples live under **`safety-core/adapters/vss/event-mappings/`**.
