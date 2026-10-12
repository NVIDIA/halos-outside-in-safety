# VSS Warehouse 3.3 — 2D Profile Overrides

Deploy VSS Warehouse 3.3 with the VSS repository's `vss-build-vision-ai` skill, warehouse
profile (`skills/vss-build-vision-ai/references/profiles/warehouse.md`), from the GitHub
release `v3.3.0rc0`. That skill owns the deploy flow — its `_builds/<name>/`
build, the env layers, the data-directory gate, readiness and teardown. This file specifies
only what must be **different** for SIL: the answers to give it, and the config files to edit
**before** its `docker compose up`.

> `<vss_repo>` = the VSS checkout at `v3.3.0rc0`; `<vss_data>` = the extracted
> `vss-warehouse-app-data`; `<wh_ops>` = `<vss_repo>/deploy/docker/industry-profiles/warehouse-operations/`,
> where the config files below live (the DeepStream config, the VST config).
>
> **Do NOT run `docker compose` from `<wh_ops>/`.** Its `compose.yml` is only an app
> fragment (it `include:`s the 2D/3D/MV3DT apps but **no infra** — Kafka, Redis,
> `sdr-controller`). Deploy the build's `resolved.yml`, as the skill does.

Isaac 6.1 RTSP *does* embed SEI, but **PSF currently doesn't support sim time** — so these
overrides **disable SEI extraction** and use **system (wall-clock) timestamps**. Feeding
Isaac's SEI sim-time as the frame timestamp makes PSF drop events as STALE; system time
keeps decisions flowing.

---

## Env Overrides

Answer the skill's warehouse questions with mode `2d`, variant `bp_wh_kafka`, size **Minimal**
and dataset `warehouse-loading-dock-3cams-synthetic`, and pick the container image tag
`develop-4c432f24cf39`, the ghcr build of the `v3.3.0rc0` commit. The skill writes them into `_builds/<name>/override.env`, the last env
layer. Against the shipped `overrides.env` that changes:

```bash
VSS_CONTAINER_TAG=develop-4c432f24cf39                          # default: develop-latest — the build of the v3.3.0rc0 commit
FOUNDATION_VARIANT=COMPOSE_PROFILES_WH_KAFKA_2D_MINIMAL         # default: …_WH_2D (with the agent) — the skill expands it into COMPOSE_PROFILES
SAMPLE_VIDEO_DATASET="warehouse-loading-dock-3cams-synthetic"   # default: warehouse-4cams-20mx20m-synthetic — SIL synthetic dataset (3 cams)
NUM_STREAMS=3                                                   # default: 4 — matches 3 Isaac Sim cameras
HARDWARE_PROFILE=L40S                                           # default: H100 — the host's GPU
LLM_MODE=none                                                   # default: local — not needed for SIL
```

`MODE=2d`, `BP_PROFILE=bp_wh_kafka`, `STREAM_TYPE=kafka` and `VLM_MODE=none` already ship.

**Why**:
- `bp_wh_kafka` (not `bp_wh`): PSF consumes events from Kafka — without Kafka, no safety decisions
- `LLM_MODE=none` / `VLM_MODE=none`: SIL only needs perception, saves GPU memory
- `NUM_STREAMS=3`: Isaac Sim provides exactly 3 camera streams; the configurator derives the
  DeepStream batch sizes from it
- Minimal: perception, behavior analytics, VST, Kafka and Redis, without Elasticsearch or
  monitoring
- `VSS_CONTAINER_TAG`: `export` it too before the skill's `config` step — `containers.env`
  expands it before any env file is read
- The perception GPU is `RT_CV_DEVICE_ID` (default `0`); keep Isaac Sim on another GPU
  (`ISAAC_GPU_DEVICE`)

Also needed before the skill's `config` step:
- **App data** — the skill's NGC source `nvstaging/vss-warehouse/vss-warehouse-app-data:v3.3.0-09152026`.
  `ngc registry resource download-version` needs `--org nvstaging --team vss-warehouse`, or it
  stops at `Missing org - If Authenticated, org is also required.` `VSS_DATA_DIR` is the inner
  `vss-warehouse-app-data` directory.
- **`NGC_CLI_API_KEY`** exported: perception downloads its models with it at start, and the
  skill's `validate_resolved_yml.py` rejects a build where it resolved empty.

**A dataset of your own** (e.g. `warehouse-40x20-two-docks-3cams-synthetic`, built per
`test_scenario.md`): put its calibration directory under
`<wh_ops>/warehouse-2d-app/calibration/sample-data/<name>/` and its videos under
`<vss_data>/videos/<name>/`, then set `SAMPLE_VIDEO_DATASET` to `<name>` in the build's
`override.env`.

---

## DeepStream Config Changes

**Apply BEFORE `docker compose up`.** If deployed first, `vss-rtvi-cv` builds its
TensorRT engine with the wrong config — a restart costs another engine build.

File:
```
<wh_ops>/warehouse-2d-app/deepstream/configs/ds-main-config.txt
```

### Disable SEI extraction in `[source-list]`

Keep SEI extraction off for SIL (comment these out; default = enabled):

```ini
[source-list]
# extract-sei-type5-data=1
# sei-uuid=NVDS_CUSTOMMETA
```

### Use system timestamp in `[streammux]`

```ini
[streammux]
attach-sys-ts-as-ntp=1       # change from 0 to 1
# extract-sei-sim-time=1     # comment out
# drop-backward-sei=1        # comment out
```

**Why**: `attach-sys-ts-as-ntp=1` tags each frame with the host's system (wall-clock) time.
Isaac 6.1 RTSP embeds SEI (which carries a sim-time value), but **PSF currently doesn't
support sim time** — using it as the frame timestamp makes PSF drop events as STALE, so
system time keeps the perception → PSF decisions flowing. (`bbox_tolerance_ms` below handles
residual VST bbox flicker.)

---

## VST Config

File: `<wh_ops>/warehouse-2d-app/vst/configs/vst_config.json` (several copies of
`vst_config.json` exist in the deploy tree; edit the one the VST container actually mounts)

```jsonc
"rtsp_streaming_over_tcp": true,    // ingest Isaac's RTSP over TCP (not UDP)
"use_sensor_ntp_time": false,       // use arrival time, consistent with attach-sys-ts-as-ntp
"bbox_tolerance_ms": 100            // default 0; widen metadata-to-frame match to reduce bbox flicker
```

- `rtsp_streaming_over_tcp: true`: Isaac's self-hosted RTSP is served over TCP; forcing TCP
  ingest avoids UDP packet loss / jitter.
- `use_sensor_ntp_time: false`: pair with `attach-sys-ts-as-ntp=1` so VST and DeepStream
  agree on wall-clock arrival time rather than the (drifting) sim-time.
- `bbox_tolerance_ms: 100`: widens the metadata-to-frame matching tolerance window, reducing
  bounding-box flickering.

---

## Verification After Deploy

> **DO NOT use `docker logs -f`** — it blocks forever. Poll non-blockingly.

### Ready signal 1: vss-rtvi-cv producing FPS on all 3 cameras

The TensorRT engine builds on first deploy. Poll until ready:

```bash
# Ready = 3 cameras each reporting a current FPS > 0. Read each camera's latest PERF value;
# a plain log line-count would false-pass on a 0-FPS zombie source.
while :; do
  live=$(docker logs --tail 800 vss-rtvi-cv 2>&1 | grep 'stream_name Camera' | awk '
    { fps=$1+0; for(i=1;i<=NF;i++) if($i=="stream_name") n=$(i+1); last[n]=fps }
    END{ c=0; for(k in last) if(last[k]>0) c++; print c }')
  [ "${live:-0}" -ge 3 ] && break
  printf '[%s] vss-rtvi-cv live cameras=%s/3 (current FPS > 0)...\n' "$(date +%H:%M:%S)" "${live:-0}"
  sleep 30
done
echo "vss-rtvi-cv READY:"
docker logs vss-rtvi-cv 2>&1 | grep 'PERF' | tail -3
```

> All 3 sources must show **non-zero** FPS. If one stays at `0.00000`, that stream
> isn't arriving yet (Isaac not streaming, wrong RTSP URL, or the TensorRT engine still building).

### Ready signal 2: Kafka `mdx-events` topic has data

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

You should see JSON event data with detected objects (Person, Forklift).

**Do NOT proceed to Halos SIL until both ready signals fire.**

---

## Notes

- PSF consumes the `mdx-events` Kafka topic. Perception (`vss-rtvi-cv`) publishes
  raw detections to `mdx-raw`; `vss-behavior-analytics` consumes those and produces
  the `mdx-events` (ROI / tripwire behaviors) that PSF reads.
- The 2D `vst_config.json` also carries a `halo_safety_*` block. `halo_safety_udp_port`
  ships as `-1` (disabled); the **`base` profile** must set it to `12345` (match
  `COMM_UDP_PORT` in `base.env`) to render the safety overlay — see `halos_deploy.md`.
  Leave it `-1` for `sil` (sil renders via comm-layer / ROS, not the overlay).
- VSS runs on a compose bridge network with published ports. What Halos talks to: VST on
  `30888` (`/vst/api/v1/sensor/*`), DeepStream REST on `9000`, Kafka on `9092` (advertised as
  `HOST_IP:9092`), and the containers `vss-rtvi-cv`, `vss-behavior-analytics`,
  `vss-vios-streamprocessing`, `vss-vios-sensor`, `sdr-controller`, `kafka`, `redis`, which
  `restart_isaac.sh` and the ready-signal polls use by name.
- **Clean redeploy** (a change of mode or dataset, or a polluted provisioning chain): stop
  Halos, run the skill's default teardown and its bind-mounted data cleanup
  (`references/teardown.md`), change the build's `override.env`, resolve again, `up`. Kafka,
  Redis and VST data under `<vss_data>/data_log` belong to the containers' users and survive
  `down -v`; the cleanup removes them with `sudo`. The models and the TensorRT engine stay
  under `<vss_data>/models/`.
- The cleanup also restores every file the configurator rewrote — `ds-main-config.txt`,
  `vst_config.json`, the behavior-analytics config — from its oldest backup. Edits made
  before the first `up` are in that backup and survive; re-check them if you applied them
  after a deploy.
