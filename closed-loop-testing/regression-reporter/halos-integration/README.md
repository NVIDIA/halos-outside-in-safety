# SIL integration

Glue between the SRR test rig and the SIL `docker-compose` stack. SRR lives at
`closed-loop-testing/regression-reporter/`; the SIL infra (safety-core +
comm-layer + Isaac Sim + VST) lives in the rest of this repo. This folder
explains how SRR's fixtures reach the `isaac-sim` container at run time and
provides the tooling to keep them in sync.

## What lives where

| Artifact | Owner | Reason |
|---|---|---|
| safety-core / comm-layer / Isaac docker images + compose | `deployments/`, `closed-loop-testing/` | Canonical SIL infra |
| `default_config_ros.yaml`, `cameras.yaml`, `robots.yaml` | `closed-loop-testing/isaac-sim/sil/configs/` | SIL runtime config |
| Warehouse scene `indicator_warehouse_*.usd` | `closed-loop-testing/isaac-sim/sil/scenes/` | Stock shipping asset (SRR uses it as-is) |
| Per-scenario behavior trees (`srr_<name>_char{0,1,2}.bt.json`) | **`regression-reporter/scenarios/behavior-trees/`** | Isaac 6.0 / IRA 1.6 form — the canonical source fixtures, emitted directly by `tools/randomize_paths.py` |
| Exported NavMesh JSON | **`regression-reporter/scenarios/scenes/navmesh.json`** | Input to `randomize_paths.py` — derived from the stock scene |
| Waypoint generator + validator | **`regression-reporter/scenarios/tools/`** | `randomize_paths.py`, `validate_waypoints.py` |
| Runtime `/gt/*` publisher (OmniGraph builder) | **`closed-loop-testing/isaac-sim/sil/scripts/action_graphs/srr_ground_truth.py`** | Built at run time behind the `--srr-gt` flag; no scene edits |
| Isaac Sim Script Editor utilities | **`regression-reporter/scenarios/isaac-scripts/`** | NavMesh bake/debug + GT prereq diagnostics |
| Host-side GT-topic probes / USD inspectors | **`regression-reporter/scenarios/host-scripts/`** | SRR-specific debug |
| SRR aggregator / recorder | **`regression-reporter/srr-service/`** | SRR pipeline proper |
| SRR skill | **`skills/hoisa-generate-regression-report/`** | Report orchestration |

## How the data flows at run time

```
   SIL infra (this repo)                  SRR fixtures (regression-reporter/)
   ─────────────────────                  ──────────────────────
   deployments/compose.yaml               scenarios/scenes/navmesh.json
   default_config_ros.yaml      ⟵──┐      scenarios/behavior-trees/srr_*.bt.json
   cameras.yaml                    │
   profiles/sil.env                │
   safety-core / VST / comm-layer  │
   isaac-sim container             │
       │                           │
       │  bind-mount               │  sync step (cp)
       │  closed-loop-testing/     │
       │    isaac-sim/sil/         │
       │  → /isaac-sim/sil/        │
       │                           │
       ▼                           │
   /isaac-sim/sil/scenes/          │  (stock warehouse scene, unchanged)
   /isaac-sim/sil/configs/  ⟵──────┘  (behavior trees + navmesh land here)
   /isaac-sim/sil/scripts/
```

When Isaac Sim starts inside the compose stack, it opens whatever scene path
`default_config_ros.yaml` points to — the stock warehouse scene at
`/isaac-sim/sil/scenes/<scene>.usd`, the in-container path of the Isaac SIL dir
`${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/scenes/` (GitHub layout: `closed-loop-testing/isaac-sim/sil/scenes/`).
The `/gt/*` ground-truth publishers are **not** baked into the scene — they're
built at run time by `action_graphs/srr_ground_truth.py` when Isaac is launched
with `--srr-gt` (see `run_multi.sh`).

So SRR's behavior trees + navmesh must end up in that dir before Isaac launches.
There are two patterns for that, see below.

## Integration patterns

### Pattern A — sync at deploy time (current, simpler)

The SRR skill (or operator) **copies SRR fixtures into the sibling
`isaac-sim/sil/` tree** once after cloning:

```bash
./halos-integration/sync_to_halos.sh $HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil
# Omit the arg to derive the SIL dir from HOISA_ROOT_PATH (set in the SRR .env),
# or fall back to the sibling ../isaac-sim/sil.
```

What it copies (with `SIL = $HOISA_ROOT_PATH/closed-loop-testing/isaac-sim/sil`):

- `scenarios/behavior-trees/srr_*.bt.json` → `$SIL/configs/` (the canonical trees, copied as-is)
- `scenarios/scenes/navmesh.json` → `$SIL/configs/`
- `scenarios/isaac-scripts/**/*.py` → `$SIL/scripts/isaac/`
- `default_config_ros.yaml.template` → `$SIL/configs/default_config_ros.yaml`

Pros: no scene edits, and a one-way copy means anyone with a fresh clone can run
after one sync while the `isaac-sim/sil/` git tree stays pristine.

Cons: changes made directly in `isaac-sim/sil/` **won't be reflected back** to
SRR. Always edit the SRR canonical copy under `regression-reporter/scenarios/`.

When to re-run `sync_to_halos.sh`:
- After cloning the repo for the first time on a new machine
- After SRR behavior-tree / NavMesh updates
- After the Isaac SIL bind-mount layout changes

### Pattern B — bind-mount SRR data dir (planned, cleaner)

The `isaac-sim` service definition (`closed-loop-testing/isaac-sim/isaac-sim.yml`)
gets a parameterized extra mount:

```yaml
services:
  isaac-sim:
    volumes:
      - ${ISAAC_SIL_DIR}:/isaac-sim/sil:ro
      - ${SRR_DATA_DIR:-/dev/null}:/srr-data:ro    # NEW
```

The SRR skill then exports before `docker compose up`:

```bash
export SRR_DATA_DIR=<repo>/closed-loop-testing/regression-reporter/scenarios
```

And `default_config_ros.yaml` points behavior-tree paths at `/srr-data/...`
(the skill writes this in the launch step).

Pros: no file copying, edits to SRR appear in the container immediately.

Cons: requires a 1-line patch on the shared `isaac-sim` compose file (an upstream
MR or a local override). Until that lands, Pattern A keeps the infra files
untouched.

For now we ship Pattern A as the default — Pattern B is a planned follow-up.

## Templates

[`default_config_ros.yaml.template`](default_config_ros.yaml.template) is the
SRR-shaped version of the SIL config (IRA 1.6) — `simulation_duration` (seconds;
the runner sets it per scenario), three `character.groups` binding the fixed
`behavior_tree` files `srr_char{0,1,2}.bt.json`, and
`environment.base_stage_asset_path` pointing at the stock warehouse scene. It's a
template, not a config — the SRR skill / runner select a scenario by copying its
trees onto the fixed names and set `simulation_duration` via `sed`.

If you want to run a single SRR scenario manually (outside the skill), copy this
template over `${HOISA_ROOT_PATH}/closed-loop-testing/isaac-sim/sil/configs/default_config_ros.yaml` (GitHub layout:
`closed-loop-testing/isaac-sim/sil/configs/`) and edit the placeholders. The
skill's `02_launch.md` Step 3c does this programmatically.

## When `regression-reporter/scenarios/` and `isaac-sim/sil/` disagree

If you find divergence between `regression-reporter/scenarios/*` and the matching
synced file under `closed-loop-testing/isaac-sim/sil/`:

- **The `regression-reporter/scenarios/` copy is canonical.** The copy under
  `isaac-sim/sil/` is a deploy-time artifact produced by `sync_to_halos.sh`.
- If the `isaac-sim/sil/` copy was edited directly, port the change back to
  `regression-reporter/scenarios/` first, then run `sync_to_halos.sh` again.
  Don't merge two divergent copies of the same scenario file.

SRR fixtures are never committed under `isaac-sim/sil/`; that tree stays part of
the repo's stock SIL asset set in git.
