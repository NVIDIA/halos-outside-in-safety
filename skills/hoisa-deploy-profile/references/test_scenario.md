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
5. Starts RTSP streaming (3 cameras, H265, via MediaMTX)
6. Registers the 3 cameras with VST — `--enable-vst` deletes existing sensors, then adds the Isaac cameras

**First-run note**: Isaac Sim goes quiet for ~5-10 min on first run (scene load + RT
shader compile, cached afterwards). **Do not gate on a shader-log string** (it does not
appear in the Isaac 5.1.0 kit log → poll hangs forever). Gate on the Isaac→VSS stream
handoff completing — poll DeepStream (`vss-rtvi-cv`) for 3 active Isaac streams (the end
of the chain, most authoritative):

```bash
# net = added - removed: docker logs accumulate across runs, so a raw count is a
# re-run false-positive. Net active sources is correct.
while :; do
  added=$(docker logs vss-rtvi-cv 2>&1 | grep -c 'new stream added \[')
  removed=$(docker logs vss-rtvi-cv 2>&1 | grep -c 'new stream removed \[')
  [ "$((added - removed))" -ge 3 ] && break
  printf '[%s] waiting: DeepStream active=%s/3 (added=%s removed=%s)\n' \
    "$(date +%H:%M:%S)" "$((added - removed))" "$added" "$removed"
  sleep 20
done
echo "DeepStream ingesting 3 Isaac streams — handoff complete"
```

> **Secondary** — mediamtx publishers for the **current** run (`--since`, and filter the
> `no one is publishing` reader-spam, which otherwise always matches ≥3):
> ```bash
> docker logs --since 3m mediamtx 2>&1 \
>   | grep "is publishing to path 'RTSPWriter_World_Cameras_Camera" | grep -v 'no one' \
>   | grep -oE 'Camera[_0-9]*_rgb' | sort -u | wc -l        # expect 3
> ```
> A bare `grep -c RTSPWriter...` on mediamtx is wrong on both counts (matches the
> reader-spam **and** counts across runs).

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
| mediamtx | `mediamtx` | `is publishing to path 'RTSPWriter_World_Cameras_<Camera>_rgb'` ×3 | `session ... destroyed` |
| Isaac Sim (kit log) | `isaac-sim` | `"rgb" of "/World/Cameras/<Camera>" will be published to "rtsp://..."` ×3 | `Subprocess on "/World/Cameras/<Camera>" has been terminated` ×3 |
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
docker logs mediamtx        2>&1 | grep -E "is publishing to path 'RTSPWriter|destroyed:"
docker logs vss-vios-sensor 2>&1 | grep -E '"change" : "camera_(add|remove)"'
docker exec isaac-sim bash -lc 'KL=$(ls -t /isaac-sim/kit/logs/Kit/*/*/kit_*.log | head -1); grep -E "will be published|Subprocess on .* has been terminated" "$KL"'
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
... NVPSB_PSD_CLIENT[34]: ... Data: PSD-Gateway: received DecisionRequest id=1 with 1 events
```
- **EVENT_0 / EVENT_1**: tripwire crossings reported by perception (forklift OUT / IN the trailer).
- **DecisionRequest**: the PSF decision-maker is invoked — it produces the corresponding MUTE/UNMUTE command shown in the OPC log above.

---

## View camera streams

`http://<HOST_IP>:30888/vst/` — live Isaac Sim camera feeds with detection overlays.
The forklift's safety disc colour follows MUTE/UNMUTE and is visible in the feeds even
in headless mode.

---

## Verify End-to-End

The system is working when:
1. `vss-rtvi-cv` shows ~30 FPS for **all 3** cameras
2. `ros2 topic info /safety/is_muted -v` shows **`Publisher count: 1`** (see ROS isolation below)
3. The OPC server log shows MUTE↔UNMUTE transitions (≥10) **after** Isaac started streaming
4. The PSF log shows ATL decision changes tied to the forklift entering / leaving the trailer
5. The VST UI shows the camera streams with bounding boxes

> "Working" means **sim-driven** transitions (the forklift cycle) — not the VSS
> sample-video bootstrap traffic that appears before Isaac streams come up.

### ROS wiring (check once per host)
```bash
docker exec comm-layer bash -c \
  "source /opt/ros/jazzy/setup.bash && ros2 topic info /safety/is_muted -v" \
  | grep -E "Publisher count|Subscription count"
```
- **`Publisher count: 1`** — exactly one (comm-layer). If `2+`, another machine on the
  network shares your `ROS_DOMAIN_ID` — see `troubleshooting.md` → "Safety Indicator
  Flickering (Multi-Machine)".
- **`Subscription count: 1`** — the Isaac Sim forklift Action Graph has connected and
  is receiving safety state. `0` means Isaac isn't subscribed yet (scene not fully up,
  or a `ROS_DOMAIN_ID` mismatch between `isaac-sim` and `comm-layer`).
