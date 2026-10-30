# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Allow-list of RTX post-processing controls, keyed by a stable logical name.

Each effect can be applied two ways, and the two use DIFFERENT names for the
same control:

  * globally, by setting a carb setting (`/rtx/post/...`) — affects the viewport
    and every render product that does not override it;
  * per camera, by authoring a USD attribute on a specific prim — overrides the
    carb value for that camera only.

The USD side is not a rename of the carb path, so the mapping has to be a table
rather than a string transform. Two things in particular bite:

  * exposure and auto-exposure are authored on the CAMERA prim
    (`exposure:iso`, `omni:rtx:autoExposure:enabled`), while grain, scanlines,
    vignetting and color grading are authored on the RENDER PRODUCT
    (`omni:rtx:post:tvNoise:*`, `omni:rtx:post:grade:*`);
  * carb spells color grading `colorgrad` but USD spells it `grade`, and carb
    flattens the tvNoise toggles (`enableFilmGrain`) where USD nests them
    (`tvNoise:filmGrain:enabled`).

Names and defaults are transcribed from the Kit renderer source rather than the
public docs table:
  kit/rendering/source/plugins/rtx.postprocessing/RtxPostProcessingSettingNames.cpp
  kit/schemas/rtx_settings/usd_plugins/generatedSchema.usda
  kit/rendering/include/rtx/postprocessing/TvNoiseSettings.h
"""

from __future__ import annotations

# Which prim carries the USD attribute for an effect.
TARGET_CAMERA = "camera"
TARGET_RENDER_PRODUCT = "render_product"

# Value kinds. Kept as strings so this table stays importable outside Kit
# (the pxr Sdf types are resolved lazily in the controller).
KIND_BOOL = "bool"
KIND_FLOAT = "float"
KIND_COLOR3 = "color3f"


class Effect:
    """One renderer control, addressable as a carb setting or a USD attribute."""

    __slots__ = ("carb_path", "usd_attr", "usd_api", "target", "kind", "doc")

    def __init__(self, carb_path, usd_attr, usd_api, target, kind, doc=""):
        self.carb_path = carb_path
        self.usd_attr = usd_attr
        self.usd_api = usd_api
        self.target = target
        self.kind = kind
        self.doc = doc


_CAM_EXPOSURE_API = "OmniRtxCameraExposureAPI_1"
_CAM_AUTO_EXPOSURE_API = "OmniRtxCameraAutoExposureAPI_1"
_GRADE_API = "OmniRtxPostColorGradingAPI_1"
_TV_NOISE_API = "OmniRtxPostTvNoiseAPI_1"


EFFECTS = {
    # --- Exposure: the "sudden low light" / "flicker" family. -------------
    # Auto-exposure must be off for iso/time to hold: with it on, the
    # histogram drives exposure back to a normal-looking image and the
    # injected anomaly disappears within a second.
    "auto_exposure.enabled": Effect(
        "/rtx/post/histogram/enabled",
        "omni:rtx:autoExposure:enabled",
        _CAM_AUTO_EXPOSURE_API,
        TARGET_CAMERA,
        KIND_BOOL,
        "Auto-exposure. Turn OFF before forcing iso/time.",
    ),
    "exposure.iso": Effect(
        "/rtx/post/tonemap/filmIso",
        "exposure:iso",
        _CAM_EXPOSURE_API,
        TARGET_CAMERA,
        KIND_FLOAT,
        "Sensor speed. 100 = nominal; lower darkens the image.",
    ),
    "exposure.time": Effect(
        "/rtx/post/tonemap/exposureTime",
        "exposure:time",
        _CAM_EXPOSURE_API,
        TARGET_CAMERA,
        KIND_FLOAT,
        "Shutter time in seconds. PER-CAMERA ONLY: the USD attribute works, but "
        "the carb key is inert in a running sim -- the render loop rewrites "
        "/rtx/post/tonemap/exposureTime every frame (it mirrors cameraShutter, "
        "the same quantity as 1/s), so a global write never reaches the "
        "tonemapper. Measured: setting it globally left RTSP brightness flat, "
        "while the same value on the camera prim halved it. For a global "
        "brightness flicker animate exposure.iso instead.",
    ),
    "exposure.f_stop": Effect(
        "/rtx/post/tonemap/fNumber",
        "exposure:fStop",
        _CAM_EXPOSURE_API,
        TARGET_CAMERA,
        KIND_FLOAT,
        "Aperture. Larger numbers darken the image. PER-CAMERA ONLY, the same "
        "way exposure.time is: the carb key does not reach the tonemapper in a "
        "running sim. Measured: a global write left RTSP brightness at 122.8 "
        "(unchanged), while the same value on the camera prim took it to 15.1. "
        "Note this is the tonemapper's aperture, not depth of field -- it "
        "changes exposure, it does not defocus.",
    ),
    # --- Color grading: the "hue shift" / white-balance-drift family. -----
    "grade.enabled": Effect(
        "/rtx/post/colorgrad/enabled",
        "omni:rtx:post:grade:enabled",
        _GRADE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "grade.gain": Effect(
        "/rtx/post/colorgrad/gain",
        "omni:rtx:post:grade:gain",
        _GRADE_API,
        TARGET_RENDER_PRODUCT,
        KIND_COLOR3,
        "Per-channel multiplier. [1.03, 1.0, 0.97] reads as a warm cast.",
    ),
    "grade.gamma": Effect(
        "/rtx/post/colorgrad/gamma",
        "omni:rtx:post:grade:gamma",
        _GRADE_API,
        TARGET_RENDER_PRODUCT,
        KIND_COLOR3,
    ),
    "grade.contrast": Effect(
        "/rtx/post/colorgrad/contrast",
        "omni:rtx:post:grade:contrast",
        _GRADE_API,
        TARGET_RENDER_PRODUCT,
        KIND_COLOR3,
    ),
    # --- TV noise: grain, scanlines, vignetting, analog artifacts. --------
    # One RTX compute pass; the toggles below become a shader bitmask, so
    # `tv_noise.enabled` gates all of them.
    "tv_noise.enabled": Effect(
        "/rtx/post/tvNoise/enabled",
        "omni:rtx:post:tvNoise:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
        "Master gate for the whole TV-noise pass.",
    ),
    "tv_noise.film_grain.enabled": Effect(
        "/rtx/post/tvNoise/enableFilmGrain",
        "omni:rtx:post:tvNoise:filmGrain:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
        "Defaults ON once the pass is enabled, unlike the other toggles.",
    ),
    "tv_noise.film_grain.amount": Effect(
        "/rtx/post/tvNoise/grainAmount",
        "omni:rtx:post:tvNoise:filmGrain:amount",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "0.0-0.2 (default 0.05).",
    ),
    "tv_noise.film_grain.size": Effect(
        "/rtx/post/tvNoise/grainSize",
        "omni:rtx:post:tvNoise:filmGrain:size",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "Inverse grain size, 1.5-2.5 (default 1.6).",
    ),
    "tv_noise.color_amount": Effect(
        "/rtx/post/tvNoise/colorAmount",
        "omni:rtx:post:tvNoise:colorAmount",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "Chroma variation per grain, 0.0-1.0 (default 0.6).",
    ),
    "tv_noise.lum_amount": Effect(
        "/rtx/post/tvNoise/lumAmount",
        "omni:rtx:post:tvNoise:lumAmount",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "Luminance variation per grain, 0.0-1.0 (default 1.0).",
    ),
    "tv_noise.scanlines.enabled": Effect(
        "/rtx/post/tvNoise/enableScanlines",
        "omni:rtx:post:tvNoise:scanlines:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "tv_noise.scanlines.spread": Effect(
        "/rtx/post/tvNoise/scanlineSpread",
        "omni:rtx:post:tvNoise:scanlines:spread",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "Scanline FREQUENCY, not amplitude: the shader does "
        "col *= 0.75 + (sin(uv.y * spread * height) + 1) * 0.25, so the swing is "
        "always +/-25% and only the band spacing changes. 1.0 on a 540-line image "
        "is ~86 bands (6 px apart). Public range 0.0-2.0, default 1.0.",
    ),
    "tv_noise.vignetting.enabled": Effect(
        "/rtx/post/tvNoise/enableVignetting",
        "omni:rtx:post:tvNoise:vignetting:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
        "Darkened border. The closest thing here to a soft lens blockage.",
    ),
    # The vignette is NOT normalised, which makes `size` easy to misread: the
    # shader computes vig = uv(1-uv).x * uv(1-uv).y * (size + 14), then raises it
    # to `strength` and MULTIPLIES the colour by it. At the image centre the uv
    # term is 0.0625, so the centre gain is 0.0625 * (size + 14) — the shipped
    # default of 107 multiplies the centre by ~7.6 and blows it to white. Use
    # size 2.0 (0.0625 * 16 = 1.0) to leave the centre alone and let `strength`
    # set how hard the edges fall off.
    "tv_noise.vignetting.size": Effect(
        "/rtx/post/tvNoise/vignettingSize",
        "omni:rtx:post:tvNoise:vignetting:size",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "Centre gain is 0.0625 * (size + 14); 2.0 keeps the centre at 1.0. "
        "Public range 0.0-255, default 107 (which overexposes the centre).",
    ),
    "tv_noise.vignetting.strength": Effect(
        "/rtx/post/tvNoise/vignettingStrength",
        "omni:rtx:post:tvNoise:vignetting:strength",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "Falloff exponent, applied as pow(vig, strength). Higher = darker edges. "
        "Public range 0.0-2.0, default 0.7.",
    ),
    "tv_noise.vignetting.flickering.enabled": Effect(
        "/rtx/post/tvNoise/enableVignettingFlickering",
        "omni:rtx:post:tvNoise:vignetting:flickering:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "tv_noise.wave_distortion.enabled": Effect(
        "/rtx/post/tvNoise/enableWaveDistortion",
        "omni:rtx:post:tvNoise:waveDistortion:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "tv_noise.vertical_lines.enabled": Effect(
        "/rtx/post/tvNoise/enableVerticalLines",
        "omni:rtx:post:tvNoise:verticalLines:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "tv_noise.random_splotches.enabled": Effect(
        "/rtx/post/tvNoise/enableRandomSplotches",
        "omni:rtx:post:tvNoise:randomSplotches:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "tv_noise.ghost_flickering.enabled": Effect(
        "/rtx/post/tvNoise/enableGhostFlickering",
        "omni:rtx:post:tvNoise:ghostFlickering:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    "tv_noise.scroll_bug.enabled": Effect(
        "/rtx/post/tvNoise/enableScrollBug",
        "omni:rtx:post:tvNoise:scrollBug:enabled",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
    ),
    # Replaces elapsed render time with a fixed seed, so two runs of the same
    # scenario produce byte-identical noise. Required for the SRR regression
    # harness, where a diffing run cannot tolerate per-frame randomness.
    "tv_noise.fixed_time.seed": Effect(
        "/rtx/post/tvNoise/fixedTimeSeed",
        "omni:rtx:post:tvNoise:fixedTime:seed",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_BOOL,
        "Deterministic noise: freeze the time input.",
    ),
    "tv_noise.fixed_time.seed_count": Effect(
        "/rtx/post/tvNoise/fixedTimeSeedCount",
        "omni:rtx:post:tvNoise:fixedTime:seedCount",
        _TV_NOISE_API,
        TARGET_RENDER_PRODUCT,
        KIND_FLOAT,
        "The frozen time value used when fixed_time.seed is true.",
    ),
}

# Only float effects can be driven by the per-frame animator.
ANIMATABLE = tuple(key for key, eff in EFFECTS.items() if eff.kind == KIND_FLOAT)
