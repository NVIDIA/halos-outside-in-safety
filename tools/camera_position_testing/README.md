# Testing a camera placement with SRR

How to take a proposed camera position, prove it is worth a run, wire it into the SIL + VSS
stack, score it with the Regression Testing Reporter, and compare it against other
positions. Written for someone who has the repo and has never done this before.

Everything here is per-position: run §2 and §3 once for each candidate, then §4 compares
them. §5 covers the variant where the tripwire and work zone move instead of the cameras.
§7 is what we measured on the gate-mount question; §8 lists the pieces that are not yet
committed, which you need before this is reproducible from a clone alone.

The one rule to carry through all of it: **a placement lives in two files at once.** Isaac
Sim renders from `cameras.yaml`; VSS back-projects detections through the extrinsics in
`calibration.json`. Change one and not the other and nothing errors — the stack stays
healthy and reports wrong world positions, which reads as a perception regression rather
than a config mistake. Every step below either changes both or checks that both agree.

---

## 1. What you need

| Piece | Where |
|---|---|
| Deployment procedure (SIL + VSS, cleanup, ready signals) | `skills/hoisa-deploy-profile/SKILL.md` |
| Running SRR (phases, gates, report reading) | `skills/hoisa-generate-regression-report/SKILL.md` + `references/01…06` |
| Placement toolkit | `tools/camera_position_testing/` — this directory; §9 lists every file |
| Scenarios (behaviour trees, navmesh, Isaac utilities) | `closed-loop-testing/regression-reporter/scenarios/`, synced into the SIL config dir by `halos-integration/sync_to_halos.sh` |
| GPU | Isaac Sim (>20 GB) and VSS perception (24 GB+) must be up together; a 16 GB card cannot host both |

Read the two skills first. This document does not repeat how to deploy or how SRR works
internally; it only covers what is specific to changing where the cameras are.

Paths used below:

```bash
HOISA=<clone>/halos-outside-in-safety
VSS=<clone>/video-search-and-summarization
CFG=$HOISA/closed-loop-testing/isaac-sim/sil/configs
GEN=$HOISA/tools/camera_position_testing
PROFILE=$VSS/deploy/docker/industry-profiles/warehouse-operations
CAL_LIVE=$PROFILE/warehouse-2d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic/calibration.json
```

---

## 2. Create and qualify one position

Budget: a few minutes for steps 2.1–2.2, ~10 minutes for 2.3, and only then ~1 h 30 of run
time in §3. Most candidates die at 2.2 or 2.3, which is the point of doing them first.

### 2.1 Author the pose

Give the mount points and the point on the floor you want the cameras to look at; let the
tool compute the rotations. Hand-edited Euler angles are where sign errors live.

```bash
cd $GEN
python3 aim_cameras.py --template $CFG/cameras.yaml \
  --mount Camera=9.574,-10.0,3.2 \
  --mount Camera_01=9.574,-20.2,3.2 \
  --drop Camera_02 \
  --aim 7.226,-15.107 --focal 8.0 \
  --label "gate line, mounts clear of the opening, z=3.2" \
  --output cameras-<name>.yaml
```

- `--mount NAME=x,y,z` once per camera, in world coordinates. Names must be the ones the
  rest of the stack knows (`Camera`, `Camera_01`, …).
- `--drop NAME` removes a camera the template has and this candidate does not.
- `--aim x,y` is a floor point both cameras look at. Aiming both at the zone centroid beat
  every diagonal variant we tried: diagonal aiming left 31.9 % of the zone visible to only
  one camera, against 2.7 % for centre aiming.
- `--aim-for NAME=x,y` instead if each camera needs its own target.
- `--focal` in mm; 8.0 mm is 105.3° horizontal. Shorter sees more and resolves less.

The output carries the template's `scene:` key over, so Isaac's `camera_loader.assert_scene_matches` accepts it.

### 2.2 Screen it offline

```bash
python3 generate_calibration.py --cameras-config cameras-<name>.yaml \
  --template "$CAL_LIVE" --drop-missing-sensors --output cal-<name>.json
python3 screen_fov.py --calibration cal-<name>.json --map-step 1.0
```

`screen_fov.py` reports, per camera and combined:

- how much of the zone has a 1.8 m target's **feet** and **whole body** in frame;
- how much of it **only one camera** can see — one occluder there and coverage is zero;
- the **view angle at each station along the tripwire**, which is what decides whether a
  crossing has enough track on both sides of the line to be given a direction.

What we used as accept/reject: head-level containment must be 100 %, sole coverage (zone
visible to one camera only) under ~3 %, and shallower view angle at the wire preferred. A
third camera mid-gate was rejected here because it moved sole coverage only from 2.7 % to
2.2 %.

### 2.3 Render it — do not skip this

The screener models a clear line of sight to an empty floor. It cannot see pipework, cages,
or the panel above the dock. In this scene it ranked `y = -9.5` best of everything tried
while the render showed wall structures covering 5.0 % of the zone at foot level, against
0.7 % at `y = -10.0`. **Every offline number is an upper bound.**

Install the candidate yaml and grab one frame per camera off Isaac's own RTSP. The ports and
mount paths come from the yaml, so a different camera count needs no arguments:

```bash
./capture_camera_shots.sh cameras-<name>.yaml shots/
```

Do **not** poll the RTSP mounts while Isaac warms up. Isaac 6.0 answers DESCRIBE before the
encoder has produced a frame; a client that arrives in that window gets "stream has no caps"
and then keeps the media wedged by re-asking, so the readiness probe is what prevents
readiness. Wait silently, then ask once.

In the frames, check: is the whole work zone in view, is the tripwire visible end to end, is
anything (structure, cage, overhead panel) covering part of the zone, and can you see far
enough past the wire on both sides for a crossing to be resolved. Two limits found this way
in the 20x20 scene: mount height at the gate line caps at **3.7 m**, and mounts should clear
the gate opening in `y` (`-10.0 / -20.2`, not `-10.6 / -19.6`) — which halves the number of
wire stations left on a single camera, 4 of 9 down to 2 of 9.

### 2.4 Derive the calibration for real

Once the pose survives the render, regenerate the calibration with the field-of-view
polygons rebuilt, because those polygons are the visible floor footprint and they move with
the camera:

```bash
python3 generate_calibration.py --cameras-config cameras-<name>.yaml \
  --template "$CAL_LIVE" --drop-missing-sensors \
  --fov-polygon frustum --vss-root "$VSS" \
  --output cal-<name>.json
```

`--verify` is a separate mode, not an extra check on a write: it re-derives the *template's*
own matrices from the yaml and reports the residuals, which is how you confirm the intrinsic
and extrinsic conventions still match the shipped calibration. Run it on its own.

`--fov-polygon frustum` needs numpy and shapely. A bare SIL host usually lacks shapely; the
`isaac-sim` container has numpy and takes `/isaac-sim/python.sh -m pip install shapely`
(container-local, gone on recreate) — copy the generator, the yaml, the template and the VSS
`libs/analytics/spatialai-data-utils` tree in and run it there. `--fov-polygon keep` is the
fallback and means accepting stale polygons. The generator intersects the frustum with the
ground plane and knows nothing about walls, so a polygon can run out to `y = -41 m` in a
building that ends at `-20`; clip to the footprint if anything downstream filters on it.

### 2.5 Install both halves

```bash
cp cameras-<name>.yaml $CFG/cameras.yaml     # Isaac renders from this
cp cal-<name>.json "$CAL_LIVE"               # VSS scores against this
```

Use `cp`, never `mv`: `calibration.json` is bind-mounted **as a file** into
`vss-behavior-analytics` and `srr`, so replacing the inode leaves both containers reading the
old content while the file on the host looks correct.

**If the camera count changed**, five places must agree or the stack waits forever for a
stream that never arrives:

| Place | What |
|---|---|
| `$PROFILE/.env` → `NUM_STREAMS` | DeepStream source slots |
| `closed-loop-testing/safety-core/configs/sensor_config.conf` | one row per camera, with its RTSP URL |
| `SENSORS` env for `srr.tw_split` / the run wrapper | default is `Camera,Camera_01,Camera_02` |
| `MOUNTS` env for the run wrapper | Isaac RTSP `port/path` pairs, same order as `SENSORS` |
| VST registrations | re-registered per scenario by `vst_sensor_manager.py`; stale entries occupy slots |

Then reprovision, because behaviour analytics reads the calibration once at startup and
`sdr-controller` replays a stale sensor-event stream if redis is not cleared:

```bash
docker exec redis redis-cli FLUSHALL
docker restart vss-behavior-analytics sdr-controller vss-rtvi-cv
```

### 2.6 Verify before you spend a run

```bash
# 1. the two halves agree — run_srr_sweep.sh does this and refuses to start if not
# 2. the zone/wire SRR will score against is the one you intend
docker exec srr python3 -c "
from pathlib import Path
from srr.aggregator import load_roi
roi, tw = load_roi(Path('/app/calibration.json'))
print(tw, roi.bounds, roi.area)"
# 3. DeepStream gives every camera its own slot, and every camera has non-zero fps
docker exec vss-rtvi-cv curl -s http://localhost:9000/api/v1/stream/get-stream-info
```

Check 3 matters more than it looks: two slots bound to the **same** camera passes every
count, name and fps check while multi-view fusion quietly consumes one view twice. If you
see `0=Camera_01 1=Camera_01 2=Camera`, the redis flush did not take.

---

## 3. Run SRR for that position

Either drive it with the `hoisa-generate-regression-report` skill, or run the wrapper
directly for an unattended sweep:

```bash
bash $GEN/run_srr_sweep.sh <label>
```

Five scenarios at the `FULL_PRESET` durations — `in-roi:300 psf-edge:300 psf-clear:300
balanced:600 fast:1200` — plus ~2 min of Isaac boot and ~5 min of analysis each; about
1 h 30 per position, so four positions is a working day. Per scenario the wrapper selects
the behaviour trees, sets the simulation duration, restarts `safety-core` and `srr`, launches
Isaac, waits for perception, gates on real detections, records, and aggregates into
`srr-service/runs/multi-test-<timestamp>-<label>/`.

Non-default camera counts pass through the environment:

```bash
SENSORS=Camera,Camera_01 MOUNTS="8554/camera 8555/camera_01" \
  bash $GEN/run_srr_sweep.sh <label>
```

Two things to watch in the log:

- `slots: 0=Camera 1=Camera_01` — the DeepStream source-to-camera mapping, per §2.6.
- `T+60/1200s fps: …` — per-camera frame rate **during** the take. A camera can die
  mid-recording: one 1200 s scenario here kept recording for 16 minutes after a camera hit
  0 fps, and the clips looked like a placement that misses half its crossings rather than
  the half-blind rig it actually was.
- **Safety Core still deciding.** It can stop publishing mute/unmute decisions mid-take
  while everything else stays healthy — events keep arriving, the PSS daemon keeps
  heartbeating, `/safety/is_muted` keeps answering — and the take then scores an undecided
  system against ground truth that expects mute. In a healthy take decisions arrive at
  ~30 Hz, so this is unambiguous:

```bash
docker logs --tail 400 safety-core | grep 'Sending decision command' | tail -1
```

The wrapper aborts and retries the take when a camera reports no frames, or when Safety Core
publishes no decision for two minutes, on two consecutive samples. If you drive SRR by hand,
sample both yourself — neither failure shows up in the report as anything but a bad
placement; `./watch_safety_core.sh` does the second one for a run already in flight.

Copy the `cameras.yaml` and `calibration.json` you used into the run directory. The report
records metrics but not the geometry they were measured against.

Clips for the debug viewer, after all positions are done (~50 min of ffmpeg each, and
nothing in the comparison depends on them):

```bash
docker exec srr python3 -m srr.clip_logs --runs-dir /app/runs/<run>
$HOISA/tools/srr-debug-viewer/view.sh        # http://<host>:8765/?run=data%2F<run>
```

Two VST behaviours to expect: a recording chunk reaches postgres only when the chunk closes,
so asking for video too early returns 404 for every scene even though the footage lands
moments later; and one camera name maps to several VST stream ids once sensors have been
re-registered, where only the newest holds live footage (`vst_video list`, then
`split-run --stream-id <id>`).

---

## 4. Compare positions

`runs/<run>/summary.md` is one row of the comparison. `compare_runs.py` reads several of them
into one table and adds the column the report does not have — how much of each scenario Safety
Core spent only heartbeating, which is what invalidates a row:

```bash
python3 compare_runs.py <run-dir> <run-dir> … --output-prefix comparison
```

Definitions that matter:

- **PASS / NEAR / FAIL** — per clip, one clip being one forklift trailer visit.
- **Mute correct %** — of the frames ground truth says *should* be muted, the fraction the
  system muted (1 − under-mute). **Unmute correct %** is the converse. These are frame
  counts, so they are the most sensitive numbers in the report and the least forgiving of a
  single bad scenario.
- **ROI entry / TW IN / TW OUT** — per event: for each ground-truth event, did behaviour
  analytics emit a matching one within ±3 s. **TW OUT also requires `direction = Left`**, so
  an event that arrives on time but carries no direction field is scored as a miss. A low
  TW OUT therefore means "detected *and* direction-resolved", not "detected".
- **Worst mute / unmute lag** — the tail matters more than the median for a safety claim.

The metrics that actually separate placements are TW IN, TW OUT and the lag tails. PASS rate
moves surprisingly little between placements, because most of a clip is the easy part.

---

## 5. Variant — move the tripwire and zone instead of the cameras

The customer constraint may be that cameras stay on the wall. Then the knob is `D`, the
distance the tripwire and work zone are pushed away from the wall. Cameras are authored once
(§2) and never touched again; per row you change three things.

**Geometry.** The generator moves the wire along the outward normal of its own `direction`
arrow, so the sign cannot be got wrong. Do not hand-edit coordinates:

```bash
for D in 0 1 2 3; do
  python3 generate_calibration.py --cameras-config cameras-<name>.yaml \
    --template cal-<name>.json --tripwire-pushout $D --roi-follow shift \
    --output cal-D$D.json
done
```

`--roi-follow` decides what the zone does when the wire moves:

| mode | effect | when |
|---|---|---|
| `none` | zone unchanged, wire detaches from its edge | zone is a fixed area the customer defined |
| `edge` | wall-side edge follows; zone **shrinks** by `D × wire length` | avoid: at `D=3` the zone here loses 64 % of its area (36.3 → 13.1 m²) and rows stop being comparable |
| `shift` | whole zone translates, area preserved | what we use |

**Waypoints.** The walk targets in the behaviour trees (`srr_<scenario>_char<i>.bt.json`,
`MoveTo` nodes) are absolute world coordinates. Leave them and the zone slides out from
under them — with the zone at `D=3`, only 58 % of the `in-roi` targets are still inside it
and 3 % of the `psf-clear` ones, so those rows stop testing occupancy. Shift every target's
`x` by `-D` and **validate the result against the SIL navmesh** (`$CFG/navmesh.json`): an
off-navmesh target is not a visible failure, the character simply never arrives and the row
looks like a detection problem instead of a bad waypoint. Keep pristine copies and restore
them when the sweep ends.

```bash
python3 shift_waypoints.py --distance $D --source-dir <pristine trees> --output-dir trees-D$D
cp trees-D$D/*.bt.json $CFG/                     # install for this row only
```

Before recording, look at what the row will actually install — the cameras do not move, so
one frame per camera plus the projected geometry is the whole picture:

```bash
python3 preview_geometry.py --calibration cal-<name>.json \
  --shot Camera=shots/Camera.png --shot Camera_01=shots/Camera_01.png \
  --pushout 0 1 2 3 --output preview.png
```

The forklift route needs no edit — it is driven by `forklift-controller` waypoints and
crosses the wire wherever the wire is, simply earlier.

**Verification.** The pose guard cannot help here: every row has identical poses by design,
so it passes even if the wrong `D` is installed. Use the `load_roi` check from §2.6 as a
hard assert per row, plus a check that the minimum target `x` in the installed trees is what
this `D` should give.

---

## 6. Traps

None of these announce themselves; every one produced a healthy-looking stack and wrong or
unusable numbers.

1. **Half a placement change.** The two-file rule at the top. Most expensive mistake
   available, and the reason the wrapper has a pose guard.
2. **The pose guard is not a geometry guard.** It compares extrinsics only, so a sweep that
   moves only the zone passes on every row, including a row with the wrong zone installed.
3. **Stream count vs camera count.** With `NUM_STREAMS=3` and two registered cameras, the
   spare slot sometimes binds to a camera that already has one and then sits at 0 fps for
   the whole run, which looks exactly like a dead camera to anything reading per-slot fps.
   Matching `NUM_STREAMS` to the camera count doubled per-camera fps to 30 here but also
   destabilised the pipeline and made rows non-comparable with earlier ones recorded at
   ~15 fps, so we kept 3 and made the watchdog camera-aware (a camera counts as dead only
   when *every* slot it occupies is at 0).
4. **Ground truth follows the calibration — which is what you want.** `srr.aggregator` and
   `srr.tw_split` read `/app/calibration.json` on every invocation, so moving the zone moves
   the ground truth with it. Note `tw_split` carries a hardcoded fallback wire at
   `x = 9.574`, used only if the file is missing, which would score a pushed-out row against
   the original line.
5. **A dead component does not stop the take.** Both failures we have seen — a camera at
   0 fps, and Safety Core no longer deciding — leave every other signal healthy and produce
   a report that reads as a worse placement. Sample liveness *during* recording (§3), and
   before trusting a row check the command mix per scenario: `psf_command` in the parquet
   should be a few percent `HEARTBEAT`, not a majority.

   ```bash
   docker exec srr python3 -c "
   import pandas as pd, glob, json, collections
   for p in sorted(glob.glob('/app/runs/<run>/*/run-*.parquet')):
       c = collections.Counter(json.loads(s)['command_name']
                               for s in pd.read_parquet(p, columns=['psf_command'])['psf_command'].dropna())
       print(p.split('/')[-2], dict(c))"
   ```

6. **`pss.log` snapshots are ~5 GB per scenario.** Four positions × five scenarios ≈ 100 GB.
7. **Stale FOV polygons** after a camera moves — §2.4.
8. **The 3D (sparse4d) profile is a different exercise, not a flag.** It keeps
   `tripwires`/`rois` at the top level of the calibration rather than per sensor, and
   duplicate source-slot binding — harmless in 2D, where sensors are independent — breaks
   multi-view fusion outright and yields a pipeline that is healthy in every log and detects
   nothing.

---

## 7. What we measured on the gate-mount question

All 2D profile. Gate mount = two cameras on the gate line at `x = 9.574`, `y = -10.0 /
-20.2`, `z = 3.2`, 8 mm, aimed at `(7.226, -15.107)`, 29.7° below horizontal, ~15 fps,
121–124 clips per row.

### 7.1 Gate mount vs the shipped in-room cameras

| | default, 3 in-room | gate mount, 2 cameras |
|---|---|---|
| PASS / NEAR / FAIL | 64 % / 31 % / 4 % | 67 % / 27 % / 6 % |
| Mute correct | 84.8 % | 96.5 % |
| Unmute correct | 96.6 % | 94.4 % |
| ROI entry | 95.0 % | 96.0 % |
| TW IN | 112/112 (100 %) | 85/121 (70.2 %) |
| TW OUT | 112/112 (100 %) | 119/121 (98.3 %) |
| Worst mute lag | 18 200 ms | 700 ms |

Headline verdicts are comparable and the gate mount is better on mute behaviour, but
tripwire IN detection drops to 70 %: from the gate line the camera cannot see into the
trailer, so an inbound crossing has little track on the far side of the wire.

### 7.2 First sweep — parameterised the wrong way round

This one moved the **cameras** out from the wall and left the wire at `x = 9.574`, i.e. it
measured `D = 0, −1, −2, −3`. It also used a steeper solver-chosen aim (39.4°). Kept for the
trend, not as an answer.

| Config | Camera x | PASS | NEAR | FAIL | Mute ok | TW IN | TW OUT | TW events with no direction |
|---|---|---|---|---|---|---|---|---|
| D=0 | 9.574 | 52 % | 40 % | 8 % | 96.1 % | 87.8 % | 62.6 % | 247 |
| D=−1 | 8.574 | 47 % | 50 % | 3 % | 97.2 % | 100 % | 100 % | 49 |
| D=−2 | 7.574 | 48 % | 46 % | 7 % | 95.4 % | 98.4 % | 98.4 % | 93 |
| D=−3 | 6.574 | 65 % | 27 % | 9 % | 90.4 % | 98.4 % | 100 % | 64 |

Why `D=0` scores 62.6 % on TW OUT while its PASS and mute rates sit with the rest: its
`fast` scenario matched every inbound crossing (55/55) and only 38/55 outbound, `in-roi`
10/14 against 8/14. The forklift is seen and timed correctly; what fails is resolving the
outbound direction, because with the wire directly beneath the cameras a track has very few
points on each side of the line. Mute behaviour barely moves, since muting depends on zone
occupancy rather than the IN/OUT label — the trace it leaves is a later exit detection
(median 1.31 s at `D=0` against 0.20–0.91 s at `D=−2/−3`). `tripwireMinPoints` in the
analytics config is the knob to test this directly.

### 7.3 Corrected sweep — cameras fixed, zone and waypoints move

In progress; `D=0` complete.

| | D=0 | D=1 | D=2 | D=3 |
|---|---|---|---|---|
| Clips | 121 | 121 | running | — |
| PASS / NEAR / FAIL | 68 % / 21 % / 12 % | 56 % / 27 % / 17 % | | |
| Mute correct | 75.2 %ᵈ | 52.1 %ᵈ | | |
| Unmute correct | 96.4 % | 97.6 % | | |
| ROI entry | 184/186 (98.9 %) | 164/164 (100 %) | | |
| TW IN | 83/121 (68.6 %) | 121/121 (100 %) | | |
| TW OUT | 115/121 (95.0 %) | 120/121 (99.2 %) | | |
| TW events with no direction | 213 | 4 | | |

ᵈ invalid, and the PASS/FAIL split with them — see below.

Where the geometry answer is already visible: pushing the wire 1 m off the wall takes
tripwire IN from 68.6 % to 100 %, TW OUT from 95.0 % to 99.2 %, and direction-less crossing
events from 213 to 4. That is the same effect §7.2 saw from the other direction, now measured
the way the question was asked.

The mute figures in this table are **not** placement results. Three scenarios were recorded
while Safety Core had stopped deciding: `D=0 fast` went silent 305 s in (55 % of its frames
heartbeat-only), `D=1 fast` at 336 s (96 %), and `D=1 psf-edge` never decided at all. In each
case a `SOFTWARE ERROR` command appeared 2 s before the last real decision, BA events kept
arriving, and the recording ran to completion. Ground truth then expected mute for thousands
of frames that no longer had a decider, which is where `D=0`'s 75.2 % and `D=1`'s 52.1 % come
from — the same configuration scored 96.5 % in §7.1.

It is not the pushed-out zone doing it: the ROI/tripwire ids are unchanged across rows, and
person traffic across the zone boundary is no higher at `D=1` (8.5/min) than at `D=0`
(10.0/min). Across every run recorded so far only 4 scenarios out of ~60 are affected. Those
three need re-recording, and the detection metrics (ROI entry, TW IN, TW OUT), which do not
depend on Safety Core, stand.

### 7.4 Open questions

1. Why behaviour analytics emits crossing events with **no direction field at all** — 247 of
   them at `D=0` against 49–93 elsewhere. Pushing the wire off the wall largely removes them
   (4 at `D=1`), so the trigger looks geometric, but the mechanism is not established.
2. Why Safety Core stops deciding (§7.3). It emits `SOFTWARE ERROR` and then heartbeats for
   the rest of the scenario; a restart clears it. Worth a bug with the `pss.log` window
   attached.
3. What a work zone that relocates 3 m into the warehouse means physically. The suggestion
   from the design review is the inverse: a fence to stop entry near the wall, with the
   loading area and the wire moved away from it.
4. **Visibility inside the trailer.** With a gate mount a person already inside is
   invisible, and the logic infers presence only from tripwire crossings. No value of `D`
   fixes that; it needs a rule change or another sensor.
5. Whether the real gate frame sits exactly on `x = 9.574`, and whether real gates carry the
   same overhead obstruction that caps mount height at 3.7 m here. Either answer shifts
   every pose.

---

## 8. Not in the repo yet

As of 2026-08-21 a fresh clone cannot run the above end to end. These exist on the test host
and need committing (or reimplementing) first:

| Missing | What it does |
|---|---|
| the 2-camera stack configuration | `cameras.yaml` and one row removed from `safety-core/configs/sensor_config.conf`. Regenerate the first from `candidates/cameras-gate-I.yaml` (§2.5); the second is a one-line edit |
| VSS-side overrides | the profile `.env` (`NUM_STREAMS`) and the live calibration sit in the VSS checkout on a non-upstream branch; §2.5 says what to set |

`navmesh.json` is generated rather than committed — bake and export it inside the `isaac-sim`
container with the tracked
`closed-loop-testing/regression-reporter/scenarios/isaac-scripts/navmesh/export_navmesh.py`,
which writes it to `/isaac-sim/sil/configs/navmesh.json`, where `shift_waypoints.py` looks for
it. Do this per scene, not once.

`candidates/` holds the placement every number in §7 was measured at — `cameras-gate-I.yaml`
and the calibration derived from it — so a row can be reproduced before authoring anything new.

Everything else described above is in this directory. The two harness scripts here are copies
of the ones in `closed-loop-testing/regression-reporter/scripts/` with the host-specific paths
removed; once the sweep using the originals has finished, the copies here become the only
version.

---

## 9. Files

| File | Purpose |
|---|---|
| `aim_cameras.py` | write a candidate `cameras.yaml` from mount points and aim targets (§2.1) |
| `screen_fov.py` | score coverage, single-camera exposure and view angle at the wire, offline (§2.2) |
| `capture_camera_shots.sh` | grab one frame per camera for the mandatory render check (§2.3) |
| `measure_render_occlusion.py` | measure from those frames how much of the zone structures cover |
| `generate_calibration.py` | derive the VSS `calibration.json`; also pushes wire and zone out by `D` (§2.4, §5) |
| `preview_geometry.py` | draw wire and zone on the frames, per `D`, before running anything (§5) |
| `shift_waypoints.py` | move the pedestrian waypoints with the zone, with the navmesh check (§5) |
| `run_srr_sweep.sh`, `run_srr_2cam.sh` | run the five scenarios for one placement, with the pose guard and the liveness watchdogs (§3) |
| `watch_safety_core.sh` | standalone monitor for the Safety Core decision path (§3) |
| `compare_runs.py` | put several runs side by side as text and CSV (§4) |
| `candidates/` | the placement §7 was measured at, as a worked example |
| `to_3d_calibration.py` | convert a calibration for the 3D (sparse4d) profile — see trap 8 |

A customer-facing version of this document, with figures, is kept outside the repo and shared
as a `.docx`; this file is the version that stays next to the code.
