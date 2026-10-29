<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# ATL — Autonomous Trailer Loading SDM

This guide covers single ATL and two ATL instances with separate zone configs,
logs and UDP command-receiver ports. For ATL+PXC, see the
[proximity guide](../proximity/README.md#atl--pxc); the separate legacy app keeps
its own [atl_proximity guide](../atl_proximity/README.md).
The config-driven multi-SDM changes do not alter ATL decision logic.

## Supported selections

[launch_psf.sh](../../pkg/deb/launch_safety_core.sh), installed as
`/opt/nvidia/psf/bin/launch_psf.sh` and used as the container entrypoint,
supports:

| Deployment | `--app` | `--sdm-config` files, in order |
| --- | --- | --- |
| ATL | `atl` | `atl_sdm_zone1.conf` |
| ATL + ATL | `atl,atl` or `atl_dual` | `atl_sdm_zone1.conf,atl_sdm_zone2.conf` |

Supply one config per instance. Two instances require two different receiver
ports; a single instance defaults to 12345. Config, port and log lists follow
`--app` order, and config/log paths must not contain commas.

Each deployment has one `mdx_client`, one PSS daemon and one PSD gateway.
`mdx_client` maps VSS/Kafka messages to `EVENT_n` using one mapping file. Each SDM
registers its configured event codes with the gateway, which forwards matching
application events to that instance. Health/fault/status traffic remains shared.
The camera safety set does not select which zone or object pair an SDM receives.

The launcher's startup validation lives in
[sdm_launch_common.sh](../../pkg/deb/sdm_launch_common.sh): it rejects
overlapping application event codes, duplicate ports/logs, wrong list lengths,
unreadable configs and logs that would overwrite configuration files. The SDM
binary then validates its complete config schema. A manually launched SDM still
needs the integrator to enforce cross-instance separation.

## Startup configuration

The package/image ships `sensor_config.conf.sample`,
`atl_sdm_zone1.conf.sample` and `atl_sdm_zone2.conf.sample` under
`/opt/nvidia/psf/bin/`.

Like `sensor_config.conf`, the SDM files are operator-owned active configs:
copy a sample, review/edit it, and explicitly select its path. Installation does
not create or select an active SDM config. Docker paths are paths **inside the
container**; bind-mount the reviewed host files at exactly those paths. On Debian
with host SDMs, pass the active host paths directly. Configs are read at startup;
restart the affected deployment to apply a change.

| Config | Application events | VSS identifiers |
| --- | --- | --- |
| [atl_sdm_zone1.conf](atl_sdm_zone1.conf) | `EVENT_0`..`EVENT_5` | `tripwire-id-1`, `roi-id-1` |
| [atl_sdm_zone2.conf](atl_sdm_zone2.conf) | `EVENT_6`..`EVENT_11` | `tripwire-id-2`, `roi-id-2` |

Single and dual ATL use one
[event_mapping_atl.pb.txt](../../adapters/vss/event-mappings/atl/event_mapping_atl.pb.txt).
To use a site-specific mapping, pass `--event-mapping <file>` and keep its
`output_event` values aligned with the SDM configs. ATL+PXC uses the combined
mapping described in the [proximity guide](../proximity/README.md).

### ATL config

All six keys are mandatory and must use distinct codes:

```text
forklift_entered = EVENT_0
forklift_exited  = EVENT_1
person_entered   = EVENT_2
person_exited    = EVENT_3
roi_violation   = EVENT_4
roi_cleared     = EVENT_5
```

With the supplied mapping, tripwire OUT means entered the trailer; IN means
exited. Zone two has the same six keys with codes 6..11. Its VSS tripwire/ROI
must actually exist: starting a second SDM does not create VSS rules. With no
zone-two rules, that SDM receives no zone-two physical events. Event silence
does not reset ATL's retained occupancy or mute state.

The parser accepts blank lines and `#` comments. Unknown/duplicate keys,
missing required bindings, duplicate codes and file read errors are fatal.
Only `EVENT_0`..`EVENT_23` may carry application situations; `SW_FAIL` and
health/status event types are reserved.

### Camera catalogue versus reviewed safety set

`sensor_config.conf` is the RTSP camera catalogue. The separate
`sensor_pipelines_config.conf` is the reviewed safety set used by the SDMs; it
ships as an active file, not a `.sample`. It is **not** part of either ATL or
PXC event-binding config. An old ATL `safety_pipelines` key is rejected.

Review this shared set against the actual deployment: the all-sensors-failed
condition depends on every listed identity failing. An identity that is never
reported can prevent that condition from being reached. In 2D the reviewed set
contains cameras; in 3D it contains the BEV group, whose member-camera topology
is in `sensor_group_config.conf`. `deploymentMode` in `nvpss.conf` selects the
mode. For 3D, also supply the matching group config at the daemon's configured
path. The examples below assume an already reviewed **2D** configuration.

## Docker: all components in one container

Use an image built from the current source, including the launchers, helper,
binaries, mappings and samples. Older images do not gain these options merely
by mounting new configs. Do not mix wire-schema versions across components.

First prepare `/etc/psf/` from the installed package (or extract the same files
from the image with `docker cp` if there is no host package):

```bash
sudo install -d /etc/psf /var/log/psf
sudo cp -n /opt/nvidia/psf/bin/sensor_config.conf /etc/psf/sensor_config.conf
for name in atl_sdm_zone1 atl_sdm_zone2; do
  sudo cp -n "/opt/nvidia/psf/bin/${name}.conf.sample" "/etc/psf/${name}.conf"
done
sudo cp -n /opt/nvidia/psf/bin/nvpss.conf /etc/psf/nvpss.conf
sudo cp -n /opt/nvidia/psf/bin/sensor_pipelines_config.conf /etc/psf/sensor_pipelines_config.conf
```

Review these copies before starting. In `nvpss.conf`, `sensorConfig` must name
the container path `/opt/nvidia/psf/bin/sensor_config.conf`; camera names, IDs,
URLs and `deploymentMode` must match the site. The process user needs write
access to `/var/log/psf`, and `/run/nvpsf` must be a real writable directory with
mode 1777. Before launching, confirm every host config path exists as a readable
regular file: `-v` creates a directory if the source path is missing.

Set up these Bash arguments once, then run **one** of the alternatives below.
Replace the image, Kafka broker and PLC address for the deployment. Do not run
these alongside an existing stack on the same sockets/ports.

```bash
PSF_IMAGE='psf-desktop:latest'   # psf-tegra:latest on aarch64
PSF_DOCKER_ARGS=(
  --name nv-psf --network host --ipc host
  -v /run/nvpsf:/run/nvpsf
  -v /var/log/psf:/var/log/psf
)
for name in nvpss sensor_config sensor_pipelines_config atl_sdm_zone1 atl_sdm_zone2; do
  PSF_DOCKER_ARGS+=(-v "/etc/psf/${name}.conf:/opt/nvidia/psf/bin/${name}.conf:ro")
done
PSF_RUNTIME_ARGS=(
  --sensor-config /opt/nvidia/psf/bin/sensor_config.conf
  --saim-mode skip --pcm-mode skip --broker localhost:9092 --cmd_rx_ip 127.0.0.1
)
```

`skip` is for these bench examples: it disables both AI monitors and does not
exercise real sensor-health or AI-pipeline reporting, and it requires
`required_safety_monitor = 0` and `required_perception_monitor = 0` in the
mounted `nvpss.conf`. For `--saim-mode active`, add `--runtime=nvidia`, set
`required_safety_monitor = 1` and mount the SAIM baselines at
`/opt/nvidia/psf/bin/saim_baseline`. For `--pcm-mode active`, set
`required_perception_monitor = 1`, mount the PCM baselines at
`/opt/nvidia/psf/bin/pcm_baseline` and, for its container health check, the
Docker socket at `/var/run/docker.sock`; see the
[perception monitor guide](../../components/perception-monitor/README.md).

### ATL

```bash
docker run -d "${PSF_DOCKER_ARGS[@]}" "$PSF_IMAGE" "${PSF_RUNTIME_ARGS[@]}" \
  --app atl --sdm-config /opt/nvidia/psf/bin/atl_sdm_zone1.conf \
  --cmd_rx_port 12345 --sdm-log-file atl_sdm.log
```

### ATL + ATL

```bash
docker run -d "${PSF_DOCKER_ARGS[@]}" "$PSF_IMAGE" "${PSF_RUNTIME_ARGS[@]}" \
  --app atl,atl \
  --sdm-config /opt/nvidia/psf/bin/atl_sdm_zone1.conf,/opt/nvidia/psf/bin/atl_sdm_zone2.conf \
  --cmd_rx_port 12345,12346 --sdm-log-file atl_sdm_zone1.log,atl_sdm_zone2.log
```

`--app atl_dual` is an alias. The older form also remains supported, but must
explicitly provide both configs:

```bash
docker run -d "${PSF_DOCKER_ARGS[@]}" "$PSF_IMAGE" "${PSF_RUNTIME_ARGS[@]}" \
  --app atl_dual \
  --atl-zone1-conf /opt/nvidia/psf/bin/atl_sdm_zone1.conf \
  --atl-zone2-conf /opt/nvidia/psf/bin/atl_sdm_zone2.conf \
  --cmd_rx_port 12345 --zone2_cmd_rx_port 12346
```

These commands write separate native logs under `/var/log/psf/`. Relative
`--sdm-log-file` paths resolve there; absolute paths are supported if their
parent directories exist and are writable. Defaults are `atl_sdm.log` for one
instance and `atl_sdm_zone1.log,atl_sdm_zone2.log` for two. The shared
control-plane log remains `psf.log`. Check each instance's startup log for its
resolved event bindings and reviewed sensor set.

## Debian: running on the host

With the runtime package installed, `launch_psf.sh` runs the same selections
natively. It reads `nvpss.conf`, `sensor_pipelines_config.conf` and
`thresholds.cfg` from `/opt/nvidia/psf/bin/`, so review those installed copies
in place; SDM config, sensor config and log paths are host paths:

```bash
sudo /opt/nvidia/psf/bin/launch_psf.sh --saim-mode skip --pcm-mode skip \
  --app atl,atl \
  --sensor-config /etc/psf/sensor_config.conf \
  --sdm-config /etc/psf/atl_sdm_zone1.conf,/etc/psf/atl_sdm_zone2.conf \
  --cmd_rx_ip 127.0.0.1 --cmd_rx_port 12345,12346 \
  --sdm-log-file atl_sdm_zone1.log,atl_sdm_zone2.log --broker localhost:9092
```

For single ATL, select `--app atl` and provide only the zone-one config,
one receiver port and one log path. The legacy dual-ATL form uses
`--app atl_dual`, `--atl-zone1-conf`, `--atl-zone2-conf`, `--cmd_rx_port 12345`
and `--zone2_cmd_rx_port 12346`. Do not mix legacy flags with config/port lists.

To keep the control plane in the container while the SDMs run on the host, pass
`--sdm-external` to the container with the same `--app`. The container then
starts the PSS daemon, the gateway and `mdx_client` only, and you start each SDM
on the host with its native flags: `--config`, `--log_file`, `--cmd_rx_port` and
`--sensor_pipelines_config`. Launcher list options are not native SDM options,
and with `--sdm-external` the host is responsible for the cross-instance checks
the launcher would otherwise make.

## Command receivers

Launch one ATL receiver per SDM, on its configured destination port. The
launchers do not start receivers. For dual ATL, use two terminals:

```bash
export LD_LIBRARY_PATH="/opt/nvidia/psf/lib:${LD_LIBRARY_PATH:-}"
/opt/nvidia/psf/apps/atl/atl_sdm_cmd_receiver -p 12345 --log-file atl_zone1_receiver.log
```

```bash
export LD_LIBRARY_PATH="/opt/nvidia/psf/lib:${LD_LIBRARY_PATH:-}"
/opt/nvidia/psf/apps/atl/atl_sdm_cmd_receiver -p 12346 --log-file atl_zone2_receiver.log
```

Keep receiver ports/logs distinct and interactive receivers attached to their
terminals. Do not send a manual safety release merely to clear a test or as
routine cleanup.

## Safety constraints and source layout

ATL counting-event mappings need one reviewed source camera per event type;
duplicating entry/exit observations across cameras can drift occupancy. All
producers sharing a PSS daemon must agree on event meanings: do not run an
unrelated simulator using the same codes alongside the live mapping. Multi-SDM
launches have not been validated in 3D.

This change does not alter the fault latch or validated manual-release policy. For mapping rules,
identity resolution and read-only Kafka inspection, see
[mdx-client/README.md](../../adapters/vss/mdx-client/README.md).

| Path | Role |
| --- | --- |
| `sdm/ATL.cpp` | CLI and startup bindings |
| `sdm/ATLControl.cpp` | Decision logic, latches, gateway and PLC link |
| `sdm/atl_sdm_config.{h,cpp}` | Mandatory event-config parser |
| `udp_cmd_receiver/` | ATL command receiver |
| `include/` | PLC wire contract and fault-report headers |

The development package includes this guide at
`/opt/nvidia/psf/examples/apps/metropolis/atl/README.md`.
