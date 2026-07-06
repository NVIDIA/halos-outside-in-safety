# VSS Warehouse — 2D Profile Overrides (VSS 3.2)

When deploying VSS Warehouse 3.2 for Halos SIL, apply these overrides **before**
running its `docker compose up`. The `vss-deploy-profile` skill handles the
standard deploy flow — this file specifies only what must be **different** for SIL.

> `<wh_ops>` = the VSS 3.2 warehouse-operations directory
> `<vss_repo>/deploy/docker/industry-profiles/warehouse-operations/` — this is where the
> override files below live (`.env`, the DeepStream config, the VST config).
>
> **Do NOT run `docker compose` from `<wh_ops>/`.** Its `compose.yml` is only an app
> fragment (it `include:`s the 2D/3D/MV3DT apps but **no infra** — Kafka, Redis,
> `sdr-controller`). The `vss-deploy-profile` skill runs the deploy from the top-level
> `<vss_repo>/deploy/docker/` (`compose.yml`) with
> `--env-file industry-profiles/warehouse-operations/.env`. Edit the override files
> below in place, then let `vss-deploy-profile` bring VSS up.

These overrides exist because of **how Isaac Sim's timestamps reach PSF** — not because
SEI is missing. Isaac 6.0 RTSP *does* embed SEI (including a simulation-time value) and
DeepStream extracts it fine; FPS is healthy either way. The real issue is the *timestamp
source*: if DeepStream feeds Isaac's SEI **sim-time** in as the NTP timestamp, the sim
clock lands outside PSF's decision window and events get dropped as **STALE**. So these
overrides force **system timestamps** instead of SEI sim-time. (Verified on a live VSS 3.2
+ Isaac 6.0 SIL run — see the A/B results below.)

---

## .env Overrides

Set in `<wh_ops>/.env` (in 3.2 these are often already the `bp_wh_kafka` defaults —
confirm they are set):

```bash
BP_PROFILE=bp_wh_kafka                                          # MUST include Kafka for PSF
LLM_MODE=none                                                   # Not needed for SIL
VLM_MODE=none                                                   # Not needed for SIL
SAMPLE_VIDEO_DATASET="warehouse-loading-dock-3cams-synthetic"   # SIL synthetic dataset (3 cams)
NUM_STREAMS=3                                                   # Matches 3 Isaac Sim cameras
```

**Why**:
- `bp_wh_kafka` (not `bp_wh`): PSF consumes events from Kafka — without Kafka, no safety decisions
- `LLM_MODE=none` / `VLM_MODE=none`: SIL only needs perception, saves GPU memory
- `NUM_STREAMS=3`: Isaac Sim provides exactly 3 camera streams

---

## DeepStream Config Changes

**Apply BEFORE `docker compose up`.** If deployed first, `vss-rtvi-cv` builds its
TensorRT engine with the wrong config — a restart costs another 15-20 min.

File:
```
<wh_ops>/warehouse-2d-app/deepstream/configs/ds-main-config.txt
```

### Leave SEI type-5 extraction commented in `[source-list]`

Keep these commented (matches NVIDIA's official SIL config). Isaac 6.0 *does* send SEI,
and A/B testing showed extracting the type-5 metadata on its own is harmless (FPS and
STALE unaffected) — but it buys nothing for SIL, so leave it off:

```ini
[source-list]
# extract-sei-type5-data=1
# sei-uuid=NVDS_CUSTOMMETA
```

### Use system timestamp in `[streammux]` — the critical setting

```ini
[streammux]
attach-sys-ts-as-ntp=1       # change from 0 to 1 — tag frames with system (wall-clock) time
# extract-sei-sim-time=1     # comment out — do NOT use Isaac's SEI sim-time as the timestamp
# drop-backward-sei=1        # comment out — only relevant when driving off SEI sim-time
```

**Why (A/B tested on Isaac 6.0):**

| DeepStream config | FPS/cam | PSF STALE | MUTE/UNMUTE |
|-------------------|---------|-----------|-------------|
| system ts (this config: `attach-sys-ts-as-ntp=1`, SEI sim-time off) | ~14, stable | 0 | normal |
| SEI type-5 extract **on**, still system ts | ~14, stable | 0 | normal |
| SEI **sim-time** as NTP (`extract-sei-sim-time=1`, `attach-sys-ts-as-ntp=0`) | ~14, stable | ~100 | degraded / stalls |

The takeaway: **the timestamp source is what matters, not SEI extraction.** Isaac's
sim-time drifts relative to PSF's ~6 s decision window, so using it as the NTP timestamp
makes PSF drop events as STALE and MUTE stops toggling. Extracting SEI does **not** hurt
FPS — the old "SEI → 0 FPS" claim did not reproduce on 6.0. Tag frames with **system
time** and safety decisions flow correctly. (`bbox_tolerance_ms` below handles residual
VST bbox flicker.)

---

## VST Config

File: `<wh_ops>/warehouse-2d-app/vst/configs/vst_config.json`

```json
"bbox_tolerance_ms": 100
```

Default is `0`. Increasing to 100 ms reduces bounding-box flickering by widening
the metadata-to-frame matching tolerance window.

---

## Verification After Deploy

> **DO NOT use `docker logs -f`** — it blocks forever. Poll non-blockingly.

### Ready signal 1: vss-rtvi-cv producing FPS on all 3 cameras

The TensorRT engine builds on first deploy (~10-15 min). Poll until ready:

```bash
until [ "$(docker logs vss-rtvi-cv 2>&1 | grep -c 'stream_name Camera')" -ge 3 ]; do
  printf '[%s] vss-rtvi-cv not ready yet...\n' "$(date +%H:%M:%S)"
  docker logs --tail 3 vss-rtvi-cv 2>&1
  sleep 30
done
echo "vss-rtvi-cv READY:"
docker logs vss-rtvi-cv 2>&1 | grep 'PERF' | tail -3
```

> All 3 sources must show **non-zero**, stable FPS (~14/cam in SIL — the GPU is shared
> with Isaac; NVIDIA's ~30 assumes a dedicated perception GPU). A source stuck at
> `0.00000` means that stream isn't arriving (Isaac not streaming yet, wrong RTSP URL, or
> the TensorRT engine still building) — **not** an SEI issue; SEI extraction does not
> affect FPS on Isaac 6.0.

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
