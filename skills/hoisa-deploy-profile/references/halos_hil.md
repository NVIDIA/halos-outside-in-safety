# Halos `hil` - Hardware-in-the-Loop across two hosts

The `hil` profile is the `sil` closed loop split across two machines: an **x86 stimulus host** (`COMPOSE_PROFILES=hil`: isaac-sim + comm-layer + forklift-controller - no local safety-core) and an **IGX Thor safety host** (VSS Warehouse perception + the Thor Safety Core). The safety decision drives the simulation, so the loop closes across the network.

```
x86 stimulus host                          IGX Thor safety host
─────────────────                          ────────────────────
isaac-sim (3 cams, RTSP :8554-8556) ──────▶ VST + DS perception (pull the Isaac streams)
forklift-controller ─ROS─▶ isaac-sim              │ BA events → Kafka → Safety Core
comm-layer ◀────────────── UDP :12346 ──── atl_sdm
     └─ROS─▶ /safety/is_muted ─▶ isaac-sim indicator / consumers
```

Only two flows cross hosts: the camera RTSP (Thor pulls from the x86) and the 64-byte safety command UDP (Thor sends to the x86 comm-layer on `COMM_UDP_PORT`, default 12346 - NOT the VST overlay port used by `base`). ROS 2 never crosses hosts; it stays on the x86.

Run the deployment (and this skill) from the **x86 host**; ssh access to the Thor makes the whole flow drivable from one seat, and `hil_preflight.sh` can then check both ends. Both hosts need a clone of this repo.

## 0. Prerequisites

- x86 host: per `prerequisites.md` (GPU for Isaac Sim), plus the NGC artifacts from `ngc_artifacts.md` §1 (sil-data).
- Thor host: IGX Thor on a current GA release with VSS Warehouse 3.2.1 deployable, plus the Safety Core package from `ngc_artifacts.md` §4 (`psf-tegra`).
- Network: the two hosts must reach each other (Isaac RTSP ports 8554-8556 toward the x86; UDP `COMM_UDP_PORT` toward the x86; VST :30888 and perception :9000 toward the Thor for orchestration).

## 1. x86: configure and start the stimulus stack

Fill `deployments/profiles/hil.env` (`HOST_IP` = this x86 host, `PEER_HOST_IP` = the Thor), then:

```bash
bash closed-loop-testing/scripts/hil_preflight.sh deployments/profiles/hil.env --thor <user>@<PEER_HOST_IP>
cd deployments && docker compose --env-file profiles/hil.env up -d
```

Ready when all three services are up (`isaac-sim`, `comm-layer` healthy, `forklift-controller`).

## 2. x86: start the scenario

```bash
docker exec -d isaac-sim bash -lc 'cd /isaac-sim/sil/scripts && ./run_sdg.sh \
  -c /isaac-sim/sil/configs/default_config_ros.yaml --start --headless \
  --cameras-config /isaac-sim/sil/configs/cameras.yaml'
```

Do NOT pass `--enable-vst`: the Thor-side configurator owns sensor registration (one owner only, see `vss_hil_overrides.md` §2). The default `configs/robots.yaml` is single-robot (forklift_b) and matches the stock scene — no `--robots-config` needed. (Two-forklift variant exists for hil too: launch with `-c /isaac-sim/sil/configs/default_config_ros_2fl.yaml --robots-config /isaac-sim/sil/configs/robots-2fl.yaml` + `COMPOSE_PROFILES=hil,multi-robot` — see the toggle notes in `robots.yaml`/`hil.env`. Both 20x20 scenes take `FORKLIFT_WAYPOINTS_DIR=./waypoints/warehouse_20x20`, which is the hil.env default; the waypoint sets are per scene because their coordinates are metres in one warehouse's world frame.) The forklift-controller then drives the forklift in a continuous loop.

Scenario behavior is shared with `sil`: the first run goes quiet for ~5-10 min (scene load + shader compile, cached afterwards), and `--start` runs until externally stopped - details in `test_scenario.md`. Take only the behavior notes from that file; its launch command carries `--enable-vst`, which must not be used in this flow.

Ready when the forklift-controller log advances (`pose=` lines with changing coordinates) and all three streams DELIVER FRAMES - a listening port is not enough, since a cold Isaac accepts RTSP clients before the encoder produces its first frame:

```bash
for s in 8554/camera 8555/camera_01 8556/camera_02; do
  ffprobe -v error -rtsp_transport tcp -count_frames -read_intervals "%+2" \
    -select_streams v:0 -show_entries stream=nb_read_frames -of default=noprint_wrappers=1 \
    "rtsp://localhost:${s%/*}/${s#*/}"
done
```

## 3. Thor: deploy VSS Warehouse for the Isaac source

Only once the streams above deliver frames, deploy VSS Warehouse on the Thor per `vss_hil_overrides.md`, applying all overrides BEFORE the first `up`. That file chains the rest: the 2D or 3D profile overrides (`vss_2d_overrides.md` / `vss_3d_overrides.md`), the `halos_thor.md` §2 IGX-Thor workarounds, sensor registration ownership, and the fresh-state teardown for a previously-used Thor. The order matters: the file-mode sensor registration has no stream-readiness gate, so deploying VSS against a cold Isaac triggers the "no caps" race (`vss_hil_overrides.md` §2; recovery in `troubleshooting.md`, RTSP Streams "no caps").

Ready when the three sensors are `online` in VST pointing at the x86 host's IP, and perception shows `Active sources : 3` with non-zero fps (low fps that tracks the Isaac render rate is normal, not a network fault - `vss_hil_overrides.md` §5).

## 4. Gate: start the Safety Core only on a clear scene

The safety decision logic initializes its internal state from the events it sees at startup. Start it on a clear scene: launching while the forklift is inside the trailer tripwire (or the scene is otherwise mid-action) can initialize it into an inconsistent state, and it should then be restarted at a clear moment. This mirrors the safety concept: a human operator clears the space, then the outside-in system starts.

Practical gate: watch the forklift-controller log for a `FORWARD -> REVERSE` segment transition (the forklift leaving the trailer) and launch the Safety Core within about 10 seconds.

## 5. Thor: launch the Safety Core

Fill `deployments/profiles/hil-thor.env` on the Thor (`PSF_CMD_RX_IP` = the x86 host, `PSF_CMD_RX_PORT` = the x86 `COMM_UDP_PORT`), then, inside a `tmux` session (the launcher runs in the foreground; an ssh drop would tear the Safety Core down):

```bash
tmux new -s safety
bash closed-loop-testing/scripts/launch_thor_safety.sh hil-thor
```

Verify per `halos_thor.md` §5: `nv-psf` up and `atl_sdm` running, then confirm decisions are actually flowing (same section).

## 6. Verify the closed loop

On the x86:

```bash
docker logs --tail 30 comm-layer   # re-run to poll; never a blocking `docker logs -f`
```

Healthy loop: a heartbeat line every ~5 s, `UNMUTE (PREVENT OPERATION)` bursts when a person enters a restricted area, and `MUTE (ALLOW OPERATION)` when the forklift is inside the trailer with the area clear - each followed by a `ROS2: ... is_muted=...` line. Decisions are event-driven: a quiet scene produces only heartbeats. The full `Seq#N | <description> | <symbol>` line format and command meanings are in `test_scenario.md`, Monitor Safety Commands.

```bash
docker exec comm-layer bash -lc "source /opt/ros/jazzy/setup.bash && ros2 topic echo --once /safety/is_muted"
```

The Isaac scene's safety indicator follows `is_muted`, and the VST WebUI on the Thor (`http://<PEER_HOST_IP>:30888/vst/`) shows the live streams for visual confirmation.

## Ready signals (hil)

| Phase | Signal |
|---|---|
| x86 stack | 3 services up; comm-layer healthy; UDP `COMM_UDP_PORT` bound |
| Scenario | All 3 RTSP streams deliver frames (ffprobe, §2); forklift-controller `pose=` lines advancing |
| Thor VSS | 3 sensors `online` in VST with the x86 IP; `Active sources : 3`, fps > 0 |
| Safety Core | `nv-psf` up on the Thor; SDM process present (`atl_sdm`) |
| Closed loop | comm-layer receives heartbeats + decisions; `/safety/is_muted` updates; sim-driven MUTE/UNMUTE transitions observed |

## Restart the scenario (hil)

Do NOT use `restart_isaac.sh` here (single-host: it pauses a local VST container; on
hil the VST runs on the Thor), and never kill/relaunch `run_actor_sdg.py` on the x86
while the Thor VST is running — its clients DESCRIBE the cold streams and wedge them
(`troubleshooting.md`, RTSP Streams "no caps"). Restart across the two hosts:

1. On the **Thor**: `docker stop vss-vios-streamprocessing`.
2. On the **x86**: kill and relaunch the driver with the §2 command (still no `--enable-vst`).
3. Confirm all three streams deliver frames (§2 ffprobe gate — not just a listening port).
4. On the **Thor**: `docker start vss-vios-streamprocessing`, then wait for 3 sensors
   `online` and perception `Active sources : 3`.

If perception does not return, re-add the sensors on the Thor side (the Thor-side
configurator owns registration, `vss_hil_overrides.md` §2), or take VSS down on the
Thor and redo §3's Isaac-warm-before-VSS bring-up order.

## Troubleshooting (hil-specific)

| Symptom | Fix |
|---|---|
| `no caps` / `could not create SDP` on the Isaac streams | Sensors were registered against a cold Isaac (deploy order, §3) - probe and recovery in `troubleshooting.md`, RTSP Streams "no caps" |
| VST sensors `online` but perception 0 fps, no errors | RTSP-over-UDP delivering no media (`vss_2d_overrides.md` VST Config); after an Isaac restart, follow "Restart the scenario (hil)" above - if it recurs after every restart it is stale sensor history on a previously-used Thor (`troubleshooting.md`, Stale Sensor History Replay) |
| Forklift never moves | ROS discovery: all three x86 containers must see each other (`ros2 topic info -v /forklift_b/odom` from the forklift-controller must show a publisher — odom/cmd_vel topics are namespaced per `FORKLIFT_ROBOT_ID` in `hil.env`, i.e. `/<FORKLIFT_ROBOT_ID>/odom`; plain `/odom` only exists if `FORKLIFT_USE_NAMESPACE=false`. On multi-interface hosts LOCALHOST discovery scoping can isolate the containers - use SUBNET). Also confirm `configs/robots.yaml` exists |
| Decisions flow but MUTE never fires | The forklift tripwire events need the forklift actually crossing the trailer tripwire; confirm the forklift moves on camera, then check the Safety Core was started on a clear scene (§4) - restart it at a clear moment otherwise |
| comm-layer receives nothing | UDP path x86:`COMM_UDP_PORT` unreachable from the Thor, or `PSF_CMD_RX_IP/PORT` wrong in `hil-thor.env` |
| Safety Core dies when the ssh session drops | Launch under `tmux`/`setsid` (§5) |

## Stop

```bash
# Thor:
bash closed-loop-testing/scripts/stop_thor_safety.sh
# x86:
cd deployments && docker compose --env-file profiles/hil.env down
bash ../closed-loop-testing/scripts/cleanup_all_datalog.sh hil
```
