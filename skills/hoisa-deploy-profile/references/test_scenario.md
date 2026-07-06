# Test Scenario and Monitoring (`sil`)

Run the Isaac Sim test scenario (`sil` profile) and monitor safety commands.

---

## Run the scenario

```bash
docker exec -d isaac-sim bash -lc 'cd /isaac-sim/sil/scripts && \
  ./run_sdg.sh -c /isaac-sim/sil/configs/default_config_ros.yaml \
  --start --headless --enable-vst \
  --cameras-config /isaac-sim/sil/configs/cameras.yaml'
```

The container already has `VST_BASE_URL`, `HOST_IP`, and `ROS_DOMAIN_ID` from the
profile env; `run_sdg.sh` sets the ROS2 environment and launches the scene.

**What happens**:
1. Loads the warehouse scene
2. Spawns the forklift + digital humans
3. Initializes the ROS2 Action Graph (the forklift safety disc subscribes `/safety/is_muted`)
4. Runs the forklift playback (`segments.json`: forward into trailer → idle → backward → idle)
5. Starts RTSP streaming — Isaac 6.0 **self-hosts** RTSP per camera (H264), no external
   `mediamtx`: `rtsp://localhost:8554/camera`, `:8555/camera_01`, `:8556/camera_02`
6. Registers the 3 cameras with VST — `--enable-vst` deletes existing sensors, then adds the Isaac cameras

**First-run note**: Isaac Sim goes quiet for ~5-10 min on first run (scene load + RT
shader compile, cached afterwards). **Do not gate on a shader-log string** (it does not
appear in the Isaac 6.0 kit log → poll hangs forever). Gate on the Isaac→VSS stream
handoff completing. **Gotcha:** the VSS sample-video bootstrap also registers ~3 DeepStream
streams *before* Isaac, so a bare "3 active streams" can fire early. Bootstrap-immune signal:
the Isaac kit log — only Isaac emits `RTSP stream started … encoding=h264`. Gate on that
first, then confirm DeepStream ingested them.

```bash
# 1) PRIMARY (bootstrap-immune): Isaac started its 3 self-hosted RTSP streams (H264)
while :; do
  n=$(docker exec isaac-sim bash -lc 'KL=$(ls -t /isaac-sim/kit/logs/Kit/*/*/kit_*.log | head -1); \
        grep -c "RTSP stream started" "$KL"' 2>/dev/null)
  [ "${n:-0}" -ge 3 ] && break
  printf '[%s] waiting: Isaac RTSP streams=%s/3\n' "$(date +%H:%M:%S)" "${n:-0}"; sleep 20
done
echo "Isaac streaming 3 RTSP cams (H264: 8554/camera, 8555/camera_01, 8556/camera_02)"

# 2) CONFIRM: DeepStream ingested them. net = added - removed (logs accumulate across runs)
while :; do
  added=$(docker logs vss-rtvi-cv 2>&1 | grep -c 'new stream added \[')
  removed=$(docker logs vss-rtvi-cv 2>&1 | grep -c 'new stream removed \[')
  [ "$((added - removed))" -ge 3 ] && break
  printf '[%s] waiting: DeepStream active=%s/3 (added=%s removed=%s)\n' \
    "$(date +%H:%M:%S)" "$((added - removed))" "$added" "$removed"; sleep 20
done
echo "DeepStream ingesting 3 Isaac streams — handoff complete"
```

> **`--start` auto-stops** after `simulation_length` frames (set in the IRA config
> `default_config_ros.yaml`), and on exit it removes the VST sensors it added. For a
> long, watchable run, raise `simulation_length`. Stopping the run with SIGINT can
> leave the Isaac VST sensors registered (orphaned) — re-running with `--enable-vst`
> cleans them (it deletes, then re-adds).

---

## Scene lifecycle — handoff signals + background completion monitor

The Isaac→VSS handoff logs matching events on **add** (scene start / streaming) and
**remove** (scene stop / teardown). Use them to confirm the scene is running and to
detect when it finishes. All signals below were verified on a live VSS 3.2 + Halos SIL run.

| Component | Container | ADD — scene streaming | REMOVE — scene done / teardown |
|-----------|-----------|-----------------------|--------------------------------|
| DeepStream (perception) | `vss-rtvi-cv` | `new stream added [<idx>:<uuid>:<Camera>]` ×3 | `new stream removed [<idx>:...]` + `gstnvtracker: Successfully removed stream <idx>` |
| Isaac RTSP self-hosted (6.0) | `isaac-sim` (kit log) | `[isaacsim.streaming.rtsp.impl.rtsp_writer] RTSP stream started on rtsp://localhost:{8554/camera, 8555/camera_01, 8556/camera_02} (…, encoding=h264)` ×3 (each preceded by `[omni.kit.livestream.rtsp.plugin] Started RTSP server at …`) | no dedicated teardown line (RTSP clients just log `Client disconnected`) — use DeepStream `new stream removed` (row 1) as the authoritative teardown |
| VST sensor mgr | `vss-vios-sensor` | `"change" : "camera_add"` · `addSensor completed: <Camera>` | `"change" : "camera_remove"` · `delete sensor: <uuid>` |

`<Camera>` = `Camera`, `Camera_01`, `Camera_02`.

### Launch a background scene-done monitor

`--start` auto-stops after `simulation_length` frames and tears the streams down. Launch
a **detached** monitor (non-blocking — does not hold the main flow) that announces when
the scene finishes, so you don't have to watch it:

```bash
nohup bash -c '
  log() { printf "[%s] %s\n" "$(date +%H:%M:%S)" "$1"; }
  # phase 1: wait until the scene is streaming (net added-removed >= 3)
  until [ "$(( $(docker logs vss-rtvi-cv 2>&1 | grep -c "new stream added \[") \
              - $(docker logs vss-rtvi-cv 2>&1 | grep -c "new stream removed \[") ))" -ge 3 ]; do
    log "scene starting (streams not up yet)..."; sleep 20
  done
  base=$(docker logs vss-rtvi-cv 2>&1 | grep -c "new stream added \[")
  log "scene RUNNING — 3 Isaac streams live; watching for completion"
  # phase 2: done when the run process exits OR this run'\''s streams are all torn down
  while :; do
    proc=$(docker exec isaac-sim pgrep -f run_actor_sdg.py 2>/dev/null | head -1)
    removed=$(docker logs vss-rtvi-cv 2>&1 | grep -c "new stream removed \[")
    { [ -z "$proc" ] || [ "$removed" -ge "$base" ]; } && { log "SCENE DONE — run exited / streams torn down"; break; }
    sleep 15
  done
' > /tmp/hoisa_scene_monitor.log 2>&1 &
echo "scene-done monitor PID $! — watch with: tail -f /tmp/hoisa_scene_monitor.log"
```

When it logs `SCENE DONE`, the run has finished — read the OPC / PSF logs below for the
run's MUTE/UNMUTE transition summary.

### Per-component handoff trace (debug)
```bash
docker logs vss-rtvi-cv     2>&1 | grep -E 'new stream (added|removed) \['
docker logs vss-vios-sensor 2>&1 | grep -E '"change" : "camera_(add|remove)"'
# Isaac 6.0 self-hosted RTSP (no mediamtx): stream-start lines from the kit log
docker exec isaac-sim bash -lc 'KL=$(ls -t /isaac-sim/kit/logs/Kit/*/*/kit_*.log | head -1); grep -E "RTSP stream started|Started RTSP server" "$KL" | grep -v "Client "'
```

---

## Monitor Safety Commands

### OPC server log
```bash
tail -n 30 "$MDX_DATA_DIR/comm-layer/opc_server.log"
```
Expected (`Seq#N | <description> | <symbol> <status> | ts=<ISO>`):
```
INFO:udp_receiver.safety_receiver:Received: Seq#0 | HEARTBEAT | 💓 Heartbeat | ts=2026-04-29T13:33:28.055302+00:00
INFO:udp_receiver.safety_receiver:Received: Seq#3 | MUTE (ALLOW OPERATION) | 🟢 Safety muted - Loading allowed | ts=2026-04-29T13:33:38.946800+00:00
INFO:udp_receiver.safety_receiver:Received: Seq#9 | UNMUTE (PREVENT OPERATION) | 🟡 Safety active + Alarm on | ts=2026-04-29T13:34:06.987138+00:00
```
| Symbol | Command | Meaning |
|--------|---------|---------|
| 🟢 | MUTE (ALLOW OPERATION) | Forklift in trailer, no humans — loading allowed |
| 🟡 | UNMUTE (PREVENT OPERATION) | Human present or forklift exiting — safety active + alarm |
| 💓 | HEARTBEAT | Periodic keep-alive (every 5 s) — confirms the PSF→comm-layer link is healthy |

### PSF log
```bash
tail -n 30 "$MDX_DATA_DIR/psf-log/pss.log"
```
```
... nv_mdx_client[59]: ... Endpoint: NVPSB_PSS_SOURCE Data: Safety event reported: EVENT_0 (rule: Forklift tripwire OUT)
... nv_mdx_client[59]: ... Endpoint: NVPSB_PSS_SOURCE Data: Safety event reported: EVENT_1 (rule: Forklift tripwire IN)
... nv_mdx_client[59]: ... Endpoint: NVPSB_PSS_SOURCE Data: Safety event reported: EVENT_4 (rule: Person restricted area ROI violation)
... nv_mdx_client[59]: ... Endpoint: NVPSB_PSS_SOURCE Data: Safety event reported: EVENT_5 (rule: Person restricted area ROI violation cleared)
... NVPSB_PSD_CLIENT[34]: ... Data: PSD-Gateway: received DecisionRequest id=1 with 1 events
```
- **EVENT_0 / EVENT_1**: forklift tripwire crossings (OUT / IN the trailer).
- **EVENT_4 / EVENT_5**: person restricted-area ROI violation / cleared (digital humans).
- The loop can be driven by **either** family depending on the scene/segments — e.g. if the
  forklift does a single pass then parks, MUTE/UNMUTE is driven by the person-ROI events.
  Don't expect only forklift events.
- **DecisionRequest**: the PSF decision-maker is invoked — it produces the corresponding MUTE/UNMUTE command shown in the OPC log above.

---

## View camera streams

`http://<HOST_IP>:30888/vst/` — live Isaac Sim camera feeds with detection overlays.
The forklift's safety disc colour follows MUTE/UNMUTE and is visible in the feeds even
in headless mode.

---

## Verify End-to-End

The system is working when:
1. `vss-rtvi-cv` shows non-zero, **stable** FPS for **all 3** cameras — gate on "non-zero
   & steady", not an exact number. In SIL expect ~14 FPS/cam (the perception GPU is shared
   with Isaac Sim); the ~30 in NVIDIA's docs assumes a dedicated perception GPU
2. **The OPC server log shows MUTE↔UNMUTE transitions after Isaac started streaming** — this
   is the authoritative end-to-end signal (it proves perception → PSF → comm-layer → ROS →
   Isaac all work). The documented `ros2 topic info … Publisher count: 1` is a *nice-to-have*
   that often can't enumerate under loopback discovery — see ROS wiring below; don't block on it
3. The PSF log shows ATL decision changes driven by **forklift tripwire (EVENT_0/1) or person
   restricted-area ROI (EVENT_4/5)** — whichever the scene produces
4. The VST UI shows the camera streams with bounding boxes

> "Working" means **sim-driven** transitions (from the Isaac scene — forklift cycle and/or
> digital-human ROI) — not the VSS sample-video bootstrap traffic that appears before Isaac
> streams come up.

### ROS wiring (best-effort — the functional OPC check above is authoritative)
```bash
docker exec comm-layer bash -c \
  "source /opt/ros/jazzy/setup.bash && ros2 topic info /safety/is_muted -v" \
  | grep -E "Publisher count|Subscription count"
```
> **Often can't enumerate under loopback.** With `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`,
> CycloneDDS hides the bridge from the `ros2` daemon → `Unknown topic`/empty even when
> healthy. **Not a failure** — the OPC MUTE/UNMUTE evidence above is authoritative.
- **`Publisher count: 1`** — one (comm-layer). `2+` = another host shares your `ROS_DOMAIN_ID`
  on `SUBNET` (multi-host only) — see `troubleshooting.md`.
- **`Subscription count: 1`** — Isaac's forklift Action Graph is subscribed. `0` usually just
  means the CLI can't see it over loopback; confirm via MUTE/UNMUTE changing forklift state.
