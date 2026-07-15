---
name: hoisa-generate-regression-report
description: >-
  Run the SRR (Safety Regression Reporter) multi-scenario test pipeline on the
  SIL stack. Picks one or more of 6 pre-built test cases (in-roi, psf-edge,
  psf-clear, balanced, fast, fixed), launches each in Isaac Sim with a clean
  compose restart, records 30 Hz parquet of GT + Safety Core state + BA / 3D
  perception Kafka events, splits by forklift tripwire crossings, and runs the
  aggregator to produce per-clip match% + Phase 2 perception reports, per-clip
  MP4 videos, and optional heatmaps. Use when asked to "run SRR multi-test",
  "regression-test Safety Core", "produce SRR report on these scenarios", "score
  perception detection/tracking", or "demo SRR pipeline".
metadata:
  author: NVIDIA
  version: 1.3.0
---

# SRR Skill — Multi-scenario Test Pipeline

When this skill is active:
1. **ALWAYS read the relevant reference doc** in [references/](references/) before running shell commands for that phase.
2. **Use Explore agents to verify ready signals** instead of fixed `sleep` timers. Spawn one short-lived agent per phase boundary check (compose-up, scene-load, recording-active, analysis-done). This avoids long blocking waits and lets the agent diagnose if a check fails.
3. **Print demo-friendly log lines** at every phase boundary (format spec below).
4. **Stop on real errors** — never silently retry. Surface the failure with a 1-line diagnosis from the verifying agent.
5. **Cancel `ScheduleWakeup` calls when their trigger condition is resolved.** If you set a wakeup to retry a probe (e.g. Kafka mdx-events consume in Step 3f.7) and the probe passes or you recover via another path, **explicitly cancel the wakeup** before moving on. Stale wakeups can fire during Phase 6 (post-process) and confuse the agent into re-running probes against a torn-down state. One wakeup outstanding per probe maximum.

This skill assumes the SIL + SRR stacks are already deployed and healthy on the host (see `hoisa-deploy-profile` skill for first-time setup), **and SRR fixtures (behavior trees, navmesh, Script-Editor utilities) have been synced into the Isaac SIL dir** via [`halos-integration/sync_to_halos.sh`](../../closed-loop-testing/regression-reporter/halos-integration/sync_to_halos.sh). The skill's launch phase verifies this and offers to sync if missing — see [references/02_launch.md](references/02_launch.md) Step 3.−1.

---

## Phase 0 — Required reading (ONE-TIME, before any compose work)

SRR runs **on top of** the SIL + VSS Warehouse stacks. Their lifecycle (clean-data-before-up, ready signals, redeploy procedure) is owned by the **`hoisa-deploy-profile`** and **`vss-deploy-profile`** skills — NOT this skill. Before any `docker compose down/up` in Phase 3a or 3b, the agent MUST have read the relevant procedure from those skills, otherwise it will skip mandatory data cleanup and miss ready-signal gates.

**Read these BEFORE running any compose command:**

| File | Why |
|---|---|
| `skills/hoisa-deploy-profile/SKILL.md` § Cleanup, § Ready Signals, § Critical Rules (`SETUP_BEFORE_UP`, `VSS_BEFORE_HALOS`, `POLL_NEVER_WAIT`) | Defines `cleanup_all_datalog.sh` mandate before every `docker compose up`, the `sil`-profile ready-signal set (safety-core, comm-layer, isaac-sim), VSS-must-come-up-first ordering |
| `skills/hoisa-deploy-profile/references/troubleshooting.md` | Failure recipes for "compose up exits but services missing", safety-core stuck, comm-layer dead. Consult before retrying a failed restart |
| `vss-deploy-profile` skill (Deployment Flow / Tear Down + `references/teardown.md`, `references/warehouse.md`, `references/warehouse-debug.md`) | VSS Warehouse teardown + data-volume wipe, deploy/bring-up flow, and perception ready signals (FPS check on `vss-rtvi-cv`). Needed when VSS is unhealthy and SRR's launch HARD GATE (3a.5 or 3f.7) fails |

**Quickly internalize:**

1. **`SETUP_BEFORE_UP`** — `cleanup_all_datalog.sh` is a one-time fresh start before the FIRST scenario only (truncates `psf-log/pss.log` + wipes `comm-layer/`). **Never run it between scenarios** — it destroys the per-scn pss.log evidence. The per-scenario compose restart already truncates pss.log via Safety Core init.
2. **`VSS_BEFORE_HALOS`** — when restarting both stacks (e.g. VSS recovery from a 3a.5 gate fail): tear down Halos first, then VSS; bring up VSS first (wait for FPS green), then Halos. Only on VSS-side incident recovery, not per-scenario.
3. **`POLL_NEVER_WAIT`** — `compose up --detach` exits before containers serve. Always poll `docker ps` + a service-specific ready check; never assume "returned 0 → ready".

If a compose restart fails during Phase 3a/3b, **stop and read the two troubleshooting docs above** before retrying. Do not just `docker compose up` again.

---

## 6 pre-built test cases

All 6 use the stock scene `indicator_warehouse_20x20_layout_overflow_test.usd` and `--agent-radius 0.8`.

| Name | Length | Stress focus |
|---|---:|---|
| **in-roi**    |  5 min | Person stays inside work zone constantly (Safety Core "person constantly present" stress) |
| **psf-edge**  |  5 min | Person sits ON ROI boundary — tests hysteresis / debounce |
| **psf-clear** |  5 min | Person clearly inside or clearly outside (clean A/B partner of `psf-edge`) |
| **balanced**  | 10 min | Mixed activity, baseline pre-redraw scenario |
| **fast**      | 20 min | Low idle, motion-heavy — tests cumulative drift over long run |
| **fixed**     |  5 min | Deterministic baseline — hand-authored waypoints (not randomized), person always in ROI. Reproducible regression anchor; use for unmute-direction checks, not mute% (mute untestable — always-present person). Waypoints NavMesh-validated at radius 0.8. |

> **`all` and `full` run the first 5 (the randomized sweep) only.** `fixed` is
> **opt-in** — select it explicitly (`../scripts/run_multi.sh fixed`, or name it
> in the prompt). It's excluded from `all`/`full` because the person is always in
> ROI (mute untestable → inflates the blended headline).

Scenario waypoints — IRA 1.6 behavior trees (Isaac 6.0 dropped command files):
- Canonical trees: `../scenarios/behavior-trees/srr_{name}_char{0,1,2}.bt.json`
  (emitted directly by `../scenarios/tools/randomize_paths.py`)
- Synced (Isaac reads from here): `${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/configs/srr_char{0,1,2}.bt.json`
  (the active scenario; `run_multi.sh` swaps the selected scenario onto these
  fixed names. GitHub layout: `<halos-repo>/closed-loop-testing/isaac-sim/sil/configs/`)

The canonical scene + NavMesh live at `../scenarios/scenes/`. To add or
re-generate scenarios, run `../scenarios/tools/randomize_paths.py --name <new-name>`
(writes `../scenarios/behavior-trees/srr_<new-name>_char{0,1,2}.bt.json`), then
re-run `../halos-integration/sync_to_halos.sh` to push into the Isaac SIL tree.

**Default recording duration: 5 minutes** for any scenario unless user overrides. The "Length" column above shows each scenario's *natural* length, but 5 min is the default for demo / regression — enough to surface most issues without burning wall clock.

**User override syntax** (parse natural language):
- `"in-roi"`                → 5 min (default)
- `"balanced for 10 min"`   → 10 min
- `"fast at 3 minutes"`     → 3 min
- `"all at 5 min"`          → 5 min for each of the 5 sweep scenarios (not `fixed`)

When invoking `../scripts/run_multi.sh`, encode override as `name:seconds`:
```
../scripts/run_multi.sh in-roi:300 balanced:600 fast:180
```

User can request any subset, or `all` for the full multi-test (still 5 min each unless overridden).

---

## Architecture (one scenario)

```
[Isaac Sim] -- /gt/character_*/tf, /gt/forklift/tf -----> [SRR rclpy]  (GT, the reference)
            -- 3 RTSP cams (Isaac self-hosted) -> [VSS] --+--> Kafka mdx-events    --> [SRR consumer]  BA ROI/TW events
                                                  +--> Kafka mdx-bev       --> [SRR consumer]  3D detections (Sparse4D bbox3d)
                                                  +--> Kafka mdx-behavior  --> [SRR consumer]  BA track positions
                                                         [Safety Core]
                                                          ↓ /safety/{is_muted,command,is_alarm}
                                                          ↓
                                                       [SRR rclpy subscribers]
                                                          ↓ 30 Hz parquet (Phase 1 + Phase 2 columns)
                                  /srr/record SetBool true|false (start/stop)
```

**Two scoring layers in one recording:**
- **Phase 1 (Safety Core decision)** — GT-expected-mute vs actual `is_muted` → `match%`, mute/unmute-correct%, reaction lag, BA ROI/TW event detection.
- **Phase 2 (perception, 3D)** — GT TF vs `mdx-bev` 3D detections + `mdx-behavior` positions → per-class **detect-fail%** / **tracking-loss%**, precision/recall/F1, position offset-vs-jitter, id-switches, coverage. See [references/06_interpret_report.md](references/06_interpret_report.md) (formula source of truth: `srr-service/srr/aggregator.py` → `compute_phase2_metrics`).

> **Phase 2 depends on Sparse4D 3D detections flowing.** VSS Warehouse 3.2 runs
> Sparse4D in `vss-rtvi-cv` → `mdx-bev` + `vss-behavior-analytics` → `mdx-behavior`,
> enabling Phase 2. A 2D-only feed (only `mdx-events`) gives **Phase 1 only**:
> `detections_json` is all-null and the Phase 2 section is omitted — expected, not
> a failure. Availability is keyed off `mdx-bev` decoding detections (Step 3f.7 gate).

The two new Kafka consumers (`BEV_TOPIC=mdx-bev`, `BEHAVIOR_TOPIC=mdx-behavior`) add five parquet columns — `detections_json`, `tracker_state_json`, `bev_frame_id`, `bev_create_time`, `ba_positions_json` — all nullable, so a Phase-1-only deploy still works. Set either topic env to empty to disable that consumer.

Per-scenario produces: 1 parquet → tw_split → N per-clip parquets → aggregator → `summary.md` (Phase 1 + Phase 2 sections) + `failures.json` + per-clip `scn_*.md` + per-clip `scn_*.mp4` auto-pulled from VST. Optional post-process: `clip_logs` evidence CSVs (incl. `detections.csv` / `tracker_state.csv` / `ba_positions.csv`) and spatial heatmaps.

**Clip semantics**: each forklift x-coordinate crossing of the trailer tripwire (x = 9.574) is a scene boundary. Consecutive boundaries delimit one clip (~40 s each). 5 min recording → ~7 clips, 10 min → ~15, 20 min → ~30.

---

## Demo log format (user-facing output)

When running the skill, print exactly this format. The phases / sections are anchors a demo video can cut around.

```
> Generating testing plan...
  Selected: in-roi (5 min), psf-edge (5 min), balanced (10 min)
  Total recording: 20 min   Estimated wall clock: ~50 min
  Output base: /app/runs/multi-test-YYYYMMDD-HHMMSS/

> Planning complete.

> Launching SIL test runs...

  Test 1/3 starts: in-roi (5 min)
  [00:00] Halos compose restarting...
  [02:48] Containers up (safety-core, comm-layer, isaac-sim, srr).
  [02:48] /safety/is_muted publishing — Safety Core chain alive.
  [02:48] Scene loading...
  [05:18] Scene loaded. Replicator generating data.
  [05:18] /srr/record true → recording active.
  [05:32] Clip 1: forklift exited trailer (x=8.51, t=00:14)
  [06:00] Clip 2: forklift entered trailer (x=10.31, t=00:42)
  [06:43] Clip 3: forklift exited trailer (x=8.46, t=01:25)
  ...
  [10:18] /srr/record false → recording stopped.
  [10:18] tw_split running...
  [10:21] tw_split: 7 clips identified.
  [10:21] aggregator running...
  [10:50] aggregator: match% avg 84.6, failures 2/7.
  [10:50] vst_video pulling per-clip MP4...
  [11:35] 7 clip videos saved to runs/multi-test-.../in-roi/videos/
  Test 1/3 completes.

  Test 2/3 starts: psf-edge (5 min)
  ...
  Test 2/3 completes.

  Test 3/3 starts: balanced (10 min)
  ...
  Test 3/3 completes.

> All test runs complete.

> Analyzing results...
  Aggregating per-scenario summary.md → cross-run REPORT.md
  Computing safety-critical unmute% per scenario...

> Done. Top-level summary: /app/runs/multi-test-YYYYMMDD-HHMMSS/summary.md
  Total: 3 scenarios, 29 clips, 29 review videos, ~50 min wall clock.
  Headline: in-roi 84.6%, psf-edge 95.6%, balanced 79.6% match.
  Mute correct: 57.8% · Unmute correct: 89.1% (safety-critical direction)
  BA detection: ROI 95.5% · TW IN/OUT 100%
  Perception (Phase 2): 🧍 detect-fail 2.5% / tracking-loss 1.9% · 🚜 detect-fail 32.0% / tracking-loss 29.1% · pos-offset 0.09 m
```

> The **Perception (Phase 2)** line is present only when the 3D pipeline was
> active (mdx-bev / mdx-behavior flowing); a Phase-1-only run omits it. Pull the
> numbers from the summary's `## Phase 2 — perception` block. See
> [references/06_interpret_report.md](references/06_interpret_report.md) for what
> `detect-fail%` vs `tracking-loss%` mean and the FAIL-clip drill-down.

The "Clip N" lines come from the live forklift-TF monitor (`../scripts/live_clip_monitor.py`), printed at each TW crossing.

---

## Agent autonomy rules — AGENT MUST ALWAYS CHECK PROGRESS

**Core rule**: never run a `sleep` longer than 60 seconds without an Explore agent actively verifying that progress is happening. If the underlying process stalled (Safety Core crashed, isaac-sim hung, parquet stopped growing), we want to know in ≤60 s, not at the end of the timer.

> ⚠️ **Resolve the perception + behavior container names first (VSS 3.2).** On VSS
> Warehouse 3.2 the perception container is **`vss-rtvi-cv`** (Sparse4D 3D warehouse,
> `DS_MODEL_FAMILY=sparse4d-warehouse`; heavier ~12–14 FPS → floor **≥ 5**) and behavior
> analytics is **`vss-behavior-analytics`** — the container names no longer carry a
> `-2d`/`-3d` suffix. (The legacy SIL stack used `perception-2d`/`perception-3d` and
> `vss-behavior-analytics-2d`/`-3d`, with 2D Sparse-free ~30 FPS → **≥ 25**.) **Don't
> hard-code any single name** — resolve once, then pick the FPS floor:
> ```bash
> PERCEPTION=$(docker ps --format '{{.Names}}' | grep -E '^(vss-rtvi-cv|perception-[23]d)$' | head -1)
> BEHAVIOR=$(docker ps --format '{{.Names}}' | grep -E '^(vss-behavior-analytics|vss-behavior-analytics-[23]d)$' | head -1)
> case "$PERCEPTION" in vss-rtvi-cv|*-3d) FPS_MIN=5;; *) FPS_MIN=25;; esac
> ```
> Every `vss-rtvi-cv` / `FPS ≥ 5` reference below and in the reference docs means
> "`$PERCEPTION` / `≥ $FPS_MIN`".

This applies to every long wait in the workflow:

| Long wait | What to verify each tick | Tick interval | Max patience |
|---|---|---:|---:|
| Halos compose up after `down` | `docker ps` shows new container IDs for the services | 30 s | 5 min |
| Scene load + shader compile | Tail `/tmp/isaac-scenario-<TS>-<LBL>.log`; expect 3 RTSPWriter lines | 30 s | 8 min (first), 3 min (subsequent) |
| Safety Core warm-up (30 s) | Confirm `/safety/is_muted` still publishing | 15 s | 90 s |
| **Recording window** (5–20 min) | Verify (a) parquet file size growing, (b) parquet `ba_events_json` rows > 0 (after 90 s), (c) `$PERCEPTION` FPS ≥ `$FPS_MIN` on all 3 cams (≥25 for 2D, ≥5 for 3D) | **60 s** | RECORD_S |
| Analysis (tw_split + aggregator + vst pull) | `ls scenes/scn_*.parquet` count, `summary.md` size > 0, mp4 count | 15 s | 5 min |

**Concretely** — at every tick, spawn an Explore agent with a tight prompt like:

```
Quick health-check for SRR multi-test scenario "<LABEL>" at T+<elapsed>s of <RECORD_S>s:

1. `docker exec srr stat -c '%s' /app/runs/run-*.parquet` — parquet size in bytes.
   Should be growing tick-over-tick (rough rate: ~80 KB/min).

2. BA events flowing into the parquet:
   docker exec srr python3 -c "
   import pandas as pd, glob
   p = sorted(glob.glob('/app/runs/run-*.parquet'))[-1]
   df = pd.read_parquet(p)
   print('rows:', len(df), 'ba_event_rows:', (df.ba_events_json != '[]').sum())
   "
   After T+90s, ba_event_rows must be > 0. If 0, perception is dead.

3. perception FPS still healthy (resolve container + threshold first):
   PERCEPTION=$(docker ps --format '{{.Names}}' | grep -E '^(vss-rtvi-cv|perception-[23]d)$' | head -1)
   case "$PERCEPTION" in vss-rtvi-cv|*-3d) FPS_MIN=5;; *) FPS_MIN=25;; esac
   docker logs --since 60s "$PERCEPTION" 2>&1 | grep PERF -A1 | tail -5
   Parse 3 FPS values from the most recent PERF block. ALL three must be ≥ $FPS_MIN
   (≥25 on a 2D deploy, ≥5 on a 3D Sparse4D deploy).

Report: parquet size, ba_event_rows, FPS triple. If parquet hasn't grown
since last tick OR ba_event_rows == 0 after T+90s OR any FPS < $FPS_MIN, flag
as STALLED.
```

If the agent reports STALLED, **stop the recording phase** and surface the
issue. Do not let a hung scenario burn 20 minutes of wall clock. (Heartbeats use
only `docker exec` / `docker logs` — the host↔container DDS bridge is unreliable
here, so host `ros2 topic hz` probes can falsely read 0.)

**Other rules**:
1. **Pre-check before recording**: containers Up + `/safety/is_muted` publishing + opcua 4840 listening. If any fails, do NOT start recording — diagnose first.
2. **Background processes**: launch `live_clip_monitor.py` BEFORE `/srr/record true`, kill it after `/srr/record false`. Stream its stdout into the demo log.
3. **Never declare a scenario "done"** until the aggregator produced N rows in `summary.md` AND N per-clip videos exist (count files in `runs/multi-test-.../<label>/videos/`).
4. **Cross-scenario isolation**: ALWAYS restart both Halos compose AND SRR compose between scenarios. Do not skip — Safety Core counter drift carries across runs and corrupts the next scenario's results.
5. **Do NOT just `bash ../scripts/run_multi.sh ...` in one shot**. The bash script is a reference/template; the skill agent orchestrates step-by-step and inserts the verification agents between every phase. One-shot invocation skips the verification layer.

---

## Ready Signals

Each phase ends when its ready signal becomes true. Poll, don't fix-time.

| Phase | Ready signal (poll until true) |
|-------|--------------------------------|
| Compose restart | `docker ps --format '{{.Names}}'` shows all 4: safety-core, comm-layer, isaac-sim, srr |
| Safety Core chain alive | `ros2 topic echo /safety/is_muted --once` exits 0 |
| Scene loaded | `/tmp/isaac-scenario.log` contains `RTSPWriter_World_Cameras_Camera*_rgb` for all 3 cams |
| Recording active | `ros2 service call /srr/record SetBool` returned `success: True` |
| Recording done | The above plus parquet file size > 100 KB and incrementing stops |
| Analysis done | `summary.md` exists with header `# SRR Aggregator — N clip(s)` matching expected clip count |
| Videos pulled | `ls runs/multi-test-.../<label>/videos/{,scenes/}scn_*.mp4 \| wc -l` ≥ expected clip count (both layouts) |
| pss.log snapshot (per-scn) | `<run>/<label>/pss.log` exists and is > 100 KB |
| pss.log concat (cross-run) | `<run>/pss.log` exists and `snapshot_pss` printed `concat N per-scn snapshot(s)` |
| Evidence bundle | `<run>.zip` exists and `[clip_logs] wrote .../multi-test-...zip (X.X MB)` printed |
| Viewer running | `view.sh` printed `Serving HTTP on 0.0.0.0 port 8765` |

---

## Workflow checklist

```
- [ ] 0. Required reading (skip on subsequent calls in same conversation):
        hoisa-deploy-profile SKILL.md § Cleanup + Ready Signals + Critical Rules;
        vss-deploy-profile SKILL.md Deployment Flow / Tear Down (references/teardown.md)
        → see "Phase 0 — Required reading" above
- [ ] 1. Generate test plan: parse user request → list of scenarios + durations
        → references/01_test_plan.md
- [ ] 2. Pre-check: Halos + SRR compose state, ROS topics, opcua port
        → references/02_launch.md
    - [ ] 2.−2 **SRR image freshness** (compare host srr-service/srr/*.py vs container /app/srr/) — rebuild or `docker cp` if stale. **MUST happen before first scenario** — a stale image is a common root cause of clip_logs missing + vst_video wrong-path.
              → references/02_launch.md Step 3.−2
    - [ ] 2.−1 SRR fixtures synced (one-time) → references/02_launch.md Step 3.−1
- [ ] 2.0 **Once, before the scenario loop** (run_multi does these automatically):
        - stop **nvstreamer** so Isaac is the SOLE camera source (the VSS sample-video
          player publishes the same camera names → churns SDR provisioning + false-positives
          the scene-ready gate). Stays down for the whole run.
        - clear stale `/app/runs/run-*.parquet` stragglers (a leftover from an aborted run
          can have a newer mtime than a fresh recording and get mis-attributed to scenario 1).
- [ ] 3. Per scenario (loop):
    - [ ] 3.0 **First scenario only, optional**: fresh-start `cleanup_all_datalog.sh`
              (truncates pss.log + wipes comm-layer/ — never run between scenarios)
              → references/02_launch.md Step 3.0
    - [ ] 3a. Halos compose down/up + verify (NO cleanup_all_datalog.sh here).
              **While Isaac is down**, purge ALL VST sensors (`vst_sensor_manager.py --delete-all`)
              so the next Isaac registration lands in a clean VST — stale dup sensors otherwise
              pile up across restarts and the SDR never provisions perception (empty parquet from
              scenario 2 on). run_multi's `phase_vst_purge` does this.
    - [ ] 3a.5 **VST API gate** (record API 200 + ≤3 online Camera sensors) — HARD GATE, fail → abort
    - [ ] 3b. SRR compose down/up + verify
    - [ ] 3c. Select scenario behavior trees + set simulation_duration (see 02_launch Step 3c/3c.1)
    - [ ] 3d. Start scene in isaac-sim (--start --headless --enable-vst)
    - [ ] 3e. Wait for shaders + 3 RTSP streams ready
    - [ ] 3f. Launch live_clip_monitor.py (background — known-broken; see 03_monitor.md)
    - [ ] 3f.5 Safety Core warm-up 30 s
    - [ ] 3f.7 **VSS perception health gate** (FPS ≥ 5 on 3 cams + Kafka mdx-events flowing; for Phase 2 also require **mdx-bev** decoding real detections > 0) — HARD GATE, fail → abort
    - [ ] 3g. /srr/record SetBool true
    - [ ] 3h. Sleep RECORD_S (with BA event + perception FPS heartbeat)
    - [ ] 3i. /srr/record SetBool false
    - [ ] 3j. Kill live_clip_monitor.py
    - [ ] 3k. Phase 4 analyze (per-scn, **DO BEFORE next iter's 3a**):
              move parquet → **`sudo chmod -R a+rwX <scn-host-dir>`** (docker mkdir creates root-owned dir on bind-mount) → **snapshot_pss.sh --per-scn** → tw_split → aggregator → **vst_video.split-run** (Step 4a VST timeline probe before pull) → flatten `videos/scenes/*.mp4` to `videos/` if stale-image fallback
              — 04_analyze.md. Batching these to end-of-multi-test loses pss.log (next 3a truncates) + MP4s (Isaac removes VST sensors after sim_length)
    - [ ] 3l. Verify: summary.md row count == expected clip count + no PERCEPTION_LIKELY_DEAD banner
- [ ] 4. Cross-run rollup → references/05_report.md
    - [ ] 4a. Concat per-scn pss.log → `<runs-base>/pss.log` (`snapshot_pss.sh` cross-run mode)
    - [ ] 4b. aggregator `--top-level` → top-level summary.md (idempotent — re-run is safe)
    - [ ] 4c. Extract headline metrics + print demo log final block
- [ ] 5. Interpret + answer user questions about the report
        → references/06_interpret_report.md
- [ ] 6. Post-process — references/05_report.md Step 6:
    - [ ] 6a. `clip_logs --zip` → per-clip evidence + `<runs-base>.zip` self-contained bundle
    - [ ] 6b. **ASK user**: launch debug viewer? `[y/n]`
    - [ ] 6c. If yes: clone srr-debug-viewer if missing → symlink run into `data/` → **(optional) update `data/index.json`** (prepend new run for stable ordering; the HTTP directory-listing fallback discovers runs without it — see [05_report.md](references/05_report.md) Step 6) → **pre-check port 8765** (ASK reuse/kill/other-port if busy) → `view.sh` → surface URL
    - [ ] 6d. **Optional, on request**: spatial perception heatmaps + coverage polygons
              (requires `clip_logs` CSVs first; scripts + matplotlib are baked into the srr image):
              `render_perception_heatmap.py` (add `--density` for occupancy, `--fov-mask` to
              clip to calibrated FOV) → `perception_heatmaps[_fovmask]/`;
              `render_coverage_polygons.py` → `coverage_polygons/`. → references/05_report.md Step 6e
    - [ ] 6e. Print final paths block (summary, reports, videos, zip, pss.log, heatmaps).
```

Mark each box as it completes. Print `[ok]` or `[fail]` per check.

---

## Reference Documents

| Document | Use When |
|----------|----------|
| [references/01_test_plan.md](references/01_test_plan.md) | Selecting scenarios, validating Halos + SRR compose are deployed |
| [references/02_launch.md](references/02_launch.md) | Per-scenario launch — compose restart, scene start, `/srr/record true` |
| [references/03_monitor.md](references/03_monitor.md) | Live recording monitor — forklift TW crossing detection, demo log lines |
| [references/04_analyze.md](references/04_analyze.md) | Post-recording: tw_split + aggregator + per-clip video pull |
| [references/05_report.md](references/05_report.md) | Cross-run rollup → top-level summary.md + headline metrics |
| [references/06_interpret_report.md](references/06_interpret_report.md) | **Read before answering anything about the report.** What every metric means, common patterns, drill-down workflow for FAIL clips |

### Related docs

User-facing + companion docs elsewhere in the repo:

| Document | Use When |
|---|---|
| [`references/06_interpret_report.md`](references/06_interpret_report.md) | What every Phase 1 / Phase 2 number means + the FAIL-clip drill-down workflow. Read before answering perception-metric questions. Formula source of truth: `srr-service/srr/aggregator.py` → `compute_phase2_metrics`. |
| [`../../tools/srr-debug-viewer/README.md`](../../tools/srr-debug-viewer/README.md) | Browser viewer for per-clip MP4 + sync panels. Launched optionally in Phase 6c (see [05_report.md](references/05_report.md) Step 6). |

---

## Critical constants

| Constant | Source | Notes |
|---|---|---|
| `TW_X` (forklift trailer tripwire) | `$CALIBRATION_JSON` (`sensors[0].tripwires[0].wire.p1.x`); fallback 9.574 | clip boundary detector key |
| `ROI` (work zone) | `$CALIBRATION_JSON`; fallback x ∈ [4.877, 9.574], y ∈ [-18.976, -11.239] | rectangular polygon |
| `SAMPLE_HZ` | 30 (built-in) | SRR recording rate |
| `PADDING_SEC` | 1.0 (built-in) | tw_split clip-boundary padding (BA TW events at x≈10.2-10.6 land in in-trailer half) |
| `ROS_DOMAIN_ID` | `$ROS_DOMAIN_ID` (Halos `.env`, default 74) | must match between containers and host probes; multi-machine on same LAN: each machine MUST have a unique ID — see `hoisa-deploy-profile/references/troubleshooting.md` (ROS_DOMAIN_ID collision / `Publisher count` > 1) |
| `VST_BASE_URL` | `$VST_BASE_URL` (Halos `.env`, e.g. `http://<HOST_IP>:30888/vst/api`) | base for sensor/record/storage endpoints. **`/vst/api/v1/...` is the API**; `/vst/...` (no `/api`) returns the SPA HTML and is meaningless for probes |
| `SENSORS` | `$SENSORS` (SRR `.env`, default `Camera,Camera_01,Camera_02`) | VSS sensor names; not in calib.json |
| `BEV_TOPIC` | `$BEV_TOPIC` (default `mdx-bev`) | Kafka topic for 3D detections (Sparse4D `bbox3d`). Empty string disables the Phase 2 detection consumer |
| `BEHAVIOR_TOPIC` | `$BEHAVIOR_TOPIC` (default `mdx-behavior`) | Kafka topic for BA track positions. Empty string disables the BA-position consumer |
| `SAMPLE_HZ` (3D too) | 30 (built-in, `service.py`) | mdx-bev is **replace-not-append** per tick; mdx-behavior kept per `track_id` with `BA_POS_TTL_S=1.0` |
| `gate_m` | 1.5 m (built-in, `aggregator.py`) | nearest-GT match gate for detect-fail / recall. Multi-gate recall also reported @0.5/1.0/1.5 m |
| `coverage_pad_m` | **0.0** (built-in) | coverage = convex hull of detections, pad=0 (a nonzero pad over-reports detect-fail by counting out-of-FOV GT as misses) |
| `boundary_m` | 2.0 m (built-in) | forklift trailer-boundary slice (±2 m of `TW_X`) |
| `split_dist_m` | 1.0 m (built-in) | same-class split/fragmentation threshold |
| forklift GT origin offset | estimated per-run (~0.39 m → ~0.08 m residual) | GT `body` TF is behind the 3D-box centre; aggregator infers + removes the bias before matching |
| Repo root | `$HOISA_ROOT_PATH` (REQUIRED) | halos-outside-in-safety repo root — everything below derives from it |
| ⤷ deployment dir | `$HOISA_ROOT_PATH/deployments` | compose.yaml + profiles/ |
| ⤷ profile env | `$HOISA_ROOT_PATH/deployments/profiles/sil.env` | source of all shared values |
| ⤷ Isaac SIL dir | `$HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil` | scenes/configs/behavior-trees/scripts |
| SRR service dir | `${SRR_PIPELINE_DIR}/srr-service/` | where to `docker compose up` |
| Output base | `${RUNS_HOST_DIR:-${SRR_PIPELINE_DIR}/srr-service/runs}/multi-test-YYYYMMDD-HHMMSS/` | per-scenario subdirs containing `scenes/`, `videos/`, `reports/` |
| Top-level summary | `<output-base>/summary.md` | cross-run rollup, written by aggregator `--top-level` |

> All host paths above resolve from the repo's `.env` (which sets `HOISA_ROOT_PATH`; scripts source the derived profile env for shared values). Skill scripts self-locate via `$(dirname "$0")` — no absolute path is baked into them.

---

## Cleanup

```bash
# After multi-test, leave containers running for inspection.
# Only tear down when explicitly asked.

# To free disk space (drop old runs — videos now live inside the run dir):
rm -rf ${RUNS_HOST_DIR}/multi-test-OLDDATE
```

---

### When VSS is corrupted (sensor/add 400, record API 503)

The VST postgres DB persists across `docker compose down` (mounted volume
at `${MDX_DATA_DIR}/data_log/vst`, ~30 GB). `down` + `up` alone will NOT
clear stale `online` Camera sensors — Isaac will get 400 on `sensor/add`
and the next run will produce ghost results (see
[`06_interpret_report.md`](references/06_interpret_report.md) — ghost-result
section).

**Symptoms** (any of):
- `/vst/api/v1/record/streams` returns 503 (envoy upstream connection failure)
- Isaac log: `[Error] [vst_sensor_manager] ✗ Failed to add sensor Camera: 400 Client Error`
- `$PERCEPTION` (`vss-rtvi-cv`; legacy `perception-2d`/`-3d`) PERF lines all show FPS 0.00000
- Aggregator emits `🚫 PERCEPTION_LIKELY_DEAD` banner

**Reset recipe** (idempotent — safe to rerun):

```bash
# 1. Stop VSS Warehouse
cd ${WAREHOUSE_DIR:-/path/to/vss-warehouse}
docker compose --env-file warehouse/.env down

# 2. Wipe accumulated data (postgres, kafka, elastic, redis, vst recordings)
bash ./cleanup_all_datalog.sh -b warehouse

# 3. Start VSS Warehouse fresh
docker compose --env-file warehouse/.env up -d

# 4. After VSS is healthy, delete any auto-registered Camera/Camera_01/Camera_02
#    that nvstreamer creates from default config (otherwise Isaac sensor/add 400)
for SID in $(curl -sf "${VST_BASE_URL}/v1/sensor/list" \
              | jq -r '.[] | select(.state=="online") | select(.name | test("^Camera(_0[12])?$")) | .sensorId'); do
  curl -sf -X DELETE "${VST_BASE_URL}/v1/sensor/${SID}"
done

# 5. Restart Halos compose so Isaac re-registers fresh sensors
cd ${HOISA_ROOT_PATH}/deployments
docker compose --env-file ${HOISA_ROOT_PATH}/deployments/profiles/sil.env down && docker compose --env-file ${HOISA_ROOT_PATH}/deployments/profiles/sil.env up -d
```

For full warehouse lifecycle (NGC artifact download, env config, etc.) see the
`vss-deploy-profile` skill — Deployment Flow + `references/teardown.md`.

For full Halos lifecycle (safety-core, comm-layer, Isaac scene config) see the
`hoisa-deploy-profile` skill (`skills/hoisa-deploy-profile/SKILL.md`).

