# Phase 2/3 — Per-Scenario Launch

Run for EACH scenario in the plan. **Restart both Halos and SRR compose between scenarios** — Safety Core counter drift carries across runs and corrupts the next scenario's results.

> ⚠️ **Resolve the perception + behavior container names first (VSS 3.2.1).** Every command
> below that says `vss-rtvi-cv` / `vss-behavior-analytics` / `FPS ≥ 5` refers to the
> resolved container. On VSS Warehouse 3.2.1 perception is **`vss-rtvi-cv`** (Sparse4D 3D,
> FPS ~12–14 → use **≥ 5**) and behavior analytics is **`vss-behavior-analytics`** — the
> names no longer carry a `-2d`/`-3d` suffix. Resolve once at the top of the run and substitute:
> ```bash
> PERCEPTION=$(docker ps --format '{{.Names}}' | grep -E '^(vss-rtvi-cv|perception-[23]d)$' | head -1)
> BEHAVIOR=$(docker ps --format '{{.Names}}' | grep -E '^(vss-behavior-analytics|vss-behavior-analytics-[23]d)$' | head -1)
> case "$PERCEPTION" in vss-rtvi-cv|*-3d) FPS_MIN=5;; *) FPS_MIN=25;; esac
> ```
> (See SKILL.md § "Agent autonomy rules" for the same note.) Phase 2 metrics require
> `mdx-bev` + `mdx-behavior` to be flowing (Sparse4D warehouse) — present on VSS 3.2.1.

---

## Step 3.−2 — SRR image freshness check (one-time / first scenario)

The `srr` Docker image bakes `/app/srr/*.py` from `srr-service/srr/` **and** the
post-process renderers `/app/scripts/*` from `scripts/` at build time (the build
context is the **regression-reporter dir**, not `srr-service/`, so `scripts/` is in
scope; `matplotlib` + `protobuf>=5.27.3` are installed for them). If the host source
has been updated since the image was last built, the container runs **stale code** —
common symptoms:

- `clip_logs.py` missing entirely → Phase 6a `docker exec` fails
- `vst_video.py` writes per-clip MP4s to `videos/scenes/scn_*.mp4` (old layout) while host source writes flat `videos/scn_*.mp4` (new layout) — breaks aggregator's `../videos/<scn>.mp4` link in per-clip MD reports
- Phase 2 perception metrics absent / wrong if `aggregator.py`, `kafka_consumer.py`, `schema.py`, or `recorder.py` predate the 3D pipeline (fd101f2)
- `render_perception_heatmap.py` / `render_coverage_polygons.py` missing or pre-`--density` → Phase 6d heatmap step fails at import or rejects the flag

**Detect**:

```bash
SRR_SRC_DIR="${SRR_PIPELINE_DIR}/srr-service/srr"
for f in clip_logs.py aggregator.py kafka_consumer.py schema.py recorder.py utils/vst_video.py tw_split.py; do
  HOST_MTIME=$(stat -c '%Y' "$SRR_SRC_DIR/$f" 2>/dev/null || echo 0)
  CTR_MTIME=$(docker exec srr stat -c '%Y' "/app/srr/$f" 2>/dev/null || echo 0)
  if (( HOST_MTIME > CTR_MTIME )); then
    echo "STALE: srr/$f (host $HOST_MTIME > container $CTR_MTIME)"
  fi
done
# Render scripts (baked at /app/scripts/ since 2026; only needed for Phase 6d heatmaps)
for f in render_perception_heatmap.py render_coverage_polygons.py; do
  HOST_MTIME=$(stat -c '%Y' "${SRR_PIPELINE_DIR}/scripts/$f" 2>/dev/null || echo 0)
  CTR_MTIME=$(docker exec srr stat -c '%Y' "/app/scripts/$f" 2>/dev/null || echo 0)
  if (( HOST_MTIME > CTR_MTIME )); then
    echo "STALE: scripts/$f (host $HOST_MTIME > container $CTR_MTIME)"
  fi
done
```

**Recover** — TWO options, prefer rebuild for any non-trivial drift:

Option A — **Rebuild image** (canonical, picks up everything):
```bash
cd "${SRR_PIPELINE_DIR}/srr-service"
docker compose build srr
docker compose up -d srr
```
~2-3 min. Permanent fix until next host-source change.

Option B — **Hot-patch via `docker cp`** (quick, ephemeral — lost on next `docker compose up --build`):
```bash
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/clip_logs.py"        srr:/app/srr/
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/aggregator.py"       srr:/app/srr/
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/kafka_consumer.py"   srr:/app/srr/   # Phase 2 ingestion
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/schema.py"           srr:/app/srr/   # Phase 2 parquet cols
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/recorder.py"         srr:/app/srr/   # Phase 2 parquet cols
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/utils/vst_video.py"  srr:/app/srr/utils/
docker cp "${SRR_PIPELINE_DIR}/srr-service/srr/tw_split.py"         srr:/app/srr/
docker cp "${SRR_PIPELINE_DIR}/scripts/render_perception_heatmap.py" srr:/app/scripts/   # Phase 6d
docker cp "${SRR_PIPELINE_DIR}/scripts/render_coverage_polygons.py"  srr:/app/scripts/   # Phase 6d
```
Use when rebuild fails (e.g. NGC cache cold) or when iterating during dev. **Note:**
`kafka_consumer.py` / `schema.py` / `recorder.py` only take effect on a **fresh
`/srr/record`** (the service process must restart to re-open the Kafka consumers and
write the new parquet columns) — `docker compose restart srr` after the `cp`. The
aggregator / render scripts read existing parquets so they apply immediately.

After either option, re-run the detect block and confirm no STALE lines.

Log:
```
  [MM:SS] SRR image freshness ok (or rebuilt / hot-patched: <list>).
```

Skip this step on the 2nd–Nth scenarios of a multi-test — image state is stable for the run.

---

## Step 3.−1 — Verify SRR fixtures synced into the Isaac SIL dir (one-time / first scenario)

SRR's canonical test fixtures (the 6 scenarios' IRA 1.6 behavior trees,
NavMesh JSON, and the Script-Editor utilities) live under
`regression-reporter/scenarios/{behavior-trees,scenes,isaac-scripts}/`. Isaac Sim reads
those at the in-container path `/isaac-sim/sil/...`, which is bind-mounted
from the Isaac SIL tree `${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil` (GitHub layout:
`<halos-repo>/closed-loop-testing/isaac-sim/sil/`). SRR ships no custom scene —
it runs against the stock scene already present in that tree.

So before any scenario can start, the SRR fixtures must be **copied into
the SIL configs dir**. This is a one-time setup per checkout (or after
SRR-side updates); not per scenario.

**Verify all fixture groups present** — Explore agent:

```
Check each path exists under ${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/:
  configs/srr_char0.bt.json                (active tree — Character)
  configs/srr_char1.bt.json                (active tree — Character_01)
  configs/srr_char2.bt.json                (active tree — Character_02)
  configs/srr_in-roi_char0.bt.json         (per-scenario trees; 6 scenarios x 3 chars)
  configs/srr_fast_char0.bt.json           (spot-check a couple more names)
  configs/navmesh.json
  scripts/isaac/check_srr_prereqs.py       (GT pre-flight diagnostic)
Report [ok] if all present, or [missing: <list>] otherwise.
```

**On missing files** — run the sync once:

```bash
${SRR_PIPELINE_DIR}/halos-integration/sync_to_halos.sh ${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil
```

This copies the canonical IRA 1.6 behavior trees plus navmesh /
isaac-scripts into the correct SIL subdirs. Idempotent — safe to re-run. See
`halos-integration/README.md` for the Pattern A (sync) vs Pattern B
(planned bind-mount) discussion.

Log:
```
  [MM:SS] SRR fixtures synced into halos compose.
```

Skip this step on the 2nd–Nth scenarios in a multi-test — once synced, the
files are stable across compose restarts.

---

## Step 3.0 — Fresh-start data cleanup (FIRST scenario only, optional)

If the user wants a clean slate (e.g. first run after a code change, or recovering from a previously-aborted multi-test), run hoisa-deploy-profile's `cleanup_all_datalog.sh` ONCE **before the first scenario's compose restart**:

```bash
cd ${HOISA_ROOT_PATH}/deployments
docker compose --env-file ${HOISA_ROOT_PATH}/deployments/profiles/sil.env down     # ensure Safety Core isn't writing
bash ${HOISA_ROOT_PATH}/deployments/../closed-loop-testing/scripts/cleanup_all_datalog.sh   # truncates pss.log + wipes comm-layer/
```

What it does (the script lives at `<halos-repo>/closed-loop-testing/scripts/cleanup_all_datalog.sh`, ~95 lines):
- Truncates `${MDX_DATA_DIR}/psf-log/pss.log` to 0 bytes
- Removes everything under `${MDX_DATA_DIR}/comm-layer/`
- Removes auxiliary files in `psf-log/` (keeps the dir + pss.log file inode)

**NEVER run between scenarios** — it destroys the previous scenario's Safety Core log evidence (the source the Phase 4 Step 1b per-scn snapshot reads). The Safety Core container's own startup truncates pss.log naturally, so per-scenario freshness is already handled.

When to skip Step 3.0: continuing a multi-test partial-run, or after `hoisa-deploy-profile` finished within the past few minutes (the deploy itself does this cleanup as part of `SETUP_BEFORE_UP`).

Log:
```
  [MM:SS] pre-run data cleanup done (one-time).
```

---

## Step 3a — Halos compose restart

```bash
cd ${HOISA_ROOT_PATH}/deployments
docker compose --env-file ${HOISA_ROOT_PATH}/deployments/profiles/sil.env down
docker compose --env-file ${HOISA_ROOT_PATH}/deployments/profiles/sil.env up -d
```

> **Do NOT call `cleanup_all_datalog.sh` here.** Per hoisa-deploy-profile `SETUP_BEFORE_UP`, that script truncates `psf-log/pss.log` and wipes `comm-layer/` — running it between scenarios destroys the per-scn pss.log evidence captured in the prior scenario's Phase 4 Step 1b. Fresh-start cleanup belongs in Phase 2 pre-check (one-time before the first scenario only). The Safety Core container's own startup truncates pss.log naturally, so between-scenarios state is already fresh on the Safety Core side.

**Demo log line at start**:
```
  Test N/M starts: <name> (<min> min)
  [00:00] Halos compose restarting...
```

**Ready signal** — verify via Explore agent:
```
Check `docker ps --format '{{.Names}}'` until all names are listed:
  safety-core, comm-layer, isaac-sim
Poll every 30 s, max 5 min total. Print one heartbeat line per poll
("[MM:SS] waiting on <missing-name>"). Report [ok] when all 4 are up,
or [fail: <reason>] if 5 min elapses with services missing.
```

When agent reports [ok], log:
```
  [MM:SS] Halos containers up.
```

---

## Step 3a.1 — Restart perception (`vss-rtvi-cv`) to clear stale RTSP sources

After every Halos compose cycle, the perception container (`$PERCEPTION` —
`vss-rtvi-cv` on VSS 3.2.1; legacy `perception-2d`/`-3d`) must be restarted to
drop RTSP source URLs left over from the previous scenario's deleted
`nvstreamer-default` cams. Skipping this is a silent failure:
`docker logs "$PERCEPTION"` shows pipeline running, but FPS reports 0.0 and
no BA events flow. Symptom matches a ghost result (BA TW IN 0/N) but root
cause is upstream of BA — perception is feeding empty frames.

The compose stack does have a tear-down script at
`${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/scripts/run_actor_sdg.py` (GitHub layout:
`closed-loop-testing/isaac-sim/sil/scripts/run_actor_sdg.py`) that removes the
sensor entries from VST cleanly, but it only runs on a graceful Isaac
shutdown. Stopping Isaac early (e.g. multi-test killing the scenario after
10–20 min) leaves the stale entries — restarting `$PERCEPTION` is the
reliable workaround.

```bash
docker restart "$PERCEPTION"   # vss-rtvi-cv (VSS 3.2.1; legacy perception-2d/-3d)
```

**Ready signal** — verify via Explore agent:
```
Wait 30 s after restart, then run:
  docker logs --tail 200 "$PERCEPTION" 2>&1 | grep PERF -A1 | tail -5
Parse the 3 FPS values from the most recent PERF block. ALL three must be ≥ $FPS_MIN
(≥25 on 2D, ≥5 on 3D Sparse4D). Report [ok] when seen, or [fail: <fps values>] if any reads 0.0.
```

Log:
```
  [MM:SS] $PERCEPTION restarted, FPS recovered.
```

---

## Step 3a.5 — VST API gate (fail-fast on stale state)

Two cheap inline checks that catch the most common ways a previous deploy
leaves VST in a state that silently breaks the run. Both run in < 2 s.

**Check 1 — record API health** (envoy upstream alive):

```bash
curl -sf -o /dev/null -w '%{http_code}\n' "${VST_BASE_URL}/v1/record/streams"
```

Must return `200`. If `503` / `504` → launch_vst record subsystem hung.
Run the [VSS reset recipe](../SKILL.md#when-vss-is-corrupted-sensoradd-400-record-api-503)
and retry. For full VSS Warehouse redeploy (when the local reset recipe doesn't recover), use the **`vss-deploy-profile` skill** teardown/redeploy path (`references/teardown.md`, then the Deployment Flow) — it owns the proper teardown / data-volume wipe / bring-up for VSS, and that procedure is mandated (don't reinvent it here).

**Check 2 — sensor list cleanliness** (no leftover Camera entries):

```bash
curl -sf "${VST_BASE_URL}/v1/sensor/list" \
  | jq '[.[] | select(.state == "online") | select(.name | test("^Camera(_0[12])?$"))] | length'
```

Must return `≤ 3`. If `> 3` → stale `Camera` / `Camera_01` / `Camera_02`
sensors are still registered as `online` from a previous run. Isaac will
get HTTP 400 on `sensor/add`. DELETE them before continuing:

```bash
for SID in $(curl -sf "${VST_BASE_URL}/v1/sensor/list" \
              | jq -r '.[] | select(.state=="online") | select(.name | test("^Camera(_0[12])?$")) | .sensorId'); do
  curl -sf -X DELETE "${VST_BASE_URL}/v1/sensor/${SID}"
done
```

**On either fail**: do NOT proceed to Step 3b. Surface the failure to
the user, suggest the appropriate recipe, then stop.

Log on success:
```
  [MM:SS] VST API gate ok (record API 200, ≤3 online Camera sensors).
```

---

## Step 3b — SRR compose restart

```bash
cd ${SRR_SERVICE_DIR}
docker compose down
docker compose up -d
```

**Ready signal** — verify via Explore agent:
```
Check `docker ps --format '{{.Names}}'` shows srr container.
Then verify SRR can reach ROS by:
  docker exec srr ros2 topic list 2>&1 | grep -c '/safety/is_muted'
Should return ≥ 1 (topic exists, even if no publisher yet).
Poll every 15 s, max 90 s. Report [ok] or [fail].
```

Log:
```
  [MM:SS] SRR container up, ROS env ready.
```

---

## Step 3c — Select the scenario's behavior trees (IRA 1.6)

Isaac Sim 6.0 / IRA 1.6 removed character command files — each pedestrian is a
behavior tree. `default_config_ros.yaml` references three FIXED tree names
(`srr_char{0,1,2}.bt.json`, one per `character.groups` entry). To select a
scenario we copy that scenario's three trees onto the fixed names (no YAML edit
needed for the character binding):

```bash
CFG_DIR=${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/configs
NAME=$1   # one of: in-roi, psf-edge, psf-clear, balanced, fast, fixed
for i in 0 1 2; do
  cp -f "${CFG_DIR}/srr_${NAME}_char${i}.bt.json" "${CFG_DIR}/srr_char${i}.bt.json"
done
```

The per-scenario trees are emitted directly by
`scenarios/tools/randomize_paths.py` (canonical source:
`scenarios/behavior-trees/`) and synced by `sync_to_halos.sh`.

**Verify** — the three active trees match the selected scenario:
```bash
for i in 0 1 2; do
  diff -q "${CFG_DIR}/srr_char${i}.bt.json" "${CFG_DIR}/srr_${NAME}_char${i}.bt.json" \
    || { echo "ABORT: srr_char${i}.bt.json != ${NAME}"; exit 3; }
done
```

If a scenario tree is missing, re-run
`randomize_paths.py --name <scenario> ...` then `sync_to_halos.sh`.

---

## Step 3c.1 — Set simulation_duration to cover the scenario

IRA 1.6 uses `simulation_duration` (seconds), replacing the old
`simulation_length` (frames). Upstream ships a short value that silently kills
Isaac mid-scenario — symptom: scene exits early, `isaac=3` drops to 0 partway
through the recording window, parquet ends short.

Set it to outlast scene-load + Safety Core warm-up + the recording window (buffer):
```bash
CONFIG=${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/configs/default_config_ros.yaml
RECORD_S=$2                       # this scenario's recording seconds
SIM_DUR=$(( RECORD_S + 30 + 600 ))   # + Safety Core warm-up + scene-load/margin buffer
sed -i "s|^\(\s*\)simulation_duration:.*|\1simulation_duration: ${SIM_DUR}.0|" "$CONFIG"
```

**Verify**:
```bash
grep -n simulation_duration "$CONFIG"
# Expected: <line>:    simulation_duration: <SIM_DUR>.0
```

If the value didn't change, abort — the sed didn't match. (`run_multi.sh`'s
`phase_set_behavior_tree` performs both this and Step 3c automatically.)

---

## Step 3d — Start scenario in isaac-sim (+ wire /gt via `--srr-gt`)

```bash
docker exec -d isaac-sim ./python.sh \
  /isaac-sim/sil/scripts/run_actor_sdg.py \
  -c /isaac-sim/sil/configs/default_config_ros.yaml \
  --start --headless --enable-vst \
  --cameras-config /isaac-sim/sil/configs/cameras.yaml \
  --srr-gt \
  > /tmp/isaac-scenario-${TIMESTAMP}-${LABEL}.log 2>&1
```

**`--srr-gt` wires the SRR ground-truth publishers at run time.** It's an opt-in
flag on `run_actor_sdg.py` (default OFF, so normal runs are unaffected). When set,
`run_actor_sdg.py` calls `action_graphs/srr_ground_truth.py`'s `build_srr_gt_graph()`
in its post-setup block — AFTER IRA has spawned the SRR char groups — which builds
`/World/SRRGraph` (the `/gt/character_{0,1,2}/tf` + `/gt/forklift/tf` publishers) by
discovering each char's animated `ManRoot` and pumping their live Fabric transforms
each frame. There is **no** "paste + Save the graph into the scene" step — IRA 6.0
spawns chars at runtime so the targets only exist live. Expect these lines in the
scenario log:
```
[srr-gt] armed; will build /World/SRRGraph once SRR chars spawn
[srr-gt] Action Graph built at /World/SRRGraph (4 publishers)
[srr-gt] SRRGraph built (SRR chars detected on stage)
```

**Note**: use `docker exec -d` (detached) — the script runs the scenario for the full RECORD_S duration.

Log:
```
  [MM:SS] Scene loading...
```

---

## Step 3e — Wait for scene loaded + 3 RTSP streams

**Ready signal** — verify via Explore agent:
```
Tail /tmp/isaac-scenario-<TIMESTAMP>-<LABEL>.log inside the host
(it's a docker exec stdout redirect — should appear on host).
Look for all 3 lines:
  RTSPWriter_World_Cameras_Camera_rgb
  RTSPWriter_World_Cameras_Camera_01_rgb
  RTSPWriter_World_Cameras_Camera_02_rgb

If first run on this scene: shaders compile takes ~5-7 min — that's normal.
If subsequent run: streams should appear within ~90 s.

Poll every 30 s. Report progress with last 2 lines of the log.
Max 8 min on first run, 3 min on subsequent.

Report [ok] when all 3 RTSP lines present, or [fail: <reason>] otherwise.
```

When [ok], log:
```
  [MM:SS] Scene loaded. Replicator generating data.
```

---

## Step 3f — Launch live clip monitor (background)

Background process to detect each forklift TW crossing and emit demo log lines.

```bash
nohup python3 ${SRR_PIPELINE_DIR}/scripts/live_clip_monitor.py \
  --tw-x 9.574 \
  --label "${LABEL}" \
  > /tmp/live-clip-${TIMESTAMP}-${LABEL}.log 2>&1 &
LIVE_PID=$!
```

The monitor subscribes to `/gt/forklift/tf` (BEST_EFFORT QoS) and prints one line on each high-edge / low-edge of `forklift_x` vs `TW_X`. Stream its stdout into the demo log.

---

## Step 3f.5 — Safety Core cold-start warm-up (IMPORTANT)

**Safety Core container starts fast, but its safety state machine takes ~30-60 s to fully settle** after first /safety/is_muted publish. If you start `/srr/record true` immediately after containers go Up, the **first clip (scn_0000) will show a cold-start outlier** — typical mute_lag spikes to 5-10+ s and match% drops to 70-80% in scn_0000 even when the rest of the run is clean.

Concrete evidence: in the multi-test-20260430-081223 dataset, `in-roi-5min` scn_0000 had mute_lag = 9133 ms vs subsequent clips at 0 ms. Watching `scn_0000.mp4` shows the unmute lag visually — that's the symptom.

**Workaround**: after containers Up + scene streams Ready, add an extra 30-60 s "warm-up sleep" before `/srr/record true`. Or, if accepting the outlier, just note it in the report and move on.

**Recommended**: between Step 3e (scene streams ready) and Step 3g (`/srr/record true`), insert:

```bash
echo "  [$(date +%M:%S)] Safety Core warm-up (30 s)..."
sleep 30
```

This trades 30 s of wall clock per scenario for cleaner scn_0000 results.

---

## Step 3f.7 — VSS perception health gate (CRITICAL)

After Safety Core warm-up, before opening record, verify VSS perception is actually
producing events. Without this gate, a broken VSS pipeline (FPS=0, empty
Kafka) records 5+ minutes of meaningless data — see ghost-result issue
documented in `06_interpret_report.md`.

Borrows the ready signals defined in
[`hoisa-deploy-profile/references/vss_2d_overrides.md`](../../hoisa-deploy-profile/references/vss_2d_overrides.md).

**Verify via Explore agent — all 3 conditions must pass:**

```
(resolve container + threshold first, see top-of-file note:
   PERCEPTION=$(docker ps --format '{{.Names}}' | grep -E '^(vss-rtvi-cv|perception-[23]d)$' | head -1)
   case "$PERCEPTION" in vss-rtvi-cv|*-3d) FPS_MIN=5;; *) FPS_MIN=25;; esac )

1. perception stream registration:
   docker logs "$PERCEPTION" 2>&1 | grep -c 'stream_name Camera'
   Must return ≥ 3.

2. perception FPS on all 3 cameras (not 0.0):
   docker logs --tail 200 "$PERCEPTION" 2>&1 | grep PERF -A1 | tail -5
   Parse the 3 FPS values from the most recent PERF block. ALL three must
   be ≥ $FPS_MIN (≥25 on 2D, ≥5 on 3D Sparse4D). If any reads 0.00000 → perception is dead, abort.

3. Kafka mdx-events has events flowing (bounded consume probe — break on first msg):
   docker exec srr python3 -c "
   from kafka import KafkaConsumer
   c = KafkaConsumer('mdx-events', bootstrap_servers='${HOST_IP}:9092',
                     consumer_timeout_ms=6000, auto_offset_reset='latest')
   n = 0
   for _ in c:
       n += 1
       if n >= 1: break
   c.close()
   print('events:', n)
   "
   Must print events: ≥ 1. ⚠️ mdx-events flows CONTINUOUSLY, so the probe MUST
   break early (the `consumer_timeout_ms` is only an idle fallback for a dead
   topic). A bare `sum(1 for _ in c)` NEVER returns on a live topic — it only
   stops after a full idle gap — and it leaks an in-container consumer you then
   have to kill. This mirrors `run_multi.sh` `phase_wait_scene_ready`.

4. (Phase 2 only — skip if mdx-bev not in use) mdx-bev decodes REAL detections:
   docker exec srr python3 -c "
   from kafka import KafkaConsumer
   from srr.kafka_consumer import parse_mdx_bev_bytes
   c = KafkaConsumer('mdx-bev', bootstrap_servers='localhost:9092',
                     auto_offset_reset='latest', value_deserializer=None,
                     consumer_timeout_ms=8000)
   n = 0
   for m in c:
       if (parse_mdx_bev_bytes(m.value).get('detections') or []): n += 1
       if n >= 1: break
   c.close()
   print('bev_frames_with_detections:', n)
   "
   Must print ≥ 1. (run_multi's scene-ready gate requires 2 consecutive non-empty
   polls — fresh after a compose restart the 3D scene takes ~3 min to load and
   perception emits EMPTY mdx-bev frames until then, so a bare "has any message"
   check false-positives. Phase 2 metrics will be absent if this never goes > 0.)

Report [ok] only if the applicable conditions pass, or [fail: which check failed]
with concrete numbers (e.g. "FPS values: 0.0/0.0/0.0").
```

**On fail**: do NOT call `/srr/record true`. Apply the staged recovery below — escalate only if the cheap step doesn't fix it. This pattern is common: a scenario hits FPS=0 right after a compose restart, and a single `docker restart "$PERCEPTION"` + re-poll recovers without user intervention.

> **`SENSOR_INFO_SOURCE=file` RTSP race (Isaac 6.0 + VSS 3.2.1).** When VSS reads
> sensor info from the static file that points at Isaac's live RTSP endpoints,
> VST spams reconnect attempts at those endpoints *while Isaac's stream still
> "has no caps"* — Isaac then fails to create the SDP (`could not create SDP`),
> the RTSP writer drops into a failed state, and perception sits at **0 active
> sources** (empty parquet) even though the renderer is busy. Confirmed fix on
> two machines: let Isaac bring up its RTSP server **first**, then restart the
> VST/vios ingestion stack + the perception container so they reconnect to
> streams that already have caps:
> ```bash
> # after Isaac's RTSP ports (8554-8556) are listening:
> docker restart vss-vios-sensor vss-vios-streamprocessing vss-vios-ingress
> sleep 12
> docker restart vss-rtvi-cv
> ```
> `scripts/run_multi.sh` bakes this in as `phase_vst_workaround` (runs
> automatically between scene-start and the scene-ready gate; set
> `SKIP_VST_WORKAROUND=1` to bypass on nvstreamer-source deployments).

**Recovery ladder for FPS=0 (try in order, each takes ≤ 90 s):**

```bash
# Tier 1 — perception restart (covers ~80% of fresh-restart FPS=0 cases)
docker restart "$PERCEPTION"            # vss-rtvi-cv (VSS 3.2.1; legacy perception-2d/-3d)
sleep 30
docker logs --tail 200 "$PERCEPTION" 2>&1 | grep PERF -A1 | tail -5
# If all 3 FPS values now ≥ $FPS_MIN → re-run the Step 3f.7 gate and continue.

# Tier 2 — perception restart + isaac-sim scene restart
# (use if Tier 1 still shows FPS=0 — the RTSP streams may have flap-disconnected)
docker restart isaac-sim "$PERCEPTION"
sleep 60
# Then re-execute Step 3e (wait for shaders + 3 RTSP streams ready)
# and re-poll the 3f.7 gate.

# Tier 3 — full VSS reset recipe (SKILL.md § "When VSS is corrupted")
#   or vss-deploy-profile skill teardown/redeploy (references/teardown.md)
# Use only after Tier 1 + 2 fail — this rebuilds VSS state and takes 5-10 min.
```

For empty Kafka (FPS ok but no `mdx-events`):
- Check `docker logs "$BEHAVIOR"` (`vss-behavior-analytics`; legacy `-2d`/`-3d`) for exceptions
- perception may be running but BA may be crash-looping → restart `bp-configurator-*` + `"$BEHAVIOR"`, then re-poll.

**Cancel any `ScheduleWakeup` set during this gate** before continuing. Stale wakeups can fire during Phase 6 (post-process) and confuse the agent — always cancel on probe pass or recovery success.

Log on success:
```
  [MM:SS] VSS perception ok (3 streams, FPS ≥ 5, Kafka flowing).
```

---

## Step 3g — Start SRR recording

```bash
docker exec srr ros2 service call /srr/record std_srvs/srv/SetBool '{data: true}'
```

**Ready signal** (cheap, inline check):
```bash
docker exec srr ros2 service call /srr/record std_srvs/srv/SetBool '{data: true}' \
  | grep -q 'success=True'
```

Log:
```
  [MM:SS] /srr/record true → recording active.
```

---

## Step 3h — Recording window

```bash
sleep ${RECORD_S}
```

**During this sleep** the live_clip_monitor prints its lines. Periodically (every 60 s) print a heartbeat:

```
  [MM:SS] Recording... (T+<elapsed>/<RECORD_S>s)
```

To avoid blocking on a single long sleep, structure as:
```bash
ELAPSED=0
while [ $ELAPSED -lt $RECORD_S ]; do
  STEP=$((RECORD_S - ELAPSED < 60 ? RECORD_S - ELAPSED : 60))
  sleep $STEP
  ELAPSED=$((ELAPSED + STEP))
  printf '  [%s] Recording... (T+%ds / %ds)\n' "$(date +%M:%S)" "$ELAPSED" "$RECORD_S"
done
```

---

## Step 3i — Stop SRR recording

```bash
docker exec srr ros2 service call /srr/record std_srvs/srv/SetBool '{data: false}'
```

Log:
```
  [MM:SS] /srr/record false → recording stopped.
```

> The service response message `'stopped, N rows written'` reports the
> **last batch flush count**, not the total rows in the parquet. On a
> 5-minute recording at 30 Hz the message often shows ~30–50 while the
> actual parquet has ~9000 rows. Verify the total via:
> ```bash
> docker exec srr python3 -c "
> import pandas as pd, glob
> p = sorted(glob.glob('/app/runs/run-*.parquet'))[-1]
> print('rows:', len(pd.read_parquet(p)))"
> ```
> Expect `rows ≈ RECORD_S × 30`. Mismatch → recording was interrupted.

---

## Step 3j — Kill live clip monitor

```bash
kill $LIVE_PID 2>/dev/null
wait $LIVE_PID 2>/dev/null
```

Then proceed to Phase 4 (analyze).
