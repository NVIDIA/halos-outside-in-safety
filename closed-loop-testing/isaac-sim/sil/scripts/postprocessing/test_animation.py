# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Pure-Python checks for the per-camera animator. No Kit, no numpy, no pytest.

Run it from `sil/scripts/`:

    python3 -m postprocessing.test_animation

The controller's config layer (`_select_preset`, `_validate_preset`,
`_preset_groups`, `_validate_*`) and `_advance_animation` deliberately import
nothing from carb or pxr, which is what makes this runnable on a laptop. The
Kit-only paths are stubbed: `_write_scoped` is replaced with a recorder, so the
groups carry fake prim paths and no stage is needed.

These are checks of the animator MATH under a synthetic clock. They say nothing
about what an RTSP consumer receives — that needs a live capture.
"""

from __future__ import annotations

import math
import os
import sys

from .controller import (
    PostProcessingController,
    _preset_groups,
    _select_preset,
    _validate_preset,
)

CONFIG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "configs",
    "postprocessing.yaml",
)


def _load_presets(path=CONFIG):
    import yaml

    with open(path) as stream:
        return yaml.safe_load(stream) or {}


class _Recorder:
    """Stands in for the whole Kit side: records what the animator would write."""

    def __init__(self):
        self.writes = []  # (t, prim_paths, key, value)
        self.now = 0.0

    def set(self, path, value):  # carb settings surface, for global groups
        self.writes.append((self.now, (), path, value))


def _controller_for(preset, recorder):
    """A controller wired to a fake clock, with the groups injected directly.

    `_resolve_groups` is skipped on purpose: it needs a USD stage to prove the
    camera prims exist. The animator does not care what the paths are, so the
    preset's camera names are used as the paths.
    """
    controller = PostProcessingController(
        config_path=CONFIG, settings=recorder, clock=lambda: recorder.now
    )
    controller._animations = [
        (list(cameras), animation)
        for cameras, _settings, animation in _preset_groups(preset)
        if animation
    ]
    controller._animation_t0 = 0.0

    def _record(key, value, prim_paths):
        recorder.writes.append((recorder.now, tuple(prim_paths), key, value))

    controller._write_scoped = _record
    return controller


def _sample(preset, times):
    """Run `_advance_animation` at each t; return {prim_paths: [(t, value)]}."""
    recorder = _Recorder()
    controller = _controller_for(preset, recorder)
    for t in times:
        recorder.now = t
        controller._advance_animation(t)

    series = {}
    for t, prim_paths, _key, value in recorder.writes:
        series.setdefault(prim_paths, []).append((t, value))
    return series


def _dft_magnitude(values, sample_rate, frequency):
    """One bin of a DFT, evaluated directly. Five lines instead of a numpy dep."""
    mean = sum(values) / len(values)
    real = imag = 0.0
    for index, value in enumerate(values):
        angle = 2.0 * math.pi * frequency * index / sample_rate
        centred = value - mean
        real += centred * math.cos(angle)
        imag -= centred * math.sin(angle)
    return math.hypot(real, imag) / len(values)


# -- checks ----------------------------------------------------------------


def check_backward_compatible(presets):
    """`flicker` has no phase_deg, so it must be bit-identical to the old math."""
    preset = presets["flicker"]
    times = [i / 30.0 for i in range(90)]
    series = _sample(preset, times)
    assert list(series) == [()], f"flicker must stay a single global group, got {list(series)}"

    worst = 0.0
    for t, value in series[()]:
        expected = 100.0 * (1.0 + 0.35 * math.sin(2.0 * math.pi * 2.0 * t))
        worst = max(worst, abs(value - expected))
    assert worst == 0.0, f"flicker drifted from the pre-phase_deg formula by {worst}"
    print(f"  flicker (global, no phase_deg): max deviation from old formula {worst:.1e}")


def check_antiphase(presets):
    """Two 1 Hz groups 180 deg apart must stay symmetric about base."""
    preset = presets["anim_antiphase"]
    # 37 samples over one cycle, offset so none lands on a zero crossing.
    times = [0.013 + i / 36.0 for i in range(37)]
    series = _sample(preset, times)
    assert len(series) == 2, f"expected two scopes, got {list(series)}"

    (paths_a, first), (paths_b, second) = sorted(series.items())
    base = 100.0
    worst_residual = 0.0
    worst_at = None
    for (t, value_a), (_, value_b) in zip(first, second):
        residual = abs(value_a + value_b - 2.0 * base)
        if residual > worst_residual:
            worst_residual, worst_at = residual, t

    values_a = [v for _, v in first]
    swing_a = max(values_a) - min(values_a)
    values_b = [v for _, v in second]
    swing_b = max(values_b) - min(values_b)

    print(
        f"  {paths_a[0]} vs {paths_b[0]}: max |v1+v2-2*base| = {worst_residual:.3e} "
        f"(at t={worst_at:.4f}s) over {len(first)} samples"
    )
    # The continuous swing is 2*0.35*100 = 70.0; 37 samples per cycle never land
    # exactly on the crest, so the observed swing sits just under it. What
    # matters is that it is nowhere near zero — otherwise "symmetric about base"
    # would be true of two flat lines.
    print(
        f"    peak-to-peak swing {swing_a:.3f} and {swing_b:.3f} ISO "
        f"(continuous ideal 70.000; discrete sampling misses the crest)"
    )
    assert worst_residual < 1e-9, f"antiphase residual too large: {worst_residual}"
    assert swing_a > 69.0 and swing_b > 69.0, "signal did not actually swing"


def check_two_frequencies(presets):
    """Two groups at 1 Hz and 3 Hz must show up as exactly those, independently."""
    preset = presets["anim_two_freq"]
    # fs = 60 Hz, N = 600 -> 0.1 Hz bin spacing, so 1.0 and 3.0 are exact bins.
    sample_rate, count = 60.0, 600
    times = [i / sample_rate for i in range(count)]
    series = _sample(preset, times)
    assert len(series) == 2, f"expected two scopes, got {list(series)}"

    for paths, samples in sorted(series.items()):
        values = [v for _, v in samples]
        bins = [(k * 0.1, _dft_magnitude(values, sample_rate, k * 0.1)) for k in range(1, 100)]
        bins.sort(key=lambda item: item[1], reverse=True)
        (peak_hz, peak_mag), (next_hz, next_mag) = bins[0], bins[1]
        ratio = peak_mag / next_mag if next_mag else float("inf")
        print(
            f"  {paths[0]}: peak {peak_hz:.1f} Hz mag {peak_mag:.4f}; "
            f"runner-up {next_hz:.1f} Hz mag {next_mag:.2e} (ratio {ratio:.3g}x)"
        )
        expected = 1.0 if paths[0].endswith("01") else 3.0
        assert abs(peak_hz - expected) < 1e-9, f"{paths[0]} peaked at {peak_hz}, want {expected}"
        assert ratio > 1e6, f"{paths[0]} spectrum is not clean: ratio {ratio}"


def check_cross_term(presets):
    """An animated group must not write to a group that only has static settings."""
    preset = presets["anim_mixed"]
    series = _sample(preset, [i / 30.0 for i in range(60)])
    scopes = sorted(series)
    print(f"  animated scopes touched by anim_mixed: {[s[0] for s in scopes]}")
    assert scopes == [("Camera_01",)], f"animator touched an unanimated camera: {scopes}"


def check_rp_warning(presets):
    """`anim_rp_target` must validate (not reject) and emit exactly one WARNING."""
    import io
    import contextlib

    captured = io.StringIO()
    with contextlib.redirect_stdout(captured):
        _validate_preset("anim_rp_target", presets["anim_rp_target"])
    lines = [line for line in captured.getvalue().splitlines() if "WARNING" in line]
    assert len(lines) == 1, f"expected exactly one WARNING, got {len(lines)}"
    assert "-7.0% FPS" in lines[0], "the warning must quote the measured cost"
    print(f"  accepted with 1 warning: {lines[0][:96]}...")

    # The same target animated GLOBALLY is not expensive and must stay quiet.
    quiet = io.StringIO()
    with contextlib.redirect_stdout(quiet):
        _validate_preset(
            "global_grain",
            {
                "settings": {"tv_noise.enabled": True},
                "animation": {
                    "target": "tv_noise.film_grain.amount",
                    "base": 0.12,
                    "amplitude_fraction": 0.5,
                    "frequency_hz": 0.5,
                },
            },
        )
    assert "WARNING" not in quiet.getvalue(), "global scope must not warn"
    print("  same target at global scope: no warning (correct — no traversal)")


def check_phase_validation():
    """phase_deg is optional, numeric and bounded."""
    def build(**animation):
        block = {
            "target": "exposure.iso",
            "base": 100.0,
            "amplitude_fraction": 0.35,
            "frequency_hz": 1.0,
        }
        block.update(animation)
        return {"settings": {}, "animation": block}

    _validate_preset("ok_default", build())
    _validate_preset("ok_zero", build(phase_deg=0))
    _validate_preset("ok_high", build(phase_deg=359.9))
    for bad in (360, -1, "90", True):
        try:
            _validate_preset("bad", build(phase_deg=bad))
        except ValueError:
            continue
        raise AssertionError(f"phase_deg={bad!r} should have been rejected")
    print("  phase_deg: default/0/359.9 accepted; 360, -1, '90', true rejected")


def check_every_preset(config):
    """Every preset in the shipped config must validate."""
    ok, bad = [], []
    for name, preset in config["presets"].items():
        try:
            _validate_preset(name, preset or {})
            ok.append(name)
        except Exception as exc:  # noqa: BLE001 - report, do not raise
            bad.append((name, exc))
    print(f"  {len(ok)} preset(s) valid, {len(bad)} rejected")
    for name, exc in bad:
        print(f"    REJECTED {name}: {exc}")
    assert not bad, "shipped config has invalid presets"

    name, preset = _select_preset(config)
    print(f"  active preset {name!r} selects cleanly ({len(_preset_groups(preset))} group(s))")


def main():
    config = _load_presets()
    presets = config["presets"]
    checks = (
        ("every shipped preset validates", lambda: check_every_preset(config)),
        ("backward compatibility (flicker)", lambda: check_backward_compatible(presets)),
        ("antiphase symmetry", lambda: check_antiphase(presets)),
        ("two independent frequencies", lambda: check_two_frequencies(presets)),
        ("no cross-term onto a static camera", lambda: check_cross_term(presets)),
        ("render-product cost warning", lambda: check_rp_warning(presets)),
        ("phase_deg validation", check_phase_validation),
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
