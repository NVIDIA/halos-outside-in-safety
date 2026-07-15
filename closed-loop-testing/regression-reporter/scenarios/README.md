# Scenarios — character waypoint configs + scene prep

Test inputs for the SRR rig, grouped by role:

- **Data** that Isaac Sim consumes at run time: `behavior-trees/` (IRA 1.6 character behavior trees) + `scenes/` (USD scenes + baked NavMesh).
- **Tools** that produce or check that data: `tools/` (behavior-tree generator + validator).
- **Runtime utilities** that prep scenes or probe live state: `isaac-scripts/` (Script Editor) + `host-scripts/` (host shell).

The companion folder [`../halos-integration/`](../halos-integration/) explains
how `behavior-trees/` and `scenes/` plug into the Halos compose stack at run time
(in short: copied/synced into the Halos Isaac SIL dir `${HALOS_SIL_DIR}` —
GitHub layout `closed-loop-testing/isaac-sim/sil/` — before Isaac Sim launches).

> **IRA 1.6 note:** Isaac Sim 6.0 dropped the legacy `omni.anim.people` text
> command files. Each pedestrian is now a behavior tree
> (`srr_<name>_char{0,1,2}.bt.json`); the old `commands/default_command.*.txt`
> layer and its `command_to_bt.py` converter have been removed.
> `randomize_paths.py` emits the trees directly.

## Layout

```
scenarios/
├── README.md                     # you are here
├── behavior-trees/               # IRA 1.6 *.bt.json — the data Isaac 6.0 consumes
│   └── srr_<6>_char{0,1,2}.bt.json  # in-roi, psf-edge, psf-clear, balanced, fast, fixed
├── scenes/                       # SRR-prepped USD scenes + NavMesh (data)
│   ├── README.md
│   ├── *.usd                     # 2 scene variants
│   └── navmesh.json              # exported NavMesh — derived from these scenes
├── tools/                        # host-side generator + validator
│   ├── README.md
│   ├── randomize_paths.py        # behavior-tree generator
│   └── validate_waypoints.py     # post-hoc walkability check
├── isaac-scripts/                # Script Editor utilities (Isaac Kit runtime)
│   ├── README.md
│   ├── debug/ navmesh/ scene_prep/ srr_pubs/
└── host-scripts/                 # host-side GT-topic probes / USD inspectors
    ├── README.md
    └── *.py / *.sh
```

## Where to find each thing

| If you want… | Look in |
|---|---|
| The 6 scenarios' behavior trees | [`behavior-trees/`](behavior-trees/) |
| One of the 2 USD scenes Isaac loads | [`scenes/`](scenes/) |
| The exported NavMesh JSON used by `randomize_paths.py` | [`scenes/navmesh.json`](scenes/navmesh.json) |
| To regenerate a scenario's `srr_<name>_char{0,1,2}.bt.json` | [`tools/randomize_paths.py`](tools/randomize_paths.py) |
| To sanity-check a hand-edited `.bt.json` | [`tools/validate_waypoints.py`](tools/validate_waypoints.py) |
| To prep a scene (NavMesh bake) | [`isaac-scripts/`](isaac-scripts/) — paste into Isaac Script Editor |
| To wire `/gt/*` publishers (IRA 6.0 runtime) | auto via `run_multi.sh --exec add_srr_gt_pubs.py`; `isaac-scripts/srr_pubs/check_srr_prereqs.py` = live diagnostic |
| To verify `/gt/*` ROS topics are alive on the host | [`host-scripts/verify_gt_topics.sh`](host-scripts/verify_gt_topics.sh) |
| To monitor character motion for 5 min from host | [`host-scripts/monitor_gt_5min.py`](host-scripts/monitor_gt_5min.py) |

## How the 5 scenarios were generated

```
                                     --roi-bias  --roi-clearance  --cycles  --idle-short  --spawn-return-every
in-roi      ............................  1.0    0                200       1,2           none
psf-edge    ............................  0.5    0                150       1,3           3
psf-clear   ............................  0.5    1.5              150       1,3           3
balanced    ............................  0.6    0                150       1,3           3
fast        ............................  0.6    0                200       1,3           3
```

All 5 use `--agent-radius 0.8` and target the working scene `indicator_warehouse_20x20_odom_srr_nav_clear.usd`.

## Generator flags (`randomize_paths.py`)

| Flag | Purpose |
|---|---|
| `--navmesh PATH` | Baked NavMesh file (sampled for reachable waypoints) |
| `--agent-radius R` | Inflation for NavMesh walkability test (default 0.6, we use 0.8) |
| `--roi-bias F` | Probability `[0..1]` of sampling waypoints inside the ROI polygon |
| `--roi-clearance D` | Forbidden band around ROI boundary in metres (psf-clear uses 1.5) |
| `--cycles N` | Number of waypoint cycles per character |
| `--idle-short MIN,MAX` | Short idle durations (seconds) at each waypoint |
| `--idle-long MIN,MAX` | Optional long idle range |
| `--initial-idle SEC` | Pre-walk idle to let scene settle |
| `--spawn-return-every K` | Return char to spawn every K cycles (0 / omitted = never) |

## Workflow — switching scenarios

1. **Pick a scenario** (e.g. `in-roi`).
2. **Make sure the scenario's behavior trees are in the Halos SIL configs dir** —
   a one-time sync (`../halos-integration/sync_to_halos.sh`) copies the
   canonical `srr_<name>_char{0,1,2}.bt.json` in.
3. **Select the scenario** — copy its three trees onto the fixed active names the
   config references:
   ```bash
   for i in 0 1 2; do
     cp -f ${HALOS_SIL_DIR}/configs/srr_in-roi_char${i}.bt.json \
           ${HALOS_SIL_DIR}/configs/srr_char${i}.bt.json
   done
   ```
4. **Restart Isaac Sim** so it picks up the new trees (Halos compose down/up).

The skill (`skills/hoisa-generate-regression-report/`) and `scripts/run_multi.sh` automate steps 2–4 per
scenario (`phase_set_behavior_tree`). See [`../halos-integration/README.md`](../halos-integration/README.md)
for the integration story (why the copy step exists today, and how a future
bind-mount approach removes it).

## Re-generating a scenario's behavior trees

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

This writes `scenarios/behavior-trees/srr_balanced_char{0,1,2}.bt.json`. The
`ZONE_CHAR0/1/2` polygons are hardcoded inside the script (per-character spawn /
wander zones for the working scene). Update them in the script if the scene
layout changes.

## Validation

`validate_waypoints.py` re-checks the `MoveTo` targets in one or more `.bt.json`
against the NavMesh (16-point agent-radius circle test). It's a pre-flight
gate — `randomize_paths.py` produces walkable WPs by construction, and any
off-mesh straggler is skipped at runtime by the `ForceStatus` modifier. Handy
for hand-edited trees:

```bash
python3 scenarios/tools/validate_waypoints.py \
  --bt scenarios/behavior-trees \
  --navmesh scenarios/scenes/navmesh.json --agent-radius 0.8
```

## Scene assumptions

These scenarios target `indicator_warehouse_20x20_odom_srr_nav_clear.usd` — the "fixed" scene where:
- All 4 obstacle Xforms are deactivated (forklift mockups, palette truck)
- All `NavMeshVolume_exclude_*` prims flipped to `nav:volume:type=Include`
- NavMesh re-baked from clean state

If you switch scenes, regenerate ALL behavior trees — waypoints baked into the trees are scene-specific.

---

## Scene structure (`indicator_warehouse_20x20_odom_srr_nav_clear.usd`)

20×20 m warehouse with a single trailer dock zone on the east side. All test scenarios use this scene; only the character waypoint script changes between them.

### Coordinate system
- **x axis** runs west (low) → east (high). Trailer is on the east; tripwire `TW_X = 9.574`.
- **y axis** runs south (low) → north (high).
- All units are metres.

### Key prims

| Prim path | Type | Notes |
|---|---|---|
| `/World/forklift_b` | Articulation root | Body link is the moving root pose. SRR recorder uses `lookup_transform("world", "body")`. Sub-links (lift, wheels) are static offsets relative to body. |
| `/World/Characters/Character/DHGen/SkelRoot` | Skeleton | Char_0, published as `/gt/character_0/tf` |
| `/World/Characters/Character_01/DHGen/SkelRoot` | Skeleton | Char_1 |
| `/World/Characters/Character_02/DHGen/SkelRoot` | Skeleton | Char_2 |
| `/World/Cameras/Camera`, `Camera_01`, `Camera_02` | Camera | 3 RTSP streams @ 30 FPS into mediamtx |
| `/World/ActionGraph` | OmniGraph | Forklift playback driver (built into scene) |
| `/World/SRRGraph` | OmniGraph | `/gt/*/tf` publishers (added via `isaac-scripts/srr_pubs/add_srr_gt_pubs.py`) |
| `/World/Loading_Zone_Objects` | Xform | Pallet stacks. **Deactivated** in `_nav_clear` scene. |
| `/World/Loading_Zone_Objects_01` | Xform | Loading dock obstacles. **Deactivated** in `_nav_clear` scene. |
| `/World/Navmesh/NavMeshVolume_*` | NavMeshVolume | All `nav:volume:type=Include` in `_nav_clear` scene (originally many were `Exclude`). |

### Calibration (from `calibration.json`)

| Item | Value |
|---|---|
| **ROI rectangle** (work zone where humans are detected) | x ∈ [4.877, 9.574], y ∈ [-18.976, -11.239] |
| **Trailer tripwire** (forklift in/out of dock) | x = 9.574 |
| **Trailer area** (where forklift docks) | x > 9.574 |

### Character zones (hardcoded in `randomize_paths.py`)

`ZONE_CHAR0`, `ZONE_CHAR1`, `ZONE_CHAR2` are simple rectangular polygons (redrawn 2026-04-30 to be wider than the original 0.17 m corridors). Each defines the spawn + wander area for one character. They're `randomize_paths.py` constants — to change zones, edit the constants and regenerate all behavior trees.

### Cameras + RTSP

Three fixed cameras feed mediamtx → VSS perception. Camera registration with VST happens automatically via `--enable-vst` on the scenario runner. Stream IDs map to: `Camera`, `Camera_01`, `Camera_02` (look up real IDs via VST `/v1/record/streams`).

### What changes between the 5 scenarios

**Only the three active behavior trees** (`srr_char{0,1,2}.bt.json`) in `${HALOS_SIL_DIR}/configs/`. Everything else (scene file, NavMesh, ROI calibration, tripwire, forklift/ROS control, cameras) stays identical. The skill / runner copy the selected scenario's `srr_<name>_char{0,1,2}.bt.json` onto those fixed names — see [`scripts/run_multi.sh`](../scripts/run_multi.sh) `phase_set_behavior_tree()`.

---

## End-to-end workflow for a NEW scene

If you're prepping a fresh warehouse USD (not the existing `_nav_clear`), the full sequence is:

```
1. Open the original scene in Isaac Sim
2. Run isaac-scripts/scene_prep/disable_exclude_root_layer.py    (Script Editor)
3. Run isaac-scripts/scene_prep/clear_navmesh_blockers.py        (Script Editor)
4. Run isaac-scripts/navmesh/clear_cache_and_rebake.py           (Script Editor — sync re-bake)
5. Run isaac-scripts/navmesh/export_navmesh.py                   (Script Editor — writes navmesh.json)
6. File → Save As… "<name>_nav_clear.usd" into scenes/   (bakes the NavMesh only)
   ── /gt/* publishers are NOT baked here (IRA 6.0 spawns chars at runtime, so the
      ManRoot targets don't exist at save time). They're wired at RUN time:
      run_multi.sh launches Isaac with `--exec .../add_srr_gt_pubs.py`, which polls
      the update loop and builds /World/SRRGraph once the chars spawn. Nothing to
      paste/save. Use srr_pubs/check_srr_prereqs.py (Script Editor, while running)
      only to debug if /gt looks wrong.
7. Copy the new navmesh.json into scenes/ alongside the .usd:
    cp /path/to/navmesh.json scenarios/scenes/navmesh.json
8. On host, regenerate behavior trees:
    python3 scenarios/tools/randomize_paths.py \
        --navmesh scenarios/scenes/navmesh.json \
        --agent-radius 0.8 --roi-bias <F> --roi-clearance <D> \
        --cycles <N> --idle-short MIN,MAX [--spawn-return-every K] \
        --name <name>
9. Optional sanity check:
    python3 scenarios/tools/validate_waypoints.py \
        --bt scenarios/behavior-trees \
        --navmesh scenarios/scenes/navmesh.json --agent-radius 0.8
```

To **reset** a scene back to original: run `isaac-scripts/scene_prep/reactivate_and_save.py`.

For NavMesh issues during prep (chars stuck, holes, viz mesh weirdness), see [`isaac-scripts/debug/`](isaac-scripts/debug/).
