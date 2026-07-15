# SRR Quick Start

End-to-end recipe: bring up Halos SIL → clone SRR → install the skill → run a multi-test sweep → view results in the debug viewer.

Estimated wall-clock: **~90 minutes** for first-time setup, **~80 minutes** per full 5-scenario sweep thereafter.

---

## Overview

The SRR pipeline wraps the Halos SIL stack (VSS Warehouse perception + PSF safety + Isaac Sim 6.0) and produces graded per-clip reports comparing PSF's mute/unmute behaviour against ground-truth sim positions. SRR ships **inside this repo** (`closed-loop-testing/regression-reporter/`), so there is nothing separate to clone. You need:

1. **Halos SIL deployed and healthy** on your host (via the `hoisa-deploy-profile` skill in `skills/`).
2. **The `hoisa-generate-regression-report` skill installed** (symlink from `skills/`).
3. **SRR fixtures synced** into the Isaac SIL dir (`./halos-integration/sync_to_halos.sh`).
4. **Multi-test sweep run** (either via skill or `scripts/run_multi.sh`).
5. **`tools/srr-debug-viewer`** launched + a test zip loaded for the browser-based per-clip debug experience.

If you only want to **view existing test results** without running new tests, jump to [§ View results](#7-view-results) — load any `multi-test-*.zip` bundle you already have.

---

## Two perception flows: 2D vs 3D profile

SRR runs on top of whichever **VSS Warehouse perception profile** Halos was deployed
with, and how much SRR can score depends on which one:

| | **2D profile** (`MODE=2d`) | **3D profile** (`MODE=3d`, Sparse4D) |
|---|---|---|
| Perception container | `vss-rtvi-cv` (~30 FPS) | `vss-rtvi-cv` (~12–14 FPS, Sparse4D BEV) |
| Behaviour analytics | `vss-behavior-analytics` | `vss-behavior-analytics` |
| Kafka topics SRR reads | `mdx-events` (ROI/TW events) | `mdx-events` **+** `mdx-bev` (3D detections) **+** `mdx-behavior` (BA positions) |
| **SRR Phase 1** (PSF mute/unmute decision) | ✅ yes | ✅ yes |
| **SRR Phase 2** (per-class detect-fail% / tracking-loss%, localization) | ❌ no (no 3D detections) | ✅ yes |

- **Phase 1** (the original Apr-30 demo scope) only needs the BA ROI/TW events on
  `mdx-events`, so it works on **either** profile.
- **Phase 2** (detection + tracking scoring) needs the 3D detector's per-object output
  on `mdx-bev` and BA positions on `mdx-behavior`, so it requires the **3D profile**.
  On a 2D-only deploy the parquet's `detections_json` is all-null and SRR simply omits
  the Phase 2 section — that's expected, not an error.

SRR auto-detects which it got (it subscribes all three topics; the two 3D consumers are
controlled by `BEV_TOPIC` / `BEHAVIOR_TOPIC` env, default `mdx-bev` / `mdx-behavior`, set
empty to disable). To **get Phase 2 metrics, deploy VSS Warehouse in 3D mode** — see the
`vss-deploy-profile` skill (warehouse profile). The 2026-06 SRR runs were all on the 3D profile.

---

## Prerequisites

- Linux host with:
  - GPU + NVIDIA driver compatible with Isaac Sim 6.0 (RTX 4090 / 5090 / PRO 5000 / PRO 6000 all confirmed working)
  - Docker + Docker Compose v2
  - `~/.claude/skills/` writable (Claude Code) or `~/.cursor/skills/` (Cursor)
- **NGC API key** with access to `nvidia/outside-in-safety` (ask the Halos team if you don't have one).
- **Claude Code** or **Cursor** installed. The skill orchestration uses the agent's Explore + tool-use capability; you can run the bare bash scripts manually if no agent host is available, but the skill catches most of the common failure modes automatically.

---

## 1. Deploy Halos SIL (one-time)

Halos SIL + VSS Warehouse are deployed by two skills that ship in this repo under
`skills/`: **`hoisa-deploy-profile`** (Halos SIL) and **`vss-deploy-profile`**
(VSS Warehouse perception backend). Install both:

```bash
REPO="$(git rev-parse --show-toplevel)"
mkdir -p ~/.claude/skills
ln -sfn "$REPO/skills/hoisa-deploy-profile"  ~/.claude/skills/hoisa-deploy-profile
ln -sfn "$REPO/skills/vss-deploy-profile"    ~/.claude/skills/vss-deploy-profile
# (For Cursor: same commands targeting ~/.cursor/skills/)

# Verify both loaded — open Claude Code, run: /skills
```

Then in Claude Code prompt:

```
Deploy Halos Outside-In Safety SIL on this machine.
```

The agent walks through VSS Warehouse + Halos SIL deployment + the test scenario end-to-end. It pauses for sudo prompts, NGC API key, GPU selection — answer when asked.

**Verify SIL healthy** after deploy:

```bash
docker ps --format 'table {{.Names}}\t{{.Status}}' | grep -E 'safety-core|isaac-sim|comm-layer|mediamtx'
# Expect 4 containers Up
```

When VSS Warehouse + Halos SIL are both up and Isaac Sim has loaded the scene + 3 RTSP streams are flowing, you're ready for SRR.

---

## 2. Configure SRR

SRR lives at `closed-loop-testing/regression-reporter/` in this repo — nothing to clone. From the repo root:

```bash
cd closed-loop-testing/regression-reporter
```

Top-level layout — `srr-service/` (the Python container), `scripts/` (bash orchestration), `scenarios/` (test fixtures), `halos-integration/` (glue), `docs/`. The Claude Code skill is a sibling at `skills/hoisa-generate-regression-report/`; the viewer at `tools/srr-debug-viewer/`.

### Configure `.env`

Copy the example and point it at the sibling Halos dirs:

```bash
cp .env.example .env
$EDITOR .env
# Set HALOS_COMPOSE_DIR=<repo>/deployments
#     HALOS_ENV_FILE=<repo>/deployments/profiles/sil.env
#     HALOS_SIL_DIR=<repo>/closed-loop-testing/isaac-sim/sil
#     CALIBRATION_JSON=<path to VSS calibration sample-data>
```

`docker compose` runs from `srr-service/`, so point that dir's env at the same file once:

```bash
ln -s ../.env srr-service/.env
```

---

## 3. Install the SRR report skill

```bash
REPO="$(git rev-parse --show-toplevel)"
mkdir -p ~/.claude/skills
ln -sfn "$REPO/skills/hoisa-generate-regression-report" ~/.claude/skills/hoisa-generate-regression-report

# Cursor users:
mkdir -p ~/.cursor/skills
ln -sfn "$REPO/skills/hoisa-generate-regression-report" ~/.cursor/skills/hoisa-generate-regression-report
```

Verify in Claude Code: `/skills` should now list `hoisa-generate-regression-report` alongside the two deploy skills.

---

## 4. Sync SRR fixtures into Halos compose

SRR's test fixtures (scenes, behavior trees, NavMesh, Script Editor utilities) live in this repo. Halos's `isaac-sim` container bind-mounts its Isaac SIL dir (`${HALOS_SIL_DIR}`, GitHub layout `closed-loop-testing/isaac-sim/sil/`) at `/isaac-sim/sil/`, so SRR's fixtures have to land in that dir. Sync them once:

```bash
./halos-integration/sync_to_halos.sh          # uses HALOS_SIL_DIR from .env (or sibling ../isaac-sim/sil)
# Or pass the Isaac SIL dir explicitly:
./halos-integration/sync_to_halos.sh ../isaac-sim/sil
```

This is **idempotent** — safe to re-run after pulling fresh SRR changes. See [`halos-integration/README.md`](../halos-integration/README.md) for the Pattern A (sync) vs Pattern B (planned bind-mount) discussion.

---

## 5. Run SRR

### Build the SRR container

```bash
cd srr-service
docker compose up --build -d
docker logs -f srr
# Expect:
#   SRR service up; sample_hz=30, out_dir=/app/runs, ...
#   Awaiting: ros2 service call /srr/record SetBool ...
# Ctrl-C to exit the follow, container keeps running.
cd ..
```

### Option A — via Claude Code skill (recommended)

Open Claude Code in the `regression-reporter/` dir and prompt:

```
Run SRR multi-test on all 5 scenarios
```

The skill:

- Verifies SRR fixtures synced into the Isaac SIL dir ([§ Step 3.−1](../../../skills/hoisa-generate-regression-report/references/02_launch.md))
- Restarts halos compose + `vss-rtvi-cv` (clears stale RTSP)
- Per scenario: copies the scenario's `srr_<name>_char{0,1,2}.bt.json` onto the active `srr_char{0,1,2}.bt.json` trees, sets `simulation_duration` (seconds) in `default_config_ros.yaml`, starts Isaac, gates on VSS perception health, opens recording, lets it run for the scenario duration, stops, runs aggregator + pulls per-clip MP4s.
- Cross-run aggregator at the end → `summary.md` at run-dir root.
- Snapshots `pss.log` slice into run dir for forensic retention.

Run takes **~80 min wall clock** for the full 5-scenario sweep (5+5+5+10+20 min recording + restart overhead). The `fixed` baseline is opt-in (`run_multi.sh fixed`) and not part of `all`/`full`.

### Option B — manual via `scripts/run_multi.sh`

```bash
./scripts/run_multi.sh all                                     # the 5 sweep scenarios at 5 min each
./scripts/run_multi.sh full                                    # 5 sweep scenarios, recommended durations
./scripts/run_multi.sh fixed                                   # deterministic baseline (opt-in; not in all/full)
./scripts/run_multi.sh in-roi:300 psf-edge:300 fast:1200       # mixed durations
./scripts/run_multi.sh in-roi                                  # single scenario, default 5 min
```

Scenario names: `in-roi`, `psf-edge`, `psf-clear`, `balanced`, `fast` (the `all`/`full` sweep) + `fixed` (deterministic baseline, run by name only). Override duration as `name:seconds`. The skill route is preferred because it adds pre-flight checks that catch ghost-result runs (perception dead while aggregator still produces "looks healthy" numbers — see [`references/06_interpret_report.md`](../../../skills/hoisa-generate-regression-report/references/06_interpret_report.md)).

### Output

```
<srr-runs-dir>/multi-test-YYYYMMDD-HHMMSS/
├── summary.md                  cross-run rollup — start here (Phase 1 + Phase 2 on a 3D run)
├── failures.json               machine-readable FAIL list
├── pss.log                     wall-time slice of PSF host syslog
├── <scenario>/
│   ├── reports/summary.md      per-scenario rollup
│   ├── reports/scn_*.md        per-clip verdict reports
│   ├── scenes/scn_*.parquet    raw 30 Hz captures (Phase 2 columns when 3D profile active)
│   ├── videos/scn_*.mp4        per-clip MP4s
│   ├── clip_logs/<scn>/        evidence CSVs (gt_positions, psf_timeline, ba_events,
│   │                             detections, tracker_state, ba_positions) + manifest.json
│   └── run-*.parquet           scenario-level BA event pool
└── perception_heatmaps[_fovmask]/   optional spatial detect-fail / tracking-loss / density maps
```

---

## 6. Generate the per-clip evidence bundle

For sharing or for the web viewer, package each clip into a folder of human-readable files (CSV + JSON + raw text + MP4):

```bash
docker exec srr python3 -m srr.clip_logs \
  --runs-dir /app/runs/multi-test-YYYYMMDD-HHMMSS \
  --calib /app/calibration.json \
  --zip
```

Produces `<scenario>/clip_logs/<scn_id>/` per clip with `manifest.json + gt_positions.csv + psf_timeline.csv + ba_events.csv + pss_log.{txt,jsonl} + video.mp4`, plus a top-level `runs/<multi-test-name>.zip` self-contained archive (~2 GB).

See [`scenarios/README.md`](../scenarios/README.md) and the viewer's data-schema docs for what each column means.

---

## 7. View results

### Option 1 — read `summary.md` directly

The fastest way to scan a run's health. From the run dir:

```bash
less <run-dir>/summary.md
```

Top of file: headline (clip count, verdict counts, mean match%, mute / unmute correct%, BA detect rates, worst lags). Then per-scenario rollup table. Then per-clip table linking to each clip's report.

Read [`references/06_interpret_report.md`](../../../skills/hoisa-generate-regression-report/references/06_interpret_report.md) to know what to look for — there's a known PSF counter-drift signature that's expected to show up in every v1.2 / v1.2.x run through SRR v1.3, recognized but not refiled as a new bug.

### Option 2 — open per-clip reports

For each FAIL clip, the aggregator wrote `<scenario>/reports/<scn_id>.md` with verdict + mismatch breakdown + per-frame match rate + reaction lag percentiles + mismatch windows. Cross-reference with the per-clip MP4 at `<scenario>/videos/scenes/<scn_id>.mp4`.

### Option 3 — `srr-debug-viewer` (interactive, recommended for FAIL clip drill-down)

Browser-based viewer pairing the per-clip MP4 with synchronized PSF state / GT positions / BA events / pss.log streams. Scrub the video and all panels jump in lockstep. (See the screenshot in the [main README](../README.md) for the layout.)

```bash
# 1. Go to the viewer (ships in this repo at tools/srr-debug-viewer)
cd "$(git rev-parse --show-toplevel)/tools/srr-debug-viewer"

# 2. Drop your zip into data/
mkdir -p data
unzip /path/to/multi-test-YYYYMMDD-HHMMSS.zip -d data/
# (or symlink: ln -s /path/to/multi-test-XXX data/)

# 3. Launch
./view.sh
# Opens browser at http://localhost:8765/
```

The landing page lists all runs in `data/` with verdict pills + GT vs PSF correct% per run. Click a run → see scenario tables. Click a clip → video + 4 sync panels. See [the viewer's README](../../../tools/srr-debug-viewer/README.md) for layout details.

Pre-baked reference bundles (to be published as GitHub Release assets when the repo goes public) illustrate the expected output:

| Bundle | Config | Clips | Use it for |
|---|---|---:|---|
| `multi-test-20260430-081223.zip` | v1.1 reference | 66 | Baseline — was counter-drift present in v1.1? (Yes, confirmed) |
| `multi-test-20260503-175731.zip` | v1.2 `maxPipelines=3` (correct sensor count) | 97 | Primary v1.2 reproducer — contains `fast-20min/scn_0042` (99.9% over-mute, catastrophic FAIL) |
| `multi-test-20260504-035555.zip` | v1.2 `maxPipelines=2` (workaround) | 82 | Confirms workaround doesn't fix the bug |

If you just want to see what the SRR output looks like without running anything yourself, unzip any of these bundles into the viewer's `data/` and launch.

---

## Troubleshooting

| Symptom | Likely fix |
|---|---|
| Skill says "SRR fixtures missing" / sed pattern doesn't match | Re-run `./halos-integration/sync_to_halos.sh` |
| Aggregator reports `match% 86.9` but `BA TW IN 0/8` and ALL clips FAIL with `under-mute 100%` | **Ghost result** — VSS perception was dead during the run. Check `docker logs vss-rtvi-cv` — if FPS=0, run the VSS DB reset recipe from [`hoisa-generate-regression-report/SKILL.md` § "When VSS is corrupted"](../../../skills/hoisa-generate-regression-report/SKILL.md), then re-deploy halos + retry. |
| Isaac Sim exits mid-scenario | Check `default_config_ros.yaml`'s `simulation_duration` (seconds) covers scene-load + PSF warm-up + the recording window. IRA 1.6 uses `simulation_duration` (was `simulation_length` in frames); the runner sets it per scenario (`RECORD_S + 30 + 600`). Skill Step 3c.1 auto-fixes this. |
| Per-clip MP4 fails to play in browser (infinite spinner) | Video is HEVC. Firefox + most Chromium on Linux don't decode it. Run `tools/srr-debug-viewer/transcode_to_h264.sh` against the unzipped dir, or open in `mpv` / `vlc` separately. |
| `srr-debug-viewer` says "No runs found under data/" | Unzip the bundle INTO `data/`, not next to it. `data/multi-test-XXX/` must contain `summary.md` directly. Hard-reload browser (`Ctrl+Shift+R`). |
| Skill stuck waiting for VSS perception | Run `docker logs vss-rtvi-cv --tail 200`. FPS=0 → ghost-result risk (don't proceed). FPS=30 → still booting, give it 30s. See `skills/hoisa-generate-regression-report/references/02_launch.md` Step 3f.7. |
| Counter-drift signature in every run (Direction A, alternating PASS/FAIL with forklift state) | **Known upstream PSF issue** — acknowledged by the Halos PSF team, fix planned post-v1.3. Recognize + move on; don't refile. Detail in [`references/06_interpret_report.md`](../../../skills/hoisa-generate-regression-report/references/06_interpret_report.md). |

For deeper troubleshooting see:

- [`skills/hoisa-generate-regression-report/SKILL.md`](../../../skills/hoisa-generate-regression-report/SKILL.md) § "When VSS is corrupted"
- [`references/06_interpret_report.md`](../../../skills/hoisa-generate-regression-report/references/06_interpret_report.md) — Common patterns
- [`tools/srr-debug-viewer/docs/troubleshooting.md`](../../../tools/srr-debug-viewer/docs/troubleshooting.md)

---

## What next

- **First time on this machine** — run a single 5-min scenario (`./scripts/run_multi.sh in-roi:300`) before committing to a full 80-min sweep. Surfaces 80% of setup issues in 12 minutes.
- **Recurring regression work** — run the `hoisa-generate-regression-report` skill on a schedule. Each run produces a comparable zip; diff `summary.md` headlines across runs to spot regressions.
- **Adding new scenarios** — see [`../scenarios/README.md`](../scenarios/README.md) and the behavior-tree generator `scenarios/tools/randomize_paths.py`.
- **Phase 2 metrics (detection / tracking)** — **shipped** (requires the 3D profile, see above). What every number means + the drill-down workflow: [`references/06_interpret_report.md`](../../../skills/hoisa-generate-regression-report/references/06_interpret_report.md).

Questions: open a GitHub issue.
