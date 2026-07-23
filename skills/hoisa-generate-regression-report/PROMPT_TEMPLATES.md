# Prompt templates for the `hoisa-generate-regression-report` skill

Copy-paste any of these into your coding agent (with the skill enabled). The skill will recognize the intent and invoke its workflow phases automatically.

---

## 1. Quickest demo (1 scenario, default 5 min)

```
Run SRR multi-test with the psf-edge case. Default 5 minutes. Show demo log.
```

→ Wall clock ≈ 13 min (3 setup + 5 record + 30s warmup + 2 analyze + 2 video pull).

---

## 2. Custom subset, default 5 min each

```
Run SRR multi-test with in-roi, psf-edge, and balanced. Default duration. Show demo log.
```

→ 3 scenarios × ~13 min ≈ 40 min wall clock.

---

## 3. Custom subset with per-scenario duration override

```
Run SRR multi-test:
- in-roi for 3 minutes
- psf-edge for 5 minutes
- balanced for 7 minutes

Show demo log with the per-clip TW crossing lines.
```

→ Translates internally to: `in-roi:180 psf-edge:300 balanced:420`.

---

## 4. Full 5-scenario regression at default 5 min each

```
Run SRR full regression — all 5 test cases, 5 minutes each. Show demo log + cross-run REPORT at the end.
```

→ ~65 min wall clock (5 scenarios × 13 min each).

---

## 5. Full multi-test at natural lengths (the original 45-min test)

```
Run SRR multi-test at natural lengths:
- in-roi 5 min
- psf-edge 5 min
- psf-clear 5 min
- balanced 10 min
- fast 20 min

Cross-run report at the end.
```

→ ~1h 42m wall clock — matches the prior multi-test-20260430-081223 run.

---

## 6. Demo-friendly log without doing extra analysis

```
Run SRR demo — psf-edge at 5 min. Optimize the log output for screen recording: show each phase boundary clearly, print every forklift TW crossing as "Clip K" lines, and print final headline (match%, failures, safety-critical unmute%).
```

→ Skill prioritizes the demo log format from `SKILL.md`. Per-clip MP4 still pulled.

---

## 7. Very short single-case smoke test (3 min)

```
Run SRR with just psf-edge for 3 minutes. I want a quick smoke test, not a full report.
```

→ ~10 min wall clock total. Useful when validating a fix without burning a full run.

---

## 8. Two cases at different times each

```
Run SRR multi-test:
- in-roi for 4 minutes
- balanced for 8 minutes
Show demo log + cross-run report.
```

→ Mix-and-match per scenario. Skill encodes as `in-roi:240 balanced:480`.

---

## 9. Quick triage across all 5 cases (2 min each)

```
Run SRR full triage — all 5 cases, 2 minutes each. Just need a quick pass to check the pipeline, full report optional.
```

→ ~50 min wall clock (5 × 10 min). Shortest viable time per scenario before tw_split runs out of forklift cycles. Use to verify pipeline plumbing across all 5 stress patterns before committing to longer runs.

---

## 10. Targeted re-run after fix

```
Run SRR with just balanced, 10 minutes — checking if the Safety Core counter drift fix landed.
```

→ ~22 min wall clock. Use when you've made a code change and want a focused regression check on the scenario that previously failed.

---

## 11. Perception (Phase 2) focus — detection/tracking + heatmaps

```
Run SRR with in-roi and balanced, 5 min each. I care about perception quality —
report per-class detect-fail% and tracking-loss%, and render the spatial heatmaps
(with FOV mask + density).
```

→ Runs the normal pipeline, then in Phase 6 builds the `clip_logs` CSVs and runs
`render_perception_heatmap.py --density --fov-mask` + `render_coverage_polygons.py`.
Skill surfaces the Phase 2 headline (🧍/🚜 detect-fail% / tracking-loss% / pos-offset) and
the heatmap PNG paths. **Requires Sparse4D 3D detections** (`vss-rtvi-cv` → mdx-bev /
`vss-behavior-analytics` → mdx-behavior) to have been flowing for Phase 2. The scene-ready
gate is now MODE-aware — it derives its topic from the VSS deploy MODE (2d → mdx-raw,
3d/mv3dt → mdx-bev) and requires decoded detections > 0, so it no longer specifically
verifies mdx-bev 3D detections; on a 2D deploy the gate passes on mdx-raw. On a 2D-only
feed (no mdx-bev) there is no Phase 2 — the skill detects that at Phase 2 and says so
rather than rendering empty heatmaps.

---

## What you control via prompt

| Knob | How to express |
|---|---|
| **Which scenarios**: any subset of the 5 | `"in-roi"`, `"in-roi and psf-edge"`, `"all 5"`, `"all"` |
| **Per-scenario duration** | `"in-roi for 3 min"`, `"balanced at 8 minutes"`, `"all at 2 min each"` |
| **Default duration** if not specified | 5 minutes (NOT each scenario's natural length) |
| **Whether to skip the cross-run report** | `"smoke test"` / `"skip the cross-run report"` |
| **Demo log emphasis** | `"show demo log"` / `"optimize log for screen recording"` |
| **Skip warm-up** (faster but expect cold-start outlier on scn_0000) | `"skip Safety Core warm-up"` (not recommended) |

The skill parses these from the natural-language prompt — no rigid syntax required.

---

## Tips

- **Default is 5 min** per scenario. To run a scenario at its natural length, say so explicitly (`"balanced for 10 minutes"` or `"all at natural lengths"`).
- **Skill always restarts both Halos and SRR compose** between scenarios (do NOT ask it to skip — Safety Core counter drift carries across runs and corrupts the next).
- **Safety Core cold-start outlier** is expected on scn_0000 (mute_lag spikes). Skill inserts a 30 s warm-up between scene-ready and `/srr/record true` to mitigate, but the first clip may still be slightly worse than the rest.
- **Live clip monitor** prints one line per forklift TW crossing during recording — these are the demo's "heartbeat" between phase headers.
- **Cross-run REPORT.md** is produced in Phase 5 — stored at `runs/multi-test-${TIMESTAMP}-REPORT.md`.

---

## What the skill does NOT do

- **Does not deploy Halos / VSS** — that's the `hoisa-deploy-profile` (Halos SIL) and `vss-deploy-profile` (VSS Warehouse) skills. Run those first if the SIL is not already up.
- **Does not edit scenarios** — the IRA 1.6 behavior trees in `../scenarios/behavior-trees/` are pre-built. To create a new stress scenario, re-run `../scenarios/tools/randomize_paths.py --name <name>` with different flags (this writes `srr_<name>_char{0,1,2}.bt.json` into `../scenarios/behavior-trees/`), run `../halos-integration/sync_to_halos.sh` (copies the trees into the Halos SIL dir), then add the name to `SCENARIO_DURATIONS` mental model + the skill's scenario list.
- **Does not send reports anywhere** — output is filesystem only. Wire up notifications later if needed.
