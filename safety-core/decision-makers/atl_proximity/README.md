<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Combined ATL + forklift/person proximity (ATL dataset)

## Purpose

Run ATL's existing safety functions and a new forklift-to-person proximity
function together, against the ATL dataset, as one deployment.

The two functions stay separate all the way to the PLC. ATL keeps its own
binary and its own command stream; proximity gets a new decision maker with its
own command stream on a different port. They share one event mapping file, one
Kafka consumer and one PSD gateway, so a single `mdx_client` feeds both.

## Description

Three processes carry the application, on top of the usual `nvpss_daemon` and
`nvpsd_gateway`:

| Process | Origin | Consumes | Purpose |
|---|---|---|---|
| `mdx_client` | `adapters/vss/mdx-client` | Kafka `mdx-events`, `mdx-frames` | Sole Kafka consumer; maps alerts to PSS safety events |
| `atl_sdm` | `decision-makers/atl`, reused as-is | `EVENT_0`..`EVENT_5` | ATL safety decisions |
| `atl_proximity_sdm` | this component | `EVENT_8`..`EVENT_10` | Forklift-to-person proximity decisions |

Each SDM registers with the gateway only for the event types it consumes, so
each sees its own slice of the stream.

**ATL is referenced, not rebuilt.** This component builds no ATL sources; the
deployment launches the `atl_sdm` binary produced by `decision-makers/atl`.
Compiling ATL's sources into a second binary would create two ATL SDMs that are
identical only while both sets of compiler flags stay in sync, and ATL's flags
are behavioural rather than cosmetic. `decision-makers/proximity` is likewise
left untouched — it is already field-validated and it does not arbitrate by
worst case, so the proximity logic here is this component's own.

### Event mapping

`event_mapping_atl_proximity.pb.txt` holds both sets. `EVENT_6` and `EVENT_7`
are intentionally unused so the proximity tiers keep the same numbering as the
standalone proximity app, which makes the two deployments comparable when
reading `pss.log` side by side.

| Event | Source | Condition | Command |
|---|---|---|---|
| `EVENT_0`..`EVENT_3` | ATL | forklift / person tripwire IN and OUT | ATL policy |
| `EVENT_4`, `EVENT_5` | ATL | person restricted-area ROI asserted / cleared | ATL policy |
| `EVENT_8` | proximity | no violation: distance > 2 m, or SD flag false | `NORMAL` |
| `EVENT_9` | proximity | 2 m >= distance > 1 m | `REDUCE` |
| `EVENT_10` | proximity | distance <= 1 m | `STOP` |

Section 1 is inherited verbatim from
`adapters/vss/event-mappings/atl/event_mapping_atl.pb.txt`. Do not renumber it:
`decision-makers/atl/sdm/ATLControl.cpp` switches on these exact values.

### Decision policy

`atl_proximity_sdm` accumulates observations into per-pair retained state, then
derives one command from a snapshot of that state. The emitted command is the
**worst tier across all live forklift/person pairs**, so a pair reporting a safe
distance cannot silence a different pair that is still critical.

A pair stays live for `--pair_ttl_ms` after its last sighting (default 2000 ms,
range 200–60000). Up to `ATLPXC_MAX_TRACKED_PAIRS` (128) pairs are tracked;
overflow is handled fail-safe in `AtlPxcPairFusionObserve()`.

## Prerequisites

### 1. Kernel POSIX message queue size

**This is required. The stack will not start without it.**

`DecisionRequest` scales with `MAX_SENSORS_DATA_SUMMARY_SIZE`
(`components/event-integrator/daemon/include/pss_protocol.h`). At the current 32 slots the struct is
**8369 bytes**, which exceeds the kernel's default `fs.mqueue.msgsize_max` of
8192. `mq_open()` then fails with `EINVAL` and PSS cannot create its queue.

Two things must agree:

- `MQ_MAX_MSG_SIZE` in
  `components/safecomm/posix_msg_que/include/posix_msg_que.h`, currently
  `16384`, must stay >= `sizeof(DecisionRequest)`.
- The enclosing IPC namespace must permit that size.

Under Docker, `mqueue` sysctls are IPC-namespaced, so set it per container:

```bash
docker run --sysctl fs.mqueue.msgsize_max=16384 ...
```

On bare metal:

```bash
sudo sysctl -w fs.mqueue.msgsize_max=16384
# persist across reboots
echo 'fs.mqueue.msgsize_max = 16384' | sudo tee /etc/sysctl.d/99-psf-mqueue.conf
```

If you change the slot count, **two constants must move together** —
`MAX_SENSORS_DATA_SUMMARY_SIZE` (wire struct) and `MAX_EVENTS_PER_QUE`
(`components/event-integrator/daemon/include/NvPSSSafetyEventManager.hpp`, PSS
queue depth). The drain
takes the smaller of the two per `DecisionRequest`, so raising only one has no
effect. Also bump `PSS_SCHEMA_VERSION`, since the change is wire-incompatible,
and rebuild and redeploy PSS, the gateway and both SDMs together.

### 2. Runtime environment

- VSS deployed and publishing to Kafka topics `mdx-events` and `mdx-frames`.
- A `sensor_config.conf` mapping sensor names to pipeline IDs
  (`pipelineId, sensorName, rtspUrl`); see `adapters/vss/mdx-client/README.md`.
- Two free UDP ports for the PLC command streams. Defaults are 12345 for
  `atl_sdm` and 12346 for `atl_proximity_sdm`; they must differ.

## How to run

### Through the launcher (recommended)

```bash
/opt/nvidia/psf/bin/launch_psf.sh \
  --app atl_proximity \
  --sensor-config /opt/nvidia/psf/configs/sensor_config.conf \
  --cmd_rx_port 12345 \
  --proximity_cmd_rx_port 12346
```

This starts the gateway, the PSS daemon, both SDMs and one `mdx_client`, in that
order. Options that apply to this app:

| Option | Effect |
|---|---|
| `--cmd_rx_port PORT` | PLC port for `atl_sdm` |
| `--proximity_cmd_rx_port PORT` | PLC port for `atl_proximity_sdm`; must differ from the above |
| `--pair_ttl_ms MS` | Pair liveness window for the proximity SDM |
| `--cmd_rx_ip IP` | PLC address, shared by both SDMs |
| `--broker ADDR` | Kafka broker passed to `mdx_client` |

The last two SDM options are accepted only with `--app atl_proximity`, since
they have no meaning for the single-SDM apps.

The decision makers are CCPLEX only; this app has no FSI variant.

### Under Docker

Remember the sysctl from the prerequisites:

```bash
docker run -d --name nv-psf --network host --runtime=nvidia \
  --sysctl fs.mqueue.msgsize_max=16384 \
  <psf-image> \
  --app atl_proximity \
  --cmd_rx_ip 127.0.0.1 --cmd_rx_port 12349 --proximity_cmd_rx_port 12350 \
  --pair_ttl_ms 2000 \
  --sensor-config /opt/nvidia/psf/configs/sensor_config.conf
```

### Running the proximity SDM directly

```bash
/opt/nvidia/psf/apps/atl_proximity/atl_proximity_sdm --help
```

Run `--help` for the full flag list; the defaults are gateway
`127.0.0.1:50000`, PLC `127.0.0.1:12346`, `--decision_interval_ms 5000` and
`--pair_ttl_ms 2000`. It reaches the gateway over UDP and links no PSF shared
library, so no `LD_LIBRARY_PATH` is needed.

## Watching what it emits

Each SDM's command stream can be decoded on its own port:

```bash
/opt/nvidia/psf/apps/atl/atl_sdm_cmd_receiver -p 12349 --log-file atl.log &
/opt/nvidia/psf/apps/atl_proximity/atl_proximity_sdm_cmd_receiver -p 12350 --log-file apx.log &
```

For the matched-event stream straight off Kafka, without registering with PSS
or disturbing the running stack:

```bash
/opt/nvidia/psf/apps/mdx-client/mdx_client \
  --config /opt/nvidia/psf/apps/atl_proximity/event_mapping_atl_proximity.pb.txt \
  --sensor-config /opt/nvidia/psf/configs/sensor_config.conf \
  --debug
```

Debug output carries the event type and both object IDs per matched rule, which
is what makes an independent per-pair replay possible.

## Constraints worth knowing

**Only forklift/person pair rules belong in this mapping.** `mdx-client`'s
`stringToObjectType()` has no `FORKLIFT` enumerator, so a forklift reaches the
SDM as the generic `OBJECT` value and is indistinguishable from any other
non-person object. The SDM can verify that a pair has exactly one person
endpoint, and warns when it does not, but it cannot confirm the peer is a
forklift. Adding a second pair type here would feed that pair into the same
forklift PLC command stream with nothing able to tell them apart. Add a
separate app and mapping instead.

**Pair scoping is enforced by the event mapping**, via
`object_type_primary` / `object_type_secondary`. Matching is unordered.

**Thresholds mirror the standalone proximity app**, so operator-facing
behaviour is unchanged across the two deployments.

## Directory layout

```
safety-core/decision-makers/atl_proximity/
├── include/
│   └── atl_proximity_pair_fusion.h     # per-pair retained state, worst-case arbitration
├── sdm/ccplex/
│   ├── AtlProximity.cpp                # app layer: CLI parsing and defaults
│   ├── AtlProximityControl.cpp         # decision core, gateway and PLC plumbing
│   └── AtlProximityControl.h
├── CMakeLists.txt
├── atl_proximity_sdm.cmake             # atl_proximity_sdm target
├── atl_proximity_sdm_cmd_receiver.cmake
└── README.md
```

The event mapping lives with the other VSS mappings, at
`safety-core/adapters/vss/event-mappings/atl_proximity/event_mapping_atl_proximity.pb.txt`.

The PLC wire contract is shared read-only with the standalone proximity app
(`decision-makers/proximity/include/proximity_cmd_pkt.h`: 64-byte `CmdPacket`,
opcodes, CRC). Both SDMs talk to the same PLC, so a private copy could drift
from the format the PLC actually implements. The command receiver is built from
`decision-makers/proximity/udp_cmd_receiver/cmd_rx.cpp` for the same reason.
