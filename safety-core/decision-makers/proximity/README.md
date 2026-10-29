<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Proximity (PXC) SDM

`proximity_sdm` supports single PXC, two PXC instances, and ATL+PXC through
startup config files. Instance selection changes event subscriptions, logs and
UDP destinations; it does not change PXC decision logic.

## Supported selections

[launch_psf.sh](../../pkg/deb/launch_safety_core.sh), installed as
`/opt/nvidia/psf/bin/launch_psf.sh` and used as the container entrypoint,
accepts:

| Deployment | `--app` | `--sdm-config` files, in order |
| --- | --- | --- |
| PXC | `pxc` | `pxc_sdm_forklift.conf` |
| ATL + PXC | `atl,pxc` | `atl_sdm_zone1.conf,pxc_sdm_forklift.conf` |
| PXC + PXC | `pxc,pxc` | `pxc_sdm_forklift.conf,pxc_sdm_robots.conf` |

`pxc,atl` is also supported; reverse config, port and log lists accordingly.
At most two instances are supported. Supply one config per instance and two
different command-receiver ports for a pair; a single instance defaults to
12345. Config/log paths must not contain commas. For ATL and ATL+ATL, see the
[ATL guide](../atl/README.md).

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

## Startup configuration and mapping

The package/image ships `pxc_sdm_forklift.conf.sample` and
`pxc_sdm_robots.conf.sample` under `/opt/nvidia/psf/bin/`, alongside the sensor
and ATL config samples.

Like `sensor_config.conf`, the SDM files are operator-owned active configs:
copy a sample, review/edit it, and explicitly select its path. Installation does
not create or select an active SDM config. Docker paths are paths **inside the
container**; bind-mount the reviewed host files at exactly those paths. On Debian
with host SDMs, pass the active host paths directly. Configs are read at startup;
restart the affected deployment to apply a change.

| Config | Application events | Pairs |
| --- | --- | --- |
| [pxc_sdm_forklift.conf](pxc_sdm_forklift.conf) | `EVENT_12`..`EVENT_14` | Forklift–Person |
| [pxc_sdm_robots.conf](pxc_sdm_robots.conf) | `EVENT_15`..`EVENT_23` | Agility humanoid–Person, Transporter–Person, Fourier humanoid–Person |

All selections above use one
[proximity_event_mapping.pb.txt](../../adapters/vss/event-mappings/proximity/proximity_event_mapping.pb.txt).
It includes both ATL zones (`EVENT_0`..`EVENT_11`) and all four PXC pair groups
(`EVENT_12`..`EVENT_23`). Unsubscribed application events are not delivered to an
SDM. Override with `--event-mapping <file>` only when its `output_event` values
match the SDM configs. Use one mapping/client, not one Kafka client per SDM.

### PXC config

The single-PXC sample handles Forklift–Person only:

```text
pair_groups = forklift_person
forklift_person_normal = EVENT_12
forklift_person_reduce = EVENT_13
forklift_person_stop   = EVENT_14
```

Each selected group requires all three tier keys; omit keys for unselected
groups. The robot config selects `humanoid_person` (Agility, codes 15..17),
`transporter_person` (18..20) and `fourier_humanoid_person` (21..23). Every group
pairs a machine with Person, not Transporter with Humanoid.

To make a single PXC handle more groups, add their names to `pair_groups` and
copy their three bindings from the robot config. No new binary or third sample
is needed. Do not also run another PXC subscribed to those same codes.

VSS must produce the selected pairs: enable `Forklift`, `Transporter`,
`Agility_Digit_Humanoid` and/or `Fourier_GR1_T2_Humanoid` as the relevant proximity
center classes, and `Person` as the surrounding class. Proximity selection uses
object-pair classes and proximity data, not ATL ROI/tripwire identifiers.
No robot detections are expected in a video that contains no robots.

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
for name in atl_sdm_zone1 pxc_sdm_forklift pxc_sdm_robots; do
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
for name in nvpss sensor_config sensor_pipelines_config atl_sdm_zone1 pxc_sdm_forklift pxc_sdm_robots; do
  PSF_DOCKER_ARGS+=(-v "/etc/psf/${name}.conf:/opt/nvidia/psf/bin/${name}.conf:ro")
done
PSF_RUNTIME_ARGS=(
  --sensor-config /opt/nvidia/psf/bin/sensor_config.conf
  --saim-mode skip --broker localhost:9092 --cmd_rx_ip 127.0.0.1
)
```

`skip` is for these bench examples: it disables the sensor-integrity monitor
and does not exercise real sensor-health reporting, and it requires
`required_safety_monitor = 0` in the mounted `nvpss.conf`. For
`--saim-mode active`, add `--runtime=nvidia`, set `required_safety_monitor = 1`
and mount the SAIM baselines at `/opt/nvidia/psf/bin/saim_baseline`.

### PXC

```bash
docker run -d "${PSF_DOCKER_ARGS[@]}" "$PSF_IMAGE" "${PSF_RUNTIME_ARGS[@]}" \
  --app pxc --sdm-config /opt/nvidia/psf/bin/pxc_sdm_forklift.conf \
  --cmd_rx_port 12345 --sdm-log-file pxc_sdm.log
```

### ATL + PXC

```bash
docker run -d "${PSF_DOCKER_ARGS[@]}" "$PSF_IMAGE" "${PSF_RUNTIME_ARGS[@]}" \
  --app atl,pxc \
  --sdm-config /opt/nvidia/psf/bin/atl_sdm_zone1.conf,/opt/nvidia/psf/bin/pxc_sdm_forklift.conf \
  --cmd_rx_port 12345,12346 --sdm-log-file atl_sdm.log,pxc_sdm.log
```

### PXC + PXC

```bash
docker run -d "${PSF_DOCKER_ARGS[@]}" "$PSF_IMAGE" "${PSF_RUNTIME_ARGS[@]}" \
  --app pxc,pxc \
  --sdm-config /opt/nvidia/psf/bin/pxc_sdm_forklift.conf,/opt/nvidia/psf/bin/pxc_sdm_robots.conf \
  --cmd_rx_port 12345,12346 --sdm-log-file pxc_sdm_forklift.log,pxc_sdm_robots.log
```

These commands select separate native logs under `/var/log/psf/`. Relative
`--sdm-log-file` paths resolve there; absolute paths are supported if their
parent directories already exist and are writable. Without the flag, single
and mixed instances use `atl_sdm.log`/`pxc_sdm.log`; a PXC pair defaults to
`pxc_sdm_zone1.log,pxc_sdm_zone2.log`.
The shared control-plane log remains `psf.log`. Check each SDM's startup log for
its resolved event bindings and reviewed sensor set.

## Debian: running on the host

With the runtime package installed, `launch_psf.sh` runs the same selections
natively. It reads `nvpss.conf`, `sensor_pipelines_config.conf` and
`thresholds.cfg` from `/opt/nvidia/psf/bin/`, so review those installed copies
in place; SDM config, sensor config and log paths are host paths:

```bash
sudo /opt/nvidia/psf/bin/launch_psf.sh --saim-mode skip \
  --app atl,pxc \
  --sensor-config /etc/psf/sensor_config.conf \
  --sdm-config /etc/psf/atl_sdm_zone1.conf,/etc/psf/pxc_sdm_forklift.conf \
  --cmd_rx_ip 127.0.0.1 --cmd_rx_port 12345,12346 \
  --sdm-log-file atl_sdm.log,pxc_sdm.log --broker localhost:9092
```

For single PXC, use `--app pxc` with the forklift config, one port and one
log. For PXC+PXC, select `--app pxc,pxc` with the forklift and robot configs,
two ports and two logs.

To keep the control plane in the container while the SDMs run on the host, pass
`--sdm-external` to the container with the same `--app`. The container then
starts the PSS daemon, the gateway and `mdx_client` only, and you start each SDM
on the host with its native flags: `--config`, `--log_file`, `--cmd_rx_port` and
`--sensor_pipelines_config`. Launcher list options are not native SDM options,
and with `--sdm-external` the host is responsible for the cross-instance checks
the launcher would otherwise make.

## Command receivers

Launch one matching receiver per SDM; the launchers do not start receivers.
For PXC+PXC, use two terminals on the receiver host:

```bash
export LD_LIBRARY_PATH="/opt/nvidia/psf/lib:${LD_LIBRARY_PATH:-}"
/opt/nvidia/psf/apps/proximity/proximity_sdm_cmd_receiver -p 12345 --log-file forklift_receiver.log
```

```bash
export LD_LIBRARY_PATH="/opt/nvidia/psf/lib:${LD_LIBRARY_PATH:-}"
/opt/nvidia/psf/apps/proximity/proximity_sdm_cmd_receiver -p 12346 --log-file robots_receiver.log
```

For ATL+PXC, use `atl_sdm_cmd_receiver` on 12345 and the PXC receiver on 12346.
Keep ports/logs distinct and interactive receivers attached to their terminals.
Do not send a manual safety release merely to clear a test or as routine cleanup.

## Difference from legacy `atl_proximity`

`--app atl,pxc` starts `atl_sdm` plus `proximity_sdm`. The separate
`--app atl_proximity` starts `atl_sdm` plus `atl_proximity_sdm`, whose retained
per-pair worst-tier policy is unchanged. Selecting a config does not make the
two proximity implementations equivalent. The legacy mapping uses proximity
codes 8..10; do not mix it with the new PXC bindings (12..23) or ATL zone-two
subscriptions (6..11). Its commands and constraints remain in
[atl_proximity/README.md](../atl_proximity/README.md).

## Safety constraints and source layout

Multi-SDM launches have not been validated in 3D. All producers on one PSS daemon must agree on event meanings; do
not run a simulator using these codes for different situations alongside the
live mapping. VSS proximity reporting must cover the warning/critical distance
bands configured in the mapping. The supplied rules do not exempt a person
riding a machine. Review camera counting-event constraints in the
[ATL guide](../atl/README.md) when running ATL+PXC.

This change does not alter the fault latch or validated manual-release policy. For mapping rules,
identity resolution and read-only Kafka inspection, see
[mdx-client/README.md](../../adapters/vss/mdx-client/README.md).

| Path | Role |
| --- | --- |
| `sdm/ccplex/Proximity.cpp` | CLI and startup bindings |
| `sdm/ccplex/ProximityControl.cpp` | Decision logic, latches, gateway and PLC link |
| `sdm/ccplex/proximity_sdm_config.{h,cpp}` | Pair-group/event-config parser |
| `udp_cmd_receiver/` | PXC command receiver, also reused by legacy ATL-proximity |
| `include/proximity_cmd_pkt.h` | Shared PLC wire contract |

The development package includes this guide at
`/opt/nvidia/psf/examples/apps/metropolis/proximity/README.md`.
