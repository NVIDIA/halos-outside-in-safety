# Phase 4 — Per-Scenario Analysis

After `/srr/record false`, run **per-scenario**: move parquet → snapshot pss.log → tw_split → aggregator → pull MP4. Every step in this list must complete for the CURRENT scenario before the next scenario's `phase_compose_restart` (Step 3a in the main checklist) — otherwise:

- PSF compose restart **truncates** the bind-mounted `/var/log/pss.log` → previous scenario's PSF logs are LOST.
- Isaac scene tear-down **removes VST sensors** → `vst_video split-run` returns `null streams` and no MP4 can be pulled.

This is why the skill MUST drive each scenario's analyze inline, not batch it after all scenarios. See `phase_analyze` in [`run_multi.sh`](../../../closed-loop-testing/regression-reporter/scripts/run_multi.sh) for the reference ordering.

---

## Step 1 — Move parquet to scenario subdir

```bash
PARQUET=$(docker exec srr bash -c "ls -t /app/runs/run-*.parquet | head -1")
docker exec srr mkdir -p "/app/runs/multi-test-${TIMESTAMP}/${LABEL}"
docker exec srr mv "$PARQUET" "/app/runs/multi-test-${TIMESTAMP}/${LABEL}/"

# REQUIRED: docker exec mkdir creates a root-owned dir on the host bind-mount;
# Step 1b's host-side snapshot_pss.sh cp will fail with EACCES without this.
sudo chmod -R a+rwX "${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}"
```

**Sanity check** (cheap, inline — no agent):
```bash
docker exec srr stat -c '%s' "/app/runs/multi-test-${TIMESTAMP}/${LABEL}/run-*.parquet"
```
Expected: parquet size ≥ 100 KB. If smaller, recording failed — surface error and stop.

---

## Step 1b — Snapshot pss.log (per-scn) — FORENSIC, RUN BEFORE NEXT COMPOSE RESTART

The next scenario's `phase_compose_restart` will truncate the source pss.log. Snapshot the current scenario's PSF data NOW, into its scenario subdir. The end-of-multi-test step then concatenates these into the run-level `pss.log` consumed by `clip_logs` default fallback.

```bash
"${SRR_PIPELINE_DIR}/scripts/snapshot_pss.sh" \
  "${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}" --per-scn
# → writes ${LABEL}/pss.log (copy of source at this instant)
```

Skipping this step means only the last scenario's PSF data survives: an end-of-run-only snapshot catches only the LAST scenario's PSF data, and the first scenario's clip_logs ends up with `pss_raw=0` lines. The mtime-window fallback in `snapshot_pss.sh` does not help — the source file itself has been truncated.

---

## Step 2 — tw_split

```bash
docker exec srr bash -c "
  cd /app && python3 -m srr.tw_split \
    /app/runs/multi-test-${TIMESTAMP}/${LABEL}/run-*.parquet \
    /app/runs/multi-test-${TIMESTAMP}/${LABEL}/scenes \
    --source gt
"
```

Log:
```
  [MM:SS] tw_split running...
```

**Ready signal**: when command exits, count clips:
```bash
N=$(docker exec srr bash -c "ls /app/runs/multi-test-${TIMESTAMP}/${LABEL}/scenes/scn_*.parquet 2>/dev/null | wc -l")
```

Log:
```
  [MM:SS] tw_split: ${N} clips identified.
```

---

## Step 3 — Aggregator (`--top-level` mode, idempotent)

**ALWAYS run with `--top-level`** — even for single-scenario tests. Per-scenario
mode (`--runs-dir <run>/scenes --out-dir <run>/reports`) misses BA events that
fire ±2s outside the scene boundary, producing the scene-boundary spillover bug
(typical symptom: `BA TW IN 6/11 (54.5%)` instead of `11/11 (100%)`). Top-level
mode pulls a per-run BA pool from `<run>/run-*.parquet` and re-emits all per-run
summaries with the correct numbers, then writes a cross-run `summary.md` at the
runs-base. Idempotent: rerun any time, output stable.

```bash
docker exec srr bash -c "
  cd /app && python3 -m srr.aggregator \
    --runs-dir /app/runs/multi-test-${TIMESTAMP} \
    --top-level
"
```

(Default `--calib /app/calibration.json` is auto-mounted by compose from `${CALIBRATION_JSON}` in the repo `.env`.)

**Ready signal**: TWO summary files exist (per-run + top-level), both > 1 KB:
```bash
SUMMARY="${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/reports/summary.md"
TOP="${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/summary.md"
for f in "$SUMMARY" "$TOP"; do
  [ -s "$f" ] && [ $(stat -c '%s' "$f") -gt 1024 ] || { echo "$f missing or too small"; exit 1; }
done
HEAD=$(head -1 "$SUMMARY")
# Expected: "# SRR Aggregator — N clip(s)"
```

> The size check guards against an aggregator failure that wrote only the
> `# SRR Aggregator — no clips found\n` stub (≈ 30 bytes). Without the
> guard, downstream metric extraction silently parses an empty file.

**Check for ghost-result banner before reporting headline:**
```bash
# If aggregator detected dead perception, surface that BEFORE match%.
if grep -q "PERCEPTION_LIKELY_DEAD" "$SUMMARY"; then
  echo "  [$(date +%M:%S)] ⚠ aggregator flagged perception likely dead — match% UNTRUSTWORTHY"
  grep "PERCEPTION_LIKELY_DEAD" "$SUMMARY"
fi
```

Extract headline metrics from the headline section (don't parse the per-clip table — column layout has changed):
```bash
# Aggregator now writes a "## Headline" block; grep that.
AVG_MATCH=$(grep -oP 'match%: mean \K[\d.]+'  "$SUMMARY")
FAILS=$(grep -oP '❌ \*\*\K\d+(?= FAIL)' "$SUMMARY")
```

Log:
```
  [MM:SS] aggregator: match% avg ${AVG_MATCH}, failures ${FAILS}/${N}.
```

**Phase 2 (perception) is produced by the same aggregator run** — no extra command.
When the per-clip parquets carry the 3D-pipeline columns (`detections_json` non-null,
i.e. mdx-bev was flowing), `compute_phase2_metrics` adds a Phase 2 section to each
`summary.md` and `scn_*.md`: per-class **detect-fail%** / **tracking-loss%**,
precision/recall/F1, position offset (median) vs jitter (p95), multi-gate recall,
id-switches, trailer-boundary slice, split%, and the per-scenario perception rollup.
If `detections_json` is all-null the Phase 2 section is omitted (Phase-1-only run) —
that's expected, not an error. Optionally surface the headline Phase 2 numbers:
```bash
grep -oP '🧍 .*detect-fail \K[\d.]+%' "$SUMMARY"   # person detect-fail
grep -oP '🚜 .*detect-fail \K[\d.]+%' "$SUMMARY"   # forklift detect-fail
```

For deeper interpretation see [06_interpret_report.md](06_interpret_report.md) (§ Phase 2 for perception metrics).

> Note: pss.log snapshot was moved to **Step 1b above** (per-scn, runs immediately after the parquet is moved — before tw_split, so it captures the source before any potential restart). The end-of-multi-test concat into a run-level `pss.log` happens in [05_report.md](05_report.md).
>
> Per-clip evidence packager (`clip_logs --zip`) and viewer handoff are now in Phase 5 — see [05_report.md](05_report.md) and [Post-process step](#post-process).

---

## Step 4 — Pull per-clip MP4 from VST

**Videos go directly into the run dir** (`<run>/videos/`) — same tree as parquets and reports, so the per-clip MD's `[../videos/scn_X.mp4]` link resolves cleanly. (Old layout under `/sil-data/video/...` is no longer used.)

### Step 4a — VST replay readiness probe (REQUIRED, before pull)

VST indexes recording asynchronously after the RTSP stream completes.
Pulling immediately after `/srr/record false` often returns 503/504 for
30–60 s. Distinguish "VST still indexing" (retry helps) from "VST broken"
(retry never helps — 2026-05-03 ghost-result run burned 5 retries × 30 s):

```bash
# Get the Camera streamId
SID=$(curl -sf "${VST_BASE_URL}/v1/sensor/list" \
       | jq -r '.[] | select(.state=="online") | select(.name=="Camera") | .sensorId' \
       | head -1)

# Poll until a timeline overlapping the record window appears, max 60 s
DEADLINE=$(($(date +%s) + 60))
until [ $(date +%s) -ge $DEADLINE ]; do
  TIMELINES=$(curl -sf "${VST_BASE_URL}/v1/record/${SID}/timelines" 2>/dev/null)
  if [ -n "$TIMELINES" ] && echo "$TIMELINES" | jq -e 'length > 0' >/dev/null 2>&1; then
    echo "  [$(date +%M:%S)] VST timeline ready"
    break
  fi
  sleep 5
done
if [ $(date +%s) -ge $DEADLINE ]; then
  echo "  [$(date +%M:%S)] VST timeline not produced in 60 s — VST may be broken."
  echo "  Skipping mp4 pull. Check: curl ${VST_BASE_URL}/v1/record/streams"
  echo "  Run the VSS reset recipe in SKILL.md if record API returns 503."
  # do NOT proceed to pull
fi
```

### Step 4b — Pull mp4

```bash
docker exec srr bash -c "
  cd /app && python3 -m srr.utils.vst_video split-run \
    --run-dir /app/runs/multi-test-${TIMESTAMP}/${LABEL} \
    --out-dir /app/runs/multi-test-${TIMESTAMP}/${LABEL}/videos
"
```

Log:
```
  [MM:SS] vst_video pulling per-clip MP4...
```

**Ready signal**: count MP4 files matches clip count. Glob BOTH layouts — older container builds (pre-2026-05-04) write to `videos/scenes/scn_*.mp4`, newer ones write flat `videos/scn_*.mp4`. The image-freshness check in [02_launch.md](02_launch.md) Step 3.−2 should keep you on the new layout, but be defensive:

```bash
NV=$(ls ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/scn_*.mp4 \
        ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/scenes/scn_*.mp4 \
     2>/dev/null | wc -l)
```

If `NV < N`, surface a warning but don't fail — VST sometimes lags 1-2 clips. Retry once after 30 s.

If MP4s landed under `videos/scenes/`, the aggregator's per-clip MD video links (`../videos/<scn>.mp4`) will be **broken** — flatten:

```bash
if [ -d "${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/scenes" ]; then
  sudo mv ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/scenes/*.mp4 \
          ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/
  sudo rmdir ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/scenes
fi
```

Log:
```
  [MM:SS] ${NV} clip videos saved to <run>/videos/
  Test K/M completes.
```

---

## Verification via Explore agent (recommended)

After all 4 steps, spawn one Explore agent for end-of-scenario verification:

```
For scenario "${LABEL}" of multi-test-${TIMESTAMP}, verify:

1. Parquet exists and is > 100 KB at:
   ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/run-*.parquet

2. ${N} per-clip parquets exist in:
   ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/scenes/scn_*.parquet

3. summary.md exists with header "# SRR Aggregator — ${N} clip(s)" at:
   ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/reports/summary.md

4. ${N} per-clip mp4 files exist at:
   ${RUNS_HOST_DIR}/multi-test-${TIMESTAMP}/${LABEL}/videos/scn_*.mp4

Report each check as [ok] or [fail: <reason>]. If any fails, this scenario's
results are incomplete — flag for retry. Don't modify anything.
```

If any [fail], do NOT proceed to next scenario. Surface to user.
