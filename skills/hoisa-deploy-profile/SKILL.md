---
name: hoisa-deploy-profile
description: >-
  Deploy Halos Outside-In Safety (HOISA) closed-loop testing by PROFILE
  (base / sil / hil) on top of the VSS Warehouse 3.3 perception backend.
  Clone the OSS repo, pick a profile, deploy. Covers prerequisites, NGC
  artifacts (sil-data + PSF + VSS images), VSS SIL overrides, the Halos
  stack (PSF, comm-layer, Isaac Sim, forklift-controller), test scenario, and
  monitoring. Use when asked to deploy Halos, deploy HOISA, deploy SIL,
  set up warehouse safety simulation, or run outside-in safety testing.
metadata:
  author: NVIDIA
  version: 1.3.0
---

# Halos Outside-In Safety — Deploy by Profile (VSS 3.3)

This skill deploys **from the HOISA OSS repository you cloned** — the compose
files, profiles, configs, and this skill all live in the repo. When active,
**ALWAYS read the relevant reference document before running commands.**
Deployment has two stacks that must come up in order — do NOT skip or reorder.

> Targets **VSS Warehouse 3.3** (code-freeze drop `dev-26.10.2`). The Thor profiles still run 3.2.1 (`references/halos_thor.md`).

---

## Profiles

Pick one profile (set `COMPOSE_PROFILES` in the profile run-env). Services started:

| Profile | Services | Use |
|---------|----------|-----|
| `base` | safety-core | Safety on an existing VSS feed; MUTE/UNMUTE rendered as the VST `halo_safety` overlay ("Standard"/"Efficient Mode" + proximity bubble). No Isaac Sim. |
| `sil` | safety-core, comm-layer, isaac-sim, forklift-controller (+ forklift-controller-b2 with the opt-in `multi-robot` profile — two-forklift toggle in `sil.env`/`robots.yaml`) | Full single-host closed loop: Isaac Sim stimulus → VSS perception → PSF → ROS → Isaac forklift(s). |
| `hil` | comm-layer, isaac-sim, forklift-controller | Two-host closed loop: this x86 stimulus stack + an IGX Thor safety host running VSS perception and the Thor Safety Core — `references/halos_hil.md`. |

> **The `sil` profile runs against either a 2D or a 3D perception backend.** 2D
> (RT-DETR detect + track) is the default — `references/vss_2d_overrides.md`. For **3D
> (Sparse4D multi-view + BEV tracking)** — better small / occluded object detection —
> apply `references/vss_3d_overrides.md` instead (VSS ships the Sparse4D model and the
> loading-dock BEV calibration; `references/calibration_3d.md` is for a scene of your own).
> The Halos stack (safety-core, comm-layer, isaac-sim, forklift-controller, forklift-controller-b2) is **identical**
> for 2D and 3D — including the Isaac Sim launch (`--enable-vst`, see `references/test_scenario.md`);
> only the VSS perception side changes, plus the two PSF 3D
> lines in the profile, which every `PSF_APP` needs on a 3D feed (`references/halos_deploy.md`).

> **`base` on IGX Thor (aarch64)** — the skill detects the platform; on Thor the SDM runs on the **CCPLEX** application cores and the stack is launched by `launch_thor_safety.sh` instead of `docker compose` (covered in `references/halos_thor.md`).

---

## Architecture (sil profile)

```
Isaac Sim (forklift + digital humans, segments-driven)
     ↓ RTSP (3 cameras, H264, self-hosted per-camera 8554/8555/8556)
VSS Warehouse 3.3 (AI Perception)
     ├─ vss-rtvi-cv: detection + tracking
     └─ vss-behavior-analytics: ROI / tripwire (forklift-in-trailer)
     ↓ Kafka events (mdx-events topic)
Safety Core (PSF)
     ├─ SEI: multi-camera fusion + event validation
     └─ SDM: ATL decision logic (MUTE/UNMUTE)
     ↓ UDP 64-byte packet (port 12346)
Communication Layer → ROS2 /safety/is_muted → Isaac Sim Action Graph (forklift safety disc)
```

`PSF_APP` picks the decision-maker: `atl` (above, default), `pxc` (proximity: NORMAL /
REDUCE / STOP for a forklift next to a person, on `/safety/proximity/*`; the
forklift-controller slows and stops the truck) or `both` (side by side in one container).
Settings and the extra override file are in `references/halos_deploy.md` → "Safety app".

**Two separate stacks**:
- **Stack 1**: VSS Warehouse 3.3 (perception) — the VSS repo's `vss-build-vision-ai` skill, warehouse profile, with the SIL overrides of `references/vss_2d_overrides.md` (3D: `references/vss_3d_overrides.md`); must be up + healthy first.
- **Stack 2**: Halos (`base`/`sil`/`hil`) — this skill.

---

## Agent Autonomy Rules

When invoked autonomously (no human in the loop):

1. **Proceed through all phases without asking for confirmation** at phase
   boundaries. Only stop on a real error or a missing input that the user
   must provide (e.g., NGC API key not present anywhere).
2. **Use the Ready Signals table below as phase gates**, not narrative time
   estimates ("wait ~15 min") and not process existence (`pgrep -f "compose up"`).
   `compose up --detach` exits before containers are actually serving.
3. **Never use `docker logs -f`** or any blocking follow command in the main
   flow. Use a poll loop with a non-blocking grep instead.
4. **Pre-pull large images in parallel** with the VSS Warehouse build: the PSF
   image (`PSF_IMAGE`) and the Isaac Sim base (`ISAAC_SIM_IMAGE`), both from the
   profile run-env (~30 GB total), to save wall time.
5. **Emit a heartbeat line every poll iteration** so the user can see the agent
   is alive during long phases (TensorRT build, Isaac Sim shader compile).
6. **Do NOT declare "deployment complete"** until the final ready signal fires:
   sim-driven safety transitions in `<sil-data>/comm-layer/opc_server.log`.
   "All containers Up" is not the same as "system working".

---

## Ready Signals

Each phase ends when its ready signal becomes true. Poll, don't wait by time.

| Phase | Ready signal (poll until true) |
|-------|--------------------------------|
| Prerequisites | All checks in `prerequisites.md` Summary checklist pass |
| NGC artifacts | sil-data extracted (`collected-assets/` present) + `PSF_IMAGE` pulled + VSS images present |
| VSS perception | DeepStream shows **3 distinct cameras at current FPS > 0** (use the FPS-based poll in `test_scenario.md`; a plain `grep -c "stream_name Camera"` count **accumulates** across the log and false-passes on a 0-FPS zombie) |
| VSS Kafka | `docker exec kafka kafka-console-consumer --bootstrap-server localhost:9092 --topic mdx-events --max-messages 1 --timeout-ms 30000` exits 0 |
| Halos services | The profile's services are all Up (`sil` = 4: safety-core, comm-layer, isaac-sim, forklift-controller — 5 with the opt-in `multi-robot` profile [+ forklift-controller-b2]; `base` = 1) |
| PSF wired | `<sil-data>/comm-layer/opc_server.log` exists and is non-empty |
| **ROS isolation** | `docker exec comm-layer bash -c "source /opt/ros/jazzy/setup.bash && ros2 topic info /safety/is_muted -v"` shows **`Publisher count: 1`** (if 2+, multi-machine `ROS_DOMAIN_ID` collision — see `troubleshooting.md`) |
| Isaac Sim streaming | Isaac's 3 self-hosted RTSP streams live **and** DeepStream ingested them (the Isaac→VSS handoff). Gate on the handoff, **not** a shader-log string; a bare "3 active streams" can false-positive on VSS bootstrap — bootstrap-immune poll commands in `test_scenario.md`. |
| Isaac Sim wired | `ros2 topic info /safety/is_muted -v` shows **`Subscription count:`** = the number of robots with `safety_indicator.enabled` in the robots config — **1** with the default `robots.yaml`. `0` = Isaac not wired yet. **`robots-40x20.yaml` is the exception**: its indicators subscribe to per-robot `/<name>/safety/is_muted` topics, so the global topic correctly shows `0` there — check `ros2 topic info /forklift_b/safety/is_muted` instead (one publisher, one subscriber each). **Under `PSF_APP=pxc`** the disks subscribe to `/<robot>/proximity/state` instead, so `/safety/is_muted` correctly shows `0`: check `ros2 topic info /forklift_b/proximity/state -v` — 1 publisher (the controller), 1 subscriber (Isaac). `both` subscribes to both |
| Sim-driven safety | forklift/trailer transitions in `<sil-data>/psf-log/pss.log` **after** Isaac streams came up (not sample-video bootstrap traffic). Isaac has replaced the bootstrap sensors once its kit log says `Registered 3/3 camera(s) with VST` |
| Proximity (`pxc` / `both`) | `EVENT_12/13/14` in `pss.log`, `Proximity:` lines in `opc_server.log`, and `PXC [...]` level changes in `docker logs forklift-controller` — `test_scenario.md` → "Proximity" |

The exact poll command for each signal is in the corresponding reference doc.

---

## Quick Start Workflow

Deploy in strict order. **Stack 1 (VSS) must be running before Stack 2 (Halos).**

```
- [ ] 1. Clone the HOISA OSS repo (this skill lives in skills/)
- [ ] 2. Check prerequisites (host setup overlaps VSS — run VSS prereq first) → references/prerequisites.md
- [ ] 3. Pull NGC artifacts (sil-data + PSF image + VSS images)  → references/ngc_artifacts.md
- [ ] 4. Deploy VSS Warehouse 3.3 (`vss-build-vision-ai`, warehouse),
        apply SIL overrides BEFORE its compose up                → references/vss_2d_overrides.md
                                                                   (3D: vss_3d_overrides.md)
- [ ] 5. Poll VSS perception + Kafka ready signals
- [ ] 6. Configure deployments/profiles/<profile>.env, then
        deploy Halos (setup.sh + cleanup + compose up --build)   → references/halos_deploy.md
- [ ] 7. Set a unique ROS_DOMAIN_ID (0-232) + verify Publisher count=1
- [ ] 8. (sil, multi-robot) Cross-check the four robot lists BEFORE booting Isaac:
        deployments/scripts/preflight.py --robots-config <the robots.yaml you will launch with>
                                        [--scene <only for a fleet with a truck baked into the scene>]
- [ ] 9. (sil) Run the Isaac Sim test scenario                   → references/test_scenario.md
- [ ] 10. Monitor until sim-driven safety transitions appear (the "complete" signal)
```

Step 8 exists because a robot has to be listed in three places — `robots.yaml`, a controller service, and its waypoint file — and **every way of getting it half-right is silent**. Two of the three produce the same symptom, a truck that sits there. The check takes a second and runs on the host; Isaac takes three minutes to boot before showing you any of it. Pass the config you will actually launch with — it has no default on purpose.

It also compares each waypoint file's `origin` against where its truck stands, which is a different kind of mistake: the names all agree, the truck drives, and only the lane is wrong. Waypoints are per scene (`waypoints/<map id>/<ROBOT_ID>.json`, the map chosen by `SCENARIO`) because the coordinates are metres in one warehouse's world frame. The scene<->config<->waypoints<->calibration pairings are tabulated in `references/test_scenario.md`; the 40x20 scene has no published calibration and is experimental.

---

## Reference Documents

| Document | Use When |
|----------|----------|
| [references/prerequisites.md](references/prerequisites.md) | Hardware/software requirements, Docker, NGC CLI, driver, GPU selection |
| [references/ngc_artifacts.md](references/ngc_artifacts.md) | Pulling sil-data + PSF image + VSS images from NGC; **Thor Safety Core `.deb` (`psf-tegra`)** |
| [references/vss_2d_overrides.md](references/vss_2d_overrides.md) | VSS Warehouse 2D env (the `vss-build-vision-ai` answers), DeepStream config, and VST config for Isaac Sim |
| [references/vss_3d_overrides.md](references/vss_3d_overrides.md) | **3D (Sparse4D) profile** — VSS env, DeepStream + `config.yaml`, behavior-analytics proximity, VST overrides |
| [references/calibration_3d.md](references/calibration_3d.md) | **3D, a scene of your own** — the 3-camera BEV calibration (`group` / `rois` / `tripwires`); the loading dock ships with VSS |
| [references/halos_deploy.md](references/halos_deploy.md) | Configuring and deploying the Halos stack by profile |
| [references/halos_thor.md](references/halos_thor.md) | Deploying `base` on IGX Thor (aarch64) — the hybrid container + host-binary Safety Core |
| [references/vss_hil_overrides.md](references/vss_hil_overrides.md) | **HIL** — VSS deltas for a remote Isaac source: stream URLs, sensor-registration ownership, fresh state |
| [references/halos_hil.md](references/halos_hil.md) | **HIL profile** — the two-host closed loop runbook (x86 stimulus + IGX Thor safety host) |
| [references/test_scenario.md](references/test_scenario.md) | Running the simulation, monitoring safety commands, viewing camera streams |
| [references/troubleshooting.md](references/troubleshooting.md) | Fixing deployment errors |

---

## Critical Rules

| Tag | Rule | Details in |
|-----|------|-----------|
| `VSS_DEPLOY` | Deploy VSS Warehouse 3.3 with the VSS repo's `vss-build-vision-ai` skill (warehouse profile); apply SIL overrides before its `docker compose up` | `vss_2d_overrides.md` |
| `VSS_IMAGE_TAG` | Pick the container tag `nightly-20261010` (the images of `dev-26.10.2`) and export `VSS_CONTAINER_TAG` before the skill's `config` step; the default `develop-latest` follows VSS `develop`, not the tag | `vss_2d_overrides.md` |
| `VSS_SIL_ENV` | The SIL answers go in the build's `_builds/<name>/override.env`: `bp_wh_kafka`, size Minimal, the 3-camera loading-dock dataset with `NUM_STREAMS=3` | `vss_2d_overrides.md`, `vss_3d_overrides.md` |
| `PSF_3D_FEED` | On a 3D feed set `PSF_NVPSS_CONFIG_SRC` + `PSF_SENSOR_PIPELINES_CONFIG_SRC` for **every** `PSF_APP`, `atl` included — the 2D config drops every event | `halos_deploy.md` |
| `BA_PROXIMITY` | **`pxc` / `both`:** set behavior analytics `proximityDetectionThreshold` to `4` and add `Forklift` to `proximityDetectionCenterClasses` — shipped values give no forklift proximity | `vss_3d_overrides.md` |
| `VSS_BEFORE_HALOS` | **`base`/`sil` only:** VSS Warehouse must be running and healthy before deploying Halos. `hil` inverts this — bring Isaac up and confirm the streams deliver frames first, then deploy VSS on the Thor, or the Thor registers sensors against a cold Isaac and hits the "no caps" race | `vss_2d_overrides.md`, `halos_hil.md` §3 |
| `KAFKA_BEFORE_PSF` | Kafka must be up before PSF starts — PSF connects to Kafka on startup | `troubleshooting.md` |
| `DEEPSTREAM_SEI` | **Disable** SEI extraction + use system timestamps (`attach-sys-ts-as-ntp=1`) in DeepStream. Isaac 6.1 embeds SEI, but **PSF doesn't support sim time** — using it drops events as STALE, so key off system (wall-clock) time | `vss_2d_overrides.md`, `vss_3d_overrides.md` |
| `BP_PROFILE_KAFKA` | VSS must use `BP_PROFILE=bp_wh_kafka` (not `bp_wh`) for Halos integration | `vss_2d_overrides.md` |
| `LLM_VLM_NONE` | Set `LLM_MODE=none` and `VLM_MODE=none` — not needed for safety SIL | `vss_2d_overrides.md` |
| `HALO_SAFETY_BASE` | **`base` profile**: to render the safety overlay, set `halo_safety_udp_port` in the VSS 2D `vst_config.json` from `-1` → `12345` (must equal `COMM_UDP_PORT`), then restart VST | `halos_deploy.md`, `vss_2d_overrides.md` |
| `THOR_BASE` | **`base`: detect the platform** (`uname -m`). `x86_64` → standard container base. **`aarch64` (IGX Thor)** → the decision-maker runs as host software on the Thor application cores (**CCPLEX**); launch via `launch_thor_safety.sh` (hybrid container + host binaries, **not** `docker compose`) | `halos_thor.md` |
| `HOST_IP_BOTH` | `HOST_IP` must be set in **both** the VSS and Halos run-env files | `halos_deploy.md` |
| `SIL_DATA_PATH` | Halos `MDX_DATA_DIR` points to **sil-data** (has `collected-assets/`), not VSS app-data | `halos_deploy.md`, `ngc_artifacts.md` |
| `ROS_DOMAIN_UNIQUE` | Assign a unique `ROS_DOMAIN_ID` (0-232) per machine; verify `Publisher count: 1` on `/safety/is_muted` | `halos_deploy.md`, `troubleshooting.md` |
| `GPU_NOT_PERCEPTION` | Run Isaac Sim on a GPU (RT cores, >20 GB) that is **not** running VSS perception | `prerequisites.md`, `halos_deploy.md` |
| `SETUP_BEFORE_UP` | Run `setup.sh <profile>` and `cleanup_all_datalog.sh <profile>` **before** `docker compose up` (both read `profiles/<profile>.env`) | `halos_deploy.md` |
| `POLL_NEVER_WAIT` | First VSS startup builds the TensorRT engine (several minutes); Isaac shaders ~5-10 min — **poll** the ready signal, never `docker logs -f` or a fixed timer | (this file) |
| `COMPLETE_MEANS_SIM_TRAFFIC` | Do not declare "complete" until **sim-driven** transitions appear in the OPC log — VSS sample-video bootstrap traffic doesn't count | `test_scenario.md` |
| `NEVER_SHOW_KEYS` | **NEVER** echo, print, or hardcode NGC API keys or secrets. Read silently from `~/.bashrc` or `~/.ngc/config` | `prerequisites.md` |

---

## Deployment Stack Summary

| Stack | Services | Source |
|-------|----------|--------|
| VSS Warehouse 3.3 | vss-vios-*, vss-rtvi-cv, vss-behavior-analytics, vss-configurator, kafka, redis, … | VSS repo at `dev-26.10.2`, `vss-build-vision-ai` skill (warehouse profile) |
| Halos (profile) | safety-core, comm-layer, isaac-sim, forklift-controller | this OSS repo (`deployments/compose.yaml`) + `PSF_IMAGE` from NGC |

---

## Key URLs (after deployment)

| Service | URL |
|---------|-----|
| VST UI (camera streams) | `http://<HOST_IP>:30888/vst/` |
| Grafana (monitoring) | `http://<HOST_IP>:35000` — extended profile only |
| Kibana (analytics) | `http://<HOST_IP>:5601` — extended profile only |

> **Grafana/Kibana only exist in the extended profile.** The `_MINIMAL` `COMPOSE_PROFILES`
> lists used for SIL exclude them, so neither URL is reachable. Only VST UI is always up.

---

## Cleanup

```bash
# Stop Halos (from the repo's deployments/ directory)
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env down
bash ../closed-loop-testing/scripts/cleanup_all_datalog.sh <profile>
docker volume prune -f

# Stop VSS Warehouse — the vss-build-vision-ai skill's teardown (references/teardown.md)
```

> ⚠️ `docker volume prune -f` deletes **every** dangling volume on the host. Safe while VSS
> is still running (its volumes are in-use), but if the VSS stack is already down it also
> deletes the VSS sensor database (`vss_vios_pg_data`). **Ask the user before running it**;
> to clear only Halos state, skip the prune — the `down` + datalog cleanup above already
> remove the per-run state.
