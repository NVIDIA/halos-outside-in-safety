<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Config-driven client for capturing messages from perception

This client consumes Kafka topics **mdx-events** and **mdx-frames**, parses Behavior and FrameMessage protobufs, and maps incoming alerts to PSS safety events using **user-defined rules** from a protobuf config file. No code changes are needed for different use-cases—only the config file.

## Usage

```bash
mdx_client -c <event_mapping_config> -s <sensor_config.conf> [-d|--debug] [-b <broker>]
```

### Required arguments

- **`-c, --config <path>`**: Event mapping config (protobuf text or binary).
- **`-s, --sensor-config <path>`**: Sensor configuration file mapping sensor names to pipeline IDs. CSV format: `pipelineId, sensorName, rtspUrl` (one sensor per line; lines starting with `#` are comments). This file is shared with SAI and the PSS daemon.

### Optional arguments

- **`-d, --debug`**: Do not register with PSS or report events. Matched events are printed to stdout instead.
- **`-b, --broker <addr>`**: Kafka broker address (default: `localhost:9092` or `MDX_MSGBUS_BROKERS` env var).

### Sensor config format (sensor_config.conf)

```
# pipelineId, sensorName, rtspUrl
1, Camera_primary,   rtsp://192.168.1.10/Camera_primary
2, Camera_secondary, rtsp://192.168.1.11/Camera_secondary
3, Camera_dock_north, rtsp://192.168.1.12/Camera_dock_north
```

When a Kafka message arrives with a `sensorId` not listed in the config, the client logs a warning and sets `pipelineID=0` (passthrough, no fusion in PSS daemon).

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

Invalid violation filters are rejected when configuration loads. Any non-empty restricted/confined filter must explicitly use `message_source: "mdx-frames"` and `alert_type: "roi"`; any non-empty generic social-distance filter must use `message_source: "mdx-frames"` and `alert_type: "social_distancing"`. Violation filters cannot be combined in one rule. Both asserted and false ROI rules may constrain `object_type`; it is evaluated only against a current-frame object proven by the ROI.

**First matching rule wins.** Rule order in the config matters.

### mdx-frames: how the three violations are derived (proto/gen/mdx-messages)

| Violation                   | Source in FrameMessage                          |
|----------------------------|-------------------------------------------------|
| restricted_area_violation  | Each readable ROI with `id`, `type`, exact `info["restrictedAreaViolation"]`, and a unique `objectIds` member that resolves to one current object of the same type. The value is exactly `"true"` or `"false"`. |
| confined_area_violation   | The same current-ROI/current-object proof, using exact `info["confinedAreaViolation"]`. |
| social_distancing_violation | Generic frame-social candidate: `proximityDetections > 0` is `true`; zero or an absent optional `socialDistancing` submessage is `false`; negative creates no generic candidate. |

Alerts from social distancing use `alert_type: "social_distancing"`.

### Per-frame assertion and clear rules

Every valid current-frame candidate is evaluated independently; there are no transition-only alert types. A missing/unreadable ROI, missing ID/type/object ID, invalid ROI value, ambiguous ID, unresolved current object, or ROI/current-object type mismatch produces no restricted/confined candidate and never infers a false value. Both asserted and false rules may constrain `object_type`; the candidate carries the proven current object type.

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

Every pair rule uses `alert_type: "social_distancing"` and an explicitly present `proximity_violation` field. `true` requires a finite positive `distance_threshold_meters`; `false` requires that the threshold be omitted. The client selects the smallest configured true threshold at or above the pair’s current Euclidean distance. Every true and false pair rule requires a unique, non-empty `rule_id` shorter than `IDENTIFIER_NAME_LENGTH`; that configured ID is emitted as `SafetyEvent.ruleIdentifier`. Pair rules cannot set `social_distancing_violation`.

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
- **Regenerate protos** (if you change `event_mapping.proto`):
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
