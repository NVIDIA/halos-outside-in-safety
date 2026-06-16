---
name: hoisa-deploy-profile
description: >-
  Deploy Halos Outside-In Safety (HOISA) closed-loop testing by PROFILE
  (base / sil / hil) on top of the VSS Warehouse 3.2 perception backend.
  Clone the OSS repo, pick a profile, deploy. Covers prerequisites, NGC
  artifacts (sil-data + PSF + VSS images), VSS SIL overrides, the Halos
  stack (PSF, comm-layer, Isaac Sim, MediaMTX), test scenario, and
  monitoring. Use when asked to deploy Halos, deploy HOISA, deploy SIL,
  set up warehouse safety simulation, or run outside-in safety testing.
metadata:
  author: NVIDIA
  version: 1.0.0
---

# Halos Outside-In Safety — Deploy by Profile (VSS 3.2)

This skill deploys **from the HOISA OSS repository you cloned** — the compose
files, profiles, configs, and this skill all live in the repo. When active,
**ALWAYS read the relevant reference document before running commands.**
Deployment has two stacks that must come up in order — do NOT skip or reorder.

> Targets **VSS Warehouse 3.2**.

---

## Profiles

Pick one profile (set `COMPOSE_PROFILES` in the profile run-env). Services started:

| Profile | Services | Use |
|---------|----------|-----|
| `base` | safety-core | Safety on an existing VSS feed; MUTE/UNMUTE rendered as the VST `halo_safety` overlay ("Standard"/"Efficient Mode" + proximity bubble). No Isaac Sim. |
| `sil` | safety-core, comm-layer, isaac-sim, mediamtx | Full single-host closed loop: Isaac Sim stimulus → VSS perception → PSF → ROS → Isaac forklift. |
| `hil` 🚧 | comm-layer, isaac-sim, mediamtx | 🚧 **Under development.** |

> **2D today. A 3D (Sparse4D) profile is 🚧 under development** — see
> `references/vss_3d_overrides.md` when it lands.

> **`base` on IGX Thor (aarch64)** — the skill detects the platform and, on Thor, asks whether to run the SDM on **CCPLEX** or **FSI** (covered in `references/halos_thor.md`). The CCPLEX path follows the x86 flow; the FSI path is advanced (a one-time firmware reflash).

---

## Architecture (sil profile)

```
Isaac Sim (forklift + digital humans, segments-driven)
     ↓ RTSP (3 cameras, H265, via MediaMTX)
VSS Warehouse 3.2 (AI Perception)
     ├─ vss-rtvi-cv: detection + tracking
     └─ vss-behavior-analytics: ROI / tripwire (forklift-in-trailer)
     ↓ Kafka events (mdx-events topic)
Safety Core (PSF)
     ├─ SEI: multi-camera fusion + event validation
     └─ SDM: ATL decision logic (MUTE/UNMUTE)
     ↓ UDP 64-byte packet (port 12346)
Communication Layer → ROS2 /safety/is_muted → Isaac Sim Action Graph (forklift safety disc)
```

**Two separate stacks**:
- **Stack 1**: VSS Warehouse 3.2 (perception) — deploy via the `vss-deploy-profile` skill (or the public VSS Warehouse docs: github.com/NVIDIA-AI-Blueprints/video-search-and-summarization); must be up + healthy first.
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
| VSS perception | `docker logs vss-rtvi-cv 2>&1 \| grep -c "stream_name Camera"` returns ≥ 3, all non-zero FPS |
| VSS Kafka | `docker exec kafka kafka-console-consumer --bootstrap-server localhost:9092 --topic mdx-events --max-messages 1 --timeout-ms 30000` exits 0 |
| Halos services | The profile's services are all Up (`sil` = 4: safety-core, comm-layer, isaac-sim, mediamtx; `base` = 1) |
| PSF wired | `<sil-data>/comm-layer/opc_server.log` exists and is non-empty |
| **ROS isolation** | `docker exec comm-layer bash -c "source /opt/ros/jazzy/setup.bash && ros2 topic info /safety/is_muted -v"` shows **`Publisher count: 1`** (if 2+, multi-machine `ROS_DOMAIN_ID` collision — see `troubleshooting.md`) |
| Isaac Sim streaming | DeepStream `vss-rtvi-cv` has 3 active Isaac streams: count of `new stream added [` **minus** `new stream removed [` ≥ 3 (net handles re-runs). First-run RT shader compile is slow — gate on this handoff, **not** a shader-log string (absent on Isaac 5.1.0). Poll command in `test_scenario.md`. |
| Isaac Sim wired | `ros2 topic info /safety/is_muted -v` shows **`Subscription count: 1`** (Isaac forklift Action Graph connected) |
| Sim-driven safety | forklift/trailer transitions in `<sil-data>/psf-log/pss.log` **after** Isaac streams came up (not sample-video bootstrap traffic) |

The exact poll command for each signal is in the corresponding reference doc.

---

## Quick Start Workflow

Deploy in strict order. **Stack 1 (VSS) must be running before Stack 2 (Halos).**

```
- [ ] 1. Clone the HOISA OSS repo (this skill lives in skills/)
- [ ] 2. Check prerequisites (host setup overlaps VSS — run VSS prereq first) → references/prerequisites.md
- [ ] 3. Pull NGC artifacts (sil-data + PSF image + VSS images)  → references/ngc_artifacts.md
- [ ] 4. Deploy VSS Warehouse 3.2 via `vss-deploy-profile`,
        apply SIL overrides BEFORE its compose up                → references/vss_2d_overrides.md
- [ ] 5. Poll VSS perception + Kafka ready signals
- [ ] 6. Configure deployments/profiles/<profile>.env, then
        deploy Halos (setup.sh + cleanup + compose up --build)   → references/halos_deploy.md
- [ ] 7. Set a unique ROS_DOMAIN_ID (0-232) + verify Publisher count=1
- [ ] 8. (sil) Run the Isaac Sim test scenario                   → references/test_scenario.md
- [ ] 9. Monitor until sim-driven safety transitions appear (the "complete" signal)
```

---

## Reference Documents

| Document | Use When |
|----------|----------|
| [references/prerequisites.md](references/prerequisites.md) | Hardware/software requirements, Docker, NGC CLI, driver, GPU selection |
| [references/ngc_artifacts.md](references/ngc_artifacts.md) | Pulling sil-data + PSF image + VSS images from NGC; **Thor Safety Core `.deb`s (`psf-tegra` + `psf-tegra-fsi`)** |
| [references/vss_2d_overrides.md](references/vss_2d_overrides.md) | VSS Warehouse 2D `.env`, DeepStream config, and VST config for Isaac Sim |
| [references/vss_3d_overrides.md](references/vss_3d_overrides.md) | 🚧 **Under development** — 3D (Sparse4D) profile overrides |
| [references/halos_deploy.md](references/halos_deploy.md) | Configuring and deploying the Halos stack by profile |
| [references/halos_thor.md](references/halos_thor.md) | Deploying `base` on IGX Thor (aarch64) — SDM on CCPLEX or FSI |
| [references/halos_hil.md](references/halos_hil.md) | 🚧 **Under development** — HIL profile |
| [references/test_scenario.md](references/test_scenario.md) | Running the simulation, monitoring safety commands, viewing camera streams |
| [references/troubleshooting.md](references/troubleshooting.md) | Fixing deployment errors |

---

## Critical Rules

| Tag | Rule | Details in |
|-----|------|-----------|
| `VSS_DEPLOY_PROFILE` | Deploy VSS Warehouse 3.2 with the `vss-deploy-profile` skill; apply SIL overrides before its `docker compose up` | `vss_2d_overrides.md` |
| `VSS_BEFORE_HALOS` | VSS Warehouse **must** be running and healthy before deploying Halos | `vss_2d_overrides.md` |
| `KAFKA_BEFORE_PSF` | Kafka must be up before PSF starts — PSF connects to Kafka on startup | `troubleshooting.md` |
| `DEEPSTREAM_SEI` | **Disable** SEI extraction + enable system timestamps in DeepStream — Isaac Sim RTSP carries no SEI | `vss_2d_overrides.md` |
| `BP_PROFILE_KAFKA` | VSS must use `BP_PROFILE=bp_wh_kafka` (not `bp_wh`) for Halos integration | `vss_2d_overrides.md` |
| `LLM_VLM_NONE` | Set `LLM_MODE=none` and `VLM_MODE=none` — not needed for safety SIL | `vss_2d_overrides.md` |
| `HALO_SAFETY_BASE` | **`base` profile**: to render the safety overlay, set `halo_safety_udp_port` in the VSS 2D `vst_config.json` from `-1` → `12345` (must equal `COMM_UDP_PORT`), then restart VST | `halos_deploy.md`, `vss_2d_overrides.md` |
| `THOR_BASE` | **`base`: detect the platform** (`uname -m`). `x86_64` → standard container base. **`aarch64` (IGX Thor)** → **ask the user** which SDM target before deploying — **CCPLEX** (decision-maker as host software on the Thor application cores; simpler, no firmware change) or **FSI** (decision-maker on the Functional Safety Island, the on-die safety microcontroller; needs a one-time firmware reflash) — then launch via `launch_thor_safety.sh` (hybrid container + host binaries, **not** `docker compose`) | `halos_thor.md` |
| `HOST_IP_BOTH` | `HOST_IP` must be set in **both** the VSS and Halos run-env files | `halos_deploy.md` |
| `SIL_DATA_PATH` | Halos `MDX_DATA_DIR` points to **sil-data** (has `collected-assets/`), not VSS app-data | `halos_deploy.md`, `ngc_artifacts.md` |
| `ROS_DOMAIN_UNIQUE` | Assign a unique `ROS_DOMAIN_ID` (0-232) per machine; verify `Publisher count: 1` on `/safety/is_muted` | `halos_deploy.md`, `troubleshooting.md` |
| `GPU_NOT_PERCEPTION` | Run Isaac Sim on a GPU (RT cores, >20 GB) that is **not** running VSS perception | `prerequisites.md`, `halos_deploy.md` |
| `SETUP_BEFORE_UP` | Run `setup.sh <profile>` and `cleanup_all_datalog.sh <profile>` **before** `docker compose up` (both read `profiles/<profile>.env`) | `halos_deploy.md` |
| `POLL_NEVER_WAIT` | First VSS startup takes 10-15 min (TensorRT build); Isaac shaders ~5-10 min — **poll** the ready signal, never `docker logs -f` or a fixed timer | (this file) |
| `COMPLETE_MEANS_SIM_TRAFFIC` | Do not declare "complete" until **sim-driven** transitions appear in the OPC log — VSS sample-video bootstrap traffic doesn't count | `test_scenario.md` |
| `NEVER_SHOW_KEYS` | **NEVER** echo, print, or hardcode NGC API keys or secrets. Read silently from `~/.bashrc` or `~/.ngc/config` | `prerequisites.md` |

---

## Deployment Stack Summary

| Stack | Services | Source |
|-------|----------|--------|
| VSS Warehouse 3.2 | vss-vios-*, vss-rtvi-cv, vss-behavior-analytics, vss-configurator, kafka, redis, … | `vss-deploy-profile` skill (VSS repo) |
| Halos (profile) | safety-core, comm-layer, isaac-sim, mediamtx | this OSS repo (`deployments/compose.yaml`) + `PSF_IMAGE` from NGC |

---

## Key URLs (after deployment)

| Service | URL |
|---------|-----|
| VST UI (camera streams) | `http://<HOST_IP>:30888/vst/` |
| Grafana (monitoring) | `http://<HOST_IP>:32900` |
| Kibana (analytics) | `http://<HOST_IP>:5601` |

---

## Cleanup

```bash
# Stop Halos (from the repo's deployments/ directory)
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env down
bash ../closed-loop-testing/scripts/cleanup_all_datalog.sh <profile>
docker volume prune -f

# Stop VSS Warehouse — see the vss-deploy-profile skill
```
