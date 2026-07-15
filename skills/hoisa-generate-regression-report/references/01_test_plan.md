# Phase 1 — Generate Test Plan

Parse user request, build scenario list, validate environment.

---

## Inputs from the user

User specifies one of:
- A subset by name: e.g., `"in-roi, psf-edge, balanced"`
- All 5: `"all"`, `"full multi-test"`
- A single name: `"just fast-20min"`

If user did not specify, **ask which scenarios** before proceeding (do NOT default to all 5 — that's 1.7 hours).

---

## 5 available scenarios

| Name | RECORD_S | Behavior trees (IRA 1.6) |
|---|---:|---|
| `in-roi`    |  300 | `srr_in-roi_char{0,1,2}.bt.json` |
| `psf-edge`  |  300 | `srr_psf-edge_char{0,1,2}.bt.json` |
| `psf-clear` |  300 | `srr_psf-clear_char{0,1,2}.bt.json` |
| `balanced`  |  600 | `srr_balanced_char{0,1,2}.bt.json` |
| `fast`      | 1200 | `srr_fast_char{0,1,2}.bt.json` |

Each scenario is 3 IRA 1.6 behavior trees `srr_<name>_char{0,1,2}.bt.json` at
`${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/configs/` (GitHub layout:
`<halos-repo>/closed-loop-testing/isaac-sim/sil/configs/`), emitted directly by
`scenarios/tools/randomize_paths.py` (canonical source: `scenarios/behavior-trees/`).

---

## Demo log output for this phase

```
> Generating testing plan...
  Selected: <name1> (<min1> min), <name2> (<min2> min), ...
  Total recording: <sum> min   Estimated wall clock: ~<sum + 8*N> min
  Output base: /app/runs/multi-test-<TIMESTAMP>/
```

Where `<TIMESTAMP>` = `date +%Y%m%d-%H%M%S` (capture once, reuse for all sub-paths).

End the phase with:
```
> Planning complete.
```

---

## Pre-check via Explore agent

Before declaring planning complete, spawn one Explore agent with this prompt:

```
Verify the SRR multi-test environment is ready:
1. Halos deployment dir exists at ${HOISA_ROOT_PATH}/deployments/ (holds compose.yaml + profiles/)
   and `docker compose --env-file ${HOISA_ROOT_PATH}/deployments/profiles/sil.env ps` shows the configured services (safety-core, comm-layer, isaac-sim).
2. SRR compose dir exists at ${SRR_SERVICE_DIR}/
   and `docker compose ps` shows srr service.
3. The selected scenarios' behavior trees exist:
   <list of srr_<name>_char{0,1,2}.bt.json paths>
4. The stock scene file exists:
   ${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/scenes/indicator_warehouse_20x20_layout_overflow_test.usd
5. Output base dir is writable; free disk on the runs output filesystem ≥ 10 GB.
6. VST API health (cheap fail-fast probes):
   a. `curl -sf -o /dev/null -w '%{http_code}' "${VST_BASE_URL}/v1/record/streams"` → must be 200
   b. `curl -sf "${VST_BASE_URL}/v1/sensor/list" | jq '[.[] | select(.state=="online") | select(.name | test("^Camera(_0[12])?$"))] | length'` → must be ≤ 3
   These same checks repeat per-scenario (Step 3a.5 in 02_launch.md), but
   running them here too means we abort the whole plan in seconds if VST
   is broken instead of attempting the first scenario.

Report each check as [ok] or [fail: reason]. Do NOT modify anything.
```

If any check fails, surface the failure to the user and stop. Do NOT proceed to launch.

---

## Output

A `plan` data structure (in-memory or persisted as `/tmp/srr-skill-plan.json`):

```json
{
  "timestamp": "20260430-153000",
  "output_base": "/app/runs/multi-test-20260430-153000",
  "video_base": "${MDX_DATA_DIR}/video/multi-test-20260430-153000",
  "scenarios": [
    {"label": "in-roi-5min",  "name": "in-roi",  "record_s": 300,  "trees": "srr_in-roi_char{0,1,2}.bt.json"},
    {"label": "psf-edge-5min","name": "psf-edge","record_s": 300,  "trees": "srr_psf-edge_char{0,1,2}.bt.json"},
    ...
  ]
}
```

Pass this plan into Phase 2.
