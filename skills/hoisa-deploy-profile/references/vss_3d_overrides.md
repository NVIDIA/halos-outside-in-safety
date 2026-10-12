# VSS Warehouse 3.3 — 3D (Sparse4D) Profile Overrides

When running the `sil` profile against the **3D (Sparse4D) perception backend**
(multi-view detection + BEV tracking) instead of 2D, deploy VSS the same way as for 2D — the
`vss-build-vision-ai` skill, warehouse profile, at `v3.3.0rc0` (`vss_2d_overrides.md`) — and
apply these overrides **before** its `docker compose up`. This file specifies only what must
be **different** for a 3D SIL run.

The Halos stack itself (safety-core, comm-layer, isaac-sim, forklift-controller) is
**identical** to 2D — 3D changes only the VSS perception side — except that PSF runs
`deploymentMode = 3D` for **every** `PSF_APP` on this feed (`halos_deploy.md` → "Safety
app"). The Sparse4D model and the loading-dock BEV calibration ship with VSS. The Isaac
Sim launch is the **same** as 2D (`--enable-vst`) — see `test_scenario.md`.

> `<wh_ops>` = `<vss_repo>/deploy/docker/industry-profiles/warehouse-operations/`. The 3D app
> files below live under `<wh_ops>/warehouse-3d-app/` (the 2D equivalents are under
> `warehouse-2d-app/`).
>
> **Do NOT run `docker compose` from `<wh_ops>/`.** Its `compose.yml` is only an app
> fragment (it `include:`s the 2D / 3D / MV3DT apps but **no infra** — Kafka, Redis,
> `sdr-controller`). Deploy the build's `resolved.yml`, as the skill does.

---

## Why SEI-off + system timestamps (Isaac Sim 6.1)

These overrides **disable SEI extraction** and use **system (wall-clock) timestamps**
(`attach-sys-ts-as-ntp=1`) — the **same** timing strategy as the 2D profile.

**Mechanism**: Isaac Sim 6.1 RTSP *does* embed SEI (which carries a **sim-time** value),
but **PSF currently doesn't support sim time**. Feeding Isaac's SEI sim-time as the frame
timestamp makes PSF drop events as **STALE**, so we disable SEI extraction and tag each
frame with the DeepStream host's **system (wall-clock) arrival time**
(`attach-sys-ts-as-ntp=1`) instead. That keeps perception → PSF decisions flowing and
survives an Isaac relaunch — exactly the same reason as the 2D profile.

> **Note:** older guidance that prescribes SEI-**on** for 3D was validated on Isaac Sim
> **5.1**; on Isaac Sim 6.1 keep SEI-**off** — PSF still doesn't consume the SEI sim-time,
> so system timestamps remain the right choice.

---

## Env Overrides

The 2D answers of `vss_2d_overrides.md` → "Env Overrides", with mode `3d`:

```bash
MODE=3d                                                         # default: 2d — warehouse-3d-app (Sparse4D + 3D behavior analytics)
FOUNDATION_VARIANT=COMPOSE_PROFILES_WH_KAFKA_3D_MINIMAL         # the skill expands it into COMPOSE_PROFILES
```

`DATASET_TYPE=synthetic` already ships, and the skill derives it from the dataset.
`SAMPLE_VIDEO_DATASET` stays `"warehouse-loading-dock-3cams-synthetic"`: the 3D app ships its
BEV calibration under the same dataset name as the 2D one.

The skill's `validate_warehouse_env.py` warns `MODE=3d on a _MINIMAL list deploys no
Elasticsearch, so the mdx-bev index is never persisted`. Expected for SIL: PSF and SRR read
`mdx-bev` from Kafka.

**Model.** There is no model step: perception downloads Sparse4D into
`<vss_data>/models/sparse4d/` before building the engine. `DATASET_TYPE=synthetic` gives RN50
v2.3 (`nvstaging/tao/sparse4d_rn50:deployable_v2.3`, so the NGC key needs `nvstaging` access),
`real` RN50 v3.0. The engine build took
about 5 min on an L40S.

**Calibration.** `<wh_ops>/warehouse-3d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic`
already carries the BEV group `bev-sensor-1` and the top-level `roi-id-1` / `tripwire-id-1`.
`calibration_3d.md` is only needed for a scene of your own (e.g. `warehouse_40x20`).

---

## DeepStream Config Changes

**Apply BEFORE `docker compose up`.** If deployed first, the 3D perception service builds
its TensorRT engine with the wrong config — a restart costs another engine build.

File:
```
<wh_ops>/warehouse-3d-app/deepstream/configs/ds-main-config.txt
```

### Disable SEI extraction in `[source-list]`

Keep SEI extraction off for SIL — comment these out (the app config ships them enabled):

```ini
[source-list]
# extract-sei-type5-data=1    # SEI off for SIL — use system timestamps (see rationale above)
# sei-uuid=NVDS_CUSTOMMETA    # comment out
low-latency-mode=0            # prioritise throughput over latency for 3D multi-view inference
```

### `[streammux]`

```ini
[streammux]
attach-sys-ts-as-ntp=1        # timestamp each frame with host arrival (wall-clock) time
# extract-sei-sim-time=1      # SEI off — do not use Isaac SEI sim-time as the timestamp
# drop-backward-sei=1         # moot with SEI extraction off
sync-inputs-ntp=0             # avoid NTP input-sync stalling on the Isaac RTSP feed (zero Kafka output)
align-first-buffer=0          # do not gate the batch on first-buffer alignment (multi-cam start)
batched-push-timeout=75000    # looser batch timeout — 3D runs at a lower FPS than 2D
drop-pipeline-eos=1           # one stream's EOS must not tear down the batched pipeline
```

> **Section placement matters** — DeepStream reads each key only from its own section:
> `low-latency-mode` belongs to `[source-list]`, the timing keys to `[streammux]`. Leave
> `num-sources`, `latency`, and `batch-size` to the shipped config — the configurator derives
> them from `NUM_STREAMS` / `max-batch-size` at startup.

**Why these differ from a latency-first default:** 3D (Sparse4D) multi-view inference is
heavier than 2D per-camera detection, so it runs at a **lower frame rate**. The looser
`batched-push-timeout` and `low-latency-mode=0` let the muxer wait long enough
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

The configurator sets `num_sensors` from `NUM_STREAMS` and the other two keys ship with
these values — nothing to edit here.

---

## Behavior analytics (3D)

3D uses the **3D behavior-analytics service**, whose image is pinned in the
`warehouse-3d-app` compose (selected on the VSS deploy side). It consumes the 3D
perception output and emits the ROI / tripwire events (`mdx-events`) and per-frame ROI
analysis (`mdx-frames`) the Safety Core reads — the **same event seam** the 2D profile
uses, so no safety-core change is needed for 3D. The ROI / tripwire ids it emits come from
the 3D `calibration.json` of the dataset and must match the `rule_id`s in `nvpss.conf`.

**Proximity (`PSF_APP=pxc` / `both`)** needs one change. The shipped 3D
config pairs only the humanoid robots with `Person`, within 2 m, so a forklift never
produces a proximity event and the HOISA tiers (REDUCE up to 3.5 m) would be cut off.

File: `<wh_ops>/warehouse-3d-app/vss-behavior-analytics/configs/vss-behavior-analytics-config.json`

```jsonc
"name": "proximityDetectionThreshold",     "value": "4"    // shipped "2"; must be >= every
                                                           // distance_threshold_meters in the
                                                           // proximity mapping (3.5 m)
"name": "proximityDetectionCenterClasses", "value": "[\"Forklift\",\"Fourier_GR1_T2_Humanoid\",\"Agility_Digit_Humanoid\"]"
                                                           // shipped without "Forklift"
```

---

## VST Config

File: `<wh_ops>/warehouse-3d-app/vst/configs/vst_config.json`

```jsonc
"rtsp_streaming_over_tcp": true,    // ingest Isaac's RTSP over TCP (not UDP)
"use_sensor_ntp_time": false,       // use arrival time, consistent with attach-sys-ts-as-ntp
"bbox_tolerance_ms": 100            // default 0; widen metadata-to-frame match to reduce bbox flicker
```

- `rtsp_streaming_over_tcp: true`: Isaac's self-hosted RTSP is served over TCP; forcing TCP
  ingest avoids UDP packet loss / jitter on the shared host.
- `use_sensor_ntp_time: false`: pair with `attach-sys-ts-as-ntp=1` so VST and DeepStream
  agree on wall-clock arrival time rather than the (drifting) sim-time.
- `bbox_tolerance_ms: 100`: widens the metadata-to-frame matching window, reducing
  bounding-box flicker at the lower 3D frame rate.

---

## Verification After Deploy

> **DO NOT use `docker logs -f`** — it blocks forever. Poll non-blockingly.

### Ready signal 1: 3D perception producing FPS on all 3 cameras

The Sparse4D TensorRT engine builds on first deploy (about 5 min on an L40S). Poll the perception
container until all 3 sources report FPS:

```bash
# Ready = 3 cameras each reporting a current FPS > 0. Read each camera's latest PERF value;
# a plain log line-count would false-pass on a 0-FPS zombie source.
while :; do
  live=$(docker logs --tail 800 vss-rtvi-cv 2>&1 | grep 'stream_name Camera' | awk '
    { fps=$1+0; for(i=1;i<=NF;i++) if($i=="stream_name") n=$(i+1); last[n]=fps }
    END{ c=0; for(k in last) if(last[k]>0) c++; print c }')
  [ "${live:-0}" -ge 3 ] && break
  printf '[%s] 3D perception live cameras=%s/3 (current FPS > 0)...\n' "$(date +%H:%M:%S)" "${live:-0}"
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
| Env | mode `2d`, `…_KAFKA_2D_MINIMAL` | mode `3d`, `…_KAFKA_3D_MINIMAL` |
| Model | shipped | Sparse4D RN50 v2.3, downloaded by perception at start |
| Calibration | use as-is | BEV group `bev-sensor-1` + top-level ROI / tripwire — shipped; `calibration_3d.md` for another scene |
| Behavior analytics | as shipped | `proximityDetectionThreshold` `4`, `Forklift` in `proximityDetectionCenterClasses` (`pxc` / `both`) |
| PSF (Halos profile) | 2D `nvpss.conf` | `PSF_NVPSS_CONFIG_SRC` + `PSF_SENSOR_PIPELINES_CONFIG_SRC` set, for every `PSF_APP` |
| SEI extraction | off | off (same — PSF doesn't support sim time) |
| `attach-sys-ts-as-ntp` | `1` | `1` |
| `sync-inputs-ntp` | (default) | **`0`** |
| `batched-push-timeout` | (default) | **`75000`** |
| `low-latency-mode` | (default `1`) | **`0`** |
| `config.yaml` | n/a | `num_sensors=3`, `num_torch_threads=8`, `gpu_postprocess: False` |
| VST | `bbox_tolerance_ms=100`, `rtsp_streaming_over_tcp`, `use_sensor_ntp_time=false` | **same** |
| Isaac launch | `--enable-vst` | **same** — `--enable-vst` (`test_scenario.md`) |
