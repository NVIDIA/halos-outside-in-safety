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

`forklift_b` is `FORKLIFT_ROBOT_ID`; a differently-named robot namespaces under its own name.

**Migrating from 1.3**

- Scripts, RViz configs and dashboards that name any topic above: add the `<robot>/` prefix. RViz users also set the fixed frame to `forklift_b/odom`.
- `FORKLIFT_USE_NAMESPACE=false` does **not** restore the old names. It points the controller at the bare topics while Isaac keeps publishing the namespaced ones, so the truck stops moving with nothing in any log. Leave it `true`.
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
- Drive knobs (`FORKLIFT_BASE_SPEED`, `FORKLIFT_HEADING_OFFSET`,
  `FORKLIFT_LOOP_PATH`, `FORKLIFT_ANGULAR_SPEED`, `FORKLIFT_NO_INVERT`,
  `FORKLIFT_END_TOLERANCE`, `FORKLIFT_END_POSE_COUNT`,
  `FORKLIFT_SPIRAL_TIMEOUT`) are no longer read from the environment. They were
  documented nowhere and set in no shipped profile, and one env var would have
  set every truck in the fleet at once. Per-truck values live in the robots
  config; `docker_run.sh` and `launch_controller.sh` still pass them as flags.

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

### Changed

- The second controller's knobs are `FORKLIFT_B2_*` env vars instead of literals in `forklift-controller.yml`; defaults are unchanged, so a deployment that sets nothing behaves exactly as before.
- `sil.env` and `hil.env` now list the forklift driving knobs (speed, heading, loop, tolerances) that were previously settable but documented nowhere.
