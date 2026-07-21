# VSS Warehouse - HIL Overrides

For the `hil` profile, VSS Warehouse runs on the IGX Thor safety host and ingests the Isaac Sim streams from the remote x86 stimulus host. The VSS-side configuration is the SAME as single-host SIL: apply `vss_2d_overrides.md` (2D, the default) or `vss_3d_overrides.md` (3D) unchanged, plus `halos_thor.md` §2 for the IGX-Thor workarounds. This file adds only what differs for hil.

## 1. Remote stream URLs

The registered sensor URLs must point at the x86 stimulus host, not localhost:

```
rtsp://<x86_host_ip>:8554/camera
rtsp://<x86_host_ip>:8555/camera_01
rtsp://<x86_host_ip>:8556/camera_02
```

The camera names must stay `Camera` / `Camera_01` / `Camera_02`: behavior analytics calibration and the safety-core event matching key on them.

## 2. Sensor registration: exactly one owner

Two things can register the Isaac cameras into VST; pick exactly one per deployment. Never enable both: two writers race on the same sensor names, the adds fail with name collisions, and perception can end up running on the wrong source.

**Configurator file mode** (the flow `halos_hil.md` uses):

- In the warehouse profile `.env`: `SENSOR_INFO_SOURCE=file`
- Fill `<wh_ops>/camera_configs/camera_info.json` with the Isaac cameras:

```json
{
    "sensors": [
      { "camera_name": "Camera",    "rtsp_url": "rtsp://<x86_host_ip>:8554/camera" },
      { "camera_name": "Camera_01", "rtsp_url": "rtsp://<x86_host_ip>:8555/camera_01" },
      { "camera_name": "Camera_02", "rtsp_url": "rtsp://<x86_host_ip>:8556/camera_02" }
    ]
}
```

The VSS configurator reads this file and POSTs each entry to the VST `/sensor/add` API on startup (VST itself never reads the file); the sensor distribution service then provisions the sensors into perception. Launch Isaac WITHOUT `--enable-vst`.

> ⚠ **File mode has no stream-readiness gate.** It registers the URLs as soon as VSS comes up, and VST then DESCRIBEs each of them eagerly. Registering against a cold or not-yet-streaming Isaac is exactly the "no caps" race - see `troubleshooting.md` (RTSP Streams "no caps"). Deploy order matters: bring the Isaac scenario up and confirm the streams deliver frames BEFORE deploying VSS with `camera_info.json` in place.

**Isaac-side registration** (the single-host `sil` flow, run cross-host): launch with `--enable-vst` and point the x86 profile env at the Thor (`VST_BASE_URL` and `PERCEPTION_BASE_URL` in `profiles/hil.env`). This path has the built-in warm-up gate (it registers only once the render is warm, avoiding the cold-DESCRIBE race) but requires VSS to already be up when the scenario starts. Leave `SENSOR_INFO_SOURCE` at its default (not `file`).

## 3. Recording on the Thor VST

Set `always_recording: false` (continuous clips fill the Thor's disk) but keep
`event_recording` **on**. The blueprint-configurator hard-writes `always_recording: true`
on every `up` — set the same `true → false` in the configurator's `blueprint_config.yml`
before `up`, or edit `vst_config.json` after the configurator finishes and
`docker restart vss-vios-streamprocessing`.

## 4. Fresh state on a previously-used Thor

On a Thor that ran VSS before, tear the stack down and wipe the Kafka volumes before redeploying - otherwise the sensor distribution service replays the old sensor history and perception sticks at 0 fps after every restart. Symptom, cause and the exact commands: `troubleshooting.md`, "Perception 0 FPS With Sensors Online (Stale Sensor History Replay)".

## 5. Verify

Use the profile doc's "Verification After Deploy" (`vss_2d_overrides.md` / `vss_3d_overrides.md`) plus the FPS-based poll in `test_scenario.md`. Expect the delivered fps to track the Isaac render rate (well below the nominal stream rate) - that is the simulation, not a network fault.
