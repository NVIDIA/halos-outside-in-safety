# Halos `base` on IGX Thor (aarch64)

Deploy the `base` profile (VSS Warehouse + Safety Core, no Isaac Sim / closed loop) on an
**IGX Thor** device. The safety decision (MUTE / UNMUTE) renders as the VST `halo_safety`
overlay, exactly like the x86 `base` profile — only the Safety Core runs on the Thor instead
of as an x86 container.

---

## Why Thor differs from x86

On x86, `base` runs the Safety Core as a single container via `docker compose`
(`profiles/base.env`). On Thor the Safety Core is **hybrid** — an `nv-psf` container (event
integration + decision gateway) plus **host binaries** (the SDM and the AI monitor)
orchestrated by `launch_hoisa.sh`. It is therefore launched by a helper script that reads an
env file, **not** by `docker compose`.

| Platform | Where the SDM runs | Mechanism |
|---|---|---|
| x86 | x86 container | `docker compose --env-file profiles/base.env` (the standard `base`) |
| **IGX Thor** | Thor application cores | `launch_thor_safety.sh` → `launch_hoisa.sh --sdm-target ccplex` |

> The same Thor Safety Core launch path is reused by the (forthcoming) HIL profile — HIL adds
> the x86 stimulus side (Isaac Sim + comm-layer + ROS) and points the safety command at the
> comm-layer instead of the VST overlay.

---

## 1. Prerequisites (Thor)

- IGX Thor flashed with a current IGX SW **GA release** (RT kernel `6.8.0-1019-nvidia-tegra-rt`).
  **No manual DeepStream edits are needed on VSS 3.2.x**: selecting `HARDWARE_PROFILE=IGX-THOR`
  (the `vss-deploy-profile` skill sets it) makes the blueprint-configurator apply the
  Thor-specific tuning automatically — including using the VIC for tracker scaling
  (`compute-hw=2`, the path the GA RT release fixed). (Pre-GA non-RT stacks needed a hand-set
  `compute-hw` plus a camera-count reduction.)
- NVIDIA driver, Container Toolkit, and Docker per `prerequisites.md`.
- The **nv-psf container image** — multi-arch (arm64 + amd64) under one tag; Docker on Thor
  (arm64) auto-selects the arm64 variant, so it is the **same tag** as the x86 `base` profile.
  Set via `PSF_IMAGE` in `base-thor.env`. (The architecture-specific parts are the host
  binaries from the `psf-tegra` package — see the next bullet.)
- Safety Core host binaries from the **`psf-tegra`** package installed under `/opt/nvidia/psf/`
  (`ngc_artifacts.md` §4): provides `launch_hoisa.sh`, the SDM apps (`atl_sdm`), `safety_monitor`,
  and the sensor config.

## 2. Deploy VSS Warehouse 3.2.1 on Thor (perception)

Deploy VSS Warehouse 3.2.1 (2D) on the Thor via the `vss-deploy-profile` skill (set
`HARDWARE_PROFILE=IGX-THOR`; the profile applies the Thor DeepStream tuning for you — §1). Wait
until perception serves all cameras (`Active sources : 3`) and the `mdx-events` Kafka topic has
data. See `vss_2d_overrides.md` for the base-vs-SIL override notes.

> **⚠ Two IGX-Thor VSS workarounds.** These apply to the VSS Warehouse deployment itself, but
> are noted here because they otherwise block the Safety Core from receiving any perception data:
> 1. **`nvstreamer-2d` needs `runtime: nvidia`.** In
>    `industry-profiles/warehouse-operations/warehouse-2d-app/warehouse-2d-app.yml`, the
>    `nvstreamer-2d` service ships `runtime: nvidia` **commented out** (perception / `rtvi-cv`
>    has it set). On IGX Thor the bare `deploy.resources.reservations.devices` GPU path does not
>    inject the GPU, so `nvstreamer-2d` fails to start — `vss-rtvi-cv` then runs but reports
>    `Active sources : 0`. **Uncomment `runtime: nvidia` on `nvstreamer-2d`**, or — sturdier —
>    add a `deploy/docker/docker-compose.override.yml` declaring `runtime: nvidia` for the
>    service and pass it at `up` (`-f compose.yml -f docker-compose.override.yml`, per the
>    HOISA Quick Start Guide, Annex A): the override file survives the VSS state wipes that
>    revert an in-place edit (`down -v`, or the datalog cleanup restoring the stock config),
>    which otherwise must be re-applied before each `up`.
> 2. **Docker Hub rate limit (HTTP 429) with `--pull always`.** Public base-image pulls (e.g.
>    `alpine`) can hit `toomanyrequests`. Drop `--pull always` after the first successful pull,
>    or use an authenticated / mirrored pull.

## 3. Configure `base-thor.env` and the sensor list

Edit `deployments/profiles/base-thor.env` (fill the `# change me` fields):

- `HOST_IP` — this Thor's IP.
- `PSF_IMAGE` — the nv-psf container (multi-arch; same tag as x86 `base`, Docker selects arm64 on Thor).
- `PSF_CMD_RX_PORT` — `12345`, the VST `halo_safety` overlay port (see §6).
- `PSF_LAUNCH_MODE` — `active` (full stack) or `skip` (omit the AI monitor; use only if the
  perception source codec is incompatible with the monitor).

Then point the Safety Core at this host's VST streams. The AI monitor reads the camera RTSP
directly; on Thor those are the VST live URLs. Copy the template and fill in the URLs:

```bash
cp closed-loop-testing/safety-core/configs/sensor_config_thor.conf \
   /opt/nvidia/psf/bin/sensor_config_thor.conf
# replace <thor_ip> and each <UUID> with values from:
#   curl http://<thor_ip>:30888/vst/api/v1/sensor/list
```

> **⚠ VST UUIDs change** whenever VST state is wiped/redeployed — refresh
> `sensor_config_thor.conf` after each VST reset.

## 4. (Optional) AI monitor baseline — `learn`

If running the AI monitor (`PSF_LAUNCH_MODE=active`), generate per-camera baselines first
with `launch_hoisa.sh --mode learn …` (it reads the same sensor config as §3). Skip when using
`PSF_LAUNCH_MODE=skip`.

## 5. Launch the Safety Core on Thor

`launch_thor_safety.sh` reads `base-thor.env` and invokes `launch_hoisa.sh`.

```bash
bash closed-loop-testing/scripts/launch_thor_safety.sh base-thor
```

Verify:

```bash
docker ps --filter name=nv-psf        # nv-psf container Up
ps -eo comm | grep -x atl_sdm         # SDM running
```

The checks above confirm the components are *up*. To confirm the decision chain is actually
*flowing*, check the logs — the Thor host install writes to `/var/log/psf/` (the launcher
prints the exact paths on start). Run while VSS perception is serving and the forklift/people
are moving:

```bash
# PSF ingested perception events and invoked the decision gateway:
sudo grep -E "Safety event reported|DecisionRequest|FUSED|PASSTHROUGH" /var/log/psf/psf.log | tail
# The SDM emitted MUTE/UNMUTE to the overlay port and got an ACK back:
sudo grep -E "Sending decision command|Received acknowledgment" /var/log/psf/atl_sdm.log | tail
```

A healthy chain shows `DecisionRequest` lines in `psf.log` and matching pairs in
`atl_sdm.log`: `Sending decision command: UNMUTE/MUTE … → 127.0.0.1:12345` immediately
followed by `Received acknowledgment … (SeqNo: N)` for the same `SeqNo`.

- No `DecisionRequest` in `psf.log` → perception isn't feeding events: re-check `Active sources`
  and that the `mdx-events` Kafka topic has data (§2).
- Commands sent in `atl_sdm.log` but **no acknowledgment** → the overlay isn't listening on the
  port: check the `halo_safety_udp_port` wiring (§6).

## 6. Enable the safety overlay (VST) and verify

Same as the x86 `base`: in the VSS 2D `vst_config.json`, set `halo_safety_udp_port` to
`12345` (ships `-1` / disabled) — it must equal `PSF_CMD_RX_PORT`. Restart VST. The overlay
then shows "Standard Mode" (MUTE) / "Efficient Mode" (UNMUTE) + the forklift proximity
bubble.

> **⚠ validate on your hardware.** Confirm the Thor launch emits the safety command to
> `127.0.0.1:12345` in the form the VST overlay consumes. If the overlay stays blank while
> decisions are being logged, check this wiring first.

## 7. Stop

```bash
bash closed-loop-testing/scripts/stop_thor_safety.sh
```

## Troubleshooting (Thor-specific)

| Symptom | Fix |
|---|---|
| `nvstreamer-2d` stuck `Created` / `Runtime=runc`, `Active sources : 0` after a VSS `down -v` or datalog cleanup | The `runtime: nvidia` fix on `nvstreamer-2d` (§2) was reverted to stock when VSS state was wiped. Re-uncomment it in `warehouse-2d-app.yml`, then `docker compose --env-file … up -d --force-recreate --no-deps nvstreamer-2d`. Re-apply after every `down -v` / cleanup, before `up`. |
| `nv-psf` / perception can't get the GPU after a reboot | The CDI spec lives on tmpfs — regenerate: `sudo nvidia-ctk cdi generate --output=/var/run/cdi/nvidia.yaml && sudo systemctl restart docker` |
| AI monitor reads no frames | `sensor_config_thor.conf` URLs/UUIDs are stale — refresh from the VST sensor list |
| Overlay blank but decisions are logged | Port mismatch — `halo_safety_udp_port` ≠ `PSF_CMD_RX_PORT` (both must be `12345`) |

For perception / STALE / SEI issues, see `troubleshooting.md`.
