# Changelog

## Unreleased

### Breaking — PSF image with configured SDMs

`sil` and `base` move to the PSF 1.4 image, whose launcher no longer starts an SDM without its event binding. `PSF_IMAGE` is a placeholder until that image is published on NGC.

- `safety-core.yml` passes `--sdm-config configs/<PSF_APP>_sdm.conf` (`atl_sdm.conf`, `pxc_sdm.conf`) and mounts `sensor_pipelines_config.conf`, the identities events arrive under. The 3D feed sets `PSF_SENSOR_PIPELINES_CONFIG_SRC=./configs/sensor_pipelines_config_bev.conf` next to `PSF_SENSOR_CONFIG_SRC`.
- `nvpss.conf` states the now-mandatory `deploymentMode = 2D` and `enableFusion = true`, adds the PSS-to-PSD delivery and queue keys, and drops `PSSDToPSDComBackend`.
- Proximity events move from `EVENT_8/9/10` to `EVENT_12/13/14`: `EVENT_0..11` are reserved for ATL.
- `pxc_sdm.log` is written to `/var/log/psf/pxc_sdm.log` in the container; the host file is unchanged.
- `PSF_APP=both` is not ported: its launcher starts the SDMs without `--config`, which this image refuses.
- The Thor profiles stay on 1.3 until an aarch64 host package of the same build is published.

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

- **Isaac Sim 6.0.0 → 6.1.0.** Both profiles, the compose default and the
  Dockerfile `ARG` name `nvcr.io/nvidia/isaac-sim:6.1.0`; both IRA configs
  declare `version: 1.7.0`; and the asset-root override plus the pinned
  `forklift_b.usd` URL move to `Assets/Isaac/6.1`.

  IRA goes 1.6.7 → 1.7.9 and its config schema is additive over 1.6 —
  `spawn_positions` / `spawn_orientations` on character and robot groups, and a
  sensor group that takes `num` on its own. Nothing was renamed or removed, so
  the shipped configs validate against the 1.7 schema unchanged.
  `SimulationManager` exposes the same methods, the ROS 2 bridge still ships
  `jazzy` at the same path, and the 6.1 asset root is a superset of 6.0.

  One behaviour changed without affecting anything shipped here: a relative
  robot `config_file_path` now resolves against the IRA config's own directory
  before the IAR sample dir. No config in this repo uses one, but a deployment
  that does should check which file it is about to pick up.

  Known limitations it touches: the concurrent-DESCRIBE "has no caps" race
  (NVBug 6478845) is fixed upstream and measured gone — see `troubleshooting.md`.
  6.1 also carries fixes for RTP delivery being interrupted by another client's
  DESCRIBE/PAUSE/TEARDOWN (6477478) and for NVENC sessions leaking when
  compressed annotators detach (6478175); neither original repro (the multi-hour
  RTSP-over-UDP wedge, UI stop/play on a GeForce 8-session cap) has been rerun, so
  VST ingest stays on TCP.

  **Needs `up -d --build`** — the `isaac-sim-sil` image rebuilds on the new base.

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

- **Proximity support touches every run, `atl` included.** Re-run `setup.sh`,
  then `up -d --build`: the controller image now copies `proximity_gate.py`,
  and without a rebuild the old controller ignores proximity while Isaac, from
  the mounted tree, subscribes to a state topic nobody publishes.
  - safety-core always mounts `pxc_sdm.log` and the proximity mapping;
    `setup.sh` creates the log files and refuses to continue if Docker has
    already turned one into a directory, and `cleanup_all_datalog.sh` truncates
    all three instead of deleting two of them.
  - `nvpss.conf` bypasses fusion for `EVENT_8/9/10` as well.
  - comm-layer adds the `Proximity*` OPC UA nodes. Of these only
    `ProximitySafeReleaseRequest` is writable: writing `True` asks the SDM to
    leave a latched safe state. The receiver no longer sends a release request
    by itself; `COMM_PXC_STARTUP_RELEASE=1` restores one automatic request,
    sent only after the SDM has reported a latch.
  - The forklift-controller publishes `/<robot>/proximity/state`, and
    `/<robot>/state` gains `proximity`, `proximity_separation_m` and
    `proximity_fault`. Its proximity gate is on only when `PSF_APP` is `pxc` or
    `both`, unless the robot's `proximity.enabled` says otherwise.
  - `PSF_APP` must be `atl`, `pxc` or `both`, exactly; anything else is an
    error where Isaac and the controller read it, instead of a quiet fallback
    to ATL.

### Removed

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
- PSF proximity in the SIL loop (`PSF_APP=pxc`, 3D feed, SIL only). comm-layer decodes the `0xA5` packets onto `/safety/proximity/mode` and `/safety/proximity/pair` (opcodes 0x02/0x07 mean the opposite of ATL's, so nothing touches `/safety/is_muted`). The OPC node keeps the most severe decision visible for 0.3 s, so a STOP followed at once by a NORMAL or a heartbeat still reaches the 10 Hz bridge; unknown opcodes are published as a fault. The forklift-controller caps the truck at `proximity.reduce_speed` on REDUCE and stops it on STOP, each held past the last packet, and stops it with fault `psf_link_stale` when nothing at all (heartbeats included) has arrived for `proximity.stale_s` (10 s). The indicator disk shows the level it applies — green / orange / red, grey before PSF's first decision or with proximity disabled. `proximity_event_mapping.pb.txt` scores Forklift × Person for the 20x20 scene (STOP ≤ 2 m, REDUCE ≤ 3.5 m). A 3D feed also needs `PSF_SENSOR_CONFIG_SRC=./configs/sensor_config_bev.conf`. One truck per decision stream: PSF names no robot, so the controller warns when several obey the same `proximity.pair_topic`. `send_packet.py --proximity` injects 0xA5 packets without PSF.
- `PSF_APP=both` with `-f closed-loop-testing/safety-core/atl-pxc-override.yaml` (or `COMPOSE_FILE`, see `sil.env`): ATL and proximity in one PSF container — one gateway, one daemon, one `mdx_client` on the two mappings concatenated, and both SDMs (two containers would collide on the gateway port and split mdx_client's fixed Kafka consumer groups). The disk shows red / orange while proximity acts on the truck, otherwise green muted / yellow alarm.
- `robots.yaml` `proximity_line:` (off by default): Isaac draws the pair of each PSF proximity decision as a line on the floor between the two positions PSF sent, in the decision's colour, with the separation ("1.97 m", white on a plate of that colour) lying flat beside it, facing the first camera in `cameras.yaml` (`label_camera` to pick another). PSF's coordinates are the scene's world frame, so nothing is matched to the stage. Geometry: the streams carry it and perception sees it.
- `comm-layer/tests/test_proximity_chain.py`: the 0xA5 path from bytes to the bridge — decoder split, ACKs, latch handling, the decision hold, OPC payload. Runs in the comm-layer image (command in the file header).

### Fixed

- `safety-core/configs/sensor_config.conf` pointed at the retired 8553 mediamtx broker and the old `RTSPWriter_*` mount names; it now matches the Isaac 6.1 mounts in `cameras.yaml`. Only read when SAIM runs (`PSF_LAUNCH_MODE=active`).
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
