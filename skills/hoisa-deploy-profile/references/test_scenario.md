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

Two-forklift variant: swap in the 2FL config pair — `-c /isaac-sim/sil/configs/default_config_ros_2fl.yaml
--robots-config /isaac-sim/sil/configs/robots-2fl.yaml` (plus `COMPOSE_PROFILES=sil,multi-robot` — notes in `robots.yaml`/`sil.env`).

The container already has `VST_BASE_URL`, `HOST_IP`, and `ROS_DOMAIN_ID` from the
profile env; `run_sdg.sh` sets the ROS2 environment and launches the scene.

**What happens**:
1. Loads the warehouse scene
2. Spawns the forklift + digital humans
3. Initializes the ROS2 Action Graph (the forklift safety disc subscribes `/safety/is_muted`)
4. Runs the forklift playback (`segments.json`: forward into trailer → idle → backward → idle)
5. Starts RTSP streaming — Isaac 6.0 **self-hosts** RTSP per camera (H264):
   `rtsp://localhost:8554/camera`, `:8555/camera_01`, `:8556/camera_02`
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

# 2) CONFIRM: DeepStream is PRODUCING FPS on all 3 cameras — not merely "added".
#    Do NOT gate on (added - removed): a stuck/zombie source stays "added" (it never logs
#    "removed") at 0 FPS, so that count reads 3 while only 2 cameras are actually live.
#    Gate on the latest per-camera current-FPS from the PERF lines instead.
while :; do
  live=$(docker logs --tail 800 vss-rtvi-cv 2>&1 | grep 'stream_name Camera' | awk '
    { fps=$1+0; for(i=1;i<=NF;i++) if($i=="stream_name") n=$(i+1); last[n]=fps }
    END{ c=0; for(k in last) if(last[k]>0) c++; print c }')
  [ "${live:-0}" -ge 3 ] && break
  printf '[%s] waiting: DeepStream live cameras=%s/3 (current FPS > 0)\n' \
    "$(date +%H:%M:%S)" "${live:-0}"; sleep 20
done
echo "DeepStream producing FPS on 3 Isaac cameras — handoff complete"
```

> **If `live` never reaches 3** — one camera stays at 0 FPS (`No data from source ...
> trying reconnection`) — that source is a stale/zombie bin, **not** a slow start. Recover
> it via `troubleshooting.md` → "DeepStream Stuck at ≤2/3 Active Sources (Zombie Source
> Bins)". A bare `added - removed` count would have hidden this (it stays 3).

> **`--start` runs until externally stopped.** (The 5.1 `simulation_length` frame-count
> auto-stop no longer applies on Isaac Sim 6.0 — the renamed `simulation_duration` only
> extends the timeline end time.) Stopping the run with SIGINT/kill leaves the Isaac VST
> sensors registered — the next `--enable-vst` run cleans them (it deletes, then re-adds).
> To restart the scenario on a live stack, use the wrapper in the next section.

### Restart the scenario on a live stack

Killing and relaunching `run_actor_sdg.py` while VST is running wedges the fresh Isaac
RTSP mounts: VST's proxy client and liveness prober reconnect within milliseconds of the
port opening and DESCRIBE the stream before the encoder has produced a frame — the
cold-window race from `troubleshooting.md` ("no caps"), which does not self-recover.
Use the wrapper, which pauses VST streamprocessing for exactly the boot window:

```bash
bash closed-loop-testing/scripts/restart_isaac.sh    # reuses the running driver's own args
```

What to expect (measured on 2D and 3D single-GPU boxes and a 2-GPU 3D box, 6/6 runs):

- Mounts warm in ~1-2 min (cached shaders), VST resumes, DeepStream back at 3/3 about
  30 s later — total ≈ 2 min, with **zero** `has no caps` lines in the
  `vss-vios-streamprocessing` and DeepStream logs (grep those, not the driver run log —
  it can be empty while streaming works).
- The driver re-registers the cameras at render-warm as usual → **fresh sensor uuids on
  every restart**; SDR re-pushes them to DeepStream automatically.
- Cosmetic leftovers, safe to ignore: one empty-name `offline` "ghost" entry per restart
  in `sensor/list` (the next restart's delete-all clears the previous one; the VST UI may
  render it as a stale tile under the old camera name), a slowly growing list of
  `removed` tombstones, and the DeepStream `source_id`↔camera mapping may shuffle.
- DeepStream has a known intermittent abort (`std::logic_error` / "basic_string:
  construction from null", exit 134) when its sources drain while it runs; Docker's
  `on-failure` policy usually revives it, and the script additionally detects a dead
  `vss-rtvi-cv`, restarts it and re-registers the sensors (a restarted DeepStream
  comes back as an empty pipeline that nothing re-populates on its own).
- If DeepStream sticks below 3/3 past the script's 5-minute wait (seen once, on a stack
  whose DeepStream provisioning was days old): recover via `troubleshooting.md` →
  "DeepStream Stuck at ≤2/3 Active Sources (Zombie Source Bins)"; if sources come up
  DUPLICATED or provisioning behaves erratically after many restart cycles, see
  "Provisioning Chain Polluted" there.

### Confirm the full chain (VST ↔ DeepStream identity)

The FPS gate proves 3 cameras are live, but not that DeepStream is ingesting **the VST
sensors it should**. After churn/re-registration DeepStream can hold a stale uuid's proxy
bin (a zombie) or a duplicate while still reporting FPS. This cross-checks each hop the way
the recovery does: VST must have exactly the 3 expected sensors **online**, and every
DeepStream `camera_id` must map to one of them (no stale/zombie id, no dup).

```bash
python3 - <<'PY'
import urllib.request, json, subprocess, os
HOST = os.environ.get("HOST_IP", "127.0.0.1")
def _get(u): return json.load(urllib.request.urlopen(u, timeout=10))
vst = {s["sensorId"]: s.get("name")
       for s in (lambda d: d if isinstance(d, list) else d.get("sensors", []))(
           _get(f"http://{HOST}:30888/vst/api/v1/sensor/list"))
       if s.get("state") == "online"}
ds = {s["camera_id"]: s["camera_name"]
      for s in _get("http://localhost:9000/api/v1/stream/get-stream-info")["stream-info"]["stream-info"]}
fps = {}
for ln in subprocess.run(["bash", "-c",
        "docker logs --tail 800 vss-rtvi-cv 2>&1 | grep 'stream_name Camera'"],
        capture_output=True, text=True).stdout.splitlines():
    t = ln.split()
    try: fps[t[t.index("stream_name") + 1]] = float(t[0])
    except Exception: pass
EXPECT = {"Camera", "Camera_01", "Camera_02"}
print("VST online :", sorted(vst.values()))
print("DS sources :", sorted(ds.values()), "| fps:", {n: fps.get(n, 0) for n in sorted(set(ds.values()))})
faults = []
if set(vst.values()) != EXPECT: faults.append(f"VST online != {sorted(EXPECT)} (missing/extra sensor)")
if len(ds) != 3:                faults.append(f"DeepStream has {len(ds)} sources, want 3 (dup/zombie)")
stale = [c for c in ds if c not in vst]
if stale:                       faults.append(f"DS holds source(s) not in VST online set (stale/zombie): {stale}")
dead = [n for n in EXPECT if fps.get(n, 0) <= 0]
if dead:                        faults.append(f"cameras at 0 FPS (no data / zombie): {dead}")
print("HEALTHY 3/3" if not faults
      else "UNHEALTHY -> troubleshooting.md 'DeepStream Stuck at <=2/3':\n  - " + "\n  - ".join(faults))
PY
```

`HEALTHY 3/3` = VST has exactly `Camera/Camera_01/Camera_02` online **and** DeepStream is
ingesting exactly those three at FPS > 0. Any `UNHEALTHY` line names the exact hop that
broke (missing VST sensor, wrong source count, a DeepStream id VST doesn't have = zombie,
or a 0-FPS camera) — each routes to the zombie-bin recovery in `troubleshooting.md`.

---

## Scene lifecycle — handoff signals + background completion monitor

The Isaac→VSS handoff logs matching events on **add** (scene start / streaming) and
**remove** (scene stop / teardown). Use them to confirm the scene is running and to
detect when it finishes. All signals below were verified on a live VSS 3.2.1 + Halos SIL run.

| Component | Container | ADD — scene streaming | REMOVE — scene done / teardown |
|-----------|-----------|-----------------------|--------------------------------|
| DeepStream (perception) | `vss-rtvi-cv` | `new stream added [<idx>:<uuid>:<Camera>]` ×3 | `new stream removed [<idx>:...]` + `gstnvtracker: Successfully removed stream <idx>` |
| Isaac RTSP self-hosted (6.0) | `isaac-sim` (kit log) | `[isaacsim.streaming.rtsp.impl.rtsp_writer] RTSP stream started on rtsp://localhost:{8554/camera, 8555/camera_01, 8556/camera_02} (…, encoding=h264)` ×3 (each preceded by `[omni.kit.livestream.rtsp.plugin] Started RTSP server at …`) | no dedicated teardown line (RTSP clients just log `Client disconnected`) — use DeepStream `new stream removed` (row 1) as the authoritative teardown |
| VST sensor mgr | `vss-vios-sensor` | `"change" : "camera_add"` · `addSensor completed: <Camera>` | `"change" : "camera_remove"` · `delete sensor: <uuid>` |

`<Camera>` = `Camera`, `Camera_01`, `Camera_02`.

### Launch a background scene-done monitor

The driver runs until externally stopped. Launch a **detached** monitor (non-blocking —
does not hold the main flow) that announces when the run exits or its streams are torn
down, so you don't have to watch it:

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
# Isaac 6.0 self-hosted RTSP: stream-start lines from the kit log
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
... NVPSB_PSD_CLIENT[34]: ... Data: PSD-Gateway: received DecisionRequest id=1 with 1 events
```
- **EVENT_0 / EVENT_1**: tripwire crossings reported by perception (forklift OUT / IN the trailer). The full ATL event map (`EVENT_0`–`EVENT_5` → forklift/person tripwire + person ROI) is documented in `closed-loop-testing/safety-core/configs/nvpss.conf` (the `bypassFusionEvents` block).
- **DecisionRequest**: the PSF decision-maker is invoked — it produces the corresponding MUTE/UNMUTE command shown in the OPC log above.

---

## View camera streams

`http://<HOST_IP>:30888/vst/` — live Isaac Sim camera feeds with detection overlays.
The forklift's safety disc colour follows MUTE/UNMUTE and is visible in the feeds even
in headless mode.

---

## Verify End-to-End

The system is working when:
1. `vss-rtvi-cv` shows **non-zero** FPS for **all 3** cameras — a source stuck at **0 FPS**
   (`No data from source ... trying reconnection`) is a stale/zombie bin, **not** "ready";
   recover via `troubleshooting.md` → "DeepStream Stuck at ≤2/3 Active Sources"
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
- **`Subscription count: 1`** — one SafetyGraph subscriber per robot with
  `safety_indicator.enabled` in the robots config (expected = the number of such
  robot blocks; **1** with the default `robots.yaml`, **2** with the two-forklift
  variant `robots-2fl.yaml` — the count follows the robots config passed to Isaac,
  not `COMPOSE_PROFILES`). `0` means Isaac isn't subscribed yet (scene
  not fully up, or a `ROS_DOMAIN_ID` mismatch between `isaac-sim` and `comm-layer`);
  a count *below* the expected number means one robot's graph didn't build (check
  the `run_actor_sdg` log).
