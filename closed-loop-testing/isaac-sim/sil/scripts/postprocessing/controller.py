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

        self._camera_prim_paths = []
        self._pending_rp_settings = []
        self._pending_warned = False

        self._animation = None
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
        self._animation = None
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
            # currently degrading the scene untouched.
            camera_prim_paths = self._resolve_scope(preset)
            # Inside the try as well, because the apply layer raises too (a
            # KIND_COLOR3 value carb refuses, a closed stage, a USD authoring
            # error).
            entered_apply = True
            self._apply(name, preset, camera_prim_paths)
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
            # The rejected preset's animation would keep driving a value every
            # frame, and a stale name would make active_preset lie about it.
            self._animation = None
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

    def _apply(self, name, preset, camera_prim_paths):
        # Revert first: switching presets must not leak the previous one's ISO,
        # gain or noise toggles into the new look.
        self._revert()

        settings = preset.get("settings") or {}
        self._camera_prim_paths = camera_prim_paths

        if self._camera_prim_paths:
            for key, value in settings.items():
                self._write_scoped(key, value)
        else:
            for key, value in settings.items():
                self._settings.set(EFFECTS[key].carb_path, value)

        self._animation = preset.get("animation")
        self._animation_t0 = self._clock()
        self._active_name = name

        scope = ", ".join(self._camera_prim_paths) if self._camera_prim_paths else "global"
        detail = f"{len(settings)} setting(s), scope={scope}"
        if self._animation:
            detail += f", animating {self._animation['target']}"
        _say(f"Applied preset {name!r} ({detail})")

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
                self._pending_rp_settings = []
                self._pending_warned = False
                self._camera_prim_paths = []

    def _snapshot_carb(self):
        for key, effect in EFFECTS.items():
            current = self._settings.get(effect.carb_path)
            self._carb_snapshot[effect.carb_path] = _MISSING if current is None else current

    # -- per-camera authoring ---------------------------------------------

    def _resolve_scope(self, preset):
        """Camera prim paths for a per-camera preset; empty list means global.

        Also proves the prims exist, so `_apply` cannot fail halfway through
        and leave the scene wearing half a preset.
        """
        cameras = preset.get("cameras") or []
        if not cameras:
            return []
        paths = self._resolve_camera_prims(cameras)
        stage = self._stage()
        missing = [p for p in paths if not stage.GetPrimAtPath(p).IsValid()]
        if missing:
            raise ValueError(f"camera prim(s) not on stage: {', '.join(missing)}")
        return paths

    def _resolve_camera_prims(self, cameras):
        """Map preset `cameras:` entries to camera prim paths.

        An entry starting with "/" is taken as a prim path; anything else is
        looked up as a camera `name` in cameras.yaml, so a preset can say
        `Camera_01` instead of repeating the full prim path.
        """
        paths = []
        by_name = None
        for entry in cameras:
            if entry.startswith("/"):
                paths.append(entry)
                continue
            if by_name is None:
                by_name = self._load_camera_name_map()
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

    def _write_scoped(self, key, value):
        effect = EFFECTS[key]
        if effect.target == TARGET_CAMERA:
            stage = self._stage()
            for prim_path in self._camera_prim_paths:
                prim = stage.GetPrimAtPath(prim_path)
                if not prim or not prim.IsValid():
                    raise RuntimeError(f"camera prim missing on stage: {prim_path}")
                self._author(prim, effect, value)
            return

        # Render products do not exist until the first tick after Play, so
        # defer instead of failing and let update() pick them up.
        products = self._render_products()
        if not products:
            self._pending_rp_settings.append((key, value))
            return
        for prim in products:
            self._author(prim, effect, value)

    def _flush_pending_render_product_settings(self):
        products = self._render_products()
        if not products:
            if not self._pending_warned:
                self._pending_warned = True
                keys = ", ".join(key for key, _ in self._pending_rp_settings)
                _say(
                    f"{len(self._pending_rp_settings)} setting(s) waiting for render "
                    f"products (created on the first tick after Play): {keys}"
                )
            return

        pending, self._pending_rp_settings = self._pending_rp_settings, []
        for key, value in pending:
            for prim in products:
                self._author(prim, EFFECTS[key], value)
        _say(f"Applied {len(pending)} deferred setting(s) to {len(products)} render product(s)")

    def _render_products(self):
        """Render product prims bound to the preset's cameras."""
        stage = self._stage()
        wanted = set(self._camera_prim_paths)
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
        """Drive one float effect per frame, for flicker-style anomalies."""
        if not self._animation:
            return
        base = float(self._animation["base"])
        amplitude = float(self._animation["amplitude_fraction"])
        frequency = float(self._animation["frequency_hz"])
        phase = 2.0 * math.pi * frequency * (now - self._animation_t0)
        value = base * (1.0 + amplitude * math.sin(phase))

        key = self._animation["target"]
        if self._camera_prim_paths:
            self._write_scoped(key, value)
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


def _validate_preset(name, preset):
    unknown = set(preset) - {"description", "settings", "cameras", "animation"}
    if unknown:
        raise ValueError(f"preset {name!r} has unsupported keys: {sorted(unknown)}")

    settings = preset.get("settings") or {}
    if not isinstance(settings, dict):
        raise ValueError(f"preset {name!r}: 'settings' must be a mapping")
    for key, value in settings.items():
        effect = EFFECTS.get(key)
        if effect is None:
            raise ValueError(f"preset {name!r}: unknown setting {key!r}")
        if effect.kind == KIND_BOOL and not isinstance(value, bool):
            raise ValueError(f"preset {name!r}: {key} must be true or false")
        if effect.kind == KIND_FLOAT and not _is_number(value):
            raise ValueError(f"preset {name!r}: {key} must be a number")
        if effect.kind == KIND_COLOR3 and not (
            isinstance(value, (list, tuple)) and len(value) == 3 and all(_is_number(v) for v in value)
        ):
            raise ValueError(f"preset {name!r}: {key} must be three numbers")

    cameras = preset.get("cameras")
    if cameras is not None:
        if not isinstance(cameras, list) or not cameras:
            raise ValueError(f"preset {name!r}: 'cameras' must be a non-empty list when present")
        if not all(isinstance(entry, str) and entry for entry in cameras):
            raise ValueError(f"preset {name!r}: 'cameras' entries must be names or prim paths")

    animation = preset.get("animation")
    if animation is None:
        return
    if not isinstance(animation, dict):
        raise ValueError(f"preset {name!r}: 'animation' must be a mapping")

    unknown = set(animation) - {"target", "waveform", "base", "amplitude_fraction", "frequency_hz"}
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
