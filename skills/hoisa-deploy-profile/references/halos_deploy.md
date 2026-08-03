# Halos Deployment by Profile (Stack 2)

Deploy the Halos stack **after** VSS Warehouse 3.2.1 is up + healthy (Stack 1).
Services and config differ per profile:

| Profile | Services | Notes |
|---------|----------|-------|
| `base` | safety-core | Safety on an existing VSS feed; MUTE/UNMUTE shown as the VST `halo_safety` overlay. No Isaac Sim. |
| `sil` | safety-core, comm-layer, isaac-sim, forklift-controller | Full single-host closed loop. |
| `hil` | comm-layer, isaac-sim, forklift-controller | Two-host closed loop: x86 stimulus + IGX Thor safety host — see `halos_hil.md`. |

---

## 0. Prerequisite — calibration must be safety-ready (2D / 3D · `base` / `sil` / `hil`)

Safety events are driven by the ROIs and tripwires in the VSS perception **`calibration.json`**.
**The shipped sample calibrations already include the required fields** — the 2D sample
(`warehouse-2d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic/calibration.json`)
ships with `restrictedObjectTypes` set, and the 3D calibration built per `calibration_3d.md` includes
it too — so with the **default sample scene there is nothing to change** here.

> This requirement is stated in the [Deployment Guide — Safety Core Prerequisites](https://docs.nvidia.com/halos-outside-in/1.3/deployment/index.html#safety-core-prerequisites)
> (the calibration *"includes `restrictedObjectTypes:["Person"]` for the ROIs, where relevant"*); the
> field itself is defined in the [VSS calibration schema](https://docs.nvidia.com/vss/3.2.1/calibration-schema.html).

This check matters only when you bring your **own calibration** (a custom scene / your own
AMC-generated `calibration.json`, e.g. a partner site). That calibration lives on the VSS side,
**not in this repo**; if it's wrong, perception still detects people / forklifts fine but the Safety
Core sees **no events** — no MUTE/UNMUTE, and **no error is logged** (a silent failure). The current use
case runs the **ATL** app (`--app atl`), whose event map is `event_mapping_atl.pb.txt` (referenced by
`safety-core/configs/nvpss.conf`); the proximity app has its own mapping file. Check these values:

| Check | In `calibration.json` | Expected value | Silent failure if missing / wrong |
|-------|-----------------------|----------------|-----------------------------------|
| Restricted class per ROI | `rois[].restrictedObjectTypes` | non-empty, e.g. `["Person"]` | person entering the ROI is **never reported to the SDM** (most common) |
| ROI id ↔ rule | `rois[].id` ↔ `rule_id` in `event_mapping_atl.pb.txt` | must match (e.g. `roi-id-1`) | ROI violation fires but maps to no `EVENT_4` / `EVENT_5` |
| Tripwire id ↔ rule | `tripwires[].id` ↔ `rule_id` in `event_mapping_atl.pb.txt` | must match (e.g. `tripwire-id-1`) | forklift / person tripwire maps to no `EVENT_0`–`EVENT_3` |

(`confinedObjectTypes`, e.g. `["Forklift"]`, is set the same way per ROI — see the ROI schema in `calibration_3d.md`.)

Using a custom calibration? validate it before deploying (the shipped sample passes as-is):

```bash
CALIB=<wh_ops>/warehouse-2d-app/calibration/sample-data/<your-scene>/calibration.json
python3 - "$CALIB" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
rois, tws = c.get("rois", []), c.get("tripwires", [])
print("ROI ids:", [r.get("id") for r in rois], "| tripwire ids:", [t.get("id") for t in tws])
missing = [r.get("id") for r in rois if not r.get("restrictedObjectTypes")]
if missing:
    sys.exit(f"ERROR: ROIs with no restrictedObjectTypes — person-in-ROI will NOT reach the SDM: {missing}")
print("OK: every ROI restricts at least one object type")
PY
```

Then confirm the printed `id`s match the `rule_id`s in `event_mapping_atl.pb.txt`.

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
docker ps --format 'table {{.Names}}\t{{.Status}}' | grep -E "safety-core|comm-layer|isaac-sim|forklift-controller"
# base = safety-core ; sil = + comm-layer + isaac-sim + forklift-controller (4 total)
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

---

## Reset to a fresh deployment (keep the caches)

A first bring-up is the cleanest state the stack ever has: an empty VST sensor
registry and an empty event backlog. Heavy restart/re-registration churn slowly
pollutes the **state layers** — `removed`-tombstone entries accumulate in the VST
registry, and the sensor lifecycle events pile up in the Redis stream that
`sdr-controller` replays in full whenever it restarts. When a box misbehaves in
ways the targeted recoveries in `troubleshooting.md` don't resolve, reset those
state layers and redeploy instead of debugging further — it reproduces the
proven-clean first bring-up **without** paying the expensive first-run costs.

What to clear vs. what to keep:

| | Contains | Action |
|---|---|---|
| VST postgres volume | sensor registry (uuids, tombstones, ghosts) | **remove** |
| Redis state | sensor lifecycle event backlog + SDR workload cache | **clear** (`FLUSHALL`, step 3) |
| TensorRT engine / model store | built perception engine | **KEEP** (rebuild costs 10-20 min) |
| `isaac-cache/` host dirs | compiled RT shaders | **KEEP** (recompile costs ~10 min) |
| Images, `sil-data/` | pulled images, scene assets | KEEP (unaffected) |

Procedure (≈10-15 minutes end-to-end):

```bash
# 1. Stop the scenario, then the Halos stack
docker exec isaac-sim bash -c 'pgrep -f run_actor_sdg.py | xargs -r kill -9'
cd <repo>/deployments && docker compose --env-file profiles/<profile>.env down
bash ../closed-loop-testing/scripts/cleanup_all_datalog.sh <profile>

# 2. Tear down VSS *with* its state volumes — but selectively.
#    Follow the vss-deploy-profile skill's teardown reference for the compose
#    project specifics; the state volume to remove is the VST postgres data
#    volume (docker volume ls | grep -iE 'pg|postgres').
#    Do NOT blanket `down -v`: that also removes the TensorRT engine volume.

# 3. Redeploy in the standard order: VSS Warehouse first (engine and caches are
#    reused, so this pass is fast), then clear redis (below), then the Halos
#    stack, then launch the scenario. VST starts empty -> the driver registers
#    the cameras at render-warm exactly as on a first bring-up.
```

Clear redis after the VSS redeploy, before relaunching the scenario (works whether
redis persists to a named volume or a host bind mount):

```bash
docker exec redis redis-cli FLUSHALL
docker restart sdr-controller
docker restart vss-rtvi-cv
```

Scope guidance: this is the third tier of recovery. Routine restarts use
`restart_isaac.sh` (~2 min, `test_scenario.md`); polluted provisioning uses the
Redis-flush runbook (~3 min, `troubleshooting.md` → "Provisioning Chain
Polluted"); reset + redeploy is for when the state itself is suspect.
