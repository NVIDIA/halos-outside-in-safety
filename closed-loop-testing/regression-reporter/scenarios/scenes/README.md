# SRR test scenes

USD scenes used by the SRR regression rig. These are **SRR-specific** variants
of the canonical Halos SIL warehouse scene, modified so that the character /
forklift behaviour is reproducible across runs and the NavMesh covers the
character zones the SRR scenarios exercise.

## What's here

| File | Size | Purpose |
|---|---:|---|
| `indicator_warehouse_20x20_odom_srr_nav_clear.usd` | 84 KB | **Canonical SRR test scene.** Original warehouse + NavMesh exclude-volumes flipped to include, NavMesh re-baked over the full floor so all 3 character zones are walkable. The 4 SRR `/gt/*/tf` publishers (Char_00/01/02, Forklift) are wired into the Action Graph so the SRR recorder can sample positions at 30 Hz. **This is the scene the skill orchestration loads.** |
| `indicator_warehouse_20x20_layout_overflow_test_odom_srr.usd` | 110 KB | Earlier variant — same NavMesh prep but built on the "layout overflow" base. Kept as a fallback in case the nav_clear scene needs to be regenerated; lets you A/B against a known-good scene. Not actively used by the skill. |
| `navmesh.json` | 2.1 MB | Exported NavMesh triangle data from the canonical scene above (produced by `../isaac-scripts/navmesh/export_navmesh.py`). It's the input to `../tools/randomize_paths.py --navmesh`. Lives here (and not in `tools/` or at scenarios root) because **the NavMesh is a derived property of a specific scene** — if a scene is regenerated the JSON must be too, and shipping them together makes the dependency explicit. |

The original (un-prepped) halos scene
`indicator_warehouse_20x20_layout_overflow_test_odom.usd` and the canonical
ActionGraph live in the halos repo (GitHub layout:
`closed-loop-testing/isaac-sim/sil/scenes/`) — this folder only owns the
SRR-modified derivatives.

## Why two scenes (and not just one)

NavMesh baking in Isaac Sim is fragile — extension API versions change between
Isaac Sim releases, exclude-volumes can be re-introduced by upstream halos
scene updates, and a re-bake can leave a hole if a single obstacle Xform was
moved. Keeping a "last known good" variant alongside the active scene means
that if the active scene goes bad you can:

1. Diff the two USDs to see what changed
2. Use the variant as a fallback while the active scene is regenerated
3. Confirm by-construction that the SRR `/gt/*` publisher prims look identical
   across both

If we end up with a third variant for a new Isaac Sim version, append rather
than overwrite — old runs need to be reproducible against the scene they
were recorded with.

## How these were made (recipe)

The full prep workflow lives in
[`../isaac-scripts/README.md`](../isaac-scripts/README.md), but at a glance:

```
1. Open the original scene in Isaac Sim
2. Script Editor → scene_prep/disable_exclude_root_layer.py
3. Script Editor → scene_prep/clear_navmesh_blockers.py
4. Script Editor → navmesh/clear_cache_and_rebake.py    (sync re-bake)
5. Script Editor → navmesh/export_navmesh.py            (writes navmesh.json)
6. File → Save As… "indicator_warehouse_20x20_odom_srr_nav_clear.usd"
   (this bakes the NavMesh into the scene — the /gt/* publishers are NOT baked)

7. On host: regenerate the scenarios' behavior trees via
    tools/randomize_paths.py --name <scenario> (one run per scenario)
```

`/gt/*` publishers are wired at RUN time, not saved into the scene: IRA 6.0
spawns the pedestrians at runtime, so `run_multi.sh` launches Isaac with
`--exec .../srr_pubs/add_srr_gt_pubs.py`, which builds `/World/SRRGraph` once the
chars spawn. Use `srr_pubs/check_srr_prereqs.py` (Script Editor, while running)
only as a live diagnostic if GT looks wrong.

## How the skill picks the scene at run time

The SRR skill's launch phase (`02_launch.md` Step 3c) writes the scene path
into `default_config_ros.yaml` before starting Isaac. Today it hardcodes
`/isaac-sim/sil/scenes/indicator_warehouse_20x20_odom_srr_nav_clear.usd`
(inside the isaac-sim container) — which means the file at that path is what
Isaac loads.

In the current setup, the Halos Isaac SIL dir `${HALOS_SIL_DIR}/scenes/`
(GitHub layout: `closed-loop-testing/isaac-sim/sil/scenes/`) is bind-mounted
into the container at `/isaac-sim/sil/scenes/`. To make Isaac load the SRR
canonical scene, the file from this folder must be present in that mounted
dir. Two ways:

**Today (manual)** — copy the file into the Isaac SIL dir once after cloning
the repo:
```bash
cp scenarios/scenes/indicator_warehouse_20x20_odom_srr_nav_clear.usd \
   ${HALOS_SIL_DIR}/scenes/
```

**Future (planned)** — `../halos-integration/sync_to_halos.sh` will do this
automatically as part of the SRR skill's prerequisite step, plus
`../halos-integration/README.md` documents an alternative `docker-compose.yml`
override that bind-mounts `regression-reporter/scenarios/scenes/` directly so no
copy is needed.

## Updating a scene

If you change one of these `.usd` files:

1. Make the change in Isaac Sim (Script Editor or UI), Save As to the same
   filename.
2. **Re-run the NavMesh bake + export** if the geometry changed — otherwise
   `../tools/randomize_paths.py` (which reads `navmesh.json` in this folder) will hand out
   waypoints in unwalkable regions.
3. Re-generate all scenarios' behavior trees via
   `tools/randomize_paths.py` — the old waypoints may now be invalid.
4. Run **one full** SRR multi-test to confirm the scene loads cleanly
   (Isaac doesn't hang on init, perception sees 3 cameras, character
   spawns).
5. Commit the new USD + the new navmesh.json + the new commands in one
   commit — they have to ship together to be reproducible.

## Why USDs are in git (not LFS)

These files are small (~100 KB each) and rarely change. Git handles them
fine without LFS. If a future scene grows past ~10 MB, revisit.

## Caveats

- USDs reference textures + meshes by **absolute path inside the container**
  (`/isaac-sim/...`). They're not portable across different deployments
  without the matching halos compose mount tree. This is acceptable because
  SRR is always co-deployed with halos compose.
- The Action Graph wired into these scenes uses extension-specific node
  types (`isaacsim.ros2.bridge`, `omni.anim.navigation.core`). Loading them
  in a stripped Isaac Sim build without those extensions will silently drop
  the SRR publishers — `check_srr_prereqs.py` exists to catch this.
