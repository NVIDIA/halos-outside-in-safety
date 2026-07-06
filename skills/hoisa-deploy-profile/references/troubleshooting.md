# Troubleshooting

Common errors and fixes for Halos SIL deployment.

---

## PSF Cannot Connect to Kafka

**Symptom**: PSF container logs show Kafka connection errors.

**Cause**: VSS Warehouse (Kafka) not running or started after Halos SIL.

**Fix**:
```bash
# Verify Kafka is running
docker ps | grep kafka

# If not running, redeploy VSS Warehouse first
# Then restart Halos SIL
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env down && docker compose --env-file profiles/<profile>.env up -d
```

**Rule**: Always deploy VSS Warehouse first, wait for health check, then Halos.

---

## STALE Events in PSF

**Symptom**: PSF log floods with `Dropping STALE event`; safety decisions lag or don't trigger.

**First — some STALE is normal.** Events reaching PSF older than `timeWindowSize`
(`6000` ms / 6 s in the SIL `nvpss.conf`) are dropped. Measure the **steady-state** rate
**after the loop has run a while** — startup / bootstrap STALE before Isaac streams
stabilise is expected. A persistent high rate (tens of % while running) points to a real
problem — most often the DeepStream timestamp source (below), not raw latency.

**Cause** (most common first):
1. **Wrong DeepStream timestamp source** — if `extract-sei-sim-time=1` (Isaac's SEI
   sim-time fed in as the NTP timestamp), the sim clock drifts outside the 6 s window and
   **nearly everything is dropped as STALE** (A/B tested on Isaac 6.0: ~100 STALE + MUTE
   stops toggling). Fix by using **system time**: `attach-sys-ts-as-ntp=1`, with
   `extract-sei-sim-time` / `drop-backward-sei` commented out (see `vss_2d_overrides.md`).
2. perception→Kafka→PSF latency occasionally exceeds `timeWindowSize` (slow or shared GPU,
   frame-timing jitter, multi-camera fusion).

**Fix**:
```bash
# 1. Confirm DeepStream uses system timestamps: attach-sys-ts-as-ntp=1, SEI sim-time off (see vss_2d_overrides.md)
# 2. If STALE is still high once running, widen the window:
nano <repo>/closed-loop-testing/safety-core/configs/nvpss.conf
# Increase timeWindowSize (e.g. 6000 -> 8000)
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env restart safety-core
```

---

## Low / Zero FPS on `vss-rtvi-cv`

**Symptom**: `vss-rtvi-cv` shows low FPS, or a camera stuck at `0.00000`.

**On Isaac 6.0, SEI extraction does *not* cause low FPS** (A/B tested — FPS held ~14/cam
with SEI on or off). The old "disable SEI to fix FPS" advice is obsolete; an SEI mis-config
shows up as **STALE events** (see above), not low FPS.

**Cause / fix by symptom**:
- **~14 FPS/cam (not ~30)** — expected in SIL: the perception GPU is shared with Isaac
  Sim. Gate on "non-zero & stable", not an exact number. NVIDIA's ~30 assumes a dedicated
  perception GPU. Not a bug.
- **A source stuck at `0.00000`** — that stream isn't arriving: Isaac not streaming yet,
  wrong RTSP URL, or the TensorRT engine still building (first run 15-20 min). Confirm the
  3 Isaac RTSP streams are up (see `test_scenario.md` → handoff signals), then
  `docker restart vss-rtvi-cv` if needed.

(Flickering bounding boxes are a separate issue — see "Bounding Box Flickering" below.)

---

## Isaac Sim Fails to Start

**Symptom**: isaac-sim container exits immediately or hangs.

**Cause**: Insufficient GPU VRAM or wrong `ISAAC_GPU_DEVICE`.

**Fix**:
```bash
# Check GPU memory
nvidia-smi

# Verify ISAAC_GPU_DEVICE in .env points to a GPU with RT cores and 24GB+ free
nano deployments/profiles/<profile>.env
# ISAAC_GPU_DEVICE: a GPU with RT cores + >20 GB NOT running VSS perception (usually not GPU 0)
```

---

## Isaac Sim Vulkan Crash (ERROR_DEVICE_LOST)

**Symptom**: Isaac Sim loads scene and compiles shaders, then crashes with:

```
VkResult: ERROR_DEVICE_LOST
vkWaitForFences failed for command queue
GPU crash dump is successfully written
```

**Cause**: NVIDIA driver too old for the GPU. Requires driver >= 580.95.05.

**Fix**:
1. Update the NVIDIA driver to >= 580.95.05 (recommended).
2. **Workaround**: Restart isaac-sim and retry — shaders are cached after
   first compilation, and the crash often does not recur on the second run:
   ```bash
   docker restart isaac-sim
   # Re-run the test scenario
   docker exec -d isaac-sim bash -lc 'cd /isaac-sim/sil/scripts && \
     ./run_sdg.sh -c /isaac-sim/sil/configs/default_config_ros.yaml \
     --start --headless --enable-vst \
     --cameras-config /isaac-sim/sil/configs/cameras.yaml'
   ```

---

## Cameras Not Showing in VST

**Symptom**: VST UI at `http://<HOST_IP>:30888/vst/` shows no cameras.

**Cause**: Isaac Sim not started with `--enable-vst` flag, or VST not running.

**Fix**:
```bash
# Check VST is running
docker ps | grep vst

# Rerun Isaac Sim with --enable-vst
docker exec -d isaac-sim bash -lc 'cd /isaac-sim/sil/scripts && \
  ./run_sdg.sh -c /isaac-sim/sil/configs/default_config_ros.yaml \
  --start --headless --enable-vst \
  --cameras-config /isaac-sim/sil/configs/cameras.yaml'
```

---

## NGC Download Fails (403 Forbidden)

**Symptom**: `ngc registry resource download-version` returns 403.

**Fix**:
```bash
# Re-authenticate
docker login nvcr.io -u '$oauthtoken' -p "$NGC_CLI_API_KEY"
ngc config set
```

---

## Docker Compose Version Mismatch

**Symptom**: `unknown shorthand flag` or compose syntax errors.

**Cause**: Docker Compose < v2.39.

**Fix**:
```bash
# Check version
docker compose version

# Update Docker to get latest compose plugin
sudo apt-get update && sudo apt-get install -y docker-compose-plugin
```

---

## Bounding Box Flickering

**Symptom**: VST shows boxes appearing/disappearing rapidly.

**Cause**: `bbox_tolerance_ms=0` in VST config — metadata-to-frame matching
window too tight for perception latency.

**Fix**: Set `bbox_tolerance_ms=100` in `vst_config.json`.

---

## PSF Indicator Not Transitioning Correctly — Check Perception Stability First

**Symptom**: PSF indicator doesn't switch between states (green / yellow /
red) as expected, or transitions feel unreliable. Tripwire / ROI events
appear inconsistent.

**Before blaming PSF or comm-layer, check upstream perception**: brief
detection drops on key objects (e.g. a forklift near a trailer or doorway)
break tracker association, so one physical object spawns multiple track IDs
and inflates the behavior count. Downstream PSF then reacts to noisy input.

**Diagnose**:
```bash
# 1. Watch a specific class in mdx-events for drop-outs (per-second histogram)
docker exec kafka kafka-console-consumer --bootstrap-server localhost:9092 \
  --topic mdx-events --timeout-ms 20000 --property print.timestamp=true \
  > /tmp/events.log
# Gaps of several seconds on a class that should be continuously visible
# point to a perception issue at that scene region.

# 2. Count batches with more behaviors than the scene really contains
docker logs --since 10m vss-behavior-analytics | \
  grep -cE "Created a total of [4-9] behavior"
# A small scene (1 forklift + 1-2 persons) should give ~0 such batches.
# A non-trivial count means tracks are fragmenting.
```

**If confirmed as a perception issue**, typical mitigations:
- Try a different perception model / weights (confirm with the VSS team
  which build suits your scene) and re-validate.
- Adjust camera layout / calibration so the problem region is covered by a
  secondary view — multi-view fusion tolerates single-view drops better.
- Extend BA `behaviorStateTimeout` to bridge brief gaps (trade-off: delays
  genuine behavior end).

If detection is clean and behavior count matches reality, the issue is
downstream (PSF fusion config, ATL logic, hysteresis) — continue from there.

---

## CUDA Errors After Perception Restart

**Symptom**: `CUDA failure: status=35` and `Failed to set pipeline to PAUSED`.

**Cause**: GPU state not fully released on container restart.

**Fix**: Full container recreate:
```bash
# vss-rtvi-cv is a VSS Warehouse container — recreate it from the VSS deploy:
docker rm -f vss-rtvi-cv
docker compose up -d vss-rtvi-cv   # run from the VSS Warehouse deploy dir (see vss-deploy-profile)
```

---

## Safety Indicator Flickering (Multi-Machine)

**Symptom**: The safety colored disc in Isaac Sim flickers between MUTE/UNMUTE
randomly, or one machine's safety state affects another machine on the same network.

**Cause**: Multiple SIL systems on the same network share the default `ROS_DOMAIN_ID=0`,
and ROS2 discovery reaches across the LAN via UDP multicast. Nodes from different machines
then publish to the same `/safety/is_muted` topic, causing cross-machine interference.

**Fix (single-host SIL — preferred)**: scope discovery to loopback so it can never see
another host. `sil.env` ships this by default:

```bash
# In deployments/profiles/<profile>.env
ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST   # discover only on this host — also fixes cloud VMs (e.g. Brev) that block multicast
```

**Fix (multi-host / HIL, where nodes *must* span machines)**: keep discovery on `SUBNET`
and give each machine a unique `ROS_DOMAIN_ID` (0-232):

```bash
# In deployments/profiles/<profile>.env — a unique number per machine
ROS_DOMAIN_ID=42
```

Then restart Halos SIL:
```bash
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env down && docker compose --env-file profiles/<profile>.env up -d --build
```

**Verify isolation**:
```bash
docker exec comm-layer bash -c \
  "source /opt/ros/jazzy/setup.bash && ros2 topic info /safety/is_muted -v"
```

Should show `Publisher count: 1`. If it shows 2+, another machine is still
using the same domain ID.

> Single-host SIL is already isolated by `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, so the
> default `ROS_DOMAIN_ID=0` is fine. This section only matters if you switch discovery to
> `SUBNET` for multi-host / HIL.

---

## `no service selected` on Halos deploy

**Symptom**: `docker compose --env-file profiles/<profile>.env up -d --build` exits with
**`no service selected`** — nothing starts.

**Cause**: Halos services are gated by `profiles:` in `compose.yaml`. `<profile>.env` sets
`COMPOSE_PROFILES=<profile>`, but Docker Compose **< 2.39 does not read `COMPOSE_PROFILES`
from `--env-file`** — so no profile is active and `up` matches zero services.

**Fix**: activate the profile explicitly (works on every version):
```bash
export COMPOSE_PROFILES=<profile>
docker compose --profile <profile> --env-file profiles/<profile>.env up -d --build
```

---

## `ros2 topic info` returns "Unknown topic" / `ros2 topic list` empty

**Symptom**: inside `comm-layer`, `ros2 topic info /safety/is_muted -v` says the topic is
unknown and `ros2 topic list` is empty — yet MUTE/UNMUTE **is** flowing to the OPC log and
the forklift reacts.

**Cause**: this is **not** a failure. Single-host SIL sets
`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`; CycloneDDS loopback SPDP discovery does not
expose the bridge participant to the `ros2` CLI daemon, so the CLI can't enumerate topics
it isn't itself part of. The pub/sub link between comm-layer and Isaac is unaffected.

**Fix**: don't rely on the CLI here. Verify **functionally** — sim-driven MUTE/UNMUTE in
`$MDX_DATA_DIR/comm-layer/opc_server.log` proves the ROS link end-to-end:
```bash
grep -E "MUTE|UNMUTE" "$MDX_DATA_DIR/comm-layer/opc_server.log" | tail -5
```

---

## Quick Reference

| Error | Fix |
|-------|-----|
| PSF Kafka connection | Deploy VSS Warehouse first |
| `no service selected` | `export COMPOSE_PROFILES=<profile>` + `--profile <profile>` (Compose <2.39) |
| `ros2 topic info` empty / Unknown topic | Not a failure under LOCALHOST discovery — verify via OPC MUTE/UNMUTE |
| STALE events / MUTE stops | System ts (`attach-sys-ts-as-ntp=1`), not SEI sim-time; then widen `timeWindowSize` |
| Low FPS (~14) in SIL | Expected on shared GPU (not SEI); `0.00000` = stream not arriving |
| Isaac Sim crash (VRAM) | Check GPU VRAM, ISAAC_GPU_DEVICE |
| Isaac Sim Vulkan crash | Update driver >= 580.95.05, or restart (cached shaders) |
| No cameras in VST | Use `--enable-vst` flag |
| NGC 403 | Re-authenticate NGC + docker login |
| Compose errors | Upgrade to Docker Compose v2.39+ |
| Bbox flickering | `bbox_tolerance_ms=100` in VST config |
| CUDA errors on restart | Full container recreate, not restart |
| Safety flickering (multi-machine) | Assign unique `ROS_DOMAIN_ID` (0-232) per machine |
