# Halos `base` on IGX Thor (aarch64) — CCPLEX & FSI

Deploy the `base` profile (VSS Warehouse + Safety Core, no Isaac Sim / closed loop) on an
**IGX Thor** device. The safety decision (MUTE / UNMUTE) renders as the VST `halo_safety`
overlay, exactly like the x86 `base` profile — only the Safety Core runs on the Thor instead
of as an x86 container.

> **Scope.** The **CCPLEX** path mirrors the x86 `base` Safety Core flow — start here. The
> **FSI** path is more involved (a one-time firmware reflash); steps to confirm on your board
> are marked **⚠ validate on your hardware**.

---

## Why Thor differs from x86

On x86, `base` runs the Safety Core as a single container via `docker compose`
(`profiles/base.env`). On Thor the Safety Core is **hybrid** — an `nv-psf` container (event
integration + decision gateway) plus **host binaries** (the SDM, the AI monitor, and, for
FSI, the FSI bridge) orchestrated by `launch_hoisa.sh`. It is therefore launched by a helper
script that reads an env file, **not** by `docker compose`.

| Variant | Where the SDM runs | Mechanism |
|---|---|---|
| x86 / CCPLEX | x86 container | `docker compose --env-file profiles/base.env` (the standard `base`) |
| **Thor / CCPLEX** | Thor application cores | `launch_thor_safety.sh` → `launch_hoisa.sh --sdm-target ccplex` |
| **Thor / CCPLEX + FSI** | Functional Safety Island | `launch_thor_safety.sh` → `launch_hoisa.sh --sdm-target fsi` (+ FSI firmware + `nvFsiCom`) |

> The same Thor Safety Core launch path is reused by the (forthcoming) HIL profile — HIL adds
> the x86 stimulus side (Isaac Sim + comm-layer + ROS) and points the safety command at the
> comm-layer instead of the VST overlay.

---

## Choose the SDM target

On a `base` deploy the skill detects the platform: x86 runs the standard container base; on IGX Thor (aarch64) it **asks you** which SDM target to use before deploying — it does not assume one. The two options:

- **CCPLEX** — the SDM (`atl_sdm`) runs as a host process on the Thor application cores. No
  firmware change. **Start here.**
- **FSI** — the SDM runs on the Functional Safety Island; the host runs `fsicom-agent` as a
  bridge. Requires a one-time FSI firmware reflash and the `nvFsiCom` daemon. **Advanced.**

---

## 1. Prerequisites (Thor)

- IGX Thor flashed with a current IGX SW **GA release** (RT kernel `6.8.0-1019-nvidia-tegra-rt`).
  The GA release resolves the earlier IGX-Thor perception (VIC) issue, so **no extra DeepStream
  config edits are needed on VSS 3.2** (pre-GA non-RT stacks needed `compute-hw=1` plus a
  camera-count reduction; the GA release removes that need).
- NVIDIA driver, Container Toolkit, and Docker per `prerequisites.md`.
- The **nv-psf container image** — multi-arch (arm64 + amd64) under one tag; Docker on Thor
  (arm64) auto-selects the arm64 variant, so it is the **same tag** as the x86 `base` profile.
  Set via `PSF_IMAGE` in `base-thor.env`. (The architecture-specific parts are the host
  binaries from the `psf-tegra` package — see the next bullet.)
- Safety Core host binaries from the **`psf-tegra`** package installed under `/opt/nvidia/psf/`
  (`ngc_artifacts.md` §4): provides `launch_hoisa.sh`, the SDM apps (`atl_sdm`), `safety_monitor`,
  and the sensor config. This is all CCPLEX needs.
- **FSI only:** the **`psf-tegra-fsi`** package (`ngc_artifacts.md` §4), which provides both the
  HOISA FSI firmware (blob `fsi-ffw-t264.bin`, reflashed onto the FSI QSPI, §5B) **and** the
  `fsicom-agent` FSI bridge binary (installed on the Thor). Plus the `nvFsiCom` daemon at
  `/opt/nvidia/ccplex_sf/fsi_ccplex_com/nvFsiCom` (ships with the GA OS).

## 2. Deploy VSS Warehouse 3.2 on Thor (perception)

Deploy VSS Warehouse 3.2 (2D) on the Thor via the `vss-deploy-profile` skill. The GA RT
release resolves the perception VIC issue, so the old `compute-hw=1` / nvmap edits are **not**
needed on 3.2. Wait until perception serves all cameras (`Active sources : 3`) and the
`mdx-events` Kafka topic has data. See `vss_2d_overrides.md` for the base-vs-SIL override notes.

> **⚠ Two IGX-Thor VSS workarounds.** These apply to the VSS Warehouse deployment itself, but
> are noted here because they otherwise block the Safety Core from receiving any perception data:
> 1. **`nvstreamer-2d` needs `runtime: nvidia`.** In
>    `industry-profiles/warehouse-operations/warehouse-2d-app/warehouse-2d-app.yml`, the
>    `nvstreamer-2d` service ships `runtime: nvidia` **commented out** (perception / `rtvi-cv`
>    has it set). On IGX Thor the bare `deploy.resources.reservations.devices` GPU path does not
>    inject the GPU, so `nvstreamer-2d` fails to start — `vss-rtvi-cv` then runs but reports
>    `Active sources : 0`. **Uncomment `runtime: nvidia` on `nvstreamer-2d`.** This edit is
>    reverted whenever VSS state is wiped (`down -v`, or the datalog cleanup restores the stock
>    config), so re-apply it before each `up`.
> 2. **Docker Hub rate limit (HTTP 429) with `--pull always`.** Public base-image pulls (e.g.
>    `alpine`) can hit `toomanyrequests`. Drop `--pull always` after the first successful pull,
>    or use an authenticated / mirrored pull.

## 3. Configure `base-thor.env` and the sensor list

Edit `deployments/profiles/base-thor.env` (fill the `# change me` fields):

- `HOST_IP` — this Thor's IP.
- `SDM_TARGET` — `ccplex` (start here) or `fsi`.
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

### 5A. SDM on CCPLEX  (start here)

Set `SDM_TARGET=ccplex` in `base-thor.env`, then:

```bash
bash closed-loop-testing/scripts/launch_thor_safety.sh base-thor
```

Verify:

```bash
docker ps --filter name=nv-psf        # nv-psf container Up
ps -eo comm | grep -x atl_sdm         # CCPLEX SDM running
```

### 5B. SDM on FSI  (advanced — ⚠ validate on your hardware)

FSI needs host setup the launcher cannot do for you:

1. **Reflash the FSI** with the HOISA safety firmware. This is done on the **flashing host**
   (the machine with the Thor in USB recovery/RCM mode + the BSP `Linux_for_Tegra` tree),
   **not** on the running Thor — and it replaces the SEP default FSI firmware with HOISA's.
   - **a. Get the firmware blob.** Download + extract `psf-tegra-fsi` (`ngc_artifacts.md` §4):
     ```bash
     dpkg -x outside-in-safety_v*-psf-tegra-fsi/psf-tegra-fsi.deb /tmp/psf-fsi/
     ls /tmp/psf-fsi/opt/nvidia/psf/etc/fsi-fw/atl/fsi-ffw-t264.bin   # the atl HOISA FSI firmware
     ```
   - **b. Stage it into the BSP**, backing up the SEP default first so you can roll back:
     ```bash
     cd <Linux_for_Tegra>/bootloader        # or the FSI firmware dir for your board
     cp fsi-ffw-t264.bin fsi-ffw-t264.bin.SEP-DEFAULT.bak
     cp /tmp/psf-fsi/opt/nvidia/psf/etc/fsi-fw/atl/fsi-ffw-t264.bin ./fsi-ffw-t264.bin
     ```
   - **c. Put the board in recovery and flash QSPI slot A** (`internal`; drop `UNIFIED_FLASH`):
     ```bash
     sudo ./tools/kernel_flash/l4t_initrd_flash.sh --qspi-only -k A_fsi-fw <board-spec> internal
     ```
     `<board-spec>` is your board's flash configuration (for example,
     `p3834-0008-p4071-0008-nv-safety` for the IGX Thor Developer Kit Mini (T5000); the
     IGX Thor Developer Kit (T7000) uses its own config — use the one from your BSP). Slot A
     alone is sufficient for evaluation (~30 s); flash slot B as well for production failover.
     The flash tool `l4t_initrd_flash.sh` and the `Linux_for_Tegra` tree come from the IGX Driver
     Package (BSP) on the [NVIDIA IGX Download Center](https://developer.nvidia.com/igx-downloads).
   - **d. Boot + confirm the FSI handshake** came up: `sudo dmesg | grep -i fsi` shows
     `epl_client … handshake done with FSI`. **Rollback:** restore the `.SEP-DEFAULT.bak`
     blob + reflash slot A (~5 min). QSPI-only flashing does **not** touch rootfs / VSS.
2. **On the Thor, install `psf-tegra-fsi`** for the `fsicom-agent` bridge binary:
   ```bash
   ngc registry resource download-version "$PSF_TEGRA_FSI_RESOURCE"   # path from base-thor.env
   sudo dpkg -i */psf-tegra-fsi.deb
   ls /opt/nvidia/psf/bin/fsicom-agent
   ```
3. **Start `nvFsiCom`** on the CCPLEX *before* the launcher:
   ```bash
   sudo /opt/nvidia/ccplex_sf/fsi_ccplex_com/nvFsiCom &
   ```
4. Set `SDM_TARGET=fsi` in `base-thor.env`, then launch:
   ```bash
   bash closed-loop-testing/scripts/launch_thor_safety.sh base-thor
   ```

Verify (FSI markers):

```bash
ps -eo comm | grep -x fsicom-agent    # FSI bridge running
ps -eo comm | grep -qx atl_sdm && echo "unexpected: CCPLEX SDM should be ABSENT in FSI mode"
```

> **⚠ FSI relay flags.** The bridge must run with the response-relay flags so FSI decisions
> reach the overlay (HOISA User Guide §2.2.2):
>
> ```bash
> sudo fsicom-agent --relay-fsi-resp --ip <cmd-rx-ip> --port <cmd-rx-port>
> ```
>
> Pass only these flags. If decisions are produced upstream (`5C`) but the overlay never
> transitions, check the `fsicom-agent` flags first.

### 5C. Verify the Safety Core is producing decisions (both targets)

The process checks above confirm the components are *up*; these confirm the decision chain is
actually *flowing*. The Thor host install writes to `/var/log/psf/` (the launcher prints the
exact paths on start). Run while VSS perception is serving and the forklift/people are moving:

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
| FSI decisions don't reach the overlay | `nvFsiCom` not running, or `fsicom-agent` not started with exactly the §5B relay flags |
| Overlay blank but decisions are logged | Port mismatch — `halo_safety_udp_port` ≠ `PSF_CMD_RX_PORT` (both must be `12345`) |

For perception / STALE / SEI issues, see `troubleshooting.md`.
