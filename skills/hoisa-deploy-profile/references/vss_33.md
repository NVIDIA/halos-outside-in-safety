# VSS Warehouse 3.3 for Halos (code-freeze drop `dev-26.10.2`)

VSS 3.3 ships no `vss-deploy-profile` skill. Its warehouse profile is deployed from the VSS
repository's `deploy/docker/` with Docker Compose, as in that repo's
`deploy/docker/README.md` → "Warehouse industry profile". This page is that flow with the
Halos settings, for the VSS 3.3.0 code-freeze drop: tag `dev-26.10.2`, images
`nightly-20261010`. The `sil` profile was run with the 2D and the 3D app (8× L40S) on the
earlier `v3.3.0rc0` (ATL, `pxc`, `both`, and the two-bay `atl-dual` run on
`warehouse_40x20`) and on `dev-26.10.2` (ATL with an SRR scenario, 3D app). Between the two
the warehouse configs Halos uses did not change.

> `<vss_repo>` = a checkout of the VSS repository at `dev-26.10.2`;
> `<wh_ops>` = `<vss_repo>/deploy/docker/industry-profiles/warehouse-operations/`;
> `<vss_data>` = the extracted `vss-warehouse-app-data` directory.

---

## 1. Repository, images, app data

```bash
git clone https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization.git <vss_repo>
git -C <vss_repo> checkout dev-26.10.2
```

`containers.env` defaults every VSS image to `ghcr.io/nvidia-ai-blueprints/vss/*:develop-latest`,
which follows the VSS `develop` branch, not the tag. Pin the images of the drop, in the
shell that runs `docker compose`:

```bash
export VSS_CONTAINER_TAG=nightly-20261010       # the images of dev-26.10.2
# optional, the same images from NGC instead of ghcr.io:
# export VSS_CONTAINER_REGISTRY=nvcr.io/nvstaging/vss-core
```

App data (videos and playback; the calibrations live in the repo):

```bash
ngc registry resource download-version \
  "nvstaging/vss-warehouse/vss-warehouse-app-data:v3.3.0-09152026" \
  --org nvstaging --team vss-warehouse
tar -xzf vss-warehouse-app-data_vv3.3.0-09152026/vss-warehouse-app-data.tar.gz -C <parent dir>
mkdir -p <vss_data>/models
chmod -R a+rwX <vss_data>
```

- Without `--org`, the CLI stops at `Missing org - If Authenticated, org is also required.`
- The archive extracts with mode `750`. Kafka runs as a non-root user in its container and
  then fails with `kafka-entrypoint.sh: /tmp/kafka-data/cluster_id: Permission denied`, and
  every service that waits for Kafka fails after it. `chmod -R a+rwX` before the first `up`.

---

## 2. `overrides.env`

3.3 splits the profile in two: `<wh_ops>/.env` keeps the defaults, and the deployment
settings live in `<wh_ops>/overrides.env`, which Compose reads last. Set:

| Key | 3D app | 2D app |
|-----|--------|--------|
| `MODE` | `3d` | `2d` |
| `BP_PROFILE` | `bp_wh_kafka` | `bp_wh_kafka` |
| `STREAM_TYPE` | `kafka` | `kafka` |
| `SAMPLE_VIDEO_DATASET` | `"warehouse-loading-dock-3cams-synthetic"` | same (or a dataset you added, §3) |
| `DATASET_TYPE` | `synthetic` | not read |
| `NUM_STREAMS` | `3` | `3` |
| `HARDWARE_PROFILE` | the host's GPU, e.g. `L40S` (default `H100`) | same |
| `LLM_MODE` / `VLM_MODE` / `LLM_NAME_SLUG` | `none` / `none` / `none` | same |
| `COMPOSE_PROFILES` | `${COMPOSE_PROFILES_WH_KAFKA_3D_MINIMAL}` | `${COMPOSE_PROFILES_WH_KAFKA_2D_MINIMAL}` |
| `VSS_APPS_DIR` | `<vss_repo>/deploy/docker` | same |
| `VSS_DATA_DIR` | `<vss_data>` | same |
| `HOST_IP` | the host's IP (also `HOST_IP` in the Halos profile) | same |

- `COMPOSE_PROFILES` does not follow `MODE`: it ships as the full 2D profile with the agent,
  Elasticsearch and monitoring. The `_MINIMAL` lists keep perception, behavior analytics,
  VST, Kafka and Redis. `MINIMAL_PROFILE` from 3.2.1 is gone.
- `NGC_CLI_API_KEY` is read from the environment (`overrides.env` defaults it to
  `${NGC_CLI_API_KEY:-}`): perception downloads its models with it when it starts. Never
  write the key into the file.
- The 3.2.1 dataset name `warehouse-loading-dock-3cams-synthetic-3d` does not exist in 3.3:
  the 3D app ships its BEV calibration under the plain name.
- The perception GPU is `RT_CV_DEVICE_ID` in `.env` (default `0`). Keep Isaac Sim on another
  GPU (`ISAAC_GPU_DEVICE`).

---

## 3. Config overrides

The DeepStream, VST and behavior-analytics keys and files are the same as in 3.2.1, and 3.3
still ships SEI extraction on. Apply `vss_2d_overrides.md` or `vss_3d_overrides.md`
before the first `up`. What changed:

- **Batch sizes.** The configurator derives `max-batch-size`, `batch-size`, `num_sensors`
  and the preprocess `network-input-shape` from `NUM_STREAMS`. Leave them alone.
- **Sparse4D model (3D).** No model step: perception downloads it into
  `<vss_data>/models/sparse4d/` before building the engine (`warehouse-3d-app/models-download.json`,
  rewritten by the configurator from `DATASET_TYPE`). `synthetic` gives RN50 v2.3
  (`nvidia/tao/sparse4d_rn50:deployable_v2.3`; `v3.3.0rc0` pulled it from `nvstaging`);
  `real` gives RN50 v3.0. The engine build
  took about 5 min on an L40S. `model_r101.md` applies to 3.2.1 only.
- **3D calibration.** `warehouse-3d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic`
  already carries the BEV group `bev-sensor-1` and the top-level `roi-id-1` / `tripwire-id-1`,
  identical to what `calibration_3d.md` builds. That page is only needed for another scene.
- **A dataset of your own** (e.g. `warehouse-40x20-two-docks-3cams-synthetic`, built per
  `test_scenario.md`): put its calibration directory under
  `<wh_ops>/warehouse-2d-app/calibration/sample-data/<name>/` and its videos under
  `<vss_data>/videos/<name>/`, then set `SAMPLE_VIDEO_DATASET` to `<name>`. The 2D
  calibration format did not change.
- **NVStreamer config.** The per-app `nvstreamer/configs/vst-config.json` files are gone;
  there is one under `<vss_repo>/deploy/docker/services/nvstreamer/configs/`. Halos does not
  need to change it.

---

## 4. Up, down, clean redeploy

```bash
cd <vss_repo>/deploy/docker
ENV_FILES=(--env-file containers.env
           --env-file industry-profiles/warehouse-operations/.env
           --env-file industry-profiles/warehouse-operations/overrides.env)
docker compose -f compose.yml "${ENV_FILES[@]}" up --detach --force-recreate --build
docker compose -f compose.yml "${ENV_FILES[@]}" down -v --remove-orphans
```

Keep the env files in this order: `overrides.env` must come last to win.

**Clean redeploy** (a change of `MODE` or dataset, or a polluted provisioning chain): stop
Halos, `down -v` (drops the sensor database volume, now `vss_vios_pg_data`), empty
`<vss_data>/data_log/redis/data`, edit `overrides.env`, `up`. The model files and the
TensorRT engine are kept under `<vss_data>/models/`.

**Networking.** 3.3 runs on a Compose bridge network with published ports; 3.2.1 used
host networking. What Halos talks to is unchanged: VST on `30888` (`/vst/api/v1/sensor/*`),
DeepStream REST on `9000`, Kafka on `9092` (advertised as `HOST_IP:9092`). So are the
container names (`vss-rtvi-cv`, `vss-behavior-analytics`, `vss-vios-streamprocessing`,
`vss-vios-sensor`, `sdr-controller`, `kafka`, `redis`), which `restart_isaac.sh` and the
ready-signal polls rely on.

---

## 5. Ready signals

The same as 3.2.1: the FPS-based perception poll and the `mdx-events` poll in
`vss_2d_overrides.md` / `vss_3d_overrides.md` → "Verification After Deploy". Topics are
unchanged: 3D perception publishes `mdx-bev`, 2D `mdx-raw`, and behavior analytics
`mdx-events` / `mdx-frames`.

After Isaac registers its cameras, check that DeepStream holds the three uuids VST lists as
online (`test_scenario.md`). If one camera is missing or stuck at 0 FPS, use
`troubleshooting.md` → "Provisioning Chain Polluted": on 3.3 the round of `FLUSHALL`,
restarting `sdr-controller` and `vss-rtvi-cv`, and one registration brought DeepStream
from 2/3 back to 3/3.
