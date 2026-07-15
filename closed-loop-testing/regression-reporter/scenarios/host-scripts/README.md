# Host-side SRR utility scripts

ROS2 / rclpy / USD probes that run on the **Isaac Sim host machine** —
not inside the isaac-sim container. They sit alongside the SRR pipeline
because they were written for SRR scene prep + GT-publisher verification
+ obstacle scanning, and they share concepts (zone polygons, agent radii,
`/gt/*` topic names) with the rest of the SRR codebase.

## What sits here vs. `../isaac-scripts/`

| Location | Runtime | When to use |
|---|---|---|
| `host-scripts/` (this folder) | Host shell with ROS2 sourced (or `python3 + usd-core`) | Verifying state from outside Isaac — topic probes, USD inspection on disk, post-hoc analysis |
| `../isaac-scripts/` | **Isaac Sim Script Editor** (needs `omni.kit.*` / `pxr.Usd` / `omni.anim.navigation.core`) | Modifying a live scene — NavMesh bake, publisher injection, scene prep |

The GT machinery does **not** live here. The `/gt/*` publisher graph is built at
run time by
`closed-loop-testing/isaac-sim/sil/scripts/action_graphs/srr_ground_truth.py` when
Isaac is launched with `--srr-gt` (see `scripts/run_multi.sh`); the live prereq
diagnostic `check_srr_prereqs.py` lives in
[`../isaac-scripts/srr_pubs/`](../isaac-scripts/srr_pubs/) (Isaac-Kit runtime).
The scripts in *this* folder are host-shell probes (`verify_gt_topics.sh`,
`monitor_gt_5min.py`, `scene_scan.py`, …) that read live ROS/USD state from
outside Isaac.

## Quick reference

| Script | What it does | When to run |
|---|---|---|
| [`verify_gt_topics.sh`](verify_gt_topics.sh) | Lists `/gt/*` ROS topics + samples one TF message from each | Right after scene starts — confirm all 4 SRR publishers (Char_00/01/02, Forklift) are alive |
| [`monitor_gt_5min.py`](monitor_gt_5min.py) | Subscribes to all 4 GT topics for 5 minutes, prints per-second rate + first/last X,Y | Soak test — catch publishers that die mid-run, or character zones that aren't being traversed |
| [`track_char1_east.py`](track_char1_east.py) | Watches Char_01's `/gt/character_1/tf` until its x exceeds a configurable threshold (east endpoint) | Verifying a per-character waypoint plan reaches its intended east endpoint before timing out |
| [`track_all_east_2min.py`](track_all_east_2min.py) | Same as above but tracks all 4 actors with a 2-minute budget | Quick "did everyone get to their endpoint" smoke test after a scene reload |
| [`scene_scan.py`](scene_scan.py) | Opens a saved USD on disk (no Isaac runtime) via `usd-core`, dumps prim positions + transforms | Cross-checking what a scene `.usd` actually contains *before* loading it — catches "saved-as" bugs where the wrong layer was active |
| [`scan_obstacles_in_zones.py`](scan_obstacles_in_zones.py) | Iterates the 3 character zones (polygon boundaries), reports any USD obstacle prim overlapping each zone | Pre-flight before regenerating the behavior trees — confirms zone-walkability matches NavMesh state |

## Why these aren't in `scripts/` (the orchestration scripts)

`scripts/` is for the run-time orchestration that the SRR skill calls
during a multi-test (start recording, run aggregator, snapshot pss.log,
stop, etc.). It's hot path.

`host-scripts/` is for one-off / debug / setup workflows. Cold path:
either run manually during scene prep, or copy-paste into a Jupyter
when investigating an anomaly post-hoc. Different audience, different
cadence, hence different folder.

## How to run

All scripts assume:
- Python 3.10+
- ROS2 (jazzy or matching the compose stack) sourced (`source /opt/ros/<distro>/setup.bash`)
- For `scene_scan.py`: `pip install usd-core` (NOT the full Isaac runtime — just the USD lib)

### `verify_gt_topics.sh`
```bash
./verify_gt_topics.sh
# Lists /gt/* topics + samples one message each. Exit 0 if all 4 alive.
```

### `monitor_gt_5min.py`
```bash
python3 monitor_gt_5min.py
# Prints one line per second per topic with rate + last (x,y)
# Stops after 5 min or Ctrl-C.
```

### `track_char1_east.py`
```bash
python3 track_char1_east.py --threshold-x 9.5 --timeout 60
# Watches /gt/character_1/tf; prints "reached" when x >= threshold-x.
# Exits 1 if timeout reached without crossing.
```

### `track_all_east_2min.py`
```bash
python3 track_all_east_2min.py
# 2-minute budget; tracks all 4 actors against their per-character thresholds.
# Useful as the first health-check after a fresh scene load.
```

### `scene_scan.py`
```bash
python3 scene_scan.py /path/to/saved_scene.usd
# Walks the USD prim tree and dumps:
#   - prim path + type
#   - world XY transform
#   - applied APIs (NavMeshVolume, etc.)
# No Isaac runtime needed — just usd-core. Good for "what's actually in this saved file" checks.
```

### `scan_obstacles_in_zones.py`
```bash
# Inside Script Editor (it needs the live stage); host-shell variant kept here
# for reference / source-of-truth diff against the canonical Script-Editor copy.
```

## Common pitfalls

- **No DDS bridging**: if you run rclpy on the host but Isaac Sim is in a
  container with a different `ROS_DOMAIN_ID` or DDS profile, the topics
  won't show up. `verify_gt_topics.sh` will exit empty — that's NOT a
  publisher-side bug, it's a bridging issue. Check `ROS_DOMAIN_ID` matches
  between host and `isaac-sim` container.
- **Stale character zones**: the polygon definitions inside these scripts
  are set for the stock `indicator_warehouse_20x20_layout_overflow_test.usd`
  scene. If a new scene moves the zones, these scripts need their zone
  constants updated to match.
- **`scene_scan.py` needs `usd-core` (small)** not Isaac Sim's bundled USD
  — `pip install usd-core` will install a clean copy. Don't try to
  source Isaac's USD from outside the container.

## Where the `/gt/*` machinery lives

The `/gt/*` publisher graph is built at run time by
`closed-loop-testing/isaac-sim/sil/scripts/action_graphs/srr_ground_truth.py`
(the `--srr-gt` flag on `run_multi.sh` turns it on). The live prereq diagnostic
`check_srr_prereqs.py` lives in `../isaac-scripts/srr_pubs/`. Neither has a
host-side duplicate — both need `omni.graph` / `pxr.Usd` and only run inside
Isaac Kit.

`randomize_paths.py` lives at `../tools/randomize_paths.py` (host-side canonical —
runs on host with `usd-core` + numpy).
