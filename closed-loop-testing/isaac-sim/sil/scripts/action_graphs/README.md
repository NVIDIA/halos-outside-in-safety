# `sil/scripts/action_graphs/` — OmniGraph ActionGraph builders

Each Python module in this package builds one OmniGraph (ActionGraph) at a predictable `/World/<Name>Graph` path. Builders are invoked from `run_actor_sdg.py`'s `SET_UP_SIMULATION_DONE_EVENT` callback, one per scenario-specific graph. Reference documentation for these graphs, including the per-graph node wiring: [Isaac Sim Action Graphs](https://docs.nvidia.com/halos-outside-in/1.3/testing/sil/components/action-graph.html).

For one-shot USD prim tweaks (deactivate prims, set xform attributes, etc) see the sibling `sil/scripts/runtime_patches/` package.

## What lives here

| Module | Public entry point | Purpose |
|---|---|---|
| `rtsp_cameras.py` | `build_rtsp_graph(config_path)` | N-camera RTSP streaming Action Graph (1 `OnPlaybackTick` shared, N `IsaacCreateRenderProduct + RTSPCameraHelper` pairs). |
| `forklift_common.py` | `build_forklift_graphs(config_path)`, `strip_baked_scene_graphs()` | Shared helpers + orchestrator that builds the three per-robot forklift graphs below. Driven by `robots.yaml`. |
| `forklift_control.py` | `build_control_graph(config_path)` | Per-robot `cmd_vel` → swivel-IK → articulation graph. Replaces the baked `ROS_Forklift_Control_Graph`. |
| `forklift_odometry.py` | `build_odometry_graph(config_path)` | Per-robot `IsaacComputeOdometry` → ROS 2 odom/tf graph. Replaces the baked `Odometry_Graph`. |
| `forklift_safety_indicator.py` | `build_safety_graph(config_path)` | Per-robot `/safety/is_muted` → indicator-color graph. Replaces the baked `Safety_indicator_Graph`. |
| `clock.py` | `build_clock_graph(config_path)` | ROS 2 `/clock` publisher graph. Replaces the baked `Clock_Publisher_Graph`. Driven by `robots.yaml` `clock:` block. |
| `srr_ground_truth.py` | `build_srr_gt_graph()` | **Opt-in** (`run_actor_sdg.py --srr-gt`, default OFF). SRR regression-harness ground-truth `/gt/*/tf` publisher (`/World/SRRGraph`). Resolves the IRA-spawned characters + forklift from the live stage and pumps their Fabric world transforms each frame. No effect on a normal Halos run. |

## Invocation contract

Each builder:

1. **Imports omni.* lazily** inside the function body. The module is imported at the top of `run_actor_sdg.py`, which runs before `SimulationApp` is instantiated — `omni.usd`, `omni.graph.core`, `pxr` etc. are not safe to import at module load time.
2. **Takes a single positional `config_path: str`** plus optional kwargs (`graph_path`, `resolution`, etc). Defaults point at container-canonical paths under `/isaac-sim/sil/configs/`.
3. **Is idempotent.** Calling twice in the same Kit session produces the same end state — the old graph is deleted before the new one is built.
4. **Prints `[<module-tag>] ...`** progress lines.
5. **Returns the `og.Graph` object** so callers can introspect or wire further.

## Adding a new graph

1. Copy `rtsp_cameras.py` as the template. Rename the module file to `<your_graph>.py`.
2. Rename the public entry point: `build_<name>_graph(config_path: str, *, graph_path: str = "/World/<Name>Graph") -> og.Graph`.
3. Write the `og.Controller.edit({...}, {CREATE_NODES + SET_VALUES + CONNECT})` body. Keep it **single-block declarative** — that's the canonical NVIDIA shape (`isaacsim.ros2.ui/.../og_utils.py:Ros2ClockGraph.make_graph`).
4. Add an idempotency guard before the `og.Controller.edit` call:

   ```python
   prim = stage.GetPrimAtPath(graph_path)
   if prim and prim.IsValid():
       stage.RemovePrim(graph_path)
   ```

5. Export the entry from `__init__.py` and add the name to `__all__`.
6. In `run_actor_sdg.py`:
   - Add a CLI flag: `--no-<name>` (`store_false`) for a default-ON graph, or `--<name>` (`store_true`) for an opt-in default-OFF graph (e.g. `--srr-gt`).
   - Import and call inside the setup-done callback when the flag is set.

## Design notes

- **Flat module-scope functions only.** No factory classes. NVIDIA's own production code at this scale (`isaacsim.ros2.ui/.../og_utils.py` ships 5 such helpers) uses the same flat shape. Class hierarchy is only justified at ≥5 graphs sharing real common logic.
- **One graph per file.** Trivially reviewable, greppable, diffable.
- **YAML config externalization.** Per-scenario shape (camera count, ROS topic list, etc.) lives in `sil/configs/*.yaml`, not in Python. Lets ops edit without touching code.
- **Graph path under `/World/`.** Operator finds the graph in the Stage panel at a predictable location.
- **Forklift graphs are Python builders here, not baked in USD.** In Isaac Sim 5.1 the forklift control / odometry / safety-indicator / clock graphs lived baked in the scene's USD layer. The 6.0 migration rebuilt them as the `forklift_*` and `clock` modules above; `forklift_common.strip_baked_scene_graphs()` removes any residual baked graphs at load time so the Python builders are authoritative — see refactor 1 in the architecture doc.

## Lifecycle ordering

```
SimulationApp(experience=...) [ext load]
        |
        v
SimulationManager.load_config_file(yaml)
        |
        v
await setup_simulation()
        |   - EnvironmentLoader.load()     (stage + USD-baked AGs load)
        |   - CharacterLoader.load()       (IRA spawns chars at NavMesh-random positions)
        |   - RobotLoader.load()
        |   - SensorLoader.load()
        v
SET_UP_SIMULATION_DONE_EVENT dispatched ──┐
                                          v
                  run_actor_sdg.py setup_done callback fires:
                      runtime_patches.apply_halos_runtime_patches()    <- sibling package
                      action_graphs.build_rtsp_graph()                  <- this package
                      action_graphs.build_forklift_graphs()             <- this package (control+odometry+safety)
                      action_graphs.build_clock_graph()                 <- this package
                      action_graphs.build_srr_gt_graph()                <- this package (opt-in, --srr-gt)
        |
        v
timeline.play()  ──> all Python-built graphs tick together
```

## See also

- Sibling runtime patches: `closed-loop-testing/isaac-sim/sil/scripts/runtime_patches/`
