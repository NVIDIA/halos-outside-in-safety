# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Config-driven RTX post-processing for in-scenario sensor-anomaly injection.

Deliberately NOT an OmniGraph. RTX post-processing is consumed by the renderer
itself, so a preset takes effect on the next rendered frame — the viewport, the
SDG RGB output and the RTSP stream all carry it, because the pass runs before
the encoder. Nothing has to be rebuilt and data generation never has to start.

Two scopes, chosen per preset:

  * global (default) — carb settings. Affects the viewport and every camera.
    Use it for "the whole site went dark".
  * per camera (`cameras:` in the preset) — USD attributes on that camera's
    prim and render product, which override the carb value for that camera
    alone. Use it for "camera 2 is degraded", the case an anomaly campaign
    actually cares about.
  * per camera, one look each (`per_camera:` in the preset) — the same USD
    authoring, but every camera carries its own `settings` block and, if it
    wants one, its own `animation` block. Use it for "camera 1 went dark WHILE
    camera 2 went grainy", which `cameras:` cannot express: one `settings`
    block means every listed camera gets the same degradation, and only one
    preset is active at a time, so applying two single-camera presets in turn
    reverts the first.

Animated groups share one clock origin, latched when the preset is applied, and
offset against each other with `animation.phase_deg`. Two cameras flickering at
1 Hz 180 degrees apart is then a property of the preset file rather than of when
each group happened to start.

Per-camera scoping has one timing quirk worth knowing: the RTSP graph creates
render products through `IsaacCreateRenderProduct`, which only runs on the
first tick after Play. Grain, scanlines, vignetting and color grading live on
the render product, so those stay pending until Play; exposure lives on the
camera prim and applies immediately. `update()` keeps retrying and says so once
per preset rather than failing.

All USD authoring goes into the stage's session layer, so a preset never dirties
the scene `.usd` on disk and never survives a restart.
"""

from __future__ import annotations

import hashlib
import math
import os
import time

from .effects import (
    ANIMATABLE,
    EFFECTS,
    KIND_BOOL,
    KIND_COLOR3,
    KIND_FLOAT,
    TARGET_CAMERA,
    TARGET_RENDER_PRODUCT,
)

DEFAULT_CONFIG = "/isaac-sim/sil/configs/postprocessing.yaml"

# Sentinel for "this carb key did not exist before we touched it".
_MISSING = object()

_LOG = "[postprocessing]"


def _say(message):
    """Log a line that survives being piped.

    Kit's own logging is C++ writing straight to the file descriptor, but these
    lines go through Python's stdout, which block-buffers when it is a pipe. Two
    or three short lines never fill the buffer, so without an explicit flush the
    whole preset history can stay invisible for an entire run while carb log
    output streams past it.
    """
    print(f"{_LOG} {message}", flush=True)


class PostProcessingController:
    """Apply one preset from `postprocessing.yaml` and hot-reload it live.

    Lifecycle, driven by run_actor_sdg.py:
        start()   once, after the simulation is set up
        update()  every frame from the run loop
        close()   on shutdown, to put the renderer back as it was
    """

    def __init__(
        self,
        config_path=DEFAULT_CONFIG,
        *,
        cameras_config_path=None,
        preset_override=None,
        poll_interval_sec=0.5,
        settings=None,
        clock=None,
    ):
        self.config_path = os.path.abspath(config_path)
        self.cameras_config_path = cameras_config_path
        # Pins the preset for the whole run, ignoring `active:` in the file.
        # postprocessing.yaml is git-tracked but set_preset.sh rewrites it in
        # place, so without a pin the last anomaly an operator injected is what
        # the NEXT run starts in. Polling stays on: an unrelated edit to the
        # file still re-applies this preset, which is a no-op look-wise.
        self.preset_override = preset_override
        # Sub-100ms polling would stat the file several times per rendered
        # frame for no operator-visible benefit.
        self.poll_interval_sec = max(float(poll_interval_sec), 0.1)

        self._clock = clock or time.monotonic
        self._settings = settings

        self._next_poll_at = 0.0
        self._digest = None
        self._rejected_digest = None

        self._active_name = None
        self._carb_snapshot = {}
        self._authored_attrs = []   # (prim_path, attr_name)
        self._authored_apis = []    # (prim_path, api_name)

        # (camera_prim_paths, key) -> value. Keyed rather than appended for two
        # reasons: two cameras in the same preset can be waiting on different
        # settings, and an ANIMATED render-product effect re-writes the same key
        # every frame — as a list that grew one entry per frame before Play, and
        # the flush traverses the stage once per entry, so the cost was
        # quadratic in the number of pre-Play frames.
        #
        # Keyed, the size is the preset's, not the frame count's. Measured on
        # the shipped `anim_rp_target` by test_controller.py: 3 pending entries
        # and 4 stage traversals per pre-Play frame (3 flush + 1 animate), flat
        # for as long as Play is delayed. Not 1 and 2 — that pair describes a
        # preset that animates the grain amount WITHOUT switching the grain pass
        # on, which renders no grain at all; every usable one carries
        # tv_noise.enabled and film_grain.enabled alongside it, and those defer
        # too. After Play the queue is empty and only the animator traverses,
        # which is the 1-per-animated-scope figure the load-time WARNING quotes.
        self._pending_rp_settings = {}
        self._pending_warned = False

        self._animations = []       # (camera_prim_paths, animation)
        # One clock origin for every group. All groups are applied in the same
        # _apply call, so per-group origins would be identical by construction
        # and add nothing; `animation.phase_deg` is how a preset offsets one
        # camera against another, explicitly and measurably.
        self._animation_t0 = 0.0

    @property
    def active_preset(self):
        return self._active_name

    # -- lifecycle ---------------------------------------------------------

    def start(self):
        """Load and apply the active preset. Call after setup_simulation()."""
        if self._settings is None:
            import carb.settings

            self._settings = carb.settings.get_settings()
        self._snapshot_carb()
        self.update(force_reload=True)

    def update(self, *, force_reload=False):
        """Re-read the config when due, retry pending writes, advance animation."""
        now = self._clock()
        if force_reload or now >= self._next_poll_at:
            self._next_poll_at = now + self.poll_interval_sec
            self._reload_if_changed()
        if self._pending_rp_settings:
            self._flush_pending_render_product_settings()
        self._advance_animation(now)

    def close(self):
        """Restore the renderer to its pre-controller state."""
        try:
            self._revert()
        except Exception as exc:
            _say(f"WARNING: could not fully restore renderer state: {exc}")
        self._animations = []
        self._active_name = None

    # -- config ------------------------------------------------------------

    def _reload_if_changed(self):
        try:
            with open(self.config_path, "rb") as stream:
                raw = stream.read()
        except OSError as exc:
            _say(f"WARNING: cannot read {self.config_path}: {exc}")
            return

        digest = hashlib.sha256(raw).digest()
        if digest in (self._digest, self._rejected_digest):
            return

        # Set immediately BEFORE the call, not inside it: _apply() starts by
        # reverting, so a raise anywhere in it means the renderer is already
        # dirty. Everything above it is pure config work that touches nothing.
        entered_apply = False
        try:
            import yaml

            config = yaml.safe_load(raw) or {}
            name, preset = _select_preset(config, self.preset_override)
            _validate_preset(name, preset)
            # Resolved before anything is reverted: a bad camera name has to be
            # rejected like any other config error, leaving the preset that is
            # currently degrading the scene untouched. Every group is resolved,
            # not just the first, so one bad name in a `per_camera:` preset
            # cannot strip the running preset off the other cameras.
            groups = self._resolve_groups(preset)
            # Inside the try as well, because the apply layer raises too (a
            # KIND_COLOR3 value carb refuses, a closed stage, a USD authoring
            # error).
            entered_apply = True
            self._apply(name, groups)
        except Exception as exc:
            # Remember the bad digest so a broken edit is reported once instead
            # of every poll — which also keeps a failing apply from retrying at
            # render rate.
            self._rejected_digest = digest

            if not entered_apply:
                # Config error: nothing was touched, so the anomaly under test
                # keeps running. _digest still describes what is live, which is
                # what makes restoring the file early-return correctly.
                _say(f"WARNING: rejected {self.config_path}: {exc}; active preset is {self._active_name!r}")
                return

            # A failure part-way through _apply cannot leave the last good preset
            # standing, because applying starts by reverting: what is on screen is
            # half of a preset that was just rejected, a look nothing in the file
            # describes. Go back to the baseline instead. This also drops the
            # _pending_rp_settings the failed apply queued, which would otherwise
            # be authored onto the render products on the first tick after Play —
            # silently installing the very preset rejected here.
            try:
                self._revert()
            except Exception as revert_exc:
                _say(f"WARNING: could not fully restore renderer state: {revert_exc}")
            # The rejected preset's animations would keep driving a value every
            # frame, and a stale name would make active_preset lie about it.
            # _revert() already clears both, and its state resets now run in
            # their own finally, so they hold even when _clear_authored_usd()
            # raises — this is belt and braces, not the guarantee it once was.
            # Kept so the branch does not silently depend on that finally
            # staying nested the way it is: what the queue holds is what
            # update() would flush onto the render products on the first tick
            # after Play, installing the very preset just rejected.
            self._animations = []
            self._pending_rp_settings = {}
            self._active_name = None
            # Forget the last good digest too. The usual recovery is to put the
            # file back the way it was, and that content still matches _digest —
            # it would early-return as "unchanged" and strand the scene on the
            # baseline for the rest of the run. _rejected_digest still absorbs
            # every re-read of the broken file, so this cannot become a retry
            # storm.
            self._digest = None
            _say(
                f"WARNING: rejected {self.config_path}: {exc}; "
                "reverted the renderer to its pre-preset state"
            )
            return

        # Committed only once the preset is actually live, so a rejected edit
        # cannot make the controller believe it applied something it did not.
        self._digest = digest
        self._rejected_digest = None

    # -- apply / revert ----------------------------------------------------

    def _apply(self, name, groups):
        # Revert first: switching presets must not leak the previous one's ISO,
        # gain or noise toggles into the new look.
        self._revert()

        total = 0
        scopes = []
        for prim_paths, settings, animation in groups:
            total += len(settings)
            for key, value in settings.items():
                if prim_paths:
                    self._write_scoped(key, value, prim_paths)
                else:
                    self._settings.set(EFFECTS[key].carb_path, value)
            if animation:
                self._animations.append((prim_paths, animation))
            scopes.append(", ".join(prim_paths) if prim_paths else "global")

        self._animation_t0 = self._clock()
        self._active_name = name

        # One group keeps the historical wording byte for byte; several groups
        # are separated by "; " so the log distinguishes "these cameras share a
        # look" from "each camera has its own".
        detail = f"{total} setting(s), scope={'; '.join(scopes)}"
        if len(self._animations) > 1:
            # Several scopes can animate the same target at different phases or
            # frequencies, so the bare target name would read as a duplicate.
            animating = ", ".join(
                f"{anim['target']} on {', '.join(paths) or 'global'}"
                for paths, anim in self._animations
            )
        else:
            animating = ", ".join(anim["target"] for _, anim in self._animations)
        if animating:
            detail += f", animating {animating}"
        _say(f"Applied preset {name!r} ({detail})")
        if len(groups) > 1:
            for prim_paths, settings, _ in groups:
                keys = ", ".join(sorted(settings)) or "nothing"
                _say(f"  {', '.join(prim_paths) or 'global'}: {keys}")

    def _revert(self):
        # Two phases, and the USD one must not be hostage to the carb one: a
        # single failing settings path would otherwise leave the previous
        # preset's per-camera attributes authored on the stage, which both
        # close() and the _apply() preset switch promise to clear.
        try:
            for path, value in self._carb_snapshot.items():
                if value is _MISSING:
                    destroy = getattr(self._settings, "destroy_item", None)
                    if destroy is not None:
                        destroy(path)
                else:
                    self._settings.set(path, value)
        finally:
            # Three phases, not two, and for the same reason: the state resets
            # below must not be hostage to the USD cleanup either. A raise from
            # _clear_authored_usd() used to skip them, leaving the reverted
            # preset's DEFERRED render-product writes queued -- and update()
            # authors those onto the render products on the first tick after
            # Play, installing a preset that active_preset reports as gone. The
            # raise still propagates once the resets have run, so close() and
            # _reload_if_changed() still log the cleanup failure.
            try:
                if self._authored_attrs or self._authored_apis:
                    self._clear_authored_usd()
            finally:
                self._pending_rp_settings = {}
                self._pending_warned = False
                # Must be cleared here, not only in _apply: the animation list is
                # appended to, so a preset switch would otherwise keep driving the
                # previous preset's target on top of the new one.
                self._animations = []

    def _snapshot_carb(self):
        for key, effect in EFFECTS.items():
            current = self._settings.get(effect.carb_path)
            self._carb_snapshot[effect.carb_path] = _MISSING if current is None else current

    # -- per-camera authoring ---------------------------------------------

    def _resolve_groups(self, preset):
        """Resolve every scope of a preset to (prim_paths, settings, animation).

        `prim_paths` is empty for a global group. Resolving all of them up
        front proves the prims exist, so `_apply` cannot fail halfway through
        and leave the scene wearing half a preset — and, for `per_camera:`, so
        one unknown camera name rejects the whole edit instead of degrading
        some cameras and not others.
        """
        by_name = {}
        resolved = []
        seen = {}
        groups = _preset_groups(preset)
        for cameras, settings, animation in groups:
            if not cameras:
                resolved.append(([], settings, animation))
                continue
            paths = self._resolve_camera_prims(cameras, by_name)
            stage = self._stage()
            missing = [p for p in paths if not stage.GetPrimAtPath(p).IsValid()]
            if missing:
                raise ValueError(f"camera prim(s) not on stage: {', '.join(missing)}")
            if len(groups) > 1:
                # Two per_camera keys can name the same camera (a name and its
                # prim path), and the loser would be silently overwritten.
                for path, entry in zip(paths, cameras):
                    if path in seen:
                        raise ValueError(
                            f"per_camera entries {seen[path]!r} and {entry!r} "
                            f"both resolve to {path}"
                        )
                    seen[path] = entry
            resolved.append((paths, settings, animation))
        return resolved

    def _resolve_camera_prims(self, cameras, by_name=None):
        """Map preset `cameras:` entries to camera prim paths.

        An entry starting with "/" is taken as a prim path; anything else is
        looked up as a camera `name` in cameras.yaml, so a preset can say
        `Camera_01` instead of repeating the full prim path.

        `by_name` is a cache shared across the groups of one preset, so a
        `per_camera:` block does not re-read cameras.yaml once per camera.
        """
        paths = []
        if by_name is None:
            by_name = {}
        for entry in cameras:
            if entry.startswith("/"):
                paths.append(entry)
                continue
            if not by_name:
                by_name.update(self._load_camera_name_map())
            prim_path = by_name.get(entry)
            if prim_path is None:
                known = ", ".join(sorted(by_name)) or "none"
                raise ValueError(
                    f"camera {entry!r} not found in {self.cameras_config_path} (known: {known})"
                )
            paths.append(prim_path)
        return paths

    def _load_camera_name_map(self):
        if not self.cameras_config_path or not os.path.isfile(self.cameras_config_path):
            raise ValueError(
                "preset selects cameras by name but cameras.yaml is unavailable; "
                "pass full prim paths instead"
            )
        import yaml

        with open(self.cameras_config_path) as stream:
            cfg = yaml.safe_load(stream) or {}
        return {
            cam["name"]: cam["camera_prim"]
            for cam in (cfg.get("cameras") or [])
            if "name" in cam and "camera_prim" in cam
        }

    def _write_scoped(self, key, value, prim_paths):
        effect = EFFECTS[key]
        if effect.target == TARGET_CAMERA:
            stage = self._stage()
            for prim_path in prim_paths:
                prim = stage.GetPrimAtPath(prim_path)
                if not prim or not prim.IsValid():
                    raise RuntimeError(f"camera prim missing on stage: {prim_path}")
                self._author(prim, effect, value)
            return

        # Render products do not exist until the first tick after Play, so
        # defer instead of failing and let update() pick them up.
        products = self._render_products(prim_paths)
        if not products:
            # Keyed, so an animated effect overwrites its own pending value
            # every frame instead of queueing a new one.
            self._pending_rp_settings[(tuple(prim_paths), key)] = value
            return
        for prim in products:
            self._author(prim, effect, value)

    def _flush_pending_render_product_settings(self):
        # Each entry keeps its own scope, so a preset where camera 1 needs a
        # render product and camera 2 does not cannot flush one onto the other.
        still_pending = {}
        flushed = 0
        touched = set()
        for (prim_paths, key), value in self._pending_rp_settings.items():
            products = self._render_products(prim_paths)
            if not products:
                still_pending[(prim_paths, key)] = value
                continue
            for prim in products:
                self._author(prim, EFFECTS[key], value)
                touched.add(str(prim.GetPath()))
            flushed += 1
        self._pending_rp_settings = still_pending

        if flushed:
            _say(f"Applied {flushed} deferred setting(s) to {len(touched)} render product(s)")
        if still_pending and not self._pending_warned:
            self._pending_warned = True
            keys = ", ".join(key for _, key in still_pending)
            _say(
                f"{len(still_pending)} setting(s) waiting for render "
                f"products (created on the first tick after Play): {keys}"
            )

    def _render_products(self, prim_paths):
        """Render product prims bound to the given cameras."""
        stage = self._stage()
        wanted = set(prim_paths)
        found = []
        for prim in stage.Traverse():
            if prim.GetTypeName() != "RenderProduct":
                continue
            rel = prim.GetRelationship("camera")
            if not rel:
                continue
            if any(str(target) in wanted for target in rel.GetTargets()):
                found.append(prim)
        return found

    def _author(self, prim, effect, value):
        """Author one USD attribute in the session layer, tracked for cleanup."""
        from pxr import Sdf, Usd

        stage = self._stage()
        prim_path = str(prim.GetPath())

        with Usd.EditContext(stage, stage.GetSessionLayer()):
            if effect.usd_api and effect.usd_api not in prim.GetAppliedSchemas():
                prim.AddAppliedSchema(effect.usd_api)
                self._authored_apis.append((prim_path, effect.usd_api))

            attr = prim.CreateAttribute(effect.usd_attr, _sdf_type(Sdf, effect.kind))

            # Record before writing, not after. CreateAttribute is what puts the
            # attribute on the prim, so a Set() that raises would otherwise leave
            # it on the stage and out of the ledger -- surviving every _revert()
            # and close() for the rest of the process, while active_preset
            # reports None. An animated preset re-authors the same attribute
            # every frame, so record it once rather than append 60 times a
            # second for as long as the scenario runs.
            entry = (prim_path, effect.usd_attr)
            if entry not in self._authored_attrs:
                self._authored_attrs.append(entry)

            attr.Set(_usd_value(effect.kind, value))

    def _clear_authored_usd(self):
        """Remove every attribute and API schema this controller authored.

        The API schemas have to go too: leaving one applied with its attribute
        removed makes the prim resolve the SCHEMA DEFAULT (e.g. tvNoise
        disabled), which would then override a later global carb preset for
        that camera — the opposite of reverting.
        """
        from pxr import Usd

        stage = self._stage(required=False)
        if stage is None:
            self._authored_attrs = []
            self._authored_apis = []
            return

        with Usd.EditContext(stage, stage.GetSessionLayer()):
            for prim_path, attr_name in self._authored_attrs:
                prim = stage.GetPrimAtPath(prim_path)
                if prim and prim.IsValid():
                    prim.RemoveProperty(attr_name)
            for prim_path, api_name in self._authored_apis:
                prim = stage.GetPrimAtPath(prim_path)
                remove = getattr(prim, "RemoveAppliedSchema", None) if prim else None
                if remove is not None:
                    remove(api_name)

        self._authored_attrs = []
        self._authored_apis = []

    def _stage(self, required=True):
        import omni.usd

        stage = omni.usd.get_context().get_stage()
        if stage is None and required:
            raise RuntimeError("no USD stage open")
        return stage

    # -- animation ---------------------------------------------------------

    def _advance_animation(self, now):
        """Drive one float effect per frame per scope, for flicker anomalies.

        Every group reads the same `_animation_t0`, so two cameras asked for the
        same frequency stay locked together; `phase_deg` is what pulls them
        apart. 180 degrees gives the antiphase case — camera 1 at its brightest
        exactly when camera 2 is at its dimmest — which a shared-t0-only design
        could not express at all.
        """
        for prim_paths, animation in self._animations:
            base = float(animation["base"])
            amplitude = float(animation["amplitude_fraction"])
            frequency = float(animation["frequency_hz"])
            offset = math.radians(float(animation.get("phase_deg", 0.0)))
            phase = 2.0 * math.pi * frequency * (now - self._animation_t0) + offset
            value = base * (1.0 + amplitude * math.sin(phase))

            key = animation["target"]
            if prim_paths:
                self._write_scoped(key, value, prim_paths)
            else:
                self._settings.set(EFFECTS[key].carb_path, value)


# -- helpers ---------------------------------------------------------------


def _sdf_type(Sdf, kind):
    return {
        KIND_BOOL: Sdf.ValueTypeNames.Bool,
        KIND_FLOAT: Sdf.ValueTypeNames.Float,
        KIND_COLOR3: Sdf.ValueTypeNames.Color3f,
    }[kind]


def _usd_value(kind, value):
    if kind == KIND_COLOR3:
        from pxr import Gf

        return Gf.Vec3f(*[float(v) for v in value])
    if kind == KIND_FLOAT:
        return float(value)
    return bool(value)


def _is_number(value):
    # bool is a subclass of int; reject it so `true` cannot pass as a number.
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _select_preset(config, override=None):
    if not isinstance(config, dict):
        raise ValueError("top level must be a mapping")
    if config.get("version") != 1:
        raise ValueError("version must be 1")

    presets = config.get("presets")
    if not isinstance(presets, dict) or not presets:
        raise ValueError("'presets' must be a non-empty mapping")

    if override is not None and not str(override).strip():
        # `--postprocessing-preset "$PRESET"` with PRESET unset arrives as an
        # empty string. Falling back to `active:` there would hand the run
        # whatever preset set_preset.sh last wrote — the exact leak the pin
        # exists to prevent — so treat it as the launcher bug it is.
        raise ValueError("--postprocessing-preset was given an empty value")

    active = override or config.get("active")
    if override and override not in presets:
        # Named against the flag, not the file: a launcher pinning a preset that
        # the config does not define is a launcher bug, and pointing the
        # operator at `active:` would send them editing the wrong thing.
        raise ValueError(
            f"--postprocessing-preset {override!r} not in presets ({', '.join(sorted(presets))})"
        )
    if isinstance(active, bool):
        # YAML 1.1 reads off/no/false as booleans, so `active: off` silently
        # becomes False and no preset matches. Name presets in words instead.
        raise ValueError(
            "'active' parsed as a boolean — quote it (\"off\") or use a word like 'baseline'"
        )
    if not isinstance(active, str) or not active:
        raise ValueError("'active' must be a non-empty preset name")
    if active not in presets:
        raise ValueError(f"active preset {active!r} not in presets ({', '.join(sorted(presets))})")

    preset = presets[active]
    if preset is None:
        preset = {}
    if not isinstance(preset, dict):
        raise ValueError(f"preset {active!r} must be a mapping")
    return active, preset


def _preset_groups(preset):
    """Split a preset into its scopes: [(cameras, settings, animation)].

    A classic preset is one group — global when `cameras:` is absent, one
    shared per-camera group when it is present. A `per_camera:` preset is one
    group per camera, which is what lets camera 1 go dark while camera 2 goes
    grainy in the same apply. Pure data, no Kit: the caller resolves the camera
    entries to prim paths.
    """
    per_camera = preset.get("per_camera")
    if per_camera:
        return [
            ([camera], (body or {}).get("settings") or {}, (body or {}).get("animation"))
            for camera, body in per_camera.items()
        ]
    return [
        (
            list(preset.get("cameras") or []),
            preset.get("settings") or {},
            preset.get("animation"),
        )
    ]


def _fold_camera_key(entry):
    """Fold the spellings of one camera entry that are equal without a stage.

    Returns `(folded, is_prim_path)`. Surrounding whitespace goes, and a prim
    path loses its repeated and trailing slashes, so `/World/Cameras/Camera_01`,
    `/World/Cameras/Camera_01/` and `//World//Cameras/Camera_01` fold together.
    A bare name is only stripped: turning it into a path needs cameras.yaml.

    Only used for duplicate detection, never for resolution — Kit resolves the
    entry as written, and a folded form that differed from it would quietly
    mean something else.
    """
    entry = entry.strip()
    if not entry.startswith("/"):
        return entry, False
    return "/" + "/".join(part for part in entry.split("/") if part), True


def _validate_preset(name, preset):
    unknown = set(preset) - {"description", "settings", "cameras", "animation", "per_camera"}
    if unknown:
        raise ValueError(f"preset {name!r} has unsupported keys: {sorted(unknown)}")

    per_camera = preset.get("per_camera")
    if per_camera is not None:
        _validate_per_camera(name, preset, per_camera)
        return

    _validate_settings(name, preset.get("settings"))

    cameras = preset.get("cameras")
    if cameras is not None:
        if not isinstance(cameras, list) or not cameras:
            raise ValueError(f"preset {name!r}: 'cameras' must be a non-empty list when present")
        if not all(isinstance(entry, str) and entry for entry in cameras):
            raise ValueError(f"preset {name!r}: 'cameras' entries must be names or prim paths")

    _validate_animation(name, preset.get("animation"), scoped=bool(cameras))


def _validate_per_camera(name, preset, per_camera):
    """Check a `per_camera:` preset — one settings/animation block per camera."""
    # Refusing the mixture is not pedantry: `cameras:`/`settings:` name a scope
    # too, and silently ignoring them would apply a preset the file does not
    # describe.
    conflicting = sorted(k for k in ("cameras", "settings", "animation") if k in preset)
    if conflicting:
        raise ValueError(
            f"preset {name!r}: 'per_camera' cannot be combined with {conflicting}; "
            f"move each of those under a per_camera entry"
        )
    if not isinstance(per_camera, dict) or not per_camera:
        raise ValueError(
            f"preset {name!r}: 'per_camera' must be a non-empty mapping of "
            f"camera name or prim path to {{settings: ...}}"
        )

    # Two keys naming one camera means one of the two blocks is silently
    # discarded — the second `_apply` write wins and nothing says so. Kit
    # catches every spelling of it in `_resolve_groups`, but only once a stage
    # is open, which is too late to be a lint. Fold what can be folded without
    # a stage here, and say plainly below what is left over.
    seen = {}
    by_basename = {}

    for camera, body in per_camera.items():
        where = f"{name}.per_camera.{camera}"
        if not isinstance(camera, str) or not camera:
            raise ValueError(f"preset {name!r}: per_camera keys must be names or prim paths")

        folded, is_path = _fold_camera_key(camera)
        if folded in seen:
            raise ValueError(
                f"preset {name!r}: per_camera entries {seen[folded]!r} and {camera!r} "
                f"name the same camera"
            )
        seen[folded] = camera
        by_basename.setdefault(folded.rsplit("/", 1)[-1], []).append((camera, is_path))

        if body is None:
            body = {}
        if not isinstance(body, dict):
            raise ValueError(f"preset {where!r}: must be a mapping")
        unknown = set(body) - {"description", "settings", "animation"}
        if unknown:
            raise ValueError(f"preset {where!r} has unsupported keys: {sorted(unknown)}")
        _validate_settings(where, body.get("settings"))
        # The same rule as a classic preset's `animation:`, deliberately not a
        # second one — a per_camera entry is just a scope, so its animation
        # block has to accept and reject exactly what the top-level block does.
        # `scoped=True` because a per_camera entry always names a camera.
        _validate_animation(where, body.get("animation"), scoped=True)

    # The case this layer cannot decide: a bare name resolves through
    # cameras.yaml, which is not read here, so `Camera_01` and
    # `/World/Cameras/Camera_01` MIGHT be the same prim or might not. Rejecting
    # on the matching last path component would refuse presets Kit accepts,
    # which is worse than the miss — an offline check that is stricter than the
    # runtime stops being usable as a lint. Warn and name both keys instead;
    # `_resolve_groups` still rejects it for certain, on a stage.
    for basename, entries in by_basename.items():
        forms = {is_path for _camera, is_path in entries}
        if len(forms) > 1:
            spellings = ", ".join(repr(camera) for camera, _is_path in entries)
            _say(
                f"WARNING: preset {name!r}: per_camera entries {spellings} both end in "
                f"{basename!r}. If cameras.yaml maps that name to that prim path they are "
                f"one camera with two blocks, and only one of them will apply — this check "
                f"cannot tell without a stage, so the run rejects the preset at apply time. "
                f"Spell every entry the same way."
            )


def _validate_settings(where, settings):
    if settings is None:
        settings = {}
    if not isinstance(settings, dict):
        raise ValueError(f"preset {where!r}: 'settings' must be a mapping")
    for key, value in settings.items():
        effect = EFFECTS.get(key)
        if effect is None:
            raise ValueError(f"preset {where!r}: unknown setting {key!r}")
        if effect.kind == KIND_BOOL and not isinstance(value, bool):
            raise ValueError(f"preset {where!r}: {key} must be true or false")
        if effect.kind == KIND_FLOAT and not _is_number(value):
            raise ValueError(f"preset {where!r}: {key} must be a number")
        if effect.kind == KIND_COLOR3 and not (
            isinstance(value, (list, tuple)) and len(value) == 3 and all(_is_number(v) for v in value)
        ):
            raise ValueError(f"preset {where!r}: {key} must be three numbers")


def _validate_animation(name, animation, *, scoped=False):
    """Check one `animation:` block, wherever it sits.

    `scoped` says the block belongs to a group that names cameras — either
    `cameras:` at preset level or a `per_camera:` entry — which is what makes
    the render-product cost warning below apply.
    """
    if animation is None:
        return
    if not isinstance(animation, dict):
        raise ValueError(f"preset {name!r}: 'animation' must be a mapping")

    unknown = set(animation) - {
        "target",
        "waveform",
        "base",
        "amplitude_fraction",
        "frequency_hz",
        "phase_deg",
    }
    if unknown:
        raise ValueError(f"preset {name!r}: animation has unsupported keys: {sorted(unknown)}")

    target = animation.get("target")
    if target not in ANIMATABLE:
        raise ValueError(
            f"preset {name!r}: animation.target must be one of {', '.join(sorted(ANIMATABLE))}"
        )
    if animation.get("waveform", "sine") != "sine":
        raise ValueError(f"preset {name!r}: only animation.waveform 'sine' is supported")
    for field in ("base", "amplitude_fraction", "frequency_hz"):
        if not _is_number(animation.get(field)):
            raise ValueError(f"preset {name!r}: animation.{field} must be a number")
    if animation["base"] <= 0:
        raise ValueError(f"preset {name!r}: animation.base must be > 0")
    if not 0 <= animation["amplitude_fraction"] < 1:
        raise ValueError(f"preset {name!r}: animation.amplitude_fraction must be in [0, 1)")
    if animation["frequency_hz"] <= 0:
        raise ValueError(f"preset {name!r}: animation.frequency_hz must be > 0")

    # Optional, default 0. Kept to one turn so two presets cannot describe the
    # same offset two ways (350 and -10), which would make captures harder to
    # compare than the field is worth.
    phase_deg = animation.get("phase_deg", 0.0)
    if not _is_number(phase_deg):
        raise ValueError(f"preset {name!r}: animation.phase_deg must be a number")
    if not 0 <= phase_deg < 360:
        raise ValueError(f"preset {name!r}: animation.phase_deg must be in [0, 360)")

    # A warning, not a rejection: it is a legal preset and someone will want a
    # grain level that breathes on one camera. But it is the expensive corner of
    # the design, so it should not be chosen by accident.
    if scoped and EFFECTS[target].target == TARGET_RENDER_PRODUCT:
        _say(
            f"WARNING: preset {name!r}: animating {target} at per-camera scope costs a "
            f"full stage traversal per animated scope PER FRAME, because render products "
            f"are found by traversal rather than by name. A preset of this shape "
            f"previously measured -7.0% FPS with the robot driving. Animate a "
            f"camera-prim effect (exposure.iso) instead, or animate this one globally "
            f"(drop cameras:/per_camera:), unless the per-camera scope is the point."
        )
