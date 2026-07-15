# Tools — behavior-tree generator + validator

Host-side Python tools that **produce** the IRA 1.6 character behavior trees in
[`../behavior-trees/`](../behavior-trees/) and **check** that their waypoints are
walkable on the baked NavMesh in [`../scenes/navmesh.json`](../scenes/navmesh.json).

These are the only host-side tools whose output is *data committed to
the repo*. Everything in [`../host-scripts/`](../host-scripts/) is for
inspecting live state at run time; the two folders sit on opposite
sides of the test-fixture lifecycle.

> **IRA 1.6 note:** Isaac Sim 6.0 / IRA 1.6 dropped the legacy
> `omni.anim.people` text command files (`default_command.<name>.txt`). Each
> pedestrian is now a behavior tree. The old command-file layer and its
> `command_to_bt.py` converter have been removed — `randomize_paths.py` emits
> the `.bt.json` trees directly, and they are the canonical source of truth.

## What's here

| Script | Purpose |
|---|---|
| [`randomize_paths.py`](randomize_paths.py) | Generator. Reads polygon zones (ZONE_CHAR0/1/2 — per-character spawn + wander areas, hardcoded in the script) + the baked NavMesh (`../scenes/navmesh.json`), samples reachable waypoints per character, and emits **IRA 1.6 behavior trees** `srr_<name>_char{0,1,2}.bt.json` into `../behavior-trees/` (`GoTo → MoveTo`, `Idle → Wait`; each `MoveTo` wrapped in a `ForceStatus→success` modifier so an unreachable waypoint is skipped rather than freezing the character). |
| [`validate_waypoints.py`](validate_waypoints.py) | Post-hoc check that every `MoveTo` target in a `.bt.json` (or a whole dir of them) lands on the NavMesh AND the agent body (16-point circle of radius `--agent-radius`) doesn't overlap unwalkable areas. Mostly a pre-flight gate — `randomize_paths.py` produces walkable waypoints by construction, and `ForceStatus` makes any stragglers non-fatal at runtime. |

## Why these aren't with the data they produce

Conceptually `randomize_paths.py` belongs WITH the `behavior-trees/` files it
emits, but it's actually a **tool** — the same source code regenerates any
of the canonical scenarios + arbitrary new ones. Keeping it in
`tools/` instead of `behavior-trees/` keeps that dir as a data-only dir
(easy to grep, easy to diff, easy to sync to the halos compose dir
without dragging tooling along).

## Usage — regenerating a scenario

The canonical scenarios were generated with these flag combinations:

```
                                     --roi-bias  --roi-clearance  --cycles  --idle-short  --spawn-return-every
in-roi      ............................  1.0    0                200       1,2           none
psf-edge    ............................  0.5    0                150       1,3           3
psf-clear   ............................  0.5    1.5              150       1,3           3
balanced    ............................  0.6    0                150       1,3           3
fast        ............................  0.6    0                200       1,3           3
```

All use `--agent-radius 0.8`. Example regen for `balanced`:

```bash
python3 scenarios/tools/randomize_paths.py \
  --navmesh scenarios/scenes/navmesh.json \
  --agent-radius 0.8 \
  --roi-bias 0.6 \
  --roi-clearance 0 \
  --cycles 150 \
  --idle-short 1,3 \
  --spawn-return-every 3 \
  --name balanced
```

This writes `scenarios/behavior-trees/srr_balanced_char{0,1,2}.bt.json`. Then
run [`../../halos-integration/sync_to_halos.sh`](../../halos-integration/sync_to_halos.sh)
to copy the trees into the Halos SIL configs dir.

## Generator flags reference

| Flag | Purpose |
|---|---|
| `--navmesh PATH` | Baked NavMesh JSON (sampled for reachable waypoints) |
| `--agent-radius R` | Inflation for NavMesh walkability test (default 0.6, we use 0.8) |
| `--roi-bias F` | Probability `[0..1]` of sampling waypoints inside the ROI polygon |
| `--roi-clearance D` | Forbidden band around ROI boundary in metres (psf-clear uses 1.5) |
| `--cycles N` | Number of waypoint cycles per character |
| `--idle-short MIN,MAX` | Short idle durations (seconds) at each waypoint |
| `--idle-long MIN,MAX` | Optional long idle range |
| `--initial-idle SEC` | Pre-walk idle to let scene settle |
| `--spawn-return-every K` | Return char to spawn every K cycles (0 / omitted = never) |
| `--name NAME` | Scenario name for the `.bt.json` filenames (`srr_<name>_char{0,1,2}.bt.json`; default: `custom`) |
| `--bt-out-dir DIR` | Where to write `.bt.json` (default: `scenarios/behavior-trees/`) |

## Hardcoded constants

`randomize_paths.py` carries three Python constants you may need to edit
if the scene layout changes:

| Constant | Purpose |
|---|---|
| `ZONE_CHAR0` | Polygon (list of `(x, y)` pairs) defining Char_00's spawn + wander area |
| `ZONE_CHAR1` | Same, for Char_01 |
| `ZONE_CHAR2` | Same, for Char_02 |

These were redrawn 2026-04-30 to be wider than the original 0.17 m
corridors. If you change them, **regenerate all behavior trees** —
existing waypoints baked against the old zones won't match the new
spawn boxes.

## Validation

`validate_waypoints.py` walks the `MoveTo` targets in one or more `.bt.json`
files, casts each waypoint plus a 16-point agent-radius ring against the
NavMesh triangles, and reports any waypoint where the ring crosses an
unwalkable edge.

```bash
# a single tree
python3 scenarios/tools/validate_waypoints.py \
  --bt scenarios/behavior-trees/srr_balanced_char0.bt.json \
  --navmesh scenarios/scenes/navmesh.json \
  --agent-radius 0.8

# every tree in the canonical dir
python3 scenarios/tools/validate_waypoints.py \
  --bt scenarios/behavior-trees \
  --navmesh scenarios/scenes/navmesh.json \
  --agent-radius 0.8
```

Report-only. Use this when:

- You hand-edited a `.bt.json` (typos, manual relocations).
- You suspect the NavMesh was re-baked but trees weren't regenerated.
- You're debugging "char stuck at waypoint N" in a multi-test run.

For routine regeneration via `randomize_paths.py` you can skip this — the
generator only emits waypoints that already pass the walkability test, and any
off-mesh waypoint is skipped at runtime by the `ForceStatus` modifier.
