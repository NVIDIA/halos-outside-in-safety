# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Isaac Sim 6.0 driver for Halos SIL (run_actor_sdg.py).
# -----------------------------------------------------------------------
# Source baseline: Isaac Sim 5.1.
# Target:          Isaac Sim 6.0.
#
# Summary of changes vs 5.1 driver:
#   1. SimulationManager() → SimulationManager.get_instance() (singleton)
#   2. set_up_simulation_from_config_file + register_set_up_simulation_done_callback
#        → await setup_simulation() (async; fires SET_UP_SIMULATION_DONE_EVENT internally)
#   3. run_data_generation_async → start_data_generation
#   4. Property-bag (get_config_file_property_group / get_config_file_property) GONE
#        → typed access via config = sim_mgr.get_config_file()
#   5. _do_camera_placement / _read_camera_json / _place_cameras / _place_one_camera DELETED
#        Camera placement is declarative via sensor.groups.<g>.aim_at_targets in YAML.
#        --sensor_placement_file CLI arg is INFORMATIONAL ONLY in 6.0 (kept for back-compat).
#   6. 9 carb settings DROPPED (silently ignored in 6.0):
#        aim_cameras_at_characters, min/max_camera_{distance,height,look_down_angle},
#        character_focus_height, frame_write_interval.
#        Values now live in YAML aim_at_targets.* (see default_config_ros.yaml).
#   7. _enable_extensions list:
#        REMOVE: omni.anim.people
#        ADD:    omni.anim.behavior.tree, omni.anim.behavior.core
#        ADD:    isaacsim.streaming.rtsp (replaces RTSPWriter; declarative RTSP via OmniGraph)
#   8. VST integration call site UNCHANGED. cameras.yaml new schema (port + mount_path per
#        camera) is handled inside vst_sensor_manager.add_sensors_from_config.

import argparse
import asyncio
import os
import sys

# NOTE: `import numpy as np` removed — only used by deleted _place_one_camera.

from isaacsim import SimulationApp

# Experience file for action and event data generation.
BASE_EXP_PATH = os.path.join(os.environ.get("EXP_PATH", ""), "isaacsim.exp.action_and_event_data_generation.base.kit")


class ActorSDGRunner:
    def __init__(
        self,
        sim_app,
        config_file_path,
        auto_start=False,
        setup_only=False,
        camera_file_path=None,
        crash_report_path=None,
        debug_print=False,
        save_usd=False,
        enable_vst=False,
        cameras_config_path=None,
        enable_runtime_patches=True,
        enable_rtsp=True,
        enable_camera_spawn=True,
        robots_config_path=None,
        enable_forklift=True,
        enable_clock=True,
        enable_srr_gt=False,
        postprocessing_config_path=None,
        enable_postprocessing=True,
        vst_register_warmup_sec=1.0,
        vst_register_timeout_sec=1200.0,
    ):
        self._sim_app = sim_app
        # Inputs
        self.config_file_path = config_file_path
        self.auto_start = auto_start
        self.setup_only = setup_only
        # NOTE(IRA 6.0): camera_file_path retained for CLI back-compat but is INFORMATIONAL
        # ONLY in 6.0. Camera placement is declarative via YAML sensor.groups.<g>.aim_at_targets.
        # Logged at setup time if non-None; otherwise unused.
        self.camera_file_path = camera_file_path
        self.crash_report_path = crash_report_path
        self.debug_print = debug_print
        self.save_usd = save_usd

        # VST Integration
        self.enable_vst = enable_vst
        self.cameras_config_path = cameras_config_path
        self._vst_manager = None
        self._vst_cleaned = False  # Track cleanup status
        # Defer VST registration until the RTSP encoder is warm (producing frames).
        # Registering while Isaac's in-process RTSP is still cold makes VST
        # DESCRIBE-churn a capless stream, which wedges the media ("has no caps",
        # DeepStream 0 sources) with no self-recovery. See _render_is_warm() + run().
        self.vst_register_warmup_sec = vst_register_warmup_sec      # sim-time (s) to advance under Play before registering
        self.vst_register_timeout_sec = vst_register_timeout_sec    # wall-clock (s) ceiling -> register best-effort
        self._warm_t0 = None          # monotonic wall-clock latched when Play first seen
        self._warm_time0 = None       # timeline time latched when Play first seen
        self._warm_last_log = 0.0     # heartbeat throttle

        # Post-setup stage modifiers + ActionGraph builders.
        # See closed-loop-testing/isaac-sim/sil/scripts/action_graphs/README.md
        # for the pattern + lifecycle ordering.
        self.enable_runtime_patches = enable_runtime_patches
        self.enable_rtsp = enable_rtsp
        # Dynamic camera spawning from cameras.yaml `spawn:` blocks.
        # Config-driven no-op when no camera carries a spawn block.
        # See sil/scripts/camera_loader.py.
        self.enable_camera_spawn = enable_camera_spawn

        # Forklift control/odometry/safety + clock graphs (replace the
        # baked OmniGraphs that used to live in the scene USD). Driven by
        # robots.yaml. See action_graphs/forklift_control.py + clock.py.
        self.robots_config_path = robots_config_path
        self.enable_forklift = enable_forklift
        self.enable_clock = enable_clock

        # SRR regression-harness ground-truth /gt/*/tf publisher (opt-in via
        # --srr-gt; default OFF). See action_graphs/srr_ground_truth.py.
        self.enable_srr_gt = enable_srr_gt

        # In-scenario sensor-anomaly injection (low light, colour cast, flicker,
        # sensor grain). Not an OmniGraph: RTX consumes post-processing itself,
        # so a preset lands on the next rendered frame in the viewport, the SDG
        # RGB output and the RTSP streams alike. Driven by configs/
        # postprocessing.yaml and hot-reloaded from the run loop.
        # See sil/scripts/postprocessing/README.md.
        self.postprocessing_config_path = postprocessing_config_path
        self.enable_postprocessing = enable_postprocessing
        self._postprocessing = None

        self.output_path = None
        # NOTE(IRA 6.0): camera_placements_json, _setup_sim_sub, _setup_sim_succeed removed —
        # all part of the 5.1 callback + manual camera placement paths.
        self._sim_manager = None
        self._settings = None

    async def run(self):
        # Enable all required extensions
        self._enable_extensions()
        await self._sim_app.app.next_update_async()

        # Set up global settings
        self._set_simulation_settings()
        await self._sim_app.app.next_update_async()


        # Init SimulationManager (singleton in 6.0).
        from isaacsim.replicator.agent.core.simulation import SimulationManager

        self._sim_manager = SimulationManager.get_instance()

        try:
            can_load_config = self._sim_manager.load_config_file(self.config_file_path)
            if not can_load_config:
                print(f"ERROR: Failed to load config file: {self.config_file_path}", file=sys.stderr)
                return False

            # Typed config access (6.0). The 5.1 property-bag API
            # (get_config_file_property_group("replicator", "writer_selection")) is GONE.
            # `output_dir` is optional in 6.0 — when streaming RTSP only (no offline writer),
            # the IRABasicWriter entry may omit `output_dir`, in which case it will be None.
            config = self._sim_manager.get_config_file()
            self.output_path = self._extract_output_path(config)

            print("Config loaded successfully")
            print(f"Output path: {self.output_path}")

            if self.camera_file_path:
                # 5.1 used this JSON to drive _place_one_camera. In 6.0 it has no functional
                # effect — placement is declarative via YAML. Log so users notice the change.
                print(
                    f"NOTE: --sensor_placement_file ({self.camera_file_path}) is informational "
                    f"only in IRA 6.0. Camera placement is now driven by "
                    f"sensor.groups.<g>.aim_at_targets in the YAML config."
                )

            # Set up simulation (async; no callback registration needed in 6.0).
            # IRA 6.0 fires IRAEvents.SET_UP_SIMULATION_DONE_EVENT itself; this coroutine
            # returns after setup completes.
            print("Setting up simulation...")
            await self._sim_manager.setup_simulation()
            print("Simulation setup complete!")

            # Post-setup stage modifiers + ActionGraph builders. Order matters:
            #   1. runtime_patches.apply_halos_runtime_patches deactivates the
            #      legacy 5.1 baked Character prims and moves IRA-spawned
            #      chars to deterministic positions. Must run BEFORE Play so
            #      chars are at canonical positions when BT first ticks.
            #   2. action_graphs.build_rtsp_graph wires N RTSP streams on top
            #      of camera prims that already exist on the stage (no dep
            #      on char positions).
            # USD-prim tweaks live in sil/scripts/runtime_patches/.
            # OmniGraph builders live in sil/scripts/action_graphs/.
            # Both packages are idempotent.
            if self.enable_runtime_patches:
                from runtime_patches import apply_halos_runtime_patches
                apply_halos_runtime_patches()
            #   2a. camera_loader spawns Camera prims declared with a
            #      `spawn:` block in cameras.yaml. Must run BEFORE
            #      build_rtsp_graph (the RTSP builder fail-fasts on
            #      missing camera prims).
            if self.enable_camera_spawn and self.cameras_config_path:
                from camera_loader import spawn_cameras
                spawn_cameras(self.cameras_config_path)
            if self.enable_rtsp and self.cameras_config_path:
                from action_graphs import build_rtsp_graph
                build_rtsp_graph(self.cameras_config_path)
            #   3. action_graphs.build_forklift_graphs wires the per-robot
            #      control + odometry + safety indicator graphs, replacing
            #      ROS_Forklift_Control_Graph / Odometry_Graph /
            #      Safety_indicator_Graph that used to be baked into the USD.
            #   4. action_graphs.build_clock_graph wires the /clock publisher
            #      (was Clock_Publisher_Graph). Both read robots.yaml.
            if self.enable_forklift and self.robots_config_path:
                from action_graphs import build_forklift_graphs
                build_forklift_graphs(self.robots_config_path)
            if self.enable_clock and self.robots_config_path:
                from action_graphs import build_clock_graph
                build_clock_graph(self.robots_config_path)
            #   5. action_graphs.build_srr_gt_graph publishes /gt/*/tf for the
            #      SRR regression harness (opt-in via --srr-gt, default OFF).
            #      Runs LAST — after IRA spawned the chars + runtime_patches
            #      repositioned them — then pumps live Fabric transforms.
            if self.enable_srr_gt:
                from action_graphs import build_srr_gt_graph
                build_srr_gt_graph()
            #   6. postprocessing applies the active RTX preset and then polls
            #      its config, so an operator can inject or lift a sensor
            #      anomaly mid-run. Runs after the RTSP graph so a per-camera
            #      preset can bind to the render products that graph declares.
            #      A bad preset must not take the SIL loop down with it, so
            #      failures here are warnings — the run continues undegraded.
            if self.enable_postprocessing and self.postprocessing_config_path:
                try:
                    from postprocessing import PostProcessingController

                    self._postprocessing = PostProcessingController(
                        self.postprocessing_config_path,
                        cameras_config_path=self.cameras_config_path,
                    )
                    self._postprocessing.start()
                except Exception as e:
                    print(f"WARNING: post-processing disabled ({e})")
                    self._postprocessing = None

            # VST Integration: registration is DEFERRED to after Play + render-warm
            # (see the run loop below). Registering here — before the RTSP encoder is
            # producing frames — makes VST DESCRIBE-churn Isaac's cold in-process RTSP
            # media and wedges it ("has no caps", DeepStream 0 sources), no self-recovery.

            # If setup-only mode, don't start data generation
            if self.setup_only:
                print("Setup complete. Waiting for manual data generation start...")
                while not self._sim_app.is_exiting():
                    await self._sim_app.app.next_update_async()
                    self._update_postprocessing()
                return True

            # If auto-start, press Play on the timeline (RTSP + BT start ticking).
            if self.auto_start:
                import omni.timeline
                print("Pressing Play (timeline.play())")
                omni.timeline.get_timeline_interface().play()
            else:
                print("Simulation ready. Waiting for data generation...")
                print("Use the UI to start data generation or pass --start flag")

            # Run loop. VST sensor registration is deferred until the RTSP encoder is
            # warm (producing frames) — see _render_is_warm(). This holds for both the
            # auto-start path and a manual UI Play, so VST never DESCRIBEs a capless
            # stream (the cause of the "has no caps" / DS-0 cold-restart wedge).
            vst_registered = False
            while not self._sim_app.is_exiting():
                await self._sim_app.app.next_update_async()
                self._update_postprocessing()
                if self.enable_vst and not vst_registered and self._render_is_warm():
                    self._vst_register_cameras()
                    vst_registered = True

            return True

        except Exception as e:
            import carb
            import traceback

            carb.log_error(f"Failed to run Actor SDG: {e}")
            traceback.print_exc()
            return False

        finally:
            # Put the renderer back as it was: a preset lives in carb settings
            # and the session layer, both of which outlive this coroutine when
            # Kit stays up (UI mode, setup re-entry).
            if self._postprocessing is not None:
                self._postprocessing.close()
                self._postprocessing = None

            # VST Integration: Cleanup cameras on exit (fallback for UI / abnormal exit)
            if self.enable_vst and self._vst_manager:
                try:
                    if hasattr(self, "_vst_cleaned") and self._vst_cleaned:
                        print("VST already cleaned up, skipping")
                    else:
                        self._vst_cleanup_cameras()
                except Exception as e:
                    print(f"WARNING: Finally VST cleanup failed: {e}")

    def _update_postprocessing(self):
        """Re-read the preset config when due and advance any animated effect.

        Runs once per rendered frame, so an exception here would otherwise spam
        the log at render rate; the first failure retires the controller and the
        run continues without post-processing.
        """
        if self._postprocessing is None:
            return
        try:
            self._postprocessing.update()
        except Exception as e:
            print(f"WARNING: post-processing update failed, disabling: {e}")
            self._postprocessing = None

    def _extract_output_path(self, config):
        """Extract IRABasicWriter output_dir from typed RootConfig, tolerant of missing keys.

        In 6.0, replicator.writers is a Map[str, WriterEntry] keyed by names matching
        ^(IRABasicWriter|CustomWriter|CosmosIRAWriter|SceneGraphWriter)(_\\d+)?$. Halos SIL
        configs may omit any offline writer entirely (RTSP-only via isaacsim.streaming.rtsp).
        Return None when no offline output is configured.
        """
        try:
            writers = getattr(getattr(config, "replicator", None), "writers", None)
            if not writers:
                return None
            # Prefer the canonical name; fall back to whatever writer is present.
            writer_cfg = None
            if "IRABasicWriter" in writers:
                writer_cfg = writers["IRABasicWriter"]
            else:
                # TODO(verify-6.0): Decide whether to honor non-IRABasicWriter writers
                # (CustomWriter, CosmosIRAWriter, SceneGraphWriter) for output_path
                # derivation in --save_usd flow.
                writer_cfg = next(iter(writers.values()), None)
            # output_dir lives on WriterEntry.parameters (the writer-specific sub-model), not on WriterEntry itself.
            params = getattr(writer_cfg, "parameters", None) if writer_cfg else None
            output_dir = getattr(params, "output_dir", None)
            return output_dir or None
        except Exception as e:
            print(f"WARNING: Could not extract output_dir from config: {e}", file=sys.stderr)
            return None

    def _vst_register_cameras(self):
        """Register RTSP cameras with VST after simulation setup.

        NOTE(IRA 6.0): cameras.yaml schema has changed (per-camera `port` + `mount_path`;
        RTSP is now served by the isaacsim.streaming.rtsp extension, not RTSPWriter).
        The VSTSensorManager.add_sensors_from_config() implementation handles the new
        schema. This driver passes the path through unchanged.
        """
        try:
            from vst_sensor_manager import VSTSensorManager

            print("=" * 60)
            print("VST Integration: Registering cameras...")
            print("=" * 60)

            self._vst_manager = VSTSensorManager()

            # First, remove all existing sensors
            print("Removing existing sensors from VST...")
            self._vst_manager.delete_all_sensors()

            # Add cameras from config file
            if self.cameras_config_path and os.path.exists(self.cameras_config_path):
                print(f"Loading cameras from: {self.cameras_config_path}")
                result = self._vst_manager.add_sensors_from_config(self.cameras_config_path)
                print(f"Registered {result['added']}/{result['attempted']} camera(s) with VST")
                if not result["schema_ok"]:
                    print(f"WARNING: camera config schema rejected: {result.get('schema_hint')}")
                for err in result["errors"]:
                    print(f"WARNING: VST sensor registration: {err}")
            else:
                print(f"WARNING: Cameras config not found: {self.cameras_config_path}")

            print("=" * 60)

        except ImportError as e:
            print(f"WARNING: VST integration unavailable (missing module): {e}")
        except Exception as e:
            print(f"WARNING: VST registration failed: {e}")

    def _render_is_warm(self):
        """Return True once the RTSP encoder is producing frames, so VST can
        register without DESCRIBE-churning a capless stream.

        isaacsim.streaming.rtsp exposes no first-frame signal, so we use a robust
        proxy: OnPlaybackTick -> RenderProduct -> RTSPCameraHelper encodes one frame
        per playback tick, and the timeline only advances as frames are actually
        rendered (a cold scene stalls the timeline while shaders/PhysX compile). So
        once the timeline has been PLAYING and advanced by `vst_register_warmup_sec`
        of sim-time, warm encoded frames exist and a VST DESCRIBE negotiates caps
        immediately instead of wedging. A wall-clock timeout registers best-effort so
        a permanently-cold scene (GPU/navmesh fault) surfaces instead of hanging.
        """
        import time
        import omni.timeline

        tl = omni.timeline.get_timeline_interface()
        if not tl.is_playing():
            return False

        now = time.monotonic()
        t = tl.get_current_time()

        # Latch the baseline the first time we observe Play.
        if self._warm_t0 is None:
            self._warm_t0 = now
            self._warm_time0 = t
            print(
                f"[vst-warmup] Play detected — deferring VST registration until render "
                f"warms (>= {self.vst_register_warmup_sec:.1f}s sim-time advance).",
                flush=True,
            )
            return False

        advanced = t - self._warm_time0
        elapsed = now - self._warm_t0

        if advanced >= self.vst_register_warmup_sec:
            print(
                f"[vst-warmup] Render warm: sim-time advanced {advanced:.1f}s in "
                f"{elapsed:.0f}s wall -> registering VST sensors now.",
                flush=True,
            )
            return True

        # Best-effort fallback: a scene that never renders (GPU/navmesh fault) would
        # otherwise defer forever. Register anyway past the ceiling, loudly.
        if elapsed >= self.vst_register_timeout_sec:
            print(
                f"[vst-warmup] WARNING: {elapsed:.0f}s elapsed but sim-time only advanced "
                f"{advanced:.1f}s (render not warming - check GPU/scene/navmesh). "
                f"Registering VST best-effort; stream may still be cold.",
                flush=True,
            )
            return True

        # Heartbeat ~every 5s so a long warm-up is visible in the log.
        if now - self._warm_last_log >= 5.0:
            self._warm_last_log = now
            print(
                f"[vst-warmup] waiting: sim-time advanced {advanced:.1f}/"
                f"{self.vst_register_warmup_sec:.1f}s (wall {elapsed:.0f}s)...",
                flush=True,
            )
        return False

    def _vst_cleanup_cameras(self):
        """Remove all cameras from VST on shutdown."""
        try:
            if self._vst_manager:
                print("=" * 60)
                print("VST Integration: Cleaning up cameras...")
                print("=" * 60)
                self._vst_manager.delete_all_sensors()
                print("VST cleanup complete")
                print("=" * 60)

                # Mark as cleaned to avoid double cleanup
                self._vst_cleaned = True
        except Exception as e:
            print(f"WARNING: VST cleanup failed: {e}")

    def _enable_extensions(self):
        import omni.kit.app

        ext_manager = omni.kit.app.get_app().get_extension_manager()

        # Required extensions for Actor SDG on Isaac Sim 6.0.
        # Diff vs 5.1:
        #   REMOVE: omni.anim.people (replaced by Open Behavior Tree pipeline)
        #   ADD:    omni.anim.behavior.tree, omni.anim.behavior.core (BT runtime)
        #   ADD:    isaacsim.streaming.rtsp (replaces removed RTSPWriter; per-camera RTSP)
        extensions = [
            "omni.kit.viewport.window",
            "omni.kit.manipulator.prim",
            "omni.kit.property.usd",
            "omni.kit.scripting",
            "omni.anim.timeline",
            "omni.anim.graph.core",
            "omni.anim.retarget.core",
            "omni.anim.navigation.core",
            # Behavior Tree replacements for omni.anim.people:
            "omni.anim.behavior.core",
            "omni.anim.behavior.tree",
            # IRA core + UI (still required in 6.0).
            "isaacsim.replicator.agent.core",
            "isaacsim.replicator.agent.ui",
            "omni.kit.mesh.raycast",
            "omni.physx.graph",  # Conveyor Belt
            # OmniGraph / Action Graph
            "omni.graph.core",
            "omni.graph.action",
            "omni.graph.nodes",
            "omni.graph.scriptnode",
            "omni.graph.window.action",
            "omni.graph.window.generic",
            # ROS2 Bridge
            "isaacsim.ros2.bridge",
            # RTSP streaming (replaces removed RTSPWriter). Provides RTSPCameraHelper
            # OmniGraph node + RTSPStreamWriter Replicator writer.
            "isaacsim.streaming.rtsp",
        ]

        for ext in extensions:
            ext_manager.set_extension_enabled_immediate(ext, True)

        print(f"Enabled {len(extensions)} extensions")

    def _set_simulation_settings(self):
        import carb
        import omni.replicator.core as rep

        rep.settings.carb_settings("/omni/replicator/backend/writeThreads", 16)
        self._settings = carb.settings.get_settings()
        self._settings.set("/app/scripting/ignoreWarningDialog", True)
        self._settings.set("/persistent/exts/omni.anim.navigation.core/navMesh/viewNavMesh", False)
        # NOTE(IRA 6.0): /exts/omni.anim.people/navigation_settings/navmesh_enabled REMOVED —
        # omni.anim.people extension is no longer enabled; setting would be ignored anyway.

        # NavMesh bake config (dev3 MoveTo-failed regression).
        # omni.anim.navigation.core 110.1.3 bumped the DEFAULT bake min agent
        # height 180->200 cm "and uses it as the navmesh minimum clearance"
        # (CHANGELOG 110.1.3). The warehouse scene authored navmeshSettings in
        # customLayerData (meters): agentMinHeight 1.0m, agentMinRadius 0.1m,
        # agentMaxRadius 1.0m, agentMaxStepHeight 0.5m, agentMaxFloorSlope 10,
        # agentMinIslandRadius 5.0m. To bake against the authored intent (and
        # restore the 5.1/alpha walkable area so the Halos baked spawn points
        # stay on the navmesh), pin the carb bake settings (centimeters) before
        # setup_simulation, where IRA bakes the navmesh + spawns characters.
        _NAV_CFG = "/exts/omni.anim.navigation.core/navMesh/config"
        for _key, _cm in (
            ("agentMinHeight", 100.0),
            ("agentMinRadius", 10.0),
            ("agentMaxRadius", 100.0),
            ("agentMaxStepHeight", 50.0),
            ("agentMaxFloorSlope", 10.0),
            ("agentMinIslandRadius", 500.0),
        ):
            self._settings.set(f"{_NAV_CFG}/{_key}", _cm)
            self._settings.set(f"/persistent{_NAV_CFG}/{_key}", _cm)

        # Behavior-core auto-avoidance (recurring `MoveTo failed status=2` on 6.0).
        # omni.anim.behavior.core exposes an "auto avoidance" feature
        # (enableAutoAvoidance): any dynamic object whose mass exceeds
        # defaultAutoAvoidanceMass is auto-registered as a crowd-sim "threat" the
        # character must steer around. Per the behavior.core CHANGELOG this feature
        # was introduced in the 109.x line and refined in 110.0.12 (predictive
        # time-to-collision); it is default-ON in the Isaac Sim 6.0 behavior.core.
        # The Isaac Sim 5.1 baseline (behavior.core 107.3.x) had no such setting, so
        # the worker loops were authored for un-obstructed navigation.
        # In the Halos scene the moving forklift_b is registered as a tracked threat
        # (log: `trackThreat: Added tracked threat /World/forklift_b/body
        # (approxRadius=1.48)`) and the MoveTo failures (status=2, error=1) recur in
        # ~60s bursts matching the forklift loop. Offline navmesh queries
        # (query_closest_point / query_shortest_path on island 2) all succeed, ruling
        # out navmesh geometry. The trackThreat -> MoveTo-failure link is strongly
        # suspected (not yet traced to the exact node emitter). Disable auto-avoidance
        # to restore the 5.1 navigation behavior so workers follow their deterministic
        # SDG loops.
        self._settings.set("/exts/omni.anim.behavior.core/enableAutoAvoidance", False)
        self._settings.set("/persistent/exts/omni.anim.behavior.core/enableAutoAvoidance", False)

        # NOTE(IRA 6.0): All 9 of the following carb settings are SILENTLY IGNORED in 6.0
        # (not present in the Isaac Sim 6.0 source). Their values must now live
        # in the YAML config under sensor.groups.<g>.aim_at_targets.* — now in
        # default_config_ros.yaml. Removed:
        #   /persistent/exts/isaacsim.replicator.agent/aim_cameras_at_characters
        #   /persistent/exts/isaacsim.replicator.agent/min_camera_distance
        #   /persistent/exts/isaacsim.replicator.agent/max_camera_distance
        #   /persistent/exts/isaacsim.replicator.agent/max_camera_look_down_angle
        #   /persistent/exts/isaacsim.replicator.agent/min_camera_look_down_angle
        #   /persistent/exts/isaacsim.replicator.agent/min_camera_height
        #   /persistent/exts/isaacsim.replicator.agent/max_camera_height
        #   /persistent/exts/isaacsim.replicator.agent/character_focus_height
        #   /persistent/exts/isaacsim.replicator.agent/frame_write_interval

        self._settings.set("/app/omni.graph.scriptnode/enable_opt_in", False)
        self._settings.set("/rtx/raytracing/fractionalCutoutOpacity", True)

        # Logging and debug print
        self._settings.set("/log/level", "info")
        self._settings.set("/log/channels/omni.replicator.core", "info")
        self._settings.set("/log/channels/isaacsim.replicator.character.core", "info")
        self._settings.set("/log/channels/omni.usd", "error")
        self._settings.set("/log/channels/omni.hydra", "error")
        self._settings.set("/log/channels/omni.kit.menu.*", "error")
        self._settings.set("/log/channels/omni.kit.property.*", "error")
        self._settings.set("/log/channels/omni.anim.graph.*", "error")
        # TODO(verify-6.0): Add /log/channels/omni.anim.behavior.* once behavior tree
        # runtime is loaded; current 5.1 omni.anim.graph filter does NOT cover the new
        # BT extensions and may produce verbose output.
        self._settings.set("/exts/isaacsim.replicator.agent/debug_print", self.debug_print)

        # Crash reporter
        self._settings.set("/crashreporter/enabled", True)
        if self.crash_report_path:
            self._settings.set("/crashreporter/dumpDir", self.crash_report_path)

    # NOTE(IRA 6.0): _setup_sim, _do_camera_placement, _read_camera_json, _place_cameras,
    # _place_one_camera all DELETED:
    #   - _setup_sim: callback-based setup pattern (register_set_up_simulation_done_callback)
    #     replaced by `await SimulationManager.get_instance().setup_simulation()` (inlined
    #     in run() above).
    #   - _do_camera_placement / _read_camera_json / _place_cameras / _place_one_camera:
    #     camera placement is now declarative via sensor.groups.<g>.aim_at_targets in YAML;
    #     CameraUtil from isaacsim.replicator.agent.core.stage_util is GONE
    #     (module deleted in IRA 1.0.0 overhaul). --sensor_placement_file CLI arg retained
    #     as informational only (see run()).


async def _save_usd(sim_app, save_as_path):
    print(f"Saving USD to: {save_as_path}")
    try:
        import omni.usd
        await omni.usd.get_context().save_as_stage_async(save_as_path)
        print("USD saved successfully")
        await omni.usd.get_context().close_stage_async()
    except Exception as e:
        print(f"Failed to save USD: {e}", file=sys.stderr)


def get_args():
    parser = argparse.ArgumentParser(
        description="Actor SDG Runner - Run Isaac Sim Actor SDG from command line",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Setup and wait for manual start:
  ./python.sh run_actor_sdg.py -c config.yaml

  # Auto start data generation:
  ./python.sh run_actor_sdg.py -c config.yaml --start

  # Headless mode with auto start:
  ./python.sh run_actor_sdg.py -c config.yaml --start --headless

  # Setup only (no data generation):
  ./python.sh run_actor_sdg.py -c config.yaml --setup-only

  # With VST integration:
  ./python.sh run_actor_sdg.py -c config.yaml --start --enable-vst --cameras-config cameras.yaml

  # Inject a sensor anomaly while the run is live (no restart):
  ./scripts/postprocessing/set_preset.sh tv_noise
        """,
    )
    parser.add_argument("-c", "--config_file", required=True, help="Path to IRA config file (yaml)")
    parser.add_argument("--start", action="store_true", help="Automatically start data generation")
    parser.add_argument("--setup-only", action="store_true", help="Only setup simulation, don't wait for data generation")
    parser.add_argument("--headless", action="store_true", help="Run in headless mode (no GUI window)")
    # NOTE(IRA 6.0): --sensor_placement_file is INFORMATIONAL ONLY. Camera placement is
    # declarative via sensor.groups.<g>.aim_at_targets in the YAML config. Argument kept
    # for CLI back-compat.
    parser.add_argument("--sensor_placement_file", help="(IRA 6.0: informational only) Path to camera placement JSON file")
    parser.add_argument("--crash_report_path", help="Path to store crash reports")
    parser.add_argument("--debug_print", action="store_true", help="Enable debug output")
    parser.add_argument("--save_usd", action="store_true", help="Save USD scene after generation")
    parser.add_argument("--width", type=int, default=1920, help="Viewport width (default: 1920)")
    parser.add_argument("--height", type=int, default=1080, help="Viewport height (default: 1080)")

    # VST Integration arguments
    parser.add_argument("--enable-vst", action="store_true", help="Enable VST sensor registration")
    parser.add_argument("--cameras-config", help="Path to cameras.yaml config file for VST + RTSP graph")
    # Defer VST registration until the RTSP encoder is warm (producing frames), so VST does
    # not DESCRIBE-churn Isaac's cold in-process RTSP media (which wedges it: "has no caps" /
    # DS 0, no self-recovery). Registration fires once the timeline has advanced by
    # --vst-register-warmup-sec of sim-time under Play; --vst-register-timeout-sec is a
    # wall-clock ceiling past which it registers best-effort (surfaces a permanently-cold scene).
    parser.add_argument("--vst-register-warmup-sec", type=float, default=1.0,
                        help="Sim-time (s) the timeline must advance under Play before VST registration (default: 1.0)")
    parser.add_argument("--vst-register-timeout-sec", type=float, default=1200.0,
                        help="Wall-clock (s) ceiling before VST registers best-effort even if render never warms (default: 1200)")

    # Post-setup stage modifiers (default ON; use --no-* to disable).
    # See sil/scripts/action_graphs/README.md for what each does.
    parser.add_argument("--no-runtime-patches", dest="enable_runtime_patches", action="store_false",
                        default=True,
                        help="Skip apply_halos_runtime_patches (legacy char deactivation + IRA char placement)")
    parser.add_argument("--no-rtsp", dest="enable_rtsp", action="store_false",
                        default=True,
                        help="Skip build_rtsp_graph (RTSP multi-camera Action Graph build)")
    parser.add_argument("--no-camera-spawn", dest="enable_camera_spawn", action="store_false",
                        default=True,
                        help="Skip camera_loader (dynamic Camera prim spawn from cameras.yaml spawn: blocks)")
    parser.add_argument("--robots-config", help="Path to robots.yaml for forklift control/odom/safety + clock graphs")
    parser.add_argument("--no-forklift", dest="enable_forklift", action="store_false",
                        default=True,
                        help="Skip build_forklift_graphs (control + odometry + safety indicator)")
    parser.add_argument("--no-clock", dest="enable_clock", action="store_false",
                        default=True,
                        help="Skip build_clock_graph (ROS 2 /clock publisher)")
    parser.add_argument("--srr-gt", dest="enable_srr_gt", action="store_true",
                        default=False,
                        help="Build the SRR ground-truth /gt/*/tf publisher graph "
                             "(default OFF; SRR regression harness only)")
    parser.add_argument("--postprocessing-config",
                        help="Path to postprocessing.yaml (RTX sensor-anomaly presets). "
                             "Defaults to configs/postprocessing.yaml when present")
    parser.add_argument("--no-postprocessing", dest="enable_postprocessing",
                        action="store_false", default=True,
                        help="Skip the RTX post-processing controller (no anomaly injection, "
                             "no config polling)")

    args, _ = parser.parse_known_args()
    return args


def main():
    args = get_args()

    # VST registers camera stream URLs that only exist once the RTSP graph
    # is built, so --enable-vst without RTSP would register dead URLs.
    if args.enable_vst and not args.enable_rtsp:
        print("ERROR: --enable-vst requires RTSP (do not pass --no-rtsp with --enable-vst).",
              file=sys.stderr)
        sys.exit(2)

    # Validate config file
    config_file_path = os.path.abspath(args.config_file)
    if not os.path.isfile(config_file_path):
        print(f"ERROR: Config file not found: {config_file_path}", file=sys.stderr)
        sys.exit(1)

    # Validate sensor placement file if provided. In IRA 6.0 this is informational only —
    # we still surface a clear error if the user passes a path that doesn't exist.
    if args.sensor_placement_file and not os.path.isfile(args.sensor_placement_file):
        print(f"ERROR: Sensor placement file not found: {args.sensor_placement_file}", file=sys.stderr)
        sys.exit(1)

    # Checked before SimulationApp boots: an explicitly requested preset file
    # that does not exist is an operator error worth failing on, and failing here
    # avoids tearing down a started Kit app.
    if args.postprocessing_config and not os.path.isfile(args.postprocessing_config):
        print(f"ERROR: Post-processing config not found: {args.postprocessing_config}", file=sys.stderr)
        sys.exit(1)

    # Resolve cameras config path
    cameras_config_path = None
    if args.cameras_config:
        cameras_config_path = os.path.abspath(args.cameras_config)
        if not os.path.isfile(cameras_config_path):
            print(f"WARNING: Cameras config file not found: {cameras_config_path}", file=sys.stderr)

    print("=" * 60)
    print("Actor SDG Runner (IRA 6.0)")
    print("=" * 60)
    print(f"Config file: {config_file_path}")
    print(f"Headless: {args.headless}")
    print(f"Auto start: {args.start}")
    print(f"Setup only: {args.setup_only}")
    print(f"Debug print: {args.debug_print}")
    print(f"Save USD: {args.save_usd}")
    print(f"VST Integration: {args.enable_vst}")
    if args.enable_vst:
        print(f"  Cameras config: {cameras_config_path}")
        print(f"  VST URL: {os.environ.get('VST_BASE_URL', 'not set')}")
        print(f"  HOST_IP: {os.environ.get('HOST_IP', 'not set')}")
    print("=" * 60)

    # App configuration
    app_config = {
        "renderer": "RayTracedLighting",
        "headless": args.headless,
        "width": args.width,
        "height": args.height,
    }

    # Override the default Nucleus asset root with the public S3 prefix
    # so Kit's provider_nucleus plugin does not require OMNI_USER/OMNI_PASS
    # at boot. The Halos asset_path / motion_library_path / scene paths in
    # default_config_ros.yaml are all S3 URLs (or local /isaac-sim/sil/...),
    # so no Nucleus call is actually needed by IRA - the JWT was only
    # consumed by the kit-boot Nucleus token refresh against the
    # extension-default Omniverse asset root.
    #
    # Override via sys.argv so SimulationApp forwards the flag to Kit before
    # provider_nucleus boots. Operator can pin a different prefix by passing
    # ISAAC_ASSET_ROOT env, or override entirely on the CLI (Kit picks the
    # last value).
    isaac_asset_root = os.environ.get(
        "ISAAC_ASSET_ROOT",
        "https://omniverse-content-production.s3-us-west-2.amazonaws.com/Assets/Isaac/6.0",
    )
    sys.argv.append(f"--/persistent/isaac/asset_root/default={isaac_asset_root}")

    # Tracy profiler backend (env-gated: HALOS_ENABLE_TRACY=1). Injected via
    # sys.argv so SimulationApp forwards them to Kit before the profiler boots.
    # Tracy server listens on TCP 8086; container runs --network=host so a Tracy
    # client on the host/VNC desktop connects to localhost:8086.
    if os.environ.get("HALOS_ENABLE_TRACY", "").lower() in ("1", "true", "yes"):
        print("Tracy profiler ENABLED (backend=tracy, port 8086)")
        sys.argv.append("--/app/profilerBackend=tracy")
        sys.argv.append("--/profiler/enabled=true")
        sys.argv.append("--/plugins/carb.profiler-tracy.plugin/fenceEnabled=true")
        sys.argv.append("--enable")
        sys.argv.append("omni.kit.profiler.tracy")
    # NOTE: 30 Hz tick gate is implemented as Option B in
    # action_graphs/rtsp_cameras.py via `IsaacSimulationGate(step=2)`.
    # The carb-side `/app/runLoops/main/rateLimitFrequency=30` override is
    # NOT effective on Isaac Sim 6.0 — leaving it out.

    # Start SimulationApp
    print("Starting Isaac Sim...")
    print(f"Asset root override: {isaac_asset_root}")
    sim_app = SimulationApp(launch_config=app_config, experience=BASE_EXP_PATH)

    # Default cameras_config_path to the canonical location when the
    # operator did not pass --cameras-config. Both consumers (RTSP graph
    # build and camera spawn) need it, so either being enabled resolves
    # the default — mirroring the robots.yaml default below.
    if (args.enable_rtsp or args.enable_camera_spawn) and cameras_config_path is None:
        default_cameras_yaml = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "configs", "cameras.yaml")
        )
        if os.path.isfile(default_cameras_yaml):
            cameras_config_path = default_cameras_yaml

    # Resolve robots config path (forklift control/odom/safety + clock).
    # Defaults to the canonical configs/robots.yaml when the operator did
    # not pass --robots-config, mirroring the cameras.yaml default above.
    robots_config_path = None
    if args.robots_config:
        robots_config_path = os.path.abspath(args.robots_config)
        if not os.path.isfile(robots_config_path):
            print(f"WARNING: Robots config file not found: {robots_config_path}", file=sys.stderr)
    elif args.enable_forklift or args.enable_clock:
        default_robots_yaml = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "configs", "robots.yaml")
        )
        if os.path.isfile(default_robots_yaml):
            robots_config_path = default_robots_yaml

    # Resolve the post-processing preset config, same defaulting as above.
    # Existence of an explicitly passed path was checked before Kit booted; a
    # missing default simply leaves anomaly injection off.
    postprocessing_config_path = None
    if args.enable_postprocessing:
        if args.postprocessing_config:
            postprocessing_config_path = os.path.abspath(args.postprocessing_config)
        else:
            default_pp_yaml = os.path.abspath(
                os.path.join(os.path.dirname(__file__), "..", "configs", "postprocessing.yaml")
            )
            if os.path.isfile(default_pp_yaml):
                postprocessing_config_path = default_pp_yaml

    # Create and run SDG
    sdg = ActorSDGRunner(
        sim_app=sim_app,
        config_file_path=config_file_path,
        auto_start=args.start,
        setup_only=args.setup_only,
        camera_file_path=args.sensor_placement_file,
        crash_report_path=args.crash_report_path,
        debug_print=args.debug_print,
        save_usd=args.save_usd,
        enable_vst=args.enable_vst,
        cameras_config_path=cameras_config_path,
        enable_runtime_patches=args.enable_runtime_patches,
        enable_rtsp=args.enable_rtsp,
        enable_camera_spawn=args.enable_camera_spawn,
        robots_config_path=robots_config_path,
        enable_forklift=args.enable_forklift,
        enable_clock=args.enable_clock,
        enable_srr_gt=args.enable_srr_gt,
        postprocessing_config_path=postprocessing_config_path,
        enable_postprocessing=args.enable_postprocessing,
        vst_register_warmup_sec=args.vst_register_warmup_sec,
        vst_register_timeout_sec=args.vst_register_timeout_sec,
    )

    from omni.kit.async_engine import run_coroutine

    task = run_coroutine(sdg.run())

    try:
        while not task.done():
            sim_app.update()

        if not task.result():
            print("Actor SDG failed!", file=sys.stderr)
            sim_app.close()
            sys.exit(1)

        # Save USD if requested. self.output_path may be None when running an RTSP-only
        # config with no offline writer — the guard below keeps that case safe.
        if args.save_usd and sdg.output_path:
            import omni.client
            save_as_path = omni.client.combine_urls(f"{sdg.output_path}/", "scene.usd")
            save_usd_task = asyncio.ensure_future(_save_usd(sim_app, save_as_path))
            while not save_usd_task.done():
                sim_app.update()
        elif args.save_usd and not sdg.output_path:
            print(
                "WARNING: --save_usd requested but no output_dir resolved from config "
                "(IRA 6.0 replicator.writers may be empty or omit output_dir). Skipping.",
                file=sys.stderr,
            )

        print("Actor SDG completed successfully!")

    finally:
        sim_app.close()


if __name__ == "__main__":
    main()
