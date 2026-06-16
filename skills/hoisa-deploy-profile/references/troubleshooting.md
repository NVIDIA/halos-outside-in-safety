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
(default 900 ms) are dropped. Measure the **steady-state** rate **after the loop has
run a while** — startup / bootstrap STALE before Isaac streams stabilise is expected.
A persistent high rate (tens of % while running) indicates real pipeline latency.

**Cause**: perception→Kafka→PSF latency occasionally exceeds `timeWindowSize` (slow or
shared GPU, frame-timing jitter, multi-camera fusion); or the SEI override is missing —
without it, frames carry no NTP timestamp and nearly everything is dropped as STALE.

**Fix**:
```bash
# 1. Confirm the DeepStream SEI override is applied (see vss_2d_overrides.md)
# 2. If STALE is still high once running, widen the window:
nano <repo>/closed-loop-testing/safety-core/configs/nvpss.conf
# Increase timeWindowSize (e.g. 900 -> 1200)
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env restart safety-core
```

---

## Low FPS / Flickering Bounding Boxes

**Symptom**: `vss-rtvi-cv` shows low FPS (<30), VST shows flickering boxes.

**Cause**: DeepStream SEI extraction enabled — incompatible with Isaac Sim RTSP.

**Fix**: Apply DeepStream config changes (see `vss_2d_overrides.md`):
- Comment out `extract-sei-type5-data` and `sei-uuid` in `[source-list]`
- Set `attach-sys-ts-as-ntp=1` in `[streammux]`
- Comment out `extract-sei-sim-time` and `drop-backward-sei`

Restart perception: `docker restart vss-rtvi-cv`

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

**Cause**: Multiple SIL systems on the same network share the default
`ROS_DOMAIN_ID=0`. ROS2 nodes from different machines publish to the same
`/safety/is_muted` topic, causing cross-machine interference.

**Fix**: Assign each machine a unique `ROS_DOMAIN_ID` (0-232) in the Halos `.env`:

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

> This only affects multi-machine setups on the same network. Single-machine
> deployments can safely use the default `ROS_DOMAIN_ID=0`.

---

## Quick Reference

| Error | Fix |
|-------|-----|
| PSF Kafka connection | Deploy VSS Warehouse first |
| STALE events | Increase `timeWindowSize` in nvpss.conf |
| Low FPS / flickering | Disable SEI in DeepStream config |
| Isaac Sim crash (VRAM) | Check GPU VRAM, ISAAC_GPU_DEVICE |
| Isaac Sim Vulkan crash | Update driver >= 580.95.05, or restart (cached shaders) |
| No cameras in VST | Use `--enable-vst` flag |
| NGC 403 | Re-authenticate NGC + docker login |
| Compose errors | Upgrade to Docker Compose v2.39+ |
| Bbox flickering | `bbox_tolerance_ms=100` in VST config |
| CUDA errors on restart | Full container recreate, not restart |
| Safety flickering (multi-machine) | Assign unique `ROS_DOMAIN_ID` (0-232) per machine |
