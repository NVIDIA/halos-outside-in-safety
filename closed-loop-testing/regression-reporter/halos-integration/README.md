# Halos integration

Glue between the SRR test rig and the Halos SIL `docker-compose` stack.
SRR lives at `closed-loop-testing/regression-reporter/`; the Halos SIL infra
(safety-core/PSF + comm-layer + Isaac Sim + VST + mediamtx) lives in the rest
of this repo. This folder explains how SRR's fixtures reach the `isaac-sim`
container at run time and provides the tooling to keep them in sync.

## Why SRR fixtures live under `regression-reporter/` (not in `isaac-sim/sil/`)

The `isaac-sim` container bind-mounts `closed-loop-testing/isaac-sim/sil/` — a
**Halos-owned** asset tree (it ships Halos' own scenes, `cameras.yaml`,
`default_config_ros.yaml`, and stock behavior trees). SRR keeps its own fixtures
(prepped scenes, per-scenario behavior trees, NavMesh, `/gt/*` publishers) under
`regression-reporter/scenarios/` instead, so that:

1. **Halos' Isaac SIL tree stays pristine in git** — SRR scenario
   regenerations, NavMesh re-bakes, and scene tweaks don't churn the
   Halos-owned `isaac-sim/sil/` history.
2. **SRR is self-contained** — the whole test rig (fixtures + service + skill +
   viewer) is reviewable as one unit under its own subtrees.

At deploy time the fixtures are copied into `isaac-sim/sil/` (Pattern A below) so
Isaac can read them; the copies are runtime artifacts, not committed to git.

## What lives where

| Artifact | Owner | Reason |
|---|---|---|
| PSF / comm-layer / Isaac docker images + compose | `deployments/`, `closed-loop-testing/` | Canonical Halos infra |
| `default_config_ros.yaml`, `cameras.yaml`, `robots.yaml` | `closed-loop-testing/isaac-sim/sil/configs/` | Halos runtime config |
| Original warehouse scene (un-prepped) `indicator_warehouse_*.usd` | `closed-loop-testing/isaac-sim/sil/scenes/` | Halos shipping asset |
| SRR-prepped scenes (`*_srr_nav_clear.usd`, `*_srr.usd`) | **`regression-reporter/scenarios/scenes/`** | Derived from Halos originals, NavMesh re-baked + `/gt/*` publishers wired in |
| Per-scenario behavior trees (`srr_<name>_char{0,1,2}.bt.json`) | **`regression-reporter/scenarios/behavior-trees/`** | Isaac 6.0 / IRA 1.6 form — the canonical source fixtures, emitted directly by `tools/randomize_paths.py` |
| Exported NavMesh JSON | **`regression-reporter/scenarios/scenes/navmesh.json`** | Input to `randomize_paths.py` — derived from a specific scene |
| Waypoint generator + validator | **`regression-reporter/scenarios/tools/`** | `randomize_paths.py`, `validate_waypoints.py` |
| Isaac Sim Script Editor utilities | **`regression-reporter/scenarios/isaac-scripts/`** | Used during SRR scene prep |
| Host-side GT-topic probes / USD inspectors | **`regression-reporter/scenarios/host-scripts/`** | SRR-specific debug |
| SRR aggregator / recorder | **`regression-reporter/srr-service/`** | SRR pipeline proper |
| SRR skill | **`skills/hoisa-generate-regression-report/`** | Claude Code orchestration |

## How the data flows at run time

```
   Halos SIL infra (this repo)           SRR fixtures (regression-reporter/)
   ─────────────────────────             ──────────────────────
   deployments/compose.yaml               scenarios/scenes/*.usd
   default_config_ros.yaml      ⟵──┐     scenarios/scenes/navmesh.json
   cameras.yaml                    │     scenarios/behavior-trees/srr_*.bt.json
   profiles/sil.env                │
   PSF / VST / comm-layer images   │
   isaac-sim container             │
       │                           │
       │  bind-mount               │  sync step (cp or bind-mount)
       │  closed-loop-testing/     │
       │    isaac-sim/sil/         │
       │  → /isaac-sim/sil/        │
       │                           │
       ▼                           │
   /isaac-sim/sil/scenes/  ⟵───────┘
   /isaac-sim/sil/configs/
   /isaac-sim/sil/scripts/
```

When Isaac Sim starts inside the halos compose stack, it opens whatever
scene path `default_config_ros.yaml` points to — and that path is
`/isaac-sim/sil/scenes/<scene>.usd`, which is the in-container path of
the Halos Isaac SIL dir `${HALOS_SIL_DIR}/scenes/` (GitHub layout:
`closed-loop-testing/isaac-sim/sil/scenes/`).

So SRR's scenes + commands must end up in that dir before Isaac launches.
There are two patterns for that, see below.

## Integration patterns

### Pattern A — sync at deploy time (current, simpler)

The SRR skill (or operator) **copies SRR assets into the sibling
`isaac-sim/sil/` tree** once after cloning:

```bash
./halos-integration/sync_to_halos.sh ${HALOS_SIL_DIR}
# HALOS_SIL_DIR = <repo>/closed-loop-testing/isaac-sim/sil
# (omit the arg to auto-read HALOS_SIL_DIR from the SRR .env, or fall back to
#  the sibling ../isaac-sim/sil)
```

What it copies (with `SIL = ${HALOS_SIL_DIR}`):

- `scenarios/scenes/*.usd` → `$SIL/scenes/`
- `scenarios/scenes/navmesh.json` → `$SIL/configs/`
- `scenarios/behavior-trees/srr_*.bt.json` → `$SIL/configs/` (the canonical trees, copied as-is)
- `scenarios/isaac-scripts/**/*.py` → `$SIL/scripts/isaac/`

Pros: zero changes to Halos files. Anyone with a fresh clone can run after one
sync — the Halos-owned `isaac-sim/sil/` git tree stays pristine.

Cons: it's a one-way copy, so changes made directly in `isaac-sim/sil/`
**won't be reflected back** to SRR. Always edit the SRR canonical copy under
`regression-reporter/scenarios/`.

When to re-run `sync_to_halos.sh`:
- After cloning the repo for the first time on a new machine
- After SRR scene / behavior-tree / NavMesh updates
- After Halos updates the Isaac SIL bind-mount layout

### Pattern B — bind-mount SRR data dir (planned, cleaner)

The Halos `isaac-sim` service definition (`closed-loop-testing/isaac-sim/isaac-sim.yml`)
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

And `default_config_ros.yaml` points scene + behavior-tree paths at `/srr-data/...`
(the skill writes this in the launch step).

Pros: no file copying, edits to SRR appear in the container immediately.

Cons: requires a 1-line patch on a **Halos-owned** compose file (an upstream MR
or a local override). Until that lands, Pattern A keeps Halos files untouched.

For now we ship Pattern A as the default — Pattern B is a planned follow-up.

## Templates

[`default_config_ros.yaml.template`](default_config_ros.yaml.template) is
the SRR-shaped version of the Halos config (IRA 1.6) — `simulation_duration`
(seconds; the runner sets it per scenario), three `character.groups` binding the
fixed `behavior_tree` files `srr_char{0,1,2}.bt.json`, and
`environment.base_stage_asset_path` pointing at the canonical SRR scene. It's a
template, not a config — the SRR skill / runner select a scenario by copying its
trees onto the fixed names and set `simulation_duration` via `sed`.

If you want to run a single SRR scenario manually (outside the skill),
copy this template over `${HALOS_SIL_DIR}/configs/default_config_ros.yaml`
(GitHub layout: `closed-loop-testing/isaac-sim/sil/configs/`) and edit the
placeholders. The skill's `02_launch.md` Step 3c does this programmatically.

## When `regression-reporter/scenarios/` and `isaac-sim/sil/` disagree

If you find divergence between `regression-reporter/scenarios/*` and the
matching synced file under `closed-loop-testing/isaac-sim/sil/`:

- **The `regression-reporter/scenarios/` copy is canonical.** The copy under
  `isaac-sim/sil/` is a deploy-time artifact produced by `sync_to_halos.sh`.
- If the `isaac-sim/sil/` copy was edited directly, port the change back to
  `regression-reporter/scenarios/` first, then `sync_to_halos.sh` again. Don't
  merge two divergent copies of the same scenario file.

SRR fixtures are never committed under `isaac-sim/sil/`; that tree stays
Halos-owned in git.
