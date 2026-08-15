# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Pure-Python checks for apply / revert / deferred-write bookkeeping.

Run it from `sil/scripts/`:

    python3 -m postprocessing.test_controller

`test_animation.py` injects `_animations` directly, so it never executes
`_apply`, `_revert`, `_resolve_groups` or the pending render-product store —
which is exactly the state this module is about. Here the controller runs for
real and Kit is replaced at the module boundary instead: fake `pxr` and
`omni.usd` modules are installed in `sys.modules`, so `_stage`, `_author`,
`_clear_authored_usd` and `_render_products` are the shipped implementations
running against a fake stage. That matters for the bookkeeping they own — the
`_authored_attrs` dedupe under animation, the order that ledger is written in
relative to the write it describes, and the pending store's size — which a
hand-written stub would have to re-implement to test, and would then be testing
itself.

The fake stage counts `Traverse()` calls, because "one traversal per animated
render-product scope per frame" is a claim in the README, the config comments
and the load-time WARNING, and it is the kind of claim that rots silently.

These checks say nothing about what Kit does with the values, only about what
the controller asks it to do, when, and how many times.
"""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import types

from .effects import EFFECTS

# -- fake Kit ---------------------------------------------------------------


class _FakeAttribute:
    def __init__(self, prim, name):
        self._prim = prim
        self._name = name

    def Set(self, value):
        error = self._prim.set_errors.get(self._name)
        if error is not None:
            raise error
        self._prim.attributes[self._name] = value


class _FakeRelationship:
    def __init__(self, targets):
        self._targets = list(targets)

    def GetTargets(self):
        return list(self._targets)


class _FakePrim:
    """Enough of Usd.Prim for the controller's authoring and traversal paths."""

    def __init__(self, path, type_name="Camera", valid=True, camera_target=None):
        self.path = path
        self.type_name = type_name
        self.valid = valid
        self.attributes = {}
        self.schemas = []
        self.removed_properties = []
        self.removed_schemas = []
        self.remove_property_error = None
        # attr name -> exception, raised from Set(). The attribute is still
        # created first, because CreateAttribute is what authors it in USD and
        # that is what a failed write leaves behind.
        self.set_errors = {}
        self._camera_target = camera_target

    # -- read
    def GetPath(self):
        return self.path

    def IsValid(self):
        return self.valid

    def GetTypeName(self):
        return self.type_name

    def GetAppliedSchemas(self):
        return list(self.schemas)

    def GetRelationship(self, name):
        if name == "camera" and self._camera_target is not None:
            return _FakeRelationship([self._camera_target])
        return None

    # -- write
    def AddAppliedSchema(self, name):
        self.schemas.append(name)

    def CreateAttribute(self, name, _sdf_type):
        self.attributes.setdefault(name, None)
        return _FakeAttribute(self, name)

    def RemoveProperty(self, name):
        if self.remove_property_error is not None:
            raise self.remove_property_error
        self.removed_properties.append(name)
        self.attributes.pop(name, None)

    def RemoveAppliedSchema(self, name):
        self.removed_schemas.append(name)
        if name in self.schemas:
            self.schemas.remove(name)


class _FakeStage:
    def __init__(self):
        self.prims = {}
        self.traversals = 0

    def add(self, prim):
        self.prims[prim.path] = prim
        return prim

    def GetPrimAtPath(self, path):
        return self.prims.get(path) or _FakePrim(path, valid=False)

    def Traverse(self):
        self.traversals += 1
        return list(self.prims.values())

    def GetSessionLayer(self):
        return "session-layer"


class _FakeSettings:
    """carb.settings surface: get/set, plus destroy_item for the _MISSING case."""

    def __init__(self):
        self.values = {}
        self.destroyed = []
        self.set_error_paths = set()

    def get(self, path):
        return self.values.get(path)

    def set(self, path, value):
        if path in self.set_error_paths:
            raise RuntimeError(f"carb refused {path}")
        self.values[path] = value

    def destroy_item(self, path):
        self.destroyed.append(path)
        self.values.pop(path, None)


def _install_fake_kit(stage):
    """Put fake `pxr` and `omni.usd` in sys.modules; return an undo callable.

    Installed unconditionally rather than only when the real ones are absent:
    the point is that this file behaves the same on a laptop and inside the
    Isaac container, so a check that passes here cannot be a property of
    whichever Kit happened to be importable.
    """
    saved = {name: sys.modules.get(name) for name in ("pxr", "omni", "omni.usd")}

    class _EditContext:
        def __init__(self, *_args):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            return False

    pxr = types.ModuleType("pxr")
    pxr.Sdf = types.SimpleNamespace(
        ValueTypeNames=types.SimpleNamespace(Bool="bool", Float="float", Color3f="color3f")
    )
    pxr.Usd = types.SimpleNamespace(EditContext=_EditContext)
    pxr.Gf = types.SimpleNamespace(Vec3f=lambda *values: tuple(values))

    omni = types.ModuleType("omni")
    omni_usd = types.ModuleType("omni.usd")
    omni_usd.get_context = lambda: types.SimpleNamespace(get_stage=lambda: stage)
    omni.usd = omni_usd

    sys.modules["pxr"] = pxr
    sys.modules["omni"] = omni
    sys.modules["omni.usd"] = omni_usd

    def undo():
        for name, module in saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module

    return undo


# -- fixtures ---------------------------------------------------------------

CAM1 = "/World/Cameras/Camera_01"
CAM2 = "/World/Cameras/Camera_02"

SHIPPED_CONFIG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "configs",
    "postprocessing.yaml",
)

# Camera_01 authors on the camera prim (immediate); Camera_02 wants the render
# product (deferred until Play). Both halves are needed: a preset that only
# defers leaves `_authored_attrs` empty, and `_revert()` then skips
# `_clear_authored_usd()` entirely — so a check built on such a preset would
# pass whether or not the cleanup path is guarded.
MIXED_CONFIG = f"""
version: 1
active: mixed_scope
presets:
  mixed_scope:
    description: One camera dimmed and flickering, one camera grainy.
    per_camera:
      {CAM1}:
        settings:
          auto_exposure.enabled: false
          exposure.iso: 60.0
        animation:
          target: exposure.iso
          waveform: sine
          base: 60.0
          amplitude_fraction: 0.4
          frequency_hz: 1.0
      {CAM2}:
        settings:
          tv_noise.enabled: true
          tv_noise.film_grain.enabled: true
          tv_noise.film_grain.amount: 0.2
"""


class _Harness:
    """A controller with a fake clock, fake carb and a fake stage."""

    def __init__(self, config_text=None, config_path=None, preset=None, with_render_products=False):
        from .controller import PostProcessingController

        self.stage = _FakeStage()
        self.stage.add(_FakePrim(CAM1))
        self.stage.add(_FakePrim(CAM2))
        self.render_products = {}
        if with_render_products:
            self.add_render_products()

        self._undo_kit = _install_fake_kit(self.stage)
        self._tempdir = None
        if config_text is not None:
            self._tempdir = tempfile.TemporaryDirectory()
            config_path = os.path.join(self._tempdir.name, "postprocessing.yaml")
            with open(config_path, "w") as stream:
                stream.write(config_text)

        self.settings = _FakeSettings()
        self.now = 0.0
        self.controller = PostProcessingController(
            config_path=config_path,
            preset_override=preset,
            # The poll is not what these checks are about, and a reload
            # part-way through one would re-read the file and re-apply,
            # contaminating every count taken after it.
            poll_interval_sec=10**9,
            settings=self.settings,
            clock=lambda: self.now,
        )
        # cameras.yaml is a plain file read, not a Kit call, but the shipped
        # presets name cameras rather than prim paths, so it has to resolve.
        self.controller._load_camera_name_map = lambda: {
            "Camera_01": CAM1,
            "Camera_02": CAM2,
        }

    def add_render_products(self):
        for index, camera in enumerate((CAM1, CAM2), start=1):
            path = f"/Render/RenderProduct_{index:02d}"
            prim = _FakePrim(path, type_name="RenderProduct", camera_target=camera)
            self.stage.add(prim)
            self.render_products[camera] = prim

    def start(self, quiet=True):
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer) if quiet else contextlib.nullcontext():
            self.controller.start()
        return buffer.getvalue()

    def tick(self, dt=1.0 / 30.0, quiet=True):
        """One run-loop frame. Never reloads: poll_interval_sec is effectively
        infinite, so `update()` exercises the flush and the animator only."""
        self.now += dt
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer) if quiet else contextlib.nullcontext():
            self.controller.update()
        return buffer.getvalue()

    def close(self):
        self._undo_kit()
        if self._tempdir is not None:
            self._tempdir.cleanup()


# -- checks -----------------------------------------------------------------


def check_apply_authors_and_defers():
    """`_apply` writes camera-prim effects now and queues render-product ones."""
    harness = _Harness(config_text=MIXED_CONFIG)
    try:
        harness.start()
        controller = harness.controller

        assert controller.active_preset == "mixed_scope", controller.active_preset
        cam1 = harness.stage.prims[CAM1]
        assert cam1.attributes, "camera-prim effects must be authored immediately"
        assert EFFECTS["auto_exposure.enabled"].usd_attr in cam1.attributes, sorted(cam1.attributes)

        cam2 = harness.stage.prims[CAM2]
        assert not cam2.attributes, f"render-product effects must not land on the camera prim: {cam2.attributes}"

        pending = controller._pending_rp_settings
        assert isinstance(pending, dict), type(pending)
        assert len(pending) == 3, f"expected 3 deferred settings, got {sorted(pending)}"
        assert all(paths == (CAM2,) for paths, _key in pending), sorted(pending)
        print(
            f"  authored {len(cam1.attributes)} attr(s) on Camera_01, "
            f"deferred {len(pending)} setting(s) for Camera_02"
        )
    finally:
        harness.close()


def check_pending_is_bounded_before_play():
    """The pending store must not grow one entry per pre-Play frame.

    This is the list -> dict change, and the reason for it: an ANIMATED
    render-product target re-writes the same key every frame, and the flush
    traverses the stage once per entry, so an append-only store cost
    O(frames^2) traversals before Play.
    """
    harness = _Harness(config_path=SHIPPED_CONFIG, preset="anim_rp_target")
    try:
        harness.start()
        controller = harness.controller
        after_apply = len(controller._pending_rp_settings)

        sizes = []
        for _ in range(120):
            harness.tick()
            sizes.append(len(controller._pending_rp_settings))

        assert max(sizes) == min(sizes) == after_apply, (
            f"pending store changed size across frames: {min(sizes)}..{max(sizes)}"
        )
        assert after_apply == 3, f"anim_rp_target should defer 3 settings, got {after_apply}"
        print(
            f"  anim_rp_target: {after_apply} deferred setting(s), unchanged over "
            f"{len(sizes)} pre-Play frames (append-only would have reached {after_apply + len(sizes)})"
        )
    finally:
        harness.close()


def measure_traversal_cost():
    """Count stage traversals per frame for the shipped `anim_rp_target`.

    Reported rather than merely asserted, because the number is quoted in the
    README, in postprocessing.yaml and in the load-time WARNING, and quoting a
    number nothing measures is how it drifts.
    """
    harness = _Harness(config_path=SHIPPED_CONFIG, preset="anim_rp_target")
    try:
        before = harness.stage.traversals
        harness.start()
        apply_frame = harness.stage.traversals - before

        per_frame = []
        for _ in range(10):
            mark = harness.stage.traversals
            harness.tick()
            per_frame.append(harness.stage.traversals - mark)

        entries = len(harness.controller._pending_rp_settings)
        steady = set(per_frame)
        assert len(steady) == 1, f"traversals per frame not steady: {sorted(steady)}"
        steady_state = per_frame[0]
        print(
            f"  anim_rp_target pre-Play: {entries} pending entry/entries, "
            f"{steady_state} stage traversals per frame "
            f"({entries} flush + 1 animate); the apply frame itself costs {apply_frame}"
        )
        # 3 settings, all render-product targets, one of them animated. The
        # figure quoted in bf9434c's commit message (1 entry, 2 traversals)
        # describes a preset that animates grain WITHOUT switching the grain
        # pass on -- i.e. one that renders no grain at all.
        assert entries == 3, entries
        assert steady_state == entries + 1, (steady_state, entries)
        return entries, steady_state, apply_frame
    finally:
        harness.close()


def check_flush_respects_scope():
    """A deferred write must reach its own camera's render product only."""
    harness = _Harness(config_text=MIXED_CONFIG)
    try:
        harness.start()
        controller = harness.controller
        assert len(controller._pending_rp_settings) == 3

        # Play: IsaacCreateRenderProduct creates the products on the first tick.
        harness.add_render_products()
        harness.tick()

        assert not controller._pending_rp_settings, controller._pending_rp_settings
        rp2 = harness.render_products[CAM2]
        rp1 = harness.render_products[CAM1]
        assert rp2.attributes, "Camera_02's deferred settings never reached its render product"
        assert not rp1.attributes, (
            f"Camera_02's settings leaked onto Camera_01's render product: {sorted(rp1.attributes)}"
        )
        print(
            f"  flushed {len(rp2.attributes)} setting(s) onto Camera_02's render product, "
            f"{len(rp1.attributes)} onto Camera_01's"
        )
    finally:
        harness.close()


def check_animation_does_not_grow_authored_list():
    """Re-authoring the same attribute every frame must record it once."""
    harness = _Harness(config_text=MIXED_CONFIG, with_render_products=True)
    try:
        harness.start()
        controller = harness.controller
        after_apply = len(controller._authored_attrs)
        for _ in range(200):
            harness.tick()
        after_frames = len(controller._authored_attrs)
        assert after_frames == after_apply, (
            f"_authored_attrs grew from {after_apply} to {after_frames} under animation"
        )
        print(
            f"  {after_frames} tracked attribute(s) after 200 animated frames "
            f"(one per attribute, not one per write)"
        )
    finally:
        harness.close()


def check_revert_clears_everything():
    """`_revert` must undo carb, USD and both pieces of pending state."""
    harness = _Harness(config_text=MIXED_CONFIG, with_render_products=True)
    try:
        harness.start()
        harness.tick()  # flush the deferred writes onto the render products
        controller = harness.controller
        cam1 = harness.stage.prims[CAM1]
        rp2 = harness.render_products[CAM2]
        assert cam1.attributes and rp2.attributes

        controller._revert()

        assert not controller._authored_attrs, controller._authored_attrs
        assert not controller._authored_apis, controller._authored_apis
        assert not controller._pending_rp_settings, controller._pending_rp_settings
        assert not controller._animations, controller._animations
        assert not cam1.attributes, f"camera prim still degraded: {sorted(cam1.attributes)}"
        assert not rp2.attributes, f"render product still degraded: {sorted(rp2.attributes)}"
        assert not cam1.schemas and not rp2.schemas, "applied API schemas survived the revert"
        print(
            f"  removed {len(cam1.removed_properties) + len(rp2.removed_properties)} attribute(s) "
            f"and {len(cam1.removed_schemas) + len(rp2.removed_schemas)} API schema(s); "
            f"pending and animations empty"
        )
    finally:
        harness.close()


def check_pending_cleared_when_usd_cleanup_raises():
    """B1: a raising `_clear_authored_usd()` must not strand the pending writes.

    `_revert()` runs the USD cleanup inside its `finally`, ahead of the state
    resets. If that cleanup raises and the resets are not themselves protected,
    `_pending_rp_settings` survives a revert -- and `update()` then authors the
    reverted preset onto the render products on the first tick after Play,
    while `active_preset` reports None.

    Driven through `_revert()` directly, which is the door `close()` and a
    direct call use. The `_reload_if_changed` rejection path resets the pending
    store itself, so a check driven through it would pass either way.
    """
    harness = _Harness(config_text=MIXED_CONFIG)
    try:
        harness.start()
        controller = harness.controller
        assert controller._pending_rp_settings, "fixture must have deferred writes to strand"
        assert controller._authored_attrs, "fixture must have authored attrs, or cleanup is skipped"

        # A USD cleanup failure, raised from inside the shipped
        # _clear_authored_usd() rather than by replacing it.
        harness.stage.prims[CAM1].remove_property_error = RuntimeError("stage closed mid-revert")

        raised = None
        try:
            controller._revert()
        except Exception as exc:  # noqa: BLE001 - the caller logs this and carries on
            raised = exc
        assert raised is not None, "fixture did not actually make the cleanup fail"

        stranded = sorted(key for _paths, key in controller._pending_rp_settings)
        stranded_animations = len(controller._animations)

        # The tick after Play: the render products now exist, so anything left
        # pending is authored here -- which is the consequence that matters, so
        # it is measured before anything is asserted rather than after.
        harness.add_render_products()
        harness.tick()
        leaked = sorted(harness.render_products[CAM2].attributes)

        assert not leaked, (
            f"the reverted preset was flushed onto the render product on the tick after "
            f"Play: {leaked} (active_preset={controller.active_preset!r}); "
            f"{len(stranded)} setting(s) had survived the failed revert: {stranded}"
        )
        assert not stranded, f"{len(stranded)} deferred setting(s) survived the failed revert: {stranded}"
        assert not stranded_animations, "animations survived the failed revert"
        print(
            f"  cleanup raised {raised.__class__.__name__}; pending empty afterwards and the "
            f"next tick authored nothing onto the render products"
        )
    finally:
        harness.close()


def check_reject_mid_apply_reverts():
    """A failure inside `_apply` must fall back to the baseline, not half a look."""
    harness = _Harness(config_text=MIXED_CONFIG, with_render_products=True)
    try:
        harness.start()
        controller = harness.controller
        assert controller.active_preset == "mixed_scope"

        # Rewrite the file so the next poll applies a preset whose camera does
        # not exist on the stage... that is a resolve error, which must NOT
        # revert. Make the apply itself fail instead: carb refuses a global
        # write. Global settings go through _settings.set inside _apply.
        harness.settings.set_error_paths.add("/rtx/post/histogram/enabled")
        with open(controller.config_path, "w") as stream:
            stream.write(
                "version: 1\n"
                "active: global_dark\n"
                "presets:\n"
                "  global_dark:\n"
                "    settings:\n"
                "      auto_exposure.enabled: false\n"
            )
        harness.now += 10**9  # force the poll
        log = harness.tick()

        assert "reverted the renderer" in log, log
        assert controller.active_preset is None, controller.active_preset
        assert controller._digest is None, "the last good digest must be forgotten"
        assert not controller._pending_rp_settings, controller._pending_rp_settings
        assert not controller._animations, controller._animations
        cam1 = harness.stage.prims[CAM1]
        assert not cam1.attributes, f"half a preset left on the stage: {sorted(cam1.attributes)}"
        print("  mid-apply failure: reverted, digest forgotten, pending and animations empty")
    finally:
        harness.close()


def check_failed_usd_write_is_recorded_before_it_runs():
    """A USD write that raises must already be in the ledger when it does.

    `CreateAttribute` is what authors the attribute, not `Set()`, so a `Set()`
    that raises leaves the attribute on the prim. Bookkeeping that runs after
    the write therefore misses exactly the attribute the failure left behind:
    `_clear_authored_usd()` never sees it, and it outlives every `_revert()`
    and `close()` for the life of the process while `active_preset` reports
    None -- one camera quietly degraded, nothing in the logs naming it.

    The check above drives its mid-apply failure through carb at global scope,
    which never enters `_author()` at all, so it holds whichever order the two
    lines are in. This one fails if they are swapped back.
    """
    harness = _Harness(config_text=MIXED_CONFIG, with_render_products=True)
    try:
        controller = harness.controller
        cam1 = harness.stage.prims[CAM1]
        iso_attr = EFFECTS["exposure.iso"].usd_attr
        # The second write on this camera, so one attribute is already recorded
        # and what is measured is the failing write rather than an empty ledger.
        cam1.set_errors[iso_attr] = RuntimeError("USD refused the value")

        log = harness.start()

        assert "reverted the renderer" in log, log
        assert controller.active_preset is None, controller.active_preset
        # What the revert removed is what the ledger held: _clear_authored_usd()
        # walks _authored_attrs and nothing else.
        assert iso_attr in cam1.removed_properties, (
            f"the failed write was never recorded: the revert removed "
            f"{cam1.removed_properties} and left {iso_attr} behind"
        )
        orphans = sorted(cam1.attributes)
        assert not orphans, (
            f"{orphans} survived the revert: CreateAttribute authored {iso_attr} "
            f"before Set() raised, so recording it only after Set() returns puts "
            f"it out of reach of every later cleanup"
        )
        assert not cam1.schemas, f"applied API schemas survived: {cam1.schemas}"
        print(
            f"  Set() raised on {iso_attr}: recorded before the write, "
            f"removed by the revert, prim clean"
        )
    finally:
        harness.close()


def check_config_error_keeps_running_preset():
    """A config error before the apply must leave the live preset alone."""
    harness = _Harness(config_text=MIXED_CONFIG, with_render_products=True)
    try:
        harness.start()
        harness.tick()
        controller = harness.controller
        cam1 = harness.stage.prims[CAM1]
        # Keys, not values: the preset animates exposure.iso, so the value on
        # the prim is expected to differ every frame. What must not change is
        # WHICH attributes are authored, and that the animation is still driving
        # them at all.
        authored = sorted(cam1.attributes)
        assert authored

        with open(controller.config_path, "w") as stream:
            stream.write(
                "version: 1\n"
                "active: broken\n"
                "presets:\n"
                "  broken:\n"
                "    settings:\n"
                "      exposure.iso: not-a-number\n"
            )
        harness.now += 10**9
        iso_attr = EFFECTS["exposure.iso"].usd_attr
        iso_before = cam1.attributes[iso_attr]
        log = harness.tick()

        assert "active preset is 'mixed_scope'" in log, log
        assert controller.active_preset == "mixed_scope", controller.active_preset
        assert sorted(cam1.attributes) == authored, "a config error disturbed the running preset"
        assert len(controller._animations) == 1, "the running preset stopped animating"
        assert cam1.attributes[iso_attr] != iso_before, (
            "the animation stalled, so the preset is frozen rather than running"
        )
        print(
            f"  config error: {len(authored)} authored attribute(s) unchanged, "
            f"animation still driving exposure.iso"
        )
    finally:
        harness.close()


def main():
    checks = (
        ("apply authors camera effects and defers render-product ones", check_apply_authors_and_defers),
        ("pending store stays bounded before Play", check_pending_is_bounded_before_play),
        ("traversal cost of anim_rp_target (measurement)", measure_traversal_cost),
        ("deferred writes flush to their own scope", check_flush_respects_scope),
        ("animation does not grow the authored-attribute list", check_animation_does_not_grow_authored_list),
        ("revert clears carb, USD, pending and animations", check_revert_clears_everything),
        ("pending cleared when the USD cleanup raises", check_pending_cleared_when_usd_cleanup_raises),
        ("mid-apply failure reverts to the baseline", check_reject_mid_apply_reverts),
        ("a failed USD write is recorded before it runs", check_failed_usd_write_is_recorded_before_it_runs),
        ("config error keeps the running preset", check_config_error_keeps_running_preset),
    )
    failed = 0
    for title, run in checks:
        print(f"[{title}]")
        try:
            run()
        except AssertionError as exc:
            failed += 1
            print(f"  FAIL: {exc}")
    print(f"\n{len(checks) - failed}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
