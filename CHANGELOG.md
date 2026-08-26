# Changelog

## Unreleased

### Breaking — ROS surface is namespaced per robot

Multi-forklift support renamed the topics, TF frames and node name of the **default single-forklift run**. A deployment that does nothing still works; anything that subscribes by name does not.

| Was | Now |
|---|---|
| `/cmd_vel` | `/forklift_b/cmd_vel` |
| `/odom` | `/forklift_b/odom` |
| `/planned_path` | `/forklift_b/planned_path` |
| `/curve_follower/markers` | `/forklift_b/markers` |
| TF `odom` → `base_link` | TF `forklift_b/odom` → `forklift_b/base_link` |
| node `robot_controller` | node `forklift_b_controller` |

`forklift_b` is the robot's `name` in the fleet file, which the controller service names as its `ROBOT_ID`; a differently-named robot namespaces under its own name.

**Migrating from 1.3**

- Scripts, RViz configs and dashboards that name any topic above: add the `<robot>/` prefix. RViz users also set the fixed frame to `forklift_b/odom`.
- Waypoints moved to a per-scene directory: `waypoints/waypoints.json` → `waypoints/<map id>/<ROBOT_ID>.json`. Coordinates are metres in one scene's world frame, so the wrong map drives a lane that was never validated in that warehouse — silently. `deployments/scripts/preflight.py` checks the pairing before Isaac boots.

### Breaking — one scenario id replaces four hand-kept choices

A run used to be described by a compose profile, three command-line flags and an
env var, each set independently and cross-checked by `preflight.py`. It is now
one id that Isaac and every controller read, so they cannot be launched against
different halves of the same run.

```
SCENARIO=warehouse_20x20_1fl
```

`closed-loop-testing/isaac-sim/sil/configs/scenarios.yaml` maps each id to its
IRA config, fleet file, cameras config and waypoint directory. `-c` is now
optional when a scenario names one; a flag still wins over the scenario.

There is deliberately no environment override for the fleet file alone. One
would pair a scenario's scene with another scenario's trucks — unnamed, absent
from the logs, and impossible to ask preflight about. A different fleet is a
different run, so it gets an id in `scenarios.yaml`; `--robots-config` is still
there for a one-off not worth naming. `WAYPOINTS_MAP` remains, because a new
waypoint set for an existing scene is genuinely the same run driven differently.

**Migrating from 1.3**

- **`FORKLIFT_WAYPOINTS_DIR` is removed.** The whole waypoints tree is mounted
  and the warehouse chosen inside the container, so an old value would nest one
  level too deep. The controller refuses to start rather than drive an
  unvalidated lane, and says what to set instead: `SCENARIO`, or `WAYPOINTS_MAP`
  to override one run.
- **`FORKLIFT_USE_NAMESPACE` is removed**, and so is the `--no-namespace` flag.
  The controller now takes its topic names from the robot's own block in the
  fleet file — `control.cmd_vel_topic` and `odometry.odom_topic`, the same two
  keys Isaac builds its graphs from — instead of rebuilding the strings from a
  namespace toggle. One declaration, so the two sides cannot disagree. Setting
  the toggle to `false` never worked: it pointed the controller at bare `/odom`
  while Isaac kept publishing the namespaced name, and the truck simply stopped
  with nothing in any log.
- **`FORKLIFT_ROBOT_ID` is removed.** Which truck a controller drives is now
  stated on its compose service, the way the second controller always stated it
  — one fact, one place, the same for every truck. A fleet whose first robot is
  not `forklift_b` sets `ROBOT_ID` on the service. Nothing fails silently: the
  controller refuses to start when its `ROBOT_ID` is not in the fleet file, and
  says which names that file does declare.
- **`FORKLIFT_WAYPOINT_FILE` no longer reaches the compose services.** It named
  one file for the whole fleet, which is only ever right with one truck. The
  scenario's waypoint directory plus each container's `ROBOT_ID` names the file
  instead. `WAYPOINT_FILE` still works for `docker_run.sh`, where an environment
  is per container by construction.
- Drive knobs (`FORKLIFT_BASE_SPEED`, `FORKLIFT_HEADING_OFFSET`,
  `FORKLIFT_LOOP_PATH`, `FORKLIFT_ANGULAR_SPEED`, `FORKLIFT_NO_INVERT`,
  `FORKLIFT_END_TOLERANCE`, `FORKLIFT_END_POSE_COUNT`,
  `FORKLIFT_SPIRAL_TIMEOUT`) are no longer read from the environment. They were
  documented nowhere and set in no shipped profile, and one env var would have
  set every truck in the fleet at once. Per-truck values live in the robots
  config. `launch_controller.sh` still passes them as flags; `docker_run.sh`
  mounts no fleet file and so runs on `fleet_config.DRIVE_DEFAULTS`, which hold
  the same values `robots.yaml` states for `forklift_b`.

### Changed

- **The 20x20 scene describes the warehouse and nothing else.** `forklift_b` and
  its safety disc are declared in `robots.yaml` — a `spawn:` block at
  `(1, -13.39)` facing 180°, and the `indicator:` size the baked disc used to
  state as geometry — the way `robots-40x20.yaml` has declared its trucks since
  they stopped being baked. No scene in `sil/scenes/` carries a forklift prim
  now, so adding or moving a truck is a config change that can be reviewed as a
  diff instead of a 35k-line USD edit.

  The `spawn:` block names the `6.0` ForkliftB URL, where the deleted prim named
  the `5.1` one, so both fleet files now say the same thing. Nothing about the
  truck changes: the two URLs serve the same file, byte for byte
  (`md5 eee8b76a…`). It still authors 16 TGS velocity iterations, so the runtime
  patch's rebalance is load-bearing rather than a historical no-op.

- **comm-layer derives its mute mirrors from the fleet file.** `COMM_ROBOT_IDS`
  was a fourth list of robot names kept by hand, and the only way to get it
  wrong was silent: a truck missing from it listens on a topic nobody publishes,
  so its disc sits on the alarm colour for the whole run with nothing in any
  log. comm-layer now mounts the same configs directory the controllers do,
  reads `SCENARIO`, and mirrors for exactly the robots whose fleet entry asks
  for a per-robot topic. The variable is gone from both profiles, and
  `preflight.py` checks three lists rather than four.

  The shipped value was already wrong for the default run: it named two trucks
  while `robots.yaml` declares one and names the global topic explicitly, so
  every 20x20 run published two mirrors nobody subscribed to and preflight
  warned about it each time. Derived, that run asks for none.

  **Needs `up -d --build`** — the comm-layer image changes.

### Removed

- **The three 5.1 baked `Character` prims and their shared `Biped_Setup` rig,
  from the 20x20 scene.** They carried no Behavior Tree in the 6.0 stack, so
  every run showed six workers of which three stood still, and
  `halos_runtime_patches.py` deactivated them on each launch to hide it. The
  three IRA-spawned characters are unaffected — the patch still moves them to
  the canonical positions. The navmesh bake is unchanged: the three `Character`
  prims declared `NavMeshExcludeAPI`, and `Biped_Setup` — which declares no api
  schemas and does compose real geometry — is `invisible`, carries no collider,
  and sits on a patch of `scenarios/scenes/navmesh.json` that is already fully
  walkable.

- **The disabled `forklift_c` prim, from both the 20x20 and 40x20 scenes.** It
  was `active = false` and `invisible` in both, referencing a
  `collected-assets` payload nothing loaded, and its only remaining mentions
  were in the `HALOS_*` bisection lists in `scene_component_isolation.py`.

- **The `warehouse_20x20_2fl` scenario, and the scene, IRA config and fleet file
  behind it.** It existed because there was no two-truck scene; `warehouse_40x20`
  is one, and it carries its second truck as a `spawn:` block rather than as a
  second copy of a warehouse. Gone with it: the 2FL scene USD, its
  `default_config_ros_2fl.yaml` and `robots-2fl.yaml`, and
  `waypoints/warehouse_20x20/forklift_b2.json`.

  **What this costs:** `warehouse_40x20` is the only two-truck scenario left, and
  it is `experimental: true` — no calibration is published for that warehouse, so
  perception runs 20x20 geometry against a different building. The 20x20 2FL
  scene shared the 20x20 calibration, so until a 40x20 calibration lands there is
  no two-truck run whose safety numbers mean anything. Watching two trucks drive,
  and every ROS-level check, is unaffected.

### Added

- `robots*.yaml` gained a `drive:` block: `speed` and `loop` per truck, and
  `heading_offset` / `no_invert` / `angular_speed` per model, folded into each
  robot the same way `control:` and `odometry:` already are. Adding a truck is a
  block there plus a compose service naming its `ROBOT_ID` — no new env var. The
  controller logs where each value came from, so an odd run says why in its
  first ten lines.
- `SCENARIO` / `scenarios.yaml`, above. `experimental: true` marks a scene with
  no published calibration, and is printed at launch instead of living in prose.

- Two-forklift deployments via the opt-in `multi-robot` compose profile (`COMPOSE_PROFILES=sil,multi-robot`), which starts a second controller. Isaac is launched with a matching robots config.
- Forklifts can be spawned from `robots.yaml` `spawn:` blocks instead of being baked into the scene USD. The generated overlay layer is written to `sil/scenes/generated/`, which `setup.sh` creates and hands to the container user.
- `COMM_ROBOT_IDS` — comm-layer mirrors the mute decision onto `/<robot>/safety/is_muted` for each listed robot. Every mirror carries the same value: the decision is made for a camera-covered zone, not for a named truck.
- 40x20 two-dock scene and its config trio. **Experimental / internal-only** — no calibration is published with it, so VSS runs 20x20 geometry against it and the safety numbers do not mean anything.
- `deployments/scripts/preflight.py` — cross-checks the robots config, controller services, waypoint files and `COMM_ROBOT_IDS` before Isaac boots, and compares each waypoint origin against where its truck stands.

### Fixed

- `safety-core/configs/sensor_config.conf` pointed at the retired 8553 mediamtx broker and the old `RTSPWriter_*` mount names; it now matches the Isaac 6.0 mounts in `cameras.yaml`. Only read when SAIM runs (`PSF_LAUNCH_MODE=active`).
- `forklift-controller/entrypoint.sh` defaulted `--heading-offset` to `0` where every other layer says `180`. Masked until now by the Dockerfile `ENV`.
- The waypoint generator opened the uncalibrated 40x20 map by default.
- `robots*.yaml` `drive:` was the only block in this config family that accepted
  unknown keys. `robots-40x20.yaml` had already lost the disc's `segments:` and
  `height_offset:` to it, one indentation level too far — merged, never read,
  and identical on screen because both values matched the loader's defaults.
  The loader now names the drive knobs and refuses the rest, saying to check the
  indentation; `fleet_config` asserts its defaults cover the same list, so the
  two halves cannot drift.
- `preflight.py` checked whichever fleet file was named on the command line
  without asking whether the run would read that one. A wrong `--robots-config`
  produced a green exit code describing a fleet nobody was launching; it is now
  an error naming the file the scenario actually selects.
- `entrypoint.sh` refuses a leftover `FORKLIFT_WAYPOINTS_DIR`, but compose never
  passed the variable through, so the refusal could not fire on the one path the
  docs teach. The env anchor now forwards it.
- Isaac reported the fleet file as coming `from ROBOTS_CONFIG` even when the
  scenario supplied it and that variable was empty — sending anyone debugging it
  after the fact to look for a variable no profile sets.
- `launch_controller.sh` passed `--speed 1` and four other drive knobs as flags.
  A flag outranks the fleet file, so the repo's own launcher drove the truck at
  1.0 m/s while `robots.yaml` said 1.5 — the fragmentation this release exists
  to remove, in the one script a developer runs by hand. It now reads the same
  fleet file Isaac does; `CONFIGS_DIR` / `ROBOTS_CONFIG` override it.
- `--no-namespace` was deleted from the parser but `parse_known_args` handed it
  to `rclpy`, which ignores it: a 1.3 caller's flag did nothing and said
  nothing. Removed flags are now refused by name, the way `entrypoint.sh`
  already refuses `FORKLIFT_WAYPOINTS_DIR`. Genuine ROS arguments still pass
  through untouched.
- `preflight.py` caught a robot with `control:` enabled and no controller
  service, but not the reverse: a controller service driving a robot whose
  `control:` is disabled publishes into a topic Isaac never subscribes, and the
  truck stands still with neither side logging anything. Both directions are
  errors now. No shipped config sets `enabled: false`, so this closes a hole
  rather than fixing an observed failure.
- `run_sdg.sh` still documented `-c` as required after `--scenario` began
  supplying the IRA config. The wrapper is a pure passthrough, so only the usage
  text was wrong; the launch itself already worked without it.

### Changed

- The second controller is no longer a copy of the first: both services share
  one env anchor and one volume anchor, and differ only by `ROBOT_ID`. Its
  speed and route come from `robots-40x20.yaml` like every other truck's.
- **The controller image is again the only source of the code it runs.** The
  `.:/app:ro` bind that mounted the host checkout over `/app` is gone, so
  `docker run forklift-controller:latest` behaves like the compose stack.
  Editing controller source now needs `up -d --build`. Two defects came out of
  that bind and are fixed with it: `fleet_config.py` was missing from the
  Dockerfile (the image alone died at import, which only `docker_run.sh` ever
  hit), and the new `/app/robots` and `/app/isaac` mounts had nowhere to attach
  under a read-only `/app` — on a fresh clone the container never started at
  all. Neither was visible to a static check: git does not track empty
  directories, and the bind hid the missing file.
