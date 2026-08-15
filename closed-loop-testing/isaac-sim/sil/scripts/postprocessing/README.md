# `sil/scripts/postprocessing/` — RTX sensor-anomaly injection

Injects visual degradation into the rendered image so the safety monitor can be
regression-tested against a bad camera: sudden low light, colour cast, exposure
flicker, sensor grain, heavy vignetting.

This is **not** an OmniGraph, and that is the point. RTX post-processing is
consumed by the renderer itself, so writing a setting changes the next rendered
frames — no graph to rebuild, no writer to attach, and **data generation never
has to start**. The pass runs before the encoder, so one preset shows up in the
viewport, in the SDG RGB output and on every RTSP stream at once. Compare with
`../action_graphs/`, where each module builds a graph and needs Play to tick.

Upstream reference: [RTX post-processing](https://docs.omniverse.nvidia.com/materials-and-rendering/latest/rtx_post-processing.html)
and the IRA visual-noise example.

## What lives here

| Module | Public entry point | Purpose |
|---|---|---|
| `effects.py` | `EFFECTS` | Allow-list mapping a stable logical key (`tv_noise.film_grain.amount`) to its carb setting, its USD attribute, and which prim carries it. |
| `controller.py` | `PostProcessingController` | Loads the active preset, applies it globally or per camera, polls the config for live edits, drives animated effects, and restores the renderer on shutdown. |
| `set_preset.sh` | — | Switches the live preset by rewriting `active:` atomically. Run it from the host while the sim is running. |
| `test_animation.py` | `python3 -m postprocessing.test_animation` | Validates every shipped preset and checks the animator math on a synthetic clock. Stdlib only — no Kit, no numpy, no pytest; run it from `sil/scripts/` on any machine. |
| `test_controller.py` | `python3 -m postprocessing.test_controller` | The state the animation suite cannot reach: `_apply`, `_revert`, `_resolve_groups` and the deferred render-product store, all running for real against fake `pxr` / `omni.usd` modules and a fake stage that counts traversals. Covers what is authored versus deferred, flush scoping, cleanup after a *failing* revert, and the reject paths. Stdlib only; same machine requirements. |

Presets live in [`../../configs/postprocessing.yaml`](../../configs/postprocessing.yaml).

## Using it

The controller is default-ON in `run_actor_sdg.py` whenever
`configs/postprocessing.yaml` exists, starting on the `baseline` preset (no
degradation). To inject an anomaly mid-run, from the host:

```bash
cd closed-loop-testing/isaac-sim/sil/scripts/postprocessing
./set_preset.sh                 # list presets, show the active one
./set_preset.sh low_light       # takes effect within ~0.5 s, no restart
./set_preset.sh baseline        # lift the anomaly
```

Editing `active:` in the YAML by hand does the same thing — the script only
exists so a campaign runner does not have to do string surgery. Both are inert
against a run launched with `--postprocessing-preset`: that run pins its preset
for its whole lifetime and never consults `active:` again.

`postprocessing.yaml` is git-tracked, and `set_preset.sh` rewrites it in place:
the preset is state that outlives the run that set it. Anything scripted has to
put it back, or the next run — which nobody thinks of as an anomaly run — starts
degraded:

```bash
PP=closed-loop-testing/isaac-sim/sil/scripts/postprocessing/set_preset.sh
trap '"$PP" baseline' EXIT
"$PP" low_light
```

A campaign should not depend on that discipline at all. Pass
`--postprocessing-preset baseline` to `run_actor_sdg.py` and the run pins its
own preset, ignoring `active:` entirely — `run_multi.sh` does this. A pinned run
chooses its anomaly at launch (pass the preset you want instead of `baseline`)
rather than mid-run; an unknown name is rejected before Kit boots, not silently
ignored — a mistyped anomaly would otherwise score a clean run as a fault run.

Turn the whole thing off with `--no-postprocessing`, or point it at a different
preset file with `--postprocessing-config`.

## Three scopes, and why the difference matters

A preset with no `cameras:` key writes **carb settings**, which the renderer
applies to the viewport and every camera. That models an environment-wide event
("the lights went out"), and it is what the IRA example demonstrates.

A preset **with** `cameras:` authors **USD attributes** on those cameras
instead, which override the carb value for them alone. That models the case an
anomaly campaign actually cares about: one camera degrades while the others stay
clean, so you can assert the monitor still covers the zone.

A preset with `per_camera:` authors the same USD attributes, but takes **one
settings block per camera** — and, optionally, one `animation` block per camera
— so different cameras can carry different faults in the same apply:

```yaml
mixed_faults:
  per_camera:
    Camera_01:
      settings: {auto_exposure.enabled: false, exposure.iso: 12.0}
    Camera_02:
      settings: {tv_noise.enabled: true, tv_noise.film_grain.enabled: true, tv_noise.film_grain.amount: 0.18}
```

`cameras:` cannot express that, and neither can applying two presets in
sequence. `cameras:` has a single `settings` block, so every camera it lists
gets an identical look; and exactly one preset is active at a time, with
`_apply` reverting before it applies, so `set_preset.sh cam1_dark` followed by
`set_preset.sh cam2_noise` leaves Camera_01 back at baseline. Measured on
`huy-rtx-5070`: after step two, Camera_01 was mean 125.1 / hf 17.4 against a
baseline of 120.0 / 17.0 — fully reverted, by design rather than by leak.

`per_camera:` is mutually exclusive with the **top-level** `cameras:`,
`settings:` and `animation:` keys; declaring both is rejected rather than
silently resolved, because those name a scope too and quietly ignoring them
would apply a preset the file does not describe. `settings:` and `animation:`
move *inside* each `per_camera` entry. Every camera is resolved to a prim path
*before* the revert, so one unknown name rejects the whole edit and leaves the
running preset in place.

### Animation, and animation per camera

`animation:` drives one float effect on a sine, sampled once per rendered frame:

```
value = base * (1 + amplitude_fraction * sin(2*pi*frequency_hz*(t - t0) + radians(phase_deg)))
```

It can sit at preset top level (global, or scoped by `cameras:`) or inside a
`per_camera` entry — the same block, validated by the same rule, so what one
accepts the other accepts. `phase_deg` is optional, defaults to `0.0`, and must
be in `[0, 360)`.

**All animated groups share one clock origin**, latched when the preset is
applied. That is deliberate rather than a limitation: every group of a preset is
applied in the same call, so per-group origins would be identical by
construction and would add bookkeeping without adding expressiveness. `phase_deg`
is what pulls two cameras apart, and unlike a start-time race it is explicit in
the file and reproducible across runs:

```yaml
anim_antiphase:
  per_camera:
    Camera_01:
      settings: {auto_exposure.enabled: false, exposure.iso: 100.0}
      animation: {target: exposure.iso, base: 100.0, amplitude_fraction: 0.35, frequency_hz: 1.0, phase_deg: 0}
    Camera_02:
      settings: {auto_exposure.enabled: false, exposure.iso: 100.0}
      animation: {target: exposure.iso, base: 100.0, amplitude_fraction: 0.35, frequency_hz: 1.0, phase_deg: 180}
```

Camera_01 peaks exactly when Camera_02 troughs. `anim_two_freq` does the same
with 1 Hz against 3 Hz, and `anim_mixed` animates one camera while the other
holds a static fault.

Repeat `auto_exposure.enabled: false` in **every** entry that animates exposure.
There is no top-level `settings:` in a `per_camera` preset to put it in once, and
with auto-exposure left on the histogram claws the image back inside a second.

> **Cost warning.** Animating a **render-product** effect (grain, scanlines,
> vignetting, colour grading) at per-camera scope makes the controller traverse
> the whole stage once per animated scope **per frame**, because render products
> are located by traversal rather than by name. A preset of that shape measured
> **−7.0% FPS** with the robot driving. The controller logs a WARNING naming the
> preset when it loads one; it does **not** reject it, because breathing grain on
> one camera is a real anomaly. Camera-prim effects (`exposure.*`) are free —
> the prim path is already known. See `anim_rp_target`.

The USD attribute names are not a rename of the carb paths, and the split is
easy to get wrong:

| Effect family | Carb (global) | USD (per camera) | Authored on |
|---|---|---|---|
| Auto-exposure | `/rtx/post/histogram/enabled` | `omni:rtx:autoExposure:enabled` | Camera prim |
| Exposure | `/rtx/post/tonemap/filmIso` (`exposureTime` is inert — see below) | `exposure:iso`, `exposure:time` | Camera prim |
| Colour grading | `/rtx/post/colorgrad/gain` | `omni:rtx:post:grade:gain` | **Render product** |
| TV noise | `/rtx/post/tvNoise/grainAmount` | `omni:rtx:post:tvNoise:filmGrain:amount` | **Render product** |

Note `colorgrad` on the carb side versus `grade` on the USD side, and the
flattened `enableFilmGrain` versus the nested `filmGrain:enabled`. `effects.py`
is transcribed from the Kit renderer source (`RtxPostProcessingSettingNames.cpp`,
`generatedSchema.usda`, `TvNoiseSettings.h`) rather than from the docs table, so
it is the place to check a name.

### Per-camera timing caveat

Render products are created by `IsaacCreateRenderProduct` inside the RTSP graph,
which only runs on the **first tick after Play**. So for a per-camera preset:

- exposure settings land immediately (the camera prim already exists);
- grain, scanlines, vignetting and colour grading stay **pending until Play**.

The controller logs the pending keys once and keeps retrying, rather than
failing. If a per-camera noise preset looks like it did nothing, check whether
the timeline is playing.

## Design notes

- **Logical keys, not raw carb paths, in YAML.** One preset then works in either
  scope, and an operator does not have to know that carb and USD disagree on
  spelling. The allow-list also turns a typo into a rejection naming the preset
  and key, instead of silently creating a carb key RTX never reads.
- **A rejected edit keeps the last good preset.** Hot reload means the config is
  edited while a scenario is mid-flight; a half-saved file must not blank the
  degradation being tested. The bad digest is remembered so the warning prints
  once instead of at poll rate. That covers every config error — a parse error,
  an unknown key, a camera not on the stage — because all of them are caught
  before anything is written.
- **A failure *inside* the apply falls back to the baseline instead.** Applying a
  preset starts by reverting the previous one, so once that has begun the last
  good preset no longer exists to keep; what is on screen is half of a preset
  that was just rejected. That case reverts to the pre-controller state, drops
  the animations and the pending render-product writes, and forgets the last good
  digest so putting the file back re-applies it rather than reading as
  "unchanged".
- **All USD authoring goes to the session layer.** A preset never dirties the
  scene `.usd` on disk and never survives a restart, so a degraded run cannot
  leak into the next one through version control.
- **Revert removes the applied API schemas too, not just the attributes.**
  Leaving `OmniRtxPostTvNoiseAPI_1` applied with its attribute removed makes the
  prim resolve the *schema default* (noise disabled), which would then override a
  later global preset for that camera — the opposite of reverting.
- **Auto-exposure is switched off in every exposure preset.** Left on, the
  histogram adapts and claws a darkened image back to normal brightness within
  about a second, so the anomaly quietly disappears.
- **One animated float, sine only — but one per scope.** Enough for mains
  flicker, which is the time-varying anomaly that comes up; anything richer
  belongs in a scenario script rather than a preset file. Each `per_camera`
  entry gets its own sine, so "one flickering camera among steady ones" and
  "two cameras out of step" are both preset-level facts.
- **One shared clock origin plus `phase_deg`, not one origin per camera.** All
  groups are applied in the same call, so per-group origins would be equal by
  construction — they would express nothing a single origin cannot. An explicit
  phase offset does add something, and it is reproducible: the same file gives
  the same relative timing on every run, where a per-group origin would inherit
  whatever the apply order happened to be.
- **Deferred render-product writes are keyed, not queued.** An animated
  render-product effect re-writes the same key every frame; before Play those
  writes are deferred, and as an append-only list they grew one entry per frame
  while the flush traversed the stage once per entry. Keying on
  `(cameras, setting)` keeps the pending set bounded by the preset's size.
  Measured on the shipped `anim_rp_target` (`test_controller.py`): **3 pending
  entries and 4 stage traversals per pre-Play frame** — 3 flush plus 1 animate —
  flat however long Play is delayed. A grain preset always carries
  `tv_noise.enabled` and `film_grain.enabled` next to the animated amount, and
  those defer too; the smaller figure of 1 entry / 2 traversals quoted in the
  original commit describes a preset with the grain pass switched off, which
  renders no grain at all. **After** Play the queue is empty and only the
  animator traverses, which is the one-per-animated-scope cost the load-time
  WARNING quotes.
- **Animate ISO for flicker, not shutter.** `/rtx/post/tonemap/exposureTime` is
  rewritten by the render loop on every frame — it mirrors `cameraShutter`, the
  same quantity expressed as 1/s — so a global write never reaches the
  tonemapper and the flicker silently does nothing. `exposure:time` on a camera
  prim does work, so shutter flicker is available at per-camera scope.
- **Animation frequency is bounded by the frame rate, not by the config.** The
  sine is sampled once per rendered frame, so the achieved rate sets the
  ceiling — and it belongs to the machine, not to the scene. This warehouse
  measured **17.8 fps** on both SIL boxes, which puts the alias threshold at
  **~8.9 Hz**. Use samples per cycle rather than a verdict on any one
  frequency: it is `achieved_fps / frequency_hz`, below 2 the signal folds to a
  different frequency entirely, and a little above 2 it is recoverable but
  coarse (6 Hz at 17.8 fps is 3.0 samples per cycle — the frequency you asked
  for, sampled about as thinly as is still meaningful). Two rates matter and
  they are not the same number: the render loop's, which is what the sine is
  sampled at, and the rate a consumer's capture *delivers*, which is what
  bounds the frequency that consumer can reconstruct. The RTSP side has
  measured well below the render side. Read both on the box you are on.
- **`tv_noise_deterministic` exists for regression runs.** `fixed_time.seed`
  freezes the renderer's time input, the intent being that two runs of one
  scenario produce identical noise; per-frame randomness would otherwise defeat
  diffing. Not yet proven — it needs two runs diffed frame by frame.

## Measured on the SIL warehouse

Numbers below come from single frames pulled off the RTSP streams with `ffmpeg`
while the scenario ran, so they describe what a consumer of the stream receives,
after H.264 encode — not what the renderer intended. `hf` is the standard
deviation of the Laplacian, which rises with grain and scanlines; `c/edge` is
centre brightness over border brightness.

| Preset | Effect on the stream |
|---|---|
| `low_light` | mean 133 → 36, all three cameras |
| `tv_noise` | `hf` 16.8 → 37.8, all three cameras |
| `vignette_heavy` | `c/edge` 0.95 → 4.2 |
| `flicker` | brightness ripple 0.13 → 14.0 over a continuous capture |
| `one_camera_degraded` | Camera_01 mean −74.7 and `hf` +16.8, while Camera and Camera_02 move −2.7 and −0.1 |

The last row is the one worth re-running after any change to scoping: it is the
only check that a per-camera preset does not leak onto the other streams.

`hue_shift` is not in the table — a 3% channel shift needs a per-channel
measurement to show up at all, and it has not been done.

## Not covered

**Blockage** (an opaque object over the lens) is not a post-processing effect.
Heavy vignetting (`vignette_heavy`) degrades the frame border-first the way
grime does, but a real occlusion still needs a prim placed in front of the
camera. There is no Isaac Sim component for it today.

**Sensor-level noise** — photon shot noise, dark-current noise, depth disparity
noise — is a different mechanism, modelled inside the sensor pipeline by
`isaacsim.sensors.experimental.rtx` (experimental) rather than in the post
stack. It is more physically faithful and correspondingly more work to
configure; post-processing is the right tool when the goal is a repeatable
image-level anomaly.
