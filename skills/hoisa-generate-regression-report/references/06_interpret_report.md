# Phase 6 — Interpret the Report

After Phase 4 (per-scenario aggregator) and the cross-run rollup at the end of Phase 5, the agent has a full report tree. This doc explains **what each file contains, how to read every metric, and what patterns to look for**.

Use this when the user asks for a verdict, when triaging a failure, or when summarizing for stakeholders.

---

## File layout

```
runs/multi-test-${TIMESTAMP}/
├── summary.md                      ← top-level (cross-run): Phase 1 + Phase 2
├── failures.json
├── pss.log                         ← cross-run Safety Core gateway log (concat of per-scn)
├── perception_heatmaps[_fovmask]/  ← optional (Phase 6e): detect-fail/tracking-loss/density PNGs
├── coverage_polygons/              ← optional (Phase 6e)
└── <run-label>/
    ├── scenes/scn_*.parquet
    ├── videos/scn_*.mp4 + cam0.mp4
    ├── pss.log                     ← per-scn snapshot
    ├── clip_logs/<scn>/            ← evidence CSVs: gt_positions, psf_timeline, ba_events,
    │                                  detections, tracker_state, ba_positions + manifest.json
    └── reports/
        ├── summary.md              ← per-run rollup
        ├── failures.json
        └── scn_*.md                ← per-clip detail (Phase 1 + Phase 2 per-class table)
```

| File | Contents |
|---|---|
| **Top-level** `summary.md` | Validity-guard banners (PSF_FEED_DEAD / PSF feed gaps / GT_FROZEN / per-scenario PERCEPTION_LIKELY_DEAD / analyze-errors, when they fire — see "Validity-guard banners" below) + Headline + per-run rollup table + 66-clip detail table with mp4 links. **Open this first.** |
| **Per-run** `<run>/reports/summary.md` | Same structure, scoped to one scenario; per-clip table only |
| **Per-clip** `<run>/reports/scn_*.md` | The actual ground truth, the deepest evidence. GT events vs Safety Core state, mismatch windows with timestamps, BA detection per event, links to MP4 |
| **failures.json** | List of clips with verdict=FAIL (match% < 80) and key fields, plus clips that failed analysis (`category: analyze_error`, e.g. incomplete GT TF — excluded from all headline numbers), ready for programmatic triage |

---

## Headline section — what each line means

```
- total clips: **66**
- verdicts: ✅ 37 PASS (56%) · ⚠ 10 NEAR (15%) · ❌ 19 FAIL (29%)
- match%: mean 82.5 · median 96.8 · min 5.5
- Mute correct%   (system muted when GT said mute):     57.8% (n=12983)
- Unmute correct% (system unmuted when GT said no mute): 89.1% (n=65660)

BA perception — per-event match (window ≤ 3 s):
- ROI entries (Person enters work zone): 84/88 (95.5%) · median 663 ms · p95 1232 ms
- TW IN  (forklift enters trailer):     66/66 (100%)  · median +2115 ms (LATE)
- TW OUT (forklift exits trailer):       66/66 (100%) · median −1569 ms (EARLY)
- TW Forklift events missing direction field: 24

- worst UNMUTE lag: 21100 ms
- worst MUTE lag:   28233 ms
- mismatch frame totals — over-mute: 7148 · under-mute: 5473
```

| Metric | What it measures | Healthy range |
|---|---|---|
| **PASS / NEAR / FAIL** | Per-clip verdict bucket — PASS ≥ 95 % match, NEAR 80–95 %, FAIL < 80 % | ≥ 70 % PASS for a healthy run |
| **match%** | Frame-by-frame agreement (expected_mute == actual_mute) at 30 Hz | mean ≥ 90 %, median ≥ 95 % |
| **Mute correct%** | Of frames GT says SHOULD mute, fraction system actually muted (= 1 − under-mute / expected_mute) | ≥ 90 % |
| **Unmute correct%** | Of frames GT says should NOT mute, fraction system actually unmuted (= 1 − over-mute / expected_unmute) | ≥ 95 % (this is the **safety-critical direction** — under-mute = alarm suppressed when person is present) |
| **`(n=X)`** | Frame count for the metric — small N (< 500) means the run barely tested that direction; treat the % with caution |
| **`n/a (unmute-test scenario)`** | Auto-detected: avg chars-in-ROI ≥ 95 % across the run → mute is essentially never expected; mute-correct meaningless |
| **`n/a (mute-test scenario)`** | Auto-detected: avg chars-in-ROI ≤ 5 % across the run |
| **BA ROI** | Per-event detection rate for "person enters ROI" events. BA fires while person is INSIDE; we match each GT entry to nearest BA event within ±3 s | ≥ 95 %; median delay ~600–800 ms is normal |
| **BA TW IN/OUT** | Per-event detection rate for forklift trailer crossings. Direction field comes from BA. | 100 % expected after run-pool fix |
| **TW events missing `direction`** | Raw count of BA TW Forklift events that didn't include the `direction` field. ~10 % is normal; means VSS/perception dropped the field on those emissions | ≤ 15 % |
| **worst UNMUTE/MUTE lag** | Max time in ms between a GT transition and the matching Safety Core state change. Long lag (> 3 s) signals state-machine drift | UNMUTE lag ≤ 1 s (safety-critical), MUTE lag ≤ 5 s |
| **mismatch frame totals** | Disjoint counts of over-mute (suppressed when shouldn't) vs under-mute (didn't suppress when should). High under-mute = annoying false alarm; high over-mute = **alarm suppressed when person present** = safety-critical |

---

## Validity-guard banners — check these FIRST (a fired banner = headline UNTRUSTWORTHY)

The aggregator prepends these to `summary.md` when a run's inputs are unreliable — the numbers below them were computed on a broken measurement rig. Do not report the headline until you have accounted for any that fired.

| Banner | Trigger | What it means |
|---|---|---|
| `🚫 PSF_FEED_DEAD` | no `/safety/is_muted` message in ANY clip | the whole PSF feed was dead → every mute/unmute number is fabricated (missing frames default to unmuted) |
| `⚠️ PSF feed gaps` | N clip(s) with no `/safety/is_muted` data (`pct_psf_data` < 100 somewhere) | partial PSF coverage → those clips' match% is inflated |
| `🚫 GT_FROZEN` | N clip(s) where the forklift GT never moved while BA reported TW crossings | frozen `/gt` TF → `expected_mute` graded against wrong ground truth |
| `🚫 PERCEPTION_LIKELY_DEAD` (pooled) / `… in <run>` (per-scenario) | BA per-event match < 0.50 and Phase-2 not alive | perception (not just SRR) likely dead → match% meaningless. The per-scenario variant catches one dead scenario the pooled rate would dilute |
| `⚠️ N clip(s) failed analysis` | clips that raised an analysis error (e.g. a `/gt/*/tf` that never published) | those clips are EXCLUDED from every headline number; they appear in `failures.json` with `category: analyze_error` |

---

## BA perception — read carefully

BA emits events asymmetrically:

| Event class | Schema | Direction info | Behaviour |
|---|---|---|---|
| ROI ⛔ Person | `{event_types: [ROI], classes: [Person], ids: [roi-id-1], _kafka_ts}` | None | Fires continuously while inside; **no exit event** — BA simply stops firing |
| TW ➡ Forklift | `{event_types: [TW], classes: [Forklift], direction: Right\|Left, ids: [tripwire-id-1], _kafka_ts}` | `direction` (`Right`=IN, `Left`=OUT) | Burst of 3–5 events per crossing; ~10 % drop the direction field |

### Why TW IN delay is **+2 s** and TW OUT delay is **−1.5 s**

BA detects TW crossing via **bbox-edge intersecting the wire**; SRR GT marks the crossing when the **centroid** crosses. For a ~3–4 m forklift moving at ~1 m/s:

```
TW IN  (right-bound):  bbox front-edge crosses first → BA fires 2 s before centroid → "BA late by +2 s vs GT"
TW OUT (left-bound):   bbox front-edge crosses first → BA fires 1.5 s before centroid → "BA early by −1.5 s vs GT"
```

Both polarities are explainable; **don't read negative delay as a bug**. The metric just shows BA's bbox-vs-centroid offset.

### ROI exits — measured as "trailing fire"

Since BA emits no ROI exit event, each per-clip report shows: for each GT exit, how long after that did BA continue firing within a 5 s window. Typical: 0–2 s. Long trailing (> 4 s) means BA tracker hung on the person leaving (e.g. fast motion, partial occlusion).

---

## Phase 2 — Perception metrics (3D detection + tracking)

Everything above is **Phase 1** — the Safety Core *decision* (mute/unmute) scored against GT
safety truth. The aggregator also emits a **Phase 2** section scoring the **perception
upstream of Safety Core**: the `mdx-bev` 3D detections (Sparse4D `bbox3d` centre) and
`mdx-behavior` BA positions vs Isaac GT TF. Present only when the run captured the 3D
pipeline columns (`detections_json` non-null). **The formula source of truth is
`srr-service/srr/aggregator.py` → `compute_phase2_metrics`; read it before
answering any perception-metric question.**

### The two headline questions (don't conflate them)

| Metric | Question it answers | What it is |
|---|---|---|
| **detect-fail%** (JSON key `track_loss_pct`) | *"Was the object SEEN this frame?"* → **detection** | `100 − recall` in coverage = % of in-coverage GT frames with **no** matched detection (within the 1.5 m gate). Ignores track id. **Renamed 2026-06-12** from the old misnomer "track-loss%". |
| **tracking-loss%** (`100 − tracking_ok_pct`) | *"Did the object KEEP one identity?"* → **tracking** | % of an actor's *matched* frames that sit off its dominant track-id (id-switch / fragmentation). Scored only where the object was already seen, so independent of detect-fail%. |

Both are reported **per class** (🧍 Person / 🚜 Forklift) — the blended number hides the
forklift, which fails far more (deep-in-trailer occlusion). Always read per-class.

### Position error: anchor offset vs jitter (the "off by 0.5 m" trap)

The match distance has two components, reported separately:
- **`pos median` = anchor offset.** GT and the detector use different reference points
  (GT `SkelRoot`/`body` vs 3D-box centre). Person ≈ 0.10 m; forklift *used to* be
  ~0.35–0.39 m. **This is geometry, not detector error.** Since 2026-06-15 the
  aggregator estimates + removes the forklift `body`-vs-centre bias (per-frame heading
  from GT velocity), so forklift median now sits ~0.08 m. **Do not read median as
  "tracking is N m wrong".**
- **`pos p95` = real localization jitter.** This is the spread you actually care about.

Multi-gate recall (`@0.5m / @1.0m / @1.5m`) is also shown — a loose 1.5 m gate can let a
near-miss or a split/ghost track count as a hit; the tighter gates expose what the
headline gate absorbs.

### Coverage — "only score what the camera can see"

A GT actor outside camera view is **not** charged as a miss. Coverage region `Ω` =
**convex hull of all detections, pad = 0** (`coverage_mode: convex_hull+pad`, changed
2026-06-15 from a 2 m pad that bled past the camera footprint and over-reported
detect-fail). `recall` is in-coverage; `recall_raw` is unfiltered; `out-cov%` is how
often the actor was outside coverage.

> **Known limitation — coverage is detection-derived, not FOV-derived.** A region the
> camera sees but where nothing was *ever* detected (forklift fully LOST deep in the
> trailer) can fall outside the hull and be silently excluded. Cross-check with the
> **trailer-boundary slice** (`forklift_boundary`, ±2 m of the tripwire) and
> `recall_raw`. Empirically boundary recall ≈0.96 vs ~0.60 overall → the loss is deep
> in the trailer, not at the edge. The proper fix (per-camera FOV polygon from
> `calibration.json`) is backlogged; `--fov-mask` on the heatmap script (Phase 6e)
> approximates it for visualization.

### Other Phase 2 numbers

| Metric | Meaning |
|---|---|
| **precision / recall / F1** | Standard, per class. precision = of detections, how many matched a GT actor (1 − FP rate). |
| **id-switches / switch-rate** | Raw count / length-normalised count of track-id changes for an actor (re-track / split symptom). |
| **split frames %** (`frames_with_split_pct`, `by_class_split_pct`) | % of frames with two same-class tracks within 1.0 m (one physical object re-tracked under multiple ids — the "many ids for one object" signal). |
| **`unique_tids_by_class`** | Distinct track ids seen per class over the clip. |
| **`ba_pos_mae_m`** | Mean error of `mdx-behavior` BA positions vs nearest GT (separate from the bev detections). |
| **`unknown`-class detections** | mdx-bev emits ~90/clip; they match no Person/Forklift GT → counted as FP or ignored. Not a localisation error. |

### Per-scenario perception rollup + degenerate flag

- **Per-scenario perception rollup** — per-class recall + detect-fail% + tracking-loss%
  + id-switches *per scenario*, so detection/tracking quality is comparable scene-by-scene
  instead of one global blend.
- **Degenerate (mute-untestable) clips** — when a person is always in ROI there is never
  an expected-mute frame, so the clip only tests UNMUTE and trivially PASSes. The headline
  flags how many such clips exist + how many PASS, so a blended PASS% isn't mistaken for
  mute reliability.
- **`∞ (never)` reaction lag** — now shown when the system never reached the required
  state for any frame that needed it (previously rendered as a benign "–" and the headline
  "worst lag" silently skipped it).

### Caveat — `track_loss_pct` JSON key

For backward compat the JSON / manifest key for **detect-fail%** is still
`track_loss_pct`. When reading raw `manifest.json` / `phase2_metrics`, `track_loss_pct`
= detect-fail (detection), and `tracking_ok_pct` / `tracking-loss%` is the genuine
tracking number. The rendered `.md` / viewer use the new labels.

---

## Per-run rollup — what to look at

```
| Run | Avg match% | Mute correct% | Unmute correct% | PASS / NEAR / FAIL | BA ROI | BA TW in | BA TW out |
```

Read pattern:
1. **Avg match%** + **PASS / NEAR / FAIL** for at-a-glance health.
2. **Mute / Unmute correct%** to see WHICH direction is failing. Unmute-low = safety-critical. Mute-low = false-alarm noise.
3. **BA ROI / TW** should always be ≥ 95 % matched after the run-pool fix. If any drops below, something changed in VSS/perception — escalate.
4. **`n/a` labels** mean the run's design didn't exercise that direction; ignore the column.

### Healthy-vs-degraded comparison (2026-04-30 baseline)

| Pattern | What it looks like | What it means |
|---|---|---|
| **Best** | `psf-edge-5min`: 95.6 % match · 100 % unmute · 84 % mute · 0 fail | Fresh Safety Core state; drift hasn't accumulated. Use as control baseline for any future regression. |
| **Length-driven drift** | `fast-20min`: 82.4 % match · 86 % unmute · 9 fails | Counter accumulates ENTRY/EXIT errors over time → state diverges from reality |
| **Cold-start (in-roi)** | mute% = `n/a (unmute-test scenario)`; ~226 frames marked under-mute | Walking-into-ROI window at start; not a real test failure |

---

## Per-clip detail — when to drill down

Open `<run>/reports/scn_*.md` when:
- Verdict = ❌ FAIL (match% < 80 %)
- Worst-lag clip (compare clip's max lag against the run's max in summary)
- Run shows odd Mute/Unmute correct% mismatch

Each per-clip MD has:

| Section | Use |
|---|---|
| **Verdict + Window** | Quick gist: PASS/FAIL, ISO times for cross-referencing |
| **Video link** | Open the MP4 directly to see what reality looked like |
| **Ground truth** | %char_in_roi, %forklift_in_trailer, %expected_mute — what the test expected |
| **Observed Safety Core state** | %actual_mute + command breakdown — what Safety Core did. May also carry a `⚠ PSF feed coverage: <pct>%` line (frames with no real `/safety/is_muted` message; the rest were scored as unmuted) and a `🚫 GT_FROZEN` line (forklift GT froze → this clip's expected_mute + verdict are untrustworthy) |
| **Match — over-mute / under-mute** | Direction of the mismatch. Under-mute = safety-critical |
| **Mismatch windows** | **Most useful section.** Lists every contiguous (≥ 0.5 s) window with `start/end/duration/direction`. Reviewer can `ffmpeg -ss <start>` directly to inspect that exact moment in the MP4 |
| **Reaction lag** | UNMUTE/MUTE: median + p95 + max + transition count |
| **BA detection per GT event** | The actual `GT entry → BA detected` table with delay per event. If BA missed something here, tracker might have lost the object |

### Drill-down workflow (for a FAIL clip)

```
1. Open the per-clip MD
2. Look at over-mute% vs under-mute% — which direction failed?
3. Read the Mismatch windows table — when (in clip-relative seconds) did the bad behaviour happen?
4. Open the MP4, scrub to that timestamp, see what the cameras saw
5. Cross-check BA detection table: did perception have valid input at that moment?
6. If yes → Safety Core logic / counter drift; if no → perception issue
```

---

## Common patterns and red flags

| Symptom | Likely cause | Action |
|---|---|---|
| **Mute correct% < 50 %** in a long run | Counter drift accumulated (known Safety Core counter-drift issue) | Restart Halos compose; rerun shorter scenario. **Don't blame as a new regression** — see Counter-drift signature note below. |
| **Unmute correct% < 90 %** | **Safety-critical** — alarm suppressed when person present (counter went negative). Pattern matches the known Safety Core counter-drift issue. | Open the FAIL clips' MD; cross-check BA ROI events were emitted; if BA was firing but Safety Core still muted → counter bug (Direction A) |
| **BA ROI < 95 %** | Person tracker lost something — partial occlusion, motion blur, or VSS regression | Open the missed-clip MD; look at the "extra BA events" count; check `pct_any_char_in_roi` |
| **BA TW IN/OUT < 100 %** | Used to mean scene-boundary spillover (now fixed via run-pool) — if it reappears, BA pool didn't load. | Verify `<run>/run-*.parquet` exists and is non-empty |
| **24 / 246 TW events missing direction** | VSS/perception drops `direction` on some emissions (closed-source) — known | No action; counted in raw total only |
| **Mute lag > 10 s** | Counter-drift residual; Safety Core state stuck | Restart between runs is the workaround |
| **Mismatch window covers entire clip** | Cold-start (Safety Core newly booted) OR the scenario is fundamentally ill-defined for the test (e.g. chars walking into ROI mid-clip) | Check scn_0000 specifically; if cold-start, expected; otherwise drill down |
| **`n/a` on Mute correct%** | Unmute-test scenario auto-detected (chars ≥ 95 % in ROI) | Don't try to interpret; that direction wasn't tested |
| **🚜 forklift detect-fail% high (30–40%) while 🧍 person is low (<5%)** | Forklift LOST **deep in the trailer** (genuine occlusion, outside camera FOV), not a detector regression | Cross-check the trailer-boundary slice (≈0.96 recall at the edge) + `recall_raw`; render the `--fov-mask` heatmap. Expected pattern, don't open a bug. |
| **forklift `pos median` ~0.35 m** | Reading a *stale* report (pre-2026-06-15) where the `body`-vs-box-centre anchor offset wasn't removed | Re-run the current aggregator; median should drop to ~0.08 m. The 0.35 m is geometry, never "tracking 0.35 m wrong". |
| **detect-fail% suspiciously high everywhere** | Coverage pad bleeding past FOV (only if running pre-`pad=0` code), or perception genuinely dead | Confirm `coverage_mode: convex_hull+pad`, `coverage_pad_m: 0`; if perception dead, the PERCEPTION_LIKELY_DEAD banner fires |
| **tracking-loss% high but detect-fail% low** | Object is seen every frame but the tracker fragments its id (split / re-track) | Check `id-switches` + `split frames %`; a re-acquire after leaving+re-entering the sensor is currently over-counted (known limitation — segment-aware fix backlogged) |

### Counter-drift signature — recognize, don't blame

The Safety Core counter-drift bug is a **known upstream Safety Core issue**, acknowledged by the
Halos Safety Core team, with a fix planned post-v1.3.
This means **every SRR run through v1.3 will surface this bug** —
recognize the signature so you don't waste time re-investigating it as
a new Safety Core regression:

- `Unmute correct% < 90%` (especially when paired with high `%char_in_roi`
  and forklift-in-trailer activity) → Direction A (over-mute, safety-critical)
- Alternating PASS/FAIL pattern aligned with forklift_in_trailer state
  (FAIL when fk in, PASS when out) → classic counter-drift
- Safety Core muted N% while GT-expected-mute was 0% → Safety Core internal state has
  drifted out of sync with reality
- `pss.log` shows `ATL: Human present (ROI: Yes, HiT: 0)` while BA
  reported 0 ROI events → decision was made on stale counter, not on
  current perception input

When you see this pattern: log it, and file a GitHub issue with the FAIL clip
attached if it's an extreme case (e.g. > 95% over-mute), then move on.
**Don't open a new bug for each occurrence.** If you see Direction A
signature WITHOUT the alternation pattern (e.g. one-off random mute mid-
clear-clip), that's something different — investigate fresh.

---

## Programmatic extraction

For agents / scripts that need to extract headline numbers without parsing markdown tables:

```python
import re
from pathlib import Path

txt = Path(f"runs/multi-test-{TS}/summary.md").read_text()

# Match headline lines
m_pass = re.search(r"✅ \*\*(\d+) PASS\*\*", txt)
m_near = re.search(r"⚠ (\d+) NEAR", txt)
m_fail = re.search(r"❌ \*\*(\d+) FAIL\*\*", txt)
m_mean = re.search(r"match%: mean ([\d.]+)", txt)
m_med  = re.search(r"median ([\d.]+) · min", txt)
m_mute = re.search(r"\*\*Mute correct%\*\*[^*]*\*\*([^*]+)\*\*", txt)
m_unm  = re.search(r"\*\*Unmute correct%\*\*[^*]*\*\*([^*]+)\*\*", txt)
m_roi  = re.search(r"ROI entries[^*]*\*\*(\d+/\d+)\*\* \(([\d.]+)%\)", txt)
m_twin = re.search(r"TW IN[^*]*\*\*(\d+/\d+)\*\* \(([\d.]+)%\)", txt)
m_twou = re.search(r"TW OUT[^*]*\*\*(\d+/\d+)\*\* \(([\d.]+)%\)", txt)

# Worst lag
m_uml  = re.search(r"worst UNMUTE lag[^*]*\*\*([\d.]+) ms\*\*", txt)
m_ml   = re.search(r"worst MUTE lag[^*]*\*\*([\d.]+) ms\*\*", txt)
```

Or just `failures.json` for a clean list of the FAIL and analyze-error clips with all fields.

---

## When you're done analyzing

Print a 3-bullet executive summary in the demo log:

```
> Analysis complete.
  • <X> of 5 scenarios passed the safety target (avg match ≥ 90 %)
  • Best run: <label> at <match%> (0 failures)
  • Open issue: <one-line root cause from per-run rollup>

  Top-level summary: runs/multi-test-<TS>/summary.md
  Drill-down: <run>/reports/scn_<NNNN>.md for any FAIL clip
```

If asked for a stakeholder version, base it on this run's top-level `summary.md`
rollup (Headline + Per-run rollup + Per-clip detail).
