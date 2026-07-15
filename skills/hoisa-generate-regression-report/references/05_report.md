# Phase 5 — Cross-Run Reporting

Phase 4 Step 3 already runs the aggregator in `--top-level` mode (idempotent),
so by the time Phase 5 starts the cross-run `summary.md` already exists at
`<runs-base>/summary.md` with up-to-date numbers. Phase 5 is **read-only**:
extract headlines, format demo log, optionally write a polished writeup.

For full guidance on what the report contains and how to interpret each metric,
see [06_interpret_report.md](06_interpret_report.md).

---

## Inputs

By the start of Phase 5 the following exist (written by Phase 4):
- `<runs-base>/summary.md` ← cross-run rollup (this is what we read)
- `<runs-base>/failures.json`
- `<run>/reports/summary.md` ← per-run with run-level BA pool
- `<run>/reports/scn_*.md` + `failures.json`
- `<run>/videos/scn_*.mp4`
- `<run>/pss.log` ← per-scn snapshot taken in Phase 4 Step 1b (one per scenario)

---

## Step 0 — Concat per-scn pss.log → run-level pss.log

The per-scenario pss.log snapshots taken in Phase 4 Step 1b each cover ONE scenario. `clip_logs` (next step) wants a single `<runs-base>/pss.log` covering everything. Concat them now (sorted by leading ISO timestamp):

```bash
"${SRR_PIPELINE_DIR}/scripts/snapshot_pss.sh" \
  "${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}"
# (no --per-scn flag = cross-run mode; auto-detects per-scn files and concats.
#  Falls back to mtime-slice of source only if no per-scn snapshots exist —
#  which is a sign Phase 4 Step 1b was skipped.)
```

Expected output line:
```
snapshot_pss: concat N per-scn snapshot(s) → .../pss.log (X lines, Y MB)
```

If the top-level summary is missing (e.g. Phase 4 was interrupted),
re-run the aggregator manually:

```bash
docker exec srr bash -c "
  cd /app && python3 -m srr.aggregator \
    --runs-dir /app/runs/multi-test-${TIMESTAMP} \
    --top-level
"
```

This is idempotent — running it again refreshes everything from the parquets.

Logs:
```
  [agg] BA pool for <run>: M events
  ...
  [agg] wrote /app/runs/multi-test-${TIMESTAMP}/summary.md and failures.json
  [agg] wrote .../<run>/reports/summary.md
```

---

## Step 2 — Extract headline metrics for the demo log

The top-level `summary.md` headline section is a stable text block — parse with grep, not awk on the per-clip table:

```bash
TOP=${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/summary.md

TOTAL=$(grep -oP 'total clips: \*\*\K\d+' "$TOP")
PASS=$(grep -oP '✅ \*\*\K\d+(?= PASS)' "$TOP")
NEAR=$(grep -oP '⚠ \K\d+(?= NEAR)' "$TOP")
FAIL=$(grep -oP '❌ \*\*\K\d+(?= FAIL)' "$TOP")
MEAN_M=$(grep -oP 'match%: mean \K[\d.]+' "$TOP")
MED_M=$(grep -oP 'median \K[\d.]+(?= · min)' "$TOP")
MUTE=$(grep -oP 'Mute correct%[^*]+\*\*\K[^*]+' "$TOP")
UNMUTE=$(grep -oP 'Unmute correct%[^*]+\*\*\K[^*]+' "$TOP")
ROI=$(grep -oP 'ROI entries[^*]+\*\*\K[^*]+' "$TOP")
TWIN=$(grep -oP 'TW IN[^*]+\*\*\K[^*]+' "$TOP")
TWOUT=$(grep -oP 'TW OUT[^*]+\*\*\K[^*]+' "$TOP")
WORST_UM=$(grep -oP 'worst UNMUTE lag[^*]+\*\*\K[\d.]+(?= ms)' "$TOP")
WORST_M=$(grep -oP 'worst MUTE lag[^*]+\*\*\K[\d.]+(?= ms)' "$TOP")

echo "  Headline:  $TOTAL clips · $PASS pass / $NEAR near / $FAIL fail · match% mean $MEAN_M"
echo "  Mute correct: $MUTE · Unmute correct: $UNMUTE"
echo "  BA ROI: $ROI · TW IN: $TWIN · TW OUT: $TWOUT"
echo "  Worst lag: UNMUTE $WORST_UM ms · MUTE $WORST_M ms"
```

---

## Step 3 — Compute safety-critical unmute% per scenario (optional)

Per-scenario "dangerous condition" check — clips where chars > 70 % in ROI AND fk > 50 % in trailer (= alarm MUST stay unmuted):

```bash
python3 ../scripts/safety_critical_unmute.py \
  --multi-test-dir ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}
```

This reads the new top-level summary (or per-run reports) and prints a small table suitable for the demo log.

---

## Step 4 — Generate REPORT-vN.md (optional)

Top-level `summary.md` IS the canonical report for most use cases. Only when the user asks for a stakeholder write-up do we generate a polished MD:

- **Detailed technical report** (use this run's top-level `summary.md` as the reference shape — Headline + Per-run rollup + Per-clip detail): fill in this run's headline + per-run rollup numbers + any new bugs found.
- **Stakeholder slides**: plain-English, 6 slides, frame as "perception solid + decision logic correct in clear cases + one known mechanism (counter drift) explains failures".

Write to `${SRR_PIPELINE_DIR}/results/multi-test-${TIMESTAMP}-REPORT.md` (or similar).

---

## Step 5 — Demo log final block

```
> All test runs complete.

> Analyzing results...
  Running aggregator in top-level mode (cross-run BA pool)...
  Computing safety-critical unmute% per scenario...

  Headline:  29 clips · 17 pass / 5 near / 7 fail · match% mean 84.6
  Mute correct: 57.8% (n=12983) · Unmute correct: 89.1% (n=65660)
  BA ROI: 84/88 (95.5%) · TW IN: 66/66 (100%) · TW OUT: 66/66 (100%)
  Worst lag: UNMUTE 21100 ms · MUTE 28233 ms

> Done. Top-level summary: runs/multi-test-${TIMESTAMP}/summary.md
  Total: 3 scenarios, 29 clips, 29 review videos, ~50 min wall clock.
  Best run: psf-edge (95.6% match, 0 fails)
```

---

## Optional: spawn final review agent

```
Read runs/multi-test-${TIMESTAMP}/summary.md and produce a 3-bullet executive
summary for a non-technical audience:
- What worked (perception, best run, etc.)
- What's the one open issue (counter drift)
- What should the user look at next (the FAIL clips with biggest lag)

Reference 06_interpret_report.md for the metric semantics.
```

This gives the user a ready-to-share summary at the end.

---

## Step 6 — Post-process: evidence bundle + optional viewer handoff

After reporting, package the per-clip evidence bundle (CSVs + manifest + pss slice + MP4 if pulled) for sharing. Then offer the browser-based debug viewer for visual scrub-sync drill-down.

### Step 6a — clip_logs bundle (always run)

```bash
docker exec srr python3 -m srr.clip_logs \
  --runs-dir /app/runs/multi-test-${TIMESTAMP} \
  --calib /app/calibration.json \
  --zip
```

- Default fallback picks `<runs-dir>/pss.log` (written by Step 0 above) — no `--pss-log` needed.
- Produces `<scenario>/clip_logs/<scn_id>/{manifest.json, gt_positions.csv, psf_timeline.csv, ba_events.csv, pss_log.txt, pss_log.jsonl, video.mp4}` per clip, plus a single `${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}.zip` self-contained bundle (~1–2 GB with MP4s, ~1 MB without).
- Use `--fails-only` if user only wants FAIL clips bundled.

Expected log lines:
```
[clip_logs] using pss.log at /app/runs/multi-test-${TIMESTAMP}/pss.log
[clip_logs] total N clip(s) across M scenario(s)
[clip_logs] wrote /app/runs/multi-test-${TIMESTAMP}.zip (X.X MB)
```

### Step 6b — ASK the user about the debug viewer

**Ask before launching anything.** The viewer is browser-based, single-user, and only useful when the user wants to drill into FAIL clips. Don't assume.

```
Multi-test done. Evidence bundle ready at:
  ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}.zip

Want to open the SRR debug viewer in browser?
  (scrub-sync per-clip MP4 ↔ GT positions ↔ PSF state ↔ BA events ↔ pss.log)

[y]es, launch viewer  /  [n]o, just give me paths
```

If user says **no**: print final paths block and stop:
```
Output:
- Top-level summary: ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/summary.md
- Per-clip reports: ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/<run>/reports/scn_*.md
- Per-clip videos:  ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/<run>/videos/scn_*.mp4 (if VST pull succeeded)
- Evidence bundle:  ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}.zip
- PSF log snapshot: ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/pss.log
- Perception heatmaps (if rendered in Step 6e): ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/perception_heatmaps[_fovmask]/
```

If user says **yes**: continue to Step 6c.

### Step 6c — Viewer handoff (only if user opted in)

Default location: `<repo>/tools/srr-debug-viewer` (ships in this repo). Resolve it from the SRR dir:

```bash
VIEWER_DIR="${VIEWER_DIR:-$(git -C "$SRR_PIPELINE_DIR" rev-parse --show-toplevel)/tools/srr-debug-viewer}"
```

Symlink (don't copy) the multi-test dir into the viewer's `data/` so disk usage stays flat:

```bash
mkdir -p "$VIEWER_DIR/data"
ln -sfn "${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}" "$VIEWER_DIR/data/multi-test-${TIMESTAMP}"
```

**Update `data/index.json`** — the viewer prefers this explicit list when present (`landing.js discoverRuns`). The HTTP directory-listing fallback works but isn't ordered, so always keep `index.json` authoritative:

```bash
INDEX="$VIEWER_DIR/data/index.json"
RUN="multi-test-${TIMESTAMP}"
if [[ -f "$INDEX" ]]; then
  # Prepend the new run (newest-first) if not already in the list
  python3 -c "
import json, sys
p = '$INDEX'
d = json.load(open(p))
runs = d.get('runs', [])
if '$RUN' not in runs:
    runs.insert(0, '$RUN')
    d['runs'] = runs
    json.dump(d, open(p, 'w'), indent=2)
    print(f'index.json: added $RUN (now {len(runs)} runs)')
else:
    print(f'index.json: $RUN already listed')
"
else
  echo '{"runs": ["'"$RUN"'"]}' > "$INDEX"
  echo "index.json: created with $RUN"
fi
```

**Pre-check port 8765 before launching** — a stale `view.sh` from a prior session listens forever and can be silently reused (sometimes fine, but masks viewer crashes):

```bash
if lsof -iTCP:8765 -sTCP:LISTEN -n 2>/dev/null | grep -q LISTEN; then
  PID=$(lsof -tiTCP:8765 -sTCP:LISTEN -n 2>/dev/null)
  echo "Port 8765 already in use by PID $PID (likely a prior viewer)."
  # ASK user: reuse / kill+relaunch / different port
  # → reuse: skip view.sh, just print URL
  # → kill: kill $PID then continue to view.sh below
  # → other port: pass it as $1 to view.sh, e.g. bash view.sh 8766
fi
```

Launch the viewer (foreground `python3 -m http.server` — Ctrl+C to stop). On most setups it auto-opens the browser at `http://localhost:8765/`:

```bash
bash "$VIEWER_DIR/view.sh"
```

If `xdg-open` is missing (headless / SSH-only host), `view.sh` just prints the URL — surface it to the user:
```
Viewer running at http://<host-ip>:8765/
Open in browser and click the multi-test-${TIMESTAMP} card.
Ctrl+C in the terminal to stop the server.
```

> Note: viewer is single-user, no auth — fine for local dev, not for shared deployment. See [`quickstart.md`](../../../closed-loop-testing/regression-reporter/docs/quickstart.md) for the full setup and [srr-debug-viewer README](../../../tools/srr-debug-viewer/README.md) for the data schema.

### Step 6d — HEVC transcode fallback (only if viewer reports stuck video)

VST records HEVC (H.265); Firefox + most Chromium on Linux don't decode it. If the user reports per-clip MP4 panels spinning forever:

```bash
bash "$VIEWER_DIR/transcode_to_h264.sh" \
  "$VIEWER_DIR/data/multi-test-${TIMESTAMP}"
```

This rewrites every `<run>/videos/scn_*.mp4` in place to H.264 (preserves audio, drops ~30% size). Don't run preemptively — adds ~1 min per clip.

### Step 6e — Perception heatmaps + coverage polygons (optional, on request)

Spatial visualizations of *where* perception fails — world-coordinate heatmaps of
detect-fail% / tracking-loss% per class, occupancy density, and the coverage region
the aggregator scores against. **Prerequisite:** `clip_logs` (Step 6a) must have run —
the renderers read `clip_logs/<scn>/{gt_positions.csv, tracker_state.csv, detections.csv}`.
Both scripts + `matplotlib` are baked into the srr image (`/app/scripts/`), so run them
via `docker exec`. They are **manual post-process** — NOT invoked by `run_multi.sh` or
the aggregator.

**Detect-fail / tracking-loss heatmaps** (`render_perception_heatmap.py`):

```bash
docker exec srr python3 /app/scripts/render_perception_heatmap.py \
  --runs-dir /app/runs/multi-test-${TIMESTAMP} \
  --calib    /app/calibration.json \
  --out      /app/runs/multi-test-${TIMESTAMP}/perception_heatmaps \
  [--density]            # also render occupancy maps (frames/bin)
  [--fov-mask]           # mask bins outside calibrated camera FOV (use --out ..._fovmask)
  [--scenario in-roi-5min] [--bin-m 0.3] [--min-visits 5]
```

Output under `--out`: `_combined/{person,forklift}_{detect_fail,tracking_loss}.png`
(+ `_density.png` with `--density`), plus a `<scenario>/` subdir per scenario. By
convention, run a second pass with `--fov-mask --out .../perception_heatmaps_fovmask`
to compare FOV-clipped vs convex-hull coverage (the FOV mask is what dropped person
detect-fail 33.9% → 4.6% in the 2026-06-15 analysis — see the aggregator's
`compute_phase2_metrics` in `srr-service/srr/aggregator.py`).

**Coverage polygons** (`render_coverage_polygons.py`) — FOV union vs convex-hull+pad
coverage, with raw detection scatter:

```bash
docker exec srr python3 /app/scripts/render_coverage_polygons.py \
  --runs-dir /app/runs/multi-test-${TIMESTAMP} \
  --calib    /app/calibration.json \
  --out      /app/runs/multi-test-${TIMESTAMP}/coverage_polygons \
  [--per-clip] [--scenario in-roi-5min] [--pad-m 2.0]
```

Output: `<out>/<scenario>.png` + `<out>/_combined.png` (+ `<out>/<scenario>/<scn>.png`
with `--per-clip`).

> If either script fails with `ModuleNotFoundError: matplotlib` or `FileNotFoundError`
> on `/app/scripts/...`, the image is stale — rebuild or `docker cp` the scripts (see
> [02_launch.md](02_launch.md) Step 3.−2).
