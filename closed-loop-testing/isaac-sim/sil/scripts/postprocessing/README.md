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

## Two scopes, and why the difference matters

A preset with no `cameras:` key writes **carb settings**, which the renderer
applies to the viewport and every camera. That models an environment-wide event
("the lights went out"), and it is what the IRA example demonstrates.

A preset **with** `cameras:` authors **USD attributes** on those cameras
instead, which override the carb value for them alone. That models the case an
anomaly campaign actually cares about: one camera degrades while the others stay
clean, so you can assert the monitor still covers the zone.

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
  the animation and the pending render-product writes, and forgets the last good
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
- **One animated float, sine only.** Enough for mains flicker, which is the
  time-varying anomaly that comes up; anything richer belongs in a scenario
  script rather than a preset file.
- **Animate ISO for flicker, not shutter.** `/rtx/post/tonemap/exposureTime` is
  rewritten by the render loop on every frame — it mirrors `cameraShutter`, the
  same quantity expressed as 1/s — so a global write never reaches the
  tonemapper and the flicker silently does nothing. `exposure:time` on a camera
  prim does work, so shutter flicker is available at per-camera scope.
- **Animation frequency is bounded by the frame rate, not by the config.** The
  sine is sampled once per rendered frame, so a request above half the achieved
  rate aliases: on the SIL box (8–9 fps for this warehouse) 6 Hz comes back as
  3 Hz. Check the achieved rate before raising `frequency_hz`.
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
