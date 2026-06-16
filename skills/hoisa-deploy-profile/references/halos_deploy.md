# Halos Deployment by Profile (Stack 2)

Deploy the Halos stack **after** VSS Warehouse 3.2 is up + healthy (Stack 1).
Services and config differ per profile:

| Profile | Services | Notes |
|---------|----------|-------|
| `base` | safety-core | Safety on an existing VSS feed; MUTE/UNMUTE shown as the VST `halo_safety` overlay. No Isaac Sim. |
| `sil` | safety-core, comm-layer, isaac-sim, mediamtx | Full single-host closed loop. |
| `hil` 🚧 | comm-layer, isaac-sim, mediamtx | 🚧 Under development — see `halos_hil.md`. |

---

## 1. Configure the profile env

Edit the profile env **in your clone** at `deployments/profiles/<profile>.env` and
fill the `# change me` placeholders (keep your filled copy local — don't commit it):

| Variable | Value | Notes |
|----------|-------|-------|
| `HOST_IP` | this host's IP | must match the VSS `.env` `HOST_IP` |
| `MDX_SAMPLE_APPS_DIR` | absolute path to the cloned repo | e.g. `$HOME/halos-outside-in-safety` |
| `MDX_DATA_DIR` | the **sil-data** dir (contains `collected-assets/`) | NOT the VSS app-data dir — see `ngc_artifacts.md` |
| `DOCKER_GID` | run `getent group docker \| cut -d: -f3` (default `999` may not match this host) | the `safety-core` container mounts `docker.sock`, so it needs the host's docker group |
| `ISAAC_GPU_DEVICE` | a GPU with RT cores + >20 GB **not** running VSS perception | see GPU selection below |
| `ROS_DOMAIN_ID` | a **unique** number per machine (0-232) | prevents cross-machine `/safety/is_muted` collisions — verify `Publisher count: 1` after deploy |

`PSF_IMAGE` and `ISAAC_SIM_IMAGE` are pre-set in the template.

### GPU selection (`sil` / `hil`)

Isaac Sim needs a GPU with **RT cores** and **> 20 GB VRAM**:
```bash
nvidia-smi --query-gpu=index,name,memory.used,memory.total --format=csv,noheader
```
- **2+ GPUs**: set `ISAAC_GPU_DEVICE` to a GPU that is NOT running VSS perception (usually not GPU 0).
- **1 GPU**: it must have RT cores + enough free VRAM for both perception and Isaac.

---

## 2. Deploy

```bash
cd <repo>/deployments

# Create data dirs + log files for this profile
../closed-loop-testing/scripts/setup.sh <profile>

# Clean previous run logs (same profile)
../closed-loop-testing/scripts/cleanup_all_datalog.sh <profile>

# Start — --build because comm-layer / isaac-sim build from local Dockerfiles
docker compose --env-file profiles/<profile>.env up -d --build
```

---

## 3. Verify

Poll until the profile's services are Up (first run builds local images — takes minutes):

```bash
docker ps --format 'table {{.Names}}\t{{.Status}}' | grep -E "safety-core|comm-layer|isaac-sim|mediamtx"
# base = safety-core ; sil = + comm-layer + isaac-sim + mediamtx (4 total)
```

### Safety overlay (`base`) — enable on the VSS side

`base` has no comm-layer / ROS; the safety decision is rendered as the VST `halo_safety`
overlay, which ships **disabled**. Enable it once in the VSS Warehouse 2D VST config
`<wh_ops>/warehouse-2d-app/vst/configs/vst_config.json`:

```jsonc
"halo_safety_udp_port": 12345   // ships as -1 (disabled); must equal COMM_UDP_PORT in base.env
```

PSF sends its 64-byte command to `127.0.0.1:${COMM_UDP_PORT}` (`12345`); VST listens on
`halo_safety_udp_port` — the two **must match**. Restart VST after editing, then the overlay
shows on the video: "Standard Mode" (MUTE) / "Efficient Mode" (UNMUTE) + the Forklift
proximity bubble. (`<wh_ops>` = the VSS `warehouse-operations` dir — see `vss_2d_overrides.md`.)

> Overlay never appears? The port is still `-1` or doesn't match `COMM_UDP_PORT`. PSF is
> already deciding — the result just isn't rendered until the port is wired.

### PSF wired to comm-layer (`sil` / `hil`)
```bash
until [ -s "$MDX_DATA_DIR/comm-layer/opc_server.log" ]; do sleep 5; done
echo "PSF → comm-layer wired"
```

### ROS isolation (`sil` / `hil`) — MUST be 1
```bash
docker exec comm-layer bash -c \
  "source /opt/ros/jazzy/setup.bash && ros2 topic info /safety/is_muted -v" | grep "Publisher count"
# Publisher count: 1 expected. If 2+, another machine shares your ROS_DOMAIN_ID — see troubleshooting.md.
```

---

## Expected log noise (ignore — not real failures)

| Source | Message | Why it's noise |
|--------|---------|----------------|
| `safety-core` | `Failed to process reported SafetyEvent` | events from VSS sample video before Isaac streams start; stops once Isaac is live |
| `safety-core` | `CONFWARN: retry.backoff.ms ... ignored by this consumer` | librdkafka warning; harmless |

---

## Startup times

| Component | First run | Subsequent |
|-----------|-----------|------------|
| Isaac Sim | 10-15 min (scene load + RT shader compile) | 3-5 min (shaders cached) |
| PSF | ~30 s | ~30 s |
| comm-layer | ~10 s | ~10 s |

Track Isaac startup (scene load + RT shader compile on first run):
```bash
docker exec isaac-sim sh -c 'tail -5 /isaac-sim/kit/logs/Kit/*/*/kit_*.log'
```
Don't gate on a shader-log string — readiness = the Isaac→VSS stream handoff completing,
polled via DeepStream net active streams (`vss-rtvi-cv`). See `test_scenario.md`.
