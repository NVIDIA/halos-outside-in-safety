# VSS Warehouse — 3D (Sparse4D) Profile Overrides (VSS 3.2)

When running the `sil` profile against the **3D (Sparse4D) perception backend**
(multi-view detection + BEV tracking) instead of 2D, apply these overrides **before**
running VSS's `docker compose up`. The `vss-deploy-profile` skill handles the standard
deploy flow — this file specifies only what must be **different** for a 3D SIL run.

The Halos stack itself (safety-core, comm-layer, isaac-sim, forklift-controller) is
**identical** to 2D — 3D changes only the VSS perception side, the calibration, and the
model. Pair this file with `model_r101.md` (the R101 Sparse4D model recipe) and
`calibration_3d.md` (the 3-camera BEV calibration). The Isaac Sim launch is the
**same** as 2D (`--enable-vst`) — see `test_scenario.md`.

> `<wh_ops>` = the VSS 3.2 warehouse-operations directory
> `<vss_repo>/deploy/docker/industry-profiles/warehouse-operations/`. The 3D app files
> below live under `<wh_ops>/warehouse-3d-app/` (the 2D equivalents are under
> `warehouse-2d-app/`).
>
> **Do NOT run `docker compose` from `<wh_ops>/`.** Its `compose.yml` is only an app
> fragment (it `include:`s the 2D / 3D / MV3DT apps but **no infra** — Kafka, Redis,
> `sdr-controller`). The `vss-deploy-profile` skill runs the deploy from the top-level
> `<vss_repo>/deploy/docker/` (`compose.yml`) with
> `--env-file industry-profiles/warehouse-operations/.env`. Selecting the **3D** app
> (`warehouse-3d-app`, which pins the 3D perception + 3D behavior-analytics images) is a
> VSS-side setting — see the `vss-deploy-profile` skill / public VSS Warehouse docs. Edit
> the override files below in place, then let `vss-deploy-profile` bring VSS up.

---

## Why SEI-off + system timestamps (Isaac Sim 6.0)

These overrides **disable SEI extraction** and use **system (wall-clock) timestamps**
(`attach-sys-ts-as-ntp=1`) — the **same** timing strategy as the 2D profile.

**Mechanism**: Isaac Sim 6.0 RTSP *does* embed SEI (which carries a **sim-time** value),
but **PSF currently doesn't support sim time**. Feeding Isaac's SEI sim-time as the frame
timestamp makes PSF drop events as **STALE**, so we disable SEI extraction and tag each
frame with the DeepStream host's **system (wall-clock) arrival time**
(`attach-sys-ts-as-ntp=1`) instead. That keeps perception → PSF decisions flowing and
survives an Isaac relaunch — exactly the same reason as the 2D profile.

> **Note:** older guidance that prescribes SEI-**on** for 3D was validated on Isaac Sim
> **5.1**; on Isaac Sim 6.0 keep SEI-**off** — PSF still doesn't consume the SEI sim-time,
> so system timestamps remain the right choice.

---

## .env Overrides

Set in `<wh_ops>/.env`:

```bash
BP_PROFILE=bp_wh_kafka                                             # MUST include Kafka for PSF
LLM_MODE=none                                                      # Not needed for SIL
VLM_MODE=none                                                      # Not needed for SIL
NUM_STREAMS=3                                                      # Matches 3 Isaac Sim cameras
SAMPLE_VIDEO_DATASET="warehouse-loading-dock-3cams-synthetic-3d"   # 3D synthetic calibration dataset
```

**Why**:
- `bp_wh_kafka` (not `bp_wh`): PSF consumes events from Kafka — without Kafka, no safety decisions.
- `LLM_MODE=none` / `VLM_MODE=none`: SIL only needs perception, saves GPU memory.
- `NUM_STREAMS=3`: Isaac Sim provides exactly 3 camera streams.

> Selecting the 3D perception app (vs 2D) is done on the VSS deploy side (the 3D app pins
> the Sparse4D perception + 3D behavior-analytics images). The exact selector is
> VSS-version-specific — see the `vss-deploy-profile` skill.

---

## DeepStream Config Changes

**Apply BEFORE `docker compose up`.** If deployed first, the 3D perception service builds
its TensorRT engine with the wrong config — a restart costs another ~10-15 min.

File:
```
<wh_ops>/warehouse-3d-app/deepstream/configs/ds-main-config.txt
```

### Disable SEI extraction in `[source-list]`

Keep SEI extraction off for SIL — comment these out (the app config ships them enabled):

```ini
[source-list]
# extract-sei-type5-data=1    # SEI off — use system timestamps (see rationale above)
# sei-uuid=NVDS_CUSTOMMETA    # comment out
num-sources=3                 # = camera count (NUM_STREAMS)
```

### `[streammux]`

```ini
[streammux]
attach-sys-ts-as-ntp=1        # tag each frame with the host arrival (wall-clock) time
# extract-sei-sim-time=1      # comment out — do NOT use Isaac SEI sim-time as the timestamp
# drop-backward-sei=1         # comment out — moot with SEI extraction off (2D style)
sync-inputs-ntp=0             # NTP input-sync stalls on the Isaac RTSP feed -> zero Kafka output
align-first-buffer=0          # do not gate the batch on first-buffer alignment (multi-cam start)
batched-push-timeout=75000    # looser batch timeout — 3D runs at a lower FPS than 2D
low-latency-mode=0            # prioritise throughput over latency for 3D inference
latency=2000                  # tolerate multi-view arrival skew across the 3 cameras
drop-pipeline-eos=1           # a single stream EOS must not tear down the batched pipeline
batch-size=3                  # = camera count (see note below)
```

> **Batch size:** set `batch-size` (and any `max-batch-size`) to the camera count (3). If
> the 3D configurator auto-derives batch sizes from `NUM_STREAMS` at startup, let it — do
> not hand-edit values it manages; only the SEI / timestamp / timeout keys above are set
> here.

**Why these differ from a latency-first default:** 3D (Sparse4D) multi-view inference is
heavier than 2D per-camera detection, so it runs at a **lower frame rate**. The looser
`batched-push-timeout`, `latency`, and `low-latency-mode=0` let the muxer wait long enough
to assemble a complete 3-camera batch instead of pushing partial batches (which produce
gaps / zero Kafka output); `sync-inputs-ntp=0` keeps the muxer from stalling on the Isaac
RTSP feed's timing.

---

## Perception model config — `config.yaml`

File:
```
<wh_ops>/warehouse-3d-app/deepstream/configs/config.yaml
```

```yaml
num_sensors: 3           # must match NUM_STREAMS
num_torch_threads: 8     # CPU threads for the Torch post-process stage
gpu_postprocess: False   # run Sparse4D post-processing on CPU
```

- `num_sensors`: number of cameras in the BEV group (3).
- `num_torch_threads`: sizes the CPU thread pool for post-processing; `8` is a reasonable
  default on a 16+ core host.
- `gpu_postprocess: False`: keep post-processing on CPU so it does not contend with the
  detector for GPU memory / SMs (important when perception and Isaac share a GPU).

The model files this config points at come from `model_r101.md` — the **R101 Sparse4D**
model (deployable ONNX + trainable kmeans anchor, version-matched).

---

## Behavior analytics (3D)

3D uses the **3D behavior-analytics service**, whose image is pinned in the
`warehouse-3d-app` compose (selected on the VSS deploy side). It consumes the 3D
perception output and emits the ROI / tripwire events (`mdx-events`) and per-frame ROI
analysis (`mdx-frames`) the Safety Core reads — the **same event seam** the 2D profile
uses, so no safety-core change is needed for 3D. The ROI / tripwire ids it emits come from
the 3D `calibration.json` (`calibration_3d.md`) and must match the `rule_id`s in
`nvpss.conf`.

---

## VST Config

File: `<wh_ops>/warehouse-3d-app/vst/configs/vst_config.json`

```jsonc
"rtsp_streaming_over_tcp": true,    // ingest Isaac's RTSP over TCP (not UDP)
"use_sensor_ntp_time": false,       // use arrival time, consistent with attach-sys-ts-as-ntp
"always_recording": false,          // recording off — SIL does not need clips
"event_recording": false,           // recording off
"bbox_tolerance_ms": 100            // default 0; widen metadata-to-frame match to reduce bbox flicker
```

- `rtsp_streaming_over_tcp: true`: Isaac's self-hosted RTSP is served over TCP; forcing TCP
  ingest avoids UDP packet loss / jitter on the shared host.
- `use_sensor_ntp_time: false`: pair with `attach-sys-ts-as-ntp=1` so VST and DeepStream
  agree on wall-clock arrival time rather than the (drifting) sim-time.
- Recording off: SIL is a live closed loop, not a capture run — leaving recording on wastes
  disk and I/O.
- `bbox_tolerance_ms: 100`: widens the metadata-to-frame matching window, reducing
  bounding-box flicker at the lower 3D frame rate.

---

## Verification After Deploy

> **DO NOT use `docker logs -f`** — it blocks forever. Poll non-blockingly.

### Ready signal 1: 3D perception producing FPS on all 3 cameras

The Sparse4D TensorRT engine builds on first deploy (~10-15 min). Poll the perception
container until all 3 sources report FPS:

```bash
until [ "$(docker logs vss-rtvi-cv 2>&1 | grep -c 'stream_name Camera')" -ge 3 ]; do
  printf '[%s] 3D perception not ready yet...\n' "$(date +%H:%M:%S)"
  docker logs --tail 3 vss-rtvi-cv 2>&1
  sleep 30
done
echo "3D perception READY:"
docker logs vss-rtvi-cv 2>&1 | grep 'PERF' | tail -3
```

> 3D per-camera FPS is **lower than 2D** — Sparse4D multi-view inference is heavier than
> per-camera 2D detection. What matters is that **all 3** sources report **non-zero** FPS;
> a source stuck at `0.00000` isn't arriving (Isaac not streaming, wrong RTSP URL, or the
> TensorRT engine still building).

### Ready signal 2: Kafka `mdx-events` topic has data

The Safety Core seam is `mdx-events` (tripwire behaviors) + `mdx-frames` (ROI analysis) —
the same as 2D. Gate on `mdx-events`:

```bash
until docker exec kafka kafka-console-consumer \
        --bootstrap-server localhost:9092 \
        --topic mdx-events --max-messages 1 --timeout-ms 30000 \
        > /dev/null 2>&1; do
  printf '[%s] mdx-events empty, retrying...\n' "$(date +%H:%M:%S)"
  sleep 15
done
echo "mdx-events READY"
```

**Do NOT proceed to the Isaac scenario until both ready signals fire.**

---

## What differs from 2D — quick diff

| Setting | 2D | 3D |
|---------|----|----|
| App dir | `warehouse-2d-app/` | `warehouse-3d-app/` |
| Perception | RT-DETR detect + NvDCF track | **Sparse4D** multi-view + BEV track |
| Model | R50 default | **R101** (`model_r101.md`) |
| Calibration | use as-is | **3D dataset** with `group` / `rois` / `tripwires` (`calibration_3d.md`) |
| SEI extraction | off | off (same — PSF doesn't support sim time) |
| `attach-sys-ts-as-ntp` | `1` | `1` |
| `sync-inputs-ntp` | (default) | **`0`** |
| `batched-push-timeout` | (default) | **`75000`** |
| `low-latency-mode` | (default `1`) | **`0`** |
| `config.yaml` | n/a | `num_sensors=3`, `num_torch_threads=8`, `gpu_postprocess: False` |
| VST | `bbox_tolerance_ms=100` | + `rtsp_streaming_over_tcp`, `use_sensor_ntp_time=false`, recording off |
| Isaac launch | `--enable-vst` | **same** — `--enable-vst` (`test_scenario.md`) |
