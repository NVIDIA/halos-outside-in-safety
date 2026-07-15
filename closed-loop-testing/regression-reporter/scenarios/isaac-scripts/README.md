# isaac-scripts — Script Editor utilities

Scripts to run inside the **Isaac Sim Script Editor** (Window → Script Editor) for one-time scene prep, NavMesh bake/export, SRR publisher wiring, and NavMesh debugging.

These need Isaac Sim's `omni.kit.*` / `pxr.Usd` / `omni.anim.navigation.core` runtime — they cannot run on the host.

## When to use what

| Goal | Subdir | Script |
|---|---|---|
| Prep a fresh scene for SRR (one-time) | [scene_prep/](scene_prep/) | run all 3 in order |
| Bake + export NavMesh JSON for waypoint generator | [navmesh/](navmesh/) | `clear_cache_and_rebake.py` → `export_navmesh.py` |
| Publish `/gt/*` topics (IRA 6.0, runtime) | [srr_pubs/](srr_pubs/) | auto via `run_multi.sh --srr-gt` (builds `action_graphs/srr_ground_truth.py`); `check_srr_prereqs.py` = live diagnostic |
| NavMesh has a hole / chars stuck | [debug/](debug/) | start with `why_navmesh_hole.py` (with prim selected) |

## End-to-end workflow for a NEW scene

```
1. Open the original scene in Isaac Sim
2. Script Editor → run scene_prep/disable_exclude_root_layer.py
3. Script Editor → run scene_prep/clear_navmesh_blockers.py
4. Script Editor → run navmesh/clear_cache_and_rebake.py   (sync re-bake)
5. Script Editor → run navmesh/export_navmesh.py           (writes navmesh.json — this is what SRR consumes)
6. (optional) File → Save As… if you want to persist the re-baked scene;
   SRR itself only needs navmesh.json from step 5 (GT publishers are NOT baked — see below)

7. On host: generate behavior trees
    cd <regression-reporter>/scenarios
    python3 tools/randomize_paths.py --navmesh /path/to/navmesh.json \
        --agent-radius 0.8 ... --name <name>
8. Optionally: python3 tools/validate_waypoints.py --bt behavior-trees \
        --navmesh /path/to/navmesh.json
```

**Ground truth (`/gt/*/tf`) is built at RUN time, not baked into the scene.** IRA
6.0 spawns the pedestrians at runtime (asset-dependent `ManRoot` path), so
`run_multi.sh` launches Isaac with `--srr-gt`; the builder
`action_graphs/srr_ground_truth.py` discovers each char's `ManRoot` and builds
`/World/SRRGraph` once they spawn. There is no "paste + Save the graph" step. Use
`srr_pubs/check_srr_prereqs.py` (Script Editor, while running) as a live
read-only diagnostic if GT looks wrong.

## Subdir details

### `scene_prep/` — one-time prep before NavMesh re-bake

| Script | What it does |
|---|---|
| `disable_exclude_root_layer.py` | Iterates all `NavMeshVolume_exclude_*` prims and flips `nav:volume:type` token from `Exclude` → `Include` on the **root layer** (so the change is persisted, not session-only). NavMesh bake reads this attribute, NOT `IsActive` — so flipping the token is required (deactivating the prim does nothing). |
| `disable_exclude_volumes.py` | Same end goal as `disable_exclude_root_layer.py` but writes to the **session layer** instead of the root layer. Use when you want to test the "no excludes" state without modifying the on-disk USD — handy for "would the NavMesh bake without these holes?" experiments where you don't want to commit the change yet. Pair with `debug/verify_disk_scene.py` to confirm the edit is session-only (won't survive `File → Save`). |
| `clear_navmesh_blockers.py` | Deactivates 4 obstacle Xforms that have collider/rigidbody overlapping the work zones: `SM_HeavyDutyPalletTruck_A01_01`, `SM_Forklift_A01_Blue_01`, `Loading_Zone_Objects`, `Loading_Zone_Objects_01`. These are non-NavMesh prims, so `prim.SetActive(False)` works correctly here (unlike NavMesh volumes). To roll a scene back to its original state, re-open the original USD (these prep edits are only needed when re-baking a fresh scene). |

### `navmesh/` — bake and export

| Script | What it does |
|---|---|
| `clear_cache_and_rebake.py` | `omni.anim.navigation.core.acquire_interface().clear_cache_dir()` + `start_navmesh_baking_and_wait()`. The `_and_wait` variant is **synchronous** — async `start_navmesh_baking()` may reuse the cache hash and skip the rebake. |
| `clear_and_rebake.py` | All-in-one: deactivates the 4 physical obstacle Xforms **and** the `NavMeshVolume_exclude_*` prims, triggers a sync bake, re-exports `navmesh.json`, prints before/after triangle counts. Toggle `CLEAR_NAVMESH_EXCLUDE_VOLUMES` at top to skip the exclude-volume deactivation. Use this when you want to do scene_prep + bake + export in one step rather than 3 separate scripts; saves clicks but harder to debug if any single step fails. |
| `export_navmesh.py` | After bake completes, walks the viz mesh prim and dumps triangles to `navmesh.json` (host-readable). This file is the input for `randomize_paths.py --navmesh`. |
| `wait_and_reexport.py` | Helper: poll until viz mesh prim materializes (~2-3 s after bake returns) then re-export. Use when a fresh `export_navmesh.py` returned stale data. |
| `check_nav_state.py` | Diagnostic: lists all `__omni_nav_mesh_viz_*` prims, their triangle counts, and active state. If you ran a bake and `export_navmesh.py` returned empty, run this first — it tells you whether the viz prim exists at all (didn't bake) vs exists but empty (baked but produced nothing) vs multiple prims (stale artifacts). |
| `get_navmesh_data.py` | Probes the nav extension's internal `get_navmesh()` API — useful when the viz mesh prim never materializes (some Isaac Sim versions). Returns triangle data directly from the nav extension instead of reading the stage. Last resort for `export_navmesh.py` when the viz prim path is broken. |
| `dump_navmesh_volume_disk.py` | Opens a USD on disk via `Usd.Stage.Open()` (NOT the live in-memory stage) and dumps every `NavMeshVolume` prim's `nav:volume:type` + `IsActive()`. Use to verify a saved scene actually has the expected `Include` / `Exclude` values persisted — catches "the bake worked in the session but Save dropped the change" bugs. Can be invoked outside Script Editor: `docker exec isaac-sim /isaac-sim/python.sh dump_navmesh_volume_disk.py <scene.usd>`. |

### `srr_pubs/` — GT prereq diagnostic

| Script | What it does |
|---|---|
| `check_srr_prereqs.py` | Read-only **live** diagnostic (IRA 6.0). Paste in the Script Editor while the scene is running: verifies ROS2 bridge enabled + each group's `ManRoot` + the forklift exist, are xformable, and report a non-zero world pose. |

The `/gt/*` publisher graph itself is **not** here — it's built at run time by
`closed-loop-testing/isaac-sim/sil/scripts/action_graphs/srr_ground_truth.py` when
Isaac is launched with `--srr-gt` (see `scripts/run_multi.sh`). It discovers each
group's animated `ManRoot` under `/World/Characters/<group>/<group>_0` and builds
`/World/SRRGraph` — one **`ROS2PublishRawTransformTree` per char** + one
`ROS2PublishTransformTree` for the forklift — publishing `/gt/character_{0,1,2}/tf`
+ `/gt/forklift/tf`. **Why raw for chars:** IRA-spawned pedestrians live on the USD
stage but are NOT in Fabric, so the normal transform-tree node fails
`getObjectType` (`eInvalid`) and freezes them at spawn; the raw node takes an
explicit translation/rotation which the builder's update-loop pump feeds each
frame from `UsdGeom.ComputeLocalToWorldTransform` (never touches Fabric). The
forklift IS in Fabric, so it keeps the simpler transform-tree node.

### `debug/` — when something is broken

| Script | Use when |
|---|---|
| `why_navmesh_hole.py` | NavMesh has an unwalkable hole at a known location. Select a nearby prim → run → reports overlapping bbox / colliders / exclude volumes. |
| `find_navmesh_exclusions.py` | List all `NavMeshVolume_exclude_*` prims + their current `nav:volume:type` value. Sanity check after `disable_exclude_root_layer.py`. |
| `inspect_navmesh_volume.py` | Dump full schema attrs of a selected NavMeshVolume prim. Use for understanding bake config differences. |
| `verify_disk_scene.py` | Open the saved USD via `Sdf.Layer.FindOrOpen()` (NOT the in-memory stage) and confirm `nav:volume:type=Include` is **persisted on disk** — catches "edit was made on session layer, not root layer" bugs. |
| `find_all_viz_meshes.py` | List all NavMesh viz mesh prims. Multiple results = stale bake artifacts left behind; fresh bake should show only one. |
| `check_navmesh_blockers.py` | Initial broad scan: `/World/Loading_Zone_Objects_01` descendants, all active prims with collider whose AABB overlaps ROI south rect, `excludePaths` setting. |
| `check_floor_geometry.py` | Floor prim vs parent NavMeshVolume bbox alignment. |
| `diag_nav_api.py` | List available methods on the nav extension interface — useful when API has changed across Isaac Sim versions. |

## Notes / gotchas

- **Always work on the root layer**, not the session layer, when modifying schema attributes — otherwise `File → Save` won't persist the edit. `disable_exclude_root_layer.py` switches edit target before writing.
- **NavMesh bake is async by default** in Kit commands. Always use the `_and_wait` variant or poll for viz mesh materialization (~2-3 s lag). Otherwise `export_navmesh.py` may dump stale triangles.
- **`prim.SetActive(False)` is silently ignored by the NavMesh bake** when the prim is a `NavMeshVolume`. Use the `nav:volume:type` token attribute instead (`scene_prep/disable_exclude_root_layer.py`).
- **Saved-on-disk vs session edit**: after every Script Editor change you care about, run `debug/verify_disk_scene.py` after **File → Save** to confirm the change persisted to the USD file. The Sdf.Layer view doesn't see session-only edits.
