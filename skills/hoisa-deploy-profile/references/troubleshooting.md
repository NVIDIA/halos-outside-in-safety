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
(6000 ms / 6 s in the SIL `nvpss.conf`; code fallback 200 ms) are dropped. Measure the **steady-state** rate **after the loop has
run a while** — startup / bootstrap STALE before Isaac streams stabilise is expected.
A persistent high rate (tens of % while running) indicates real pipeline latency.

**Cause**: perception→Kafka→PSF latency occasionally exceeds `timeWindowSize` (slow or
shared GPU, frame-timing jitter, multi-camera fusion); or the DeepStream override is
missing — without `attach-sys-ts-as-ntp=1`, frames don't get a proper (wall-clock) NTP
timestamp and nearly everything is dropped as STALE.

**Fix**:
```bash
# 1. Confirm the DeepStream SEI override is applied (see vss_2d_overrides.md)
# 2. If STALE is still high once running, widen the window:
nano <repo>/closed-loop-testing/safety-core/configs/nvpss.conf
# Increase timeWindowSize (e.g. 6000 -> 8000)
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env restart safety-core
```

---

## Low FPS / Flickering Bounding Boxes

**Symptom**: `vss-rtvi-cv` shows low FPS (<30), VST shows flickering boxes.

**Cause**: the DeepStream SIL override isn't applied (no system timestamps) — bboxes flicker and events drop as STALE. A source stuck at `0.00000` isn't arriving at all (Isaac not streaming yet, wrong RTSP URL, or TensorRT still building).

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
     ./run_sdg.sh --start --headless --enable-vst'
   ```

---

## RTSP Streams "no caps / could not create SDP" (Cold-Start Race)

**Symptom**: after a **cold** Isaac restart on a heavy scene, the perception client
can't pull Isaac Sim's self-hosted RTSP streams; DeepStream stays at `Active sources : 0`:

```
stream has no caps
could not create SDP
```

The encoder looks idle even though the RTSP server is accepting connections.
Warmer / lighter scenes that pre-roll quickly don't hit this.

**Cause** (not a network problem): Isaac Sim 6.0 self-hosts RTSP **in-process**, one
server per camera. Two things provoke "no caps":

1. **Cold pre-roll** — on a cold run the RTX render + encoder pre-roll is slow; the RTSP
   server accepts a client **before the encoder has produced its first frame**, so a
   client that DESCRIBEs in that window gets "no caps".
2. **Concurrent DESCRIBE (the persistent trigger)** — a **single, sequential** client
   negotiates clean caps even on a fairly cold server, but **multiple clients DESCRIBEing
   at the same instant race and all get "no caps"**. Isaac's only RTSP client is **VST**
   (`vss-vios-streamprocessing`): it DESCRIBEs each registered sensor **eagerly, each on its
   own thread**, so registering 3 sensors fires ~3 simultaneous DESCRIBEs at Isaac. (DeepStream
   is **not** the client here — it pulls VST's `/live/<id>` **proxy** re-stream, never Isaac
   directly.) This bites only when Isaac is **cold** at registration time.

Either way, a client that **DESCRIBE-churns** the wedged media keeps blocking caps
negotiation, so it **does NOT self-recover** (verified: a run left untouched stayed
wedged for 16 min). Clearing it needs **both** a **warm render** *and* the churn stopped —
i.e. re-register VST sensors only once Isaac is warm (the built-in warm-up gate does this).

**Confirm which trigger** — from inside the `isaac-sim` container, probe each port
sequentially, then all at once, on a **freshly restarted** Isaac (one concurrent churn
already wedges the media, so the contrast only shows cleanly before anything else probes):

```bash
declare -A MP=( [8554]=camera [8555]=camera_01 [8556]=camera_02 )
# sequential — a healthy port prints codec_name=h264; a wedged one times out
for p in 8554 8555 8556; do
  echo "== :$p =="
  ffprobe -rtsp_transport tcp -rw_timeout 5000000 -v error \
    -select_streams v -show_entries stream=codec_name \
    rtsp://127.0.0.1:$p/${MP[$p]} 2>&1 | grep -aE "codec_name|Could not|timed out|method DESCRIBE"
done
# concurrent — if the sequential pass worked but this fails, it is the concurrency race
for p in 8554 8555 8556; do
  ffprobe -rtsp_transport tcp -rw_timeout 5000000 -v error \
    -select_streams v -show_entries stream=codec_name \
    rtsp://127.0.0.1:$p/${MP[$p]} >/tmp/ff_$p.log 2>&1 & done; wait
for p in 8554 8555 8556; do
  grep -qi h264 /tmp/ff_$p.log && echo ":$p OK" || echo ":$p WEDGED"
done
```

**Prevention (built-in)**: the SIL launch (`run_actor_sdg.py --enable-vst`) **defers VST
sensor registration until the render is warm** — it waits for the timeline to advance
`--vst-register-warmup-sec` of sim-time under Play (default 1.0s) before registering, so
VST never DESCRIBEs a capless stream (look for `[vst-warmup]` in the run log). A fresh
`--start --enable-vst` run should not hit this. You can still hit it if **stale** VST
sensors from a previous run are pointed at Isaac's RTSP and churn it during pre-roll.

**Prevention (restarts)**: the warm-up gate cannot help across a **restart** — the
previous run's sensors are still registered and their VST clients hammer the new Isaac
the instant its ports open. Restart via the wrapper, which pauses
`vss-vios-streamprocessing` for the boot window and resumes it once the mounts deliver:

```bash
bash closed-loop-testing/scripts/restart_isaac.sh   # see test_scenario.md for expected behavior
```

Single-host stacks only (`base`/`sil`): the wrapper pauses a **local** VST container.
On the two-host `hil` profile it cannot reach the Thor-side VST — follow "Restart the
scenario (hil)" in `halos_hil.md` instead.

**Recovery** — if you do wedge (e.g. stale sensors churning), stop the churn and
re-provision cleanly:

```bash
# 1. Confirm the render is actually warm (producing frames). If the timeline never
#    advances / GPU is idle, the render isn't warming — that's a GPU/scene/navmesh
#    fault, not this race (check the GPU is visible INSIDE the container):
docker exec isaac-sim nvidia-smi -L

# 2. Stop the DESCRIBE-churn and re-register: delete all VST sensors, then re-add from
#    config. The fresh sensor-add events are forwarded to the LIVE DeepStream, which
#    returns to N active sources in ~1 min.
docker exec isaac-sim /isaac-sim/kit/python/bin/python3 \
  /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all
docker exec isaac-sim /isaac-sim/kit/python/bin/python3 \
  /isaac-sim/sil/scripts/vst_sensor_manager.py --add-from-config \
  /isaac-sim/sil/configs/cameras.yaml
```

**Do NOT** "recover" by restarting the config-adaptor chain (`vss-configurator` →
`vss-rtvi-cv-config-adaptor` → `vss-rtvi-cv`): it duplicates sensors and does **not**
re-push the source list to a freshly-restarted DeepStream (`use-nvmultiurisrcbin=1`
only ingests on sensor-add events), leaving DeepStream at 0. The heavier fallback is
relaunching the Isaac kit (`docker restart isaac-sim`) so the built-in warm-up gate
registers cleanly. This is a cold-start race — not a symptom of a broken network,
RTSP config, or GPU.

---

## DeepStream Stuck at ≤2/3 Active Sources (Zombie Source Bins)

**Symptom**: DeepStream comes up but never reaches 3/3 — `vss-rtvi-cv` logs
`Active sources : 1` or `2`, and the missing camera is stuck at 0 fps with
`No data from source ... trying reconnection`. A **specific** camera stays dead (the one
whose sensor uuid drifted) — it doesn't "reshuffle". For the 3D (Sparse4D) profile this is
**not** healthy: Sparse4D fuses all 3
views (`num_sensors: 3`), so 2/3 corrupts the BEV — treat anything below 3/3 as a failure.

**Cause — stale-identity provisioning gap, NOT a caps race.** DeepStream ingests VST's
`/live/<uuid>` **proxy** re-stream (not Isaac directly). Each time a camera **re-registers**
(Isaac restart / RTSP churn) VST mints a **new sensor uuid**; DeepStream keeps the **old**
uuid's proxy source bin — a **zombie** whose `/live/<old-uuid>` mount is now 404 (its proxy
port was torn down) → it loops "No data / reconnection" at 0 fps — while the **live** uuid is
never re-added to DeepStream. Net: one slot stuck at 0 fps → <3/3. A clean, un-churned bring-up
reaches 3/3; this is a **churn / re-registration** artifact, not a concurrency race.

**Fix — purge the zombie, then add the live uuid** (validated live: 2/3 → 3/3 @~12 fps). Drive
the DeepStream `nvmultiurisrcbin` REST on `:9000`:

```bash
# 1. Identify live vs zombie. VST lists each ONLINE sensor's real /live/<uuid> proxy url:
curl -s http://<HOST_IP>:30888/vst/api/v1/sensor/list      # names + uuids + state
curl -s http://<HOST_IP>:30888/vst/api/v1/sensor/streams   # live rtsp://.../live/<uuid> per sensor
# What DeepStream currently holds (the 0-fps camera_id is the zombie):
curl -s http://localhost:9000/api/v1/stream/get-stream-info

# 2. Remove the zombie. TWO things must be exact:
#    - change at value.change (else HTTP 400 "Sensor API change string not supported" -> it survives)
#    - camera_url = the EXACT url DS registered it with — often an OLD, torn-down proxy port, NOT
#      the current one; a wrong url returns "No record found". Recover it from the logs:
#      docker logs sdr-controller 2>&1 | grep <zombie-uuid>
curl -s -X POST http://localhost:9000/api/v1/stream/remove -H 'Content-Type: application/json' \
  -d '{"key":"sensor","value":{"camera_id":"<zombie-uuid>","camera_name":"<name>",
       "camera_url":"rtsp://<HOST_IP>:<OLD_PORT>/live/<zombie-uuid>","change":"camera_remove","metadata":{}}}'
#      -> expect: STREAM_REMOVE_SUCCESS

# 3. Add the live uuid — AFTER the remove succeeds: max-batch-size is 3, so the zombie must free
#    its slot first, or the add fails "Active sources exceeded max-batch-size".
curl -s -X POST http://localhost:9000/api/v1/stream/add -H 'Content-Type: application/json' \
  -d '{"key":"sensor","value":{"camera_id":"<live-uuid>","camera_name":"Camera_01",
       "camera_url":"rtsp://<HOST_IP>:<PORT>/live/<live-uuid>","change":"camera_add","metadata":{}}}'
#      -> expect: STREAM_ADD_SUCCESS, then `Active sources : 3`
```

> ℹ️ **Separate issue** — the Isaac **cold**-DESCRIBE wedge (`Active sources : 0`, "no caps") is
> the section above; its upstream fix is an Isaac RFE: gate the RTSP server's DESCRIBE response on
> the **first encoded frame** so caps are cached before VST's concurrent DESCRIBEs arrive.

---

## Provisioning Chain Polluted (Duplicate DS Sources / Event Replay Storms)

**Symptom**: after many sensor re-registration cycles (repeated Isaac restarts, recovery
attempts), DeepStream's `get-stream-info` shows **duplicate camera names** (e.g.
`Camera_02` twice) or a camera never lands no matter how often you re-register; restarting
`sdr-controller` makes it *worse* (a burst of `camera_add` pushes replays).

**Cause**: the sensor lifecycle events live in a **Redis stream** with a long retention.
`sdr-controller` replays it on restart — including every stale add/remove from previous
cycles — and stale entries steal DeepStream's `max-batch-size` slots.

**Fix — flush the event backlog, then ONE clean registration round** (order matters):

```bash
docker exec redis redis-cli FLUSHALL          # clears the event backlog + SDR workload cache
docker restart sdr-controller                 # comes up with an empty, clean cache
docker restart vss-rtvi-cv                    # DeepStream restarts as an EMPTY pipeline
sleep 15
# one clean registration round -> the only events in the stream are the fresh ones
docker exec isaac-sim bash -lc 'cd /isaac-sim && \
  ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --delete-all && sleep 3 && \
  ./python.sh /isaac-sim/sil/scripts/vst_sensor_manager.py --add-from-config /isaac-sim/sil/configs/cameras.yaml'
# DeepStream reaches N/N in ~30-60 s
```

If a camera STILL never comes up after this, check its Isaac mount directly
(`ffprobe -rtsp_transport tcp rtsp://localhost:<port>/<mount>` from inside `isaac-sim`) —
a dead mount is the upstream "no caps" wedge (section above); restart the scenario with
`restart_isaac.sh`.

**Full reset (last resort)** — when the state itself is suspect, return the stack to the
proven-clean first-bring-up state without paying the TensorRT rebuild: procedure in
`halos_deploy.md` → "Reset to a fresh deployment (keep the caches)".

---

## Perception 0 FPS With Sensors Online (Stale Sensor History Replay)

**Symptom**: on a host that ran VSS **before**, sensors show `online` in VST, perception
logs show the right stream names, no errors anywhere — and the sources sit at 0 fps.
Re-adding the streams helps, but they die again after the next perception restart.

**Cause**: the sensor distribution service (`sdr-controller`) (re)provisions perception by
replaying the **Kafka sensor history**. On a previously-used deploy the topics hold the whole
add/remove history — including stale stream URLs from earlier runs — and every perception
restart replays it, pushing the stale URLs back in over the live ones.

**Fix** — fresh state before redeploy: tear the VSS stack down and clear the Kafka state.

```bash
# From the VSS deploy dir: down WITHOUT -v (keeps the TensorRT engine cache + sensor DB)
docker compose --env-file industry-profiles/warehouse-operations/.env down --remove-orphans

# Remove ONLY the kafka volumes — targeted, NOT `docker volume prune`: with the stack down,
# prune also deletes the dangling perception engine cache (a 15-20 min rebuild) and the
# sensor database.
docker volume ls | grep -i kafka
docker volume rm <the kafka volumes>

# then bring VSS back up
```

Related state rules:

- Replacing sensors mints new VST UUIDs; refresh anything that embeds the old stream URLs
  (e.g. the safety-core sensor config).
- If a source stalls at 0 fps once after an Isaac restart, just re-add the stream (or restart
  the perception container and let the configurator re-provision). If it recurs after **every**
  perception restart, it is this replay issue — wipe the Kafka state.

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
  ./run_sdg.sh --start --headless --enable-vst'
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

## No ROI / Tripwire Events Reaching the Safety Core (detections look fine)

**Symptom**: Perception detects people / forklifts correctly (boxes look good in
VST), but the Safety Core never reacts — no MUTE/UNMUTE as a person enters a
restricted ROI or the forklift crosses the tripwire. **No error is logged** — the
events simply never arrive at the SDM.

**Cause**: a `calibration.json` misconfiguration. The shipped sample calibrations
(2D and 3D) already set these correctly — this shows up on a **custom scene / your own
AMC-generated calibration** (VSS side, not shipped in this repo), most commonly one of:
1. **A restricted ROI is missing `restrictedObjectTypes`** (e.g. `["Person"]`). The ROI
   geometry is defined, but no object class is marked restricted, so a person inside it is
   never reported as a violation. This is the most common cause.
2. **The ROI / tripwire `id` does not match the `rule_id`** in the ATL event-mapping file
   (`event_mapping_atl.pb.txt`, referenced by `safety-core/configs/nvpss.conf`) — the event
   fires but the Safety Core can't map it to `EVENT_0`…`EVENT_5`.

**Fix**: run the calibration prerequisite check in `halos_deploy.md` (§0) — it flags ROIs
with no `restrictedObjectTypes` and prints the ids to cross-check against the event map. Add
the missing field per the ROI schema in `calibration_3d.md`. The AMC / Calibration Toolkit
does not expose restricted / confined object types as a dedicated field, so set them via its
**Full Control** JSON editor at the [export step](https://docs.nvidia.com/vss/3.2.1/autocalib-workflow-steps.html#export-calibration-data)
(advanced) or by editing the exported `calibration.json` directly. Then regenerate / re-mount
the calibration and **recreate** (not restart) the VSS perception + safety-core containers so
the new `calibration.json` and event mapping are picked up.

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
| Low FPS / flickering | Apply DeepStream SIL override — see `vss_2d_overrides.md` |
| Isaac Sim crash (VRAM) | Check GPU VRAM, ISAAC_GPU_DEVICE |
| Isaac Sim Vulkan crash | Update driver >= 580.95.05, or restart (cached shaders) |
| RTSP "no caps / could not create SDP" | Cold Isaac + **VST** concurrent DESCRIBE — warm render, re-register VST only when warm (built-in warm-up gate) |
| DeepStream stuck ≤2/3 active (zombie bins) | Stale-identity: purge the 0-fps zombie (`value.change`, **exact old proxy url**) then add the live uuid via `:9000` |
| No cameras in VST | Use `--enable-vst` flag |
| NGC 403 | Re-authenticate NGC + docker login |
| Compose errors | Upgrade to Docker Compose v2.39+ |
| Bbox flickering | `bbox_tolerance_ms=100` in VST config |
| CUDA errors on restart | Full container recreate, not restart |
| Safety flickering (multi-machine) | Assign unique `ROS_DOMAIN_ID` (0-232) per machine |
| No ROI/tripwire events (detections OK) | `restrictedObjectTypes` missing in `calibration.json`, or roi/tripwire `id` ≠ `rule_id` in the event map — see `halos_deploy.md` §0 |
