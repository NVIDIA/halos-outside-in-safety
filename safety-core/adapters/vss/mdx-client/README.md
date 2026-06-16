<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Config-driven client for capturing messages from perception

This client consumes Kafka topics **mdx-events** and **mdx-frames** parses Behavior and FrameMessage protobufs, and maps incoming alerts to PSS safety events using **user-defined rules** from a protobuf config file.

## Usage

```bash
mdx_client -c <event_mapping_config> -s <sensor_config.conf> [-d|--debug] [-b <broker>]
```

### Required arguments

- **`-c, --config <path>`**: Event mapping config (protobuf text or binary).
- **`-s, --sensor-config <path>`**: Sensor configuration file mapping sensor names to pipeline IDs. CSV format: `pipelineId, sensorName, rtspUrl` (one sensor per line; lines starting with `#` are comments). 

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
**Protobuf text format** (e.g. `safety-core/adapters/vss/event-mappings/`)


## Event mapping config (proto)

Rules are defined in `proto/event_mapping.proto`:

- **EventMappingConfig**: repeated **EventMappingRule**
- **EventMappingRule**:
  - **Conditions** (empty string = match any):
    `message_source`, `alert_type`, `event_type`, `object_type`, `rule_id` (exact match, case-insensitive)
  - **mdx-frames violation filters** (values: `"any"` / empty, `"true"`, `"false"`):
    - **restricted_area_violation** – from `FrameMessage.rois` (TypeCount): restricted area
    - **confined_area_violation** – from `FrameMessage.rois` (TypeCount): confined area
    - **social_distancing_violation** – from `FrameMessage.socialDistancing` (SD.proximityDetections)
  - **Output**: `output_event`, `severity`

**First matching rule wins.** Rule order in the config matters.

### mdx-frames: how the three violations are derived (proto/gen/mdx-messages)

| Violation                   | Source in FrameMessage                          |
|----------------------------|-------------------------------------------------|
| restricted_area_violation  | `repeated TypeCount rois` (field 7): roi type or metadata |
| confined_area_violation   | `repeated TypeCount rois` (field 7): roi type or metadata |
| social_distancing_violation | `socialDistancing` (SD, field 8): `proximityDetections > 0` |

Alerts from social distancing use `alert_type: "social_distancing"`.

### Violation cleared events

When a violation transitions from active to cleared (e.g. person leaves restricted area), the client emits a **separate** alert so you can map it to a different event:

| alert_type                             | When emitted |
|----------------------------------------|--------------|
| `restrictedAreaViolationCleared`       | ROI that had restricted violation now has none |
| `confinedAreaViolationCleared`         | ROI that had confined violation now has none   |
| `socialDistancingViolationCleared`     | Frame had SD violation, current frame has none |

Add rules with these `alert_type` values to map cleared transitions to distinct output events (e.g. EVENT_6, EVENT_8, EVENT_9).

### Custom proximity (distance threshold)

The AI pipeline may emit `socialDistancingViolation` at a fixed threshold (e.g. 2 m). To trigger a **separate** event when two objects are closer than a **custom** distance (e.g. 1 m), add a rule with:

- **distance_threshold_meters** (e.g. `1.0`)
- **object_type** (e.g. `"person"`) – first object type
- **object_type_secondary** (optional) – second type for mixed pairs (e.g. person + forklift); omit for same type (e.g. person–person)
- **output_event**, **severity** as usual

The client uses **Object.coordinate** (x, y, z) from `FrameMessage.objects` and assumes **meters**. Only objects that have `coordinate` set are considered. At most one event per rule per frame is reported when any qualifying pair is within the threshold.

## Example mappings

| User intent                     | Rule conditions (example)                                       | Output   |
|---------------------------------|-----------------------------------------------------------------|----------|
| Person crossed tripwire-1       | mdx-events, tripwire, IN, person, rule_id=tripwire-1            | EVENT_0  |
| Person enters restricted area   | mdx-frames, roi, person, restricted_area_violation=true         | EVENT_2  |
| Person in confined area         | mdx-frames, roi, confined_area_violation=true                   | EVENT_3  |
| Social distancing violation     | mdx-frames, social_distancing, social_distancing_violation=true | EVENT_4  |
| Restricted area cleared         | mdx-frames, alert_type=restrictedAreaViolationCleared           | EVENT_6  |
| Confined area cleared           | mdx-frames, alert_type=confinedAreaViolationCleared             | EVENT_8  |
| Social distancing cleared       | mdx-frames, alert_type=socialDistancingViolationCleared         | EVENT_9  |
| Person–person within 1 m        | mdx-frames, distance_threshold_meters=1.0, object_type=person   | EVENT_10 |

See `safety-core/adapters/vss/event-mappings` for a full text-format example.

## Build

- **With CMake**: Build the `mdx_client` target from the top-level `safety-core` build.
- **Regenerate protos** (if you change `event_mapping.proto`):
  ```bash
  protoc -I proto --cpp_out=proto/gen/event-mapping proto/event_mapping.proto
  ```
