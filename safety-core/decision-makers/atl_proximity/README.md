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

This legacy app is distinct from config-driven `--app atl,pxc`, which launches
`proximity_sdm` instead. See the [PXC guide](../proximity/README.md) for that
combination; its introduction does not change this app's decision policy.

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
remain unused, and this app retains its historic proximity codes 8..10.
The config-driven PXC mapping now uses 12..23; do not mix the two mappings or
combine this legacy mapping with ATL zone-two subscriptions (6..11).

| Event | Source | Condition | Command |
|---|---|---|---|
| `EVENT_0`..`EVENT_3` | ATL | forklift / person tripwire IN and OUT | ATL policy |
| `EVENT_4`, `EVENT_5` | ATL | person restricted-area ROI asserted / cleared | ATL policy |
| `EVENT_8` | proximity | no violation: distance > 2 m, or SD flag false | `NORMAL` |
| `EVENT_9` | proximity | 2 m >= distance > 1 m | `REDUCE` |
| `EVENT_10` | proximity | distance <= 1 m | `STOP` |

The ATL rules use the zone-one bindings in `atl_sdm_zone1.conf`. Keep that
selected config aligned with this mapping; ATL reads its bindings at startup.

### Decision policy

`atl_proximity_sdm` accumulates observations into per-pair retained state, then
derives one command from a snapshot of that state. The emitted command is the
**worst tier across all live forklift/person pairs**, so a pair reporting a safe
distance cannot silence a different pair that is still critical.

A pair stays live for `--pair_ttl_ms` after its last sighting (default 2000 ms,
range 200–60000). Up to `ATLPXC_MAX_TRACKED_PAIRS` (128) pairs are tracked;
overflow is handled fail-safe in `AtlPxcPairFusionObserve()`.

## Prerequisites

- VSS deployed and publishing to Kafka topics `mdx-events` and `mdx-frames`.
- A `sensor_config.conf` mapping sensor names to pipeline IDs
  (`pipelineId, sensorName, rtspUrl`); see the
  [MDX client guide](../../adapters/vss/mdx-client/README.md).
- An active `atl_sdm_zone1.conf`, copied and reviewed from
  `/opt/nvidia/psf/bin/atl_sdm_zone1.conf.sample`. Pass its path with
  `--atl-zone1-conf`; the ATL SDM has no default event binding.
- A readable `sensor_pipelines_config.conf`. **`atl_proximity_sdm` refuses to
  start without it** — it is the reviewed sensor set behind the
  all-safety-relevant-sensors-failed latch, and it is configuration rather than
  a compiled table so that this SDM, `atl_sdm` and `proximity_sdm` cannot
  disagree about which sensors the latch covers. The package installs one at
  `/opt/nvidia/psf/bin/sensor_pipelines_config.conf`; override with
  `--sensor_pipelines_config <PATH>`.

  Keep it consistent with the cameras actually deployed. An id listed there that
  no camera ever reports can never fail, so the latch condition can never be met
  and the safe hold is silently disabled. The SDM logs the set it resolved at
  startup — read that line against the cameras you have.
- Two free UDP ports for the PLC command streams. Defaults are 12345 for
  `atl_sdm` and 12346 for `atl_proximity_sdm`; they must differ.

## Build and install

Every `/opt/nvidia/psf/...` path in this README is an **installed** path. It
exists only once the runtime Debian package is installed, or inside the Docker
image built from that package. A plain `cmake --build` leaves the binaries in
the build tree and creates nothing under `/opt`.

`safety-core/BUILD.md` covers the prerequisites, the CUDA root and the
configure step for each architecture. Configure as documented there first, then
build the packages from that same build directory.

### Debian packages

```bash
# x86_64
cmake --build safety-core/build-x86_64 --target safety_core_debian --parallel
sudo apt-get install ./safety-core/build-x86_64/packages/psf-desktop.deb

# aarch64/Tegra
cmake --build safety-core/build-aarch64 --target safety_core_debian --parallel
sudo apt-get install ./safety-core/build-aarch64/packages/psf-tegra.deb
```

Install with `apt-get install`, not `dpkg -i`: the runtime package depends on
`librdkafka1` and `libprotobuf32t64`, which apt will pull in.

Installing is what makes the paths in the next section resolve —
`bin/launch_psf.sh`, `apps/atl_proximity/atl_proximity_sdm` and its command
receiver, `apps/atl_proximity/event_mapping_atl_proximity.pb.txt`, and
`configs/sensor_config.conf`. The matching `-dev` package adds the public
headers and this component's example sources.

### Docker image

The Docker target builds the Debian packages itself and installs them into the
image, so it needs no separate deb step:

```bash
cmake --build safety-core/build-x86_64 --target safety_core_docker --parallel
docker load -i safety-core/build-x86_64/packages/psf-desktop.docker.tar
```

That produces `psf-desktop:latest`; the aarch64 build directory produces
`psf-tegra:latest` from `psf-tegra.docker.tar`. The image entrypoint is
`launch_psf.sh`, so whatever you pass to `docker run` after the image name goes
straight to the launcher, and `/opt/nvidia/psf` is already populated inside.

## How to run

The paths below assume the runtime package is installed, or that you are inside
the container image.

### Through the launcher

```bash
cp /opt/nvidia/psf/bin/atl_sdm_zone1.conf.sample ~/atl_sdm_zone1.conf   # review it
/opt/nvidia/psf/bin/launch_psf.sh \
  --saim-mode skip \
  --app atl_proximity \
  --atl-zone1-conf ~/atl_sdm_zone1.conf \
  --sensor-config /opt/nvidia/psf/configs/sensor_config.conf \
  --cmd_rx_port 12345 \
  --proximity_cmd_rx_port 12346
```

This starts the PSS daemon, the gateway, both SDMs and one `mdx_client`, in that
order. Options that apply to this app:

| Option | Effect |
|---|---|
| `--saim-mode active\|skip\|learn` | `active` also runs the Safety AI Monitor; `learn` only captures SAIM baselines and exits. `required_safety_monitor` in `nvpss.conf` must be `1` for `active` and `0` for `skip` |
| `--atl-zone1-conf PATH` | Required active ATL event-binding config |
| `--cmd_rx_port PORT` | PLC port for `atl_sdm` |
| `--proximity_cmd_rx_port PORT` | PLC port for `atl_proximity_sdm`; must differ from the above |
| `--pair_ttl_ms MS` | Pair liveness window for the proximity SDM |
| `--cmd_rx_ip IP` | PLC address, shared by both SDMs |
| `--broker ADDR` | Kafka broker (default `localhost:9092`) |

`--proximity_cmd_rx_port` and `--pair_ttl_ms` are accepted only with
`--app atl_proximity`. The generic `--sdm-config`, `--sdm-log-file`,
`--event-mapping` and `--sdm-external` options are for the config-driven ATL/PXC
selections, not this app.

The shipped `nvpss.conf` sets `required_safety_monitor = 1`, which matches
`--saim-mode active`. To run without the Safety AI Monitor, set
`required_safety_monitor = 0` and pass `--saim-mode skip`; that removes the
sensor-integrity monitor, so the sensor-health safe-hold path is not exercised.
See the "Required safety sources" section of the
[event integrator guide](../../components/event-integrator/README.md).

### Under Docker

The image entrypoint is `launch_psf.sh`, so everything after the image name is
a launcher option. Mount the reviewed configs read-only and pass their container
paths. Confirm every host config path exists as a readable regular file before
launching: `-v` creates a directory if the source is missing.

```bash
docker run -d --name nv-psf --network host \
  -v /run/nvpsf:/run/nvpsf \
  -v /var/log/psf:/var/log/psf \
  -v /path/to/nvpss.conf:/opt/nvidia/psf/bin/nvpss.conf:ro \
  -v /path/to/sensor_config.conf:/opt/nvidia/psf/bin/sensor_config.conf:ro \
  -v /path/to/sensor_pipelines_config.conf:/opt/nvidia/psf/bin/sensor_pipelines_config.conf:ro \
  -v /path/to/atl_sdm_zone1.conf:/opt/nvidia/psf/bin/atl_sdm_zone1.conf:ro \
  psf-desktop:latest \
  --saim-mode skip \
  --app atl_proximity \
  --atl-zone1-conf /opt/nvidia/psf/bin/atl_sdm_zone1.conf \
  --cmd_rx_ip 127.0.0.1 --cmd_rx_port 12349 --proximity_cmd_rx_port 12350 \
  --pair_ttl_ms 2000 \
  --sensor-config /opt/nvidia/psf/bin/sensor_config.conf
```

Use `psf-tegra:latest` on Tegra. With `--saim-mode skip` the mounted
`nvpss.conf` must set `required_safety_monitor = 0`. For `--saim-mode active`,
add `--runtime=nvidia` (the Safety AI Monitor uses the GPU) and mount the SAIM
baseline directory at `/opt/nvidia/psf/bin/saim_baseline`.

### Logs

`launch_psf.sh` starts the combined SDMs from `/var/log/psf`. Their dedicated
logs are `/var/log/psf/atl_sdm.log` and
`/var/log/psf/atl_proximity_sdm.log`; the syslog stream from the PSS daemon,
the gateway and `mdx_client` is written to `/var/log/psf/psf.log` and, in the
container, is also available through container stdout.

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

**The supplied thresholds use the same 2 m / 1 m bands as standalone PXC.**
Their decision policies remain distinct; equal thresholds do not make the
two implementations equivalent.

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
