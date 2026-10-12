# 3D Calibration Dataset (3-camera BEV) — a scene of your own

> **The loading-dock scene needs nothing here.** VSS 3.3 ships
> `warehouse-3d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic` with the
> BEV group `bev-sensor-1` and the top-level `roi-id-1` / `tripwire-id-1`; use it as the
> reference for the format. Follow this page for another scene, e.g. `warehouse_40x20`
> (dataset `warehouse-40x20-two-docks-3cams-synthetic`, `test_scenario.md`).

The 3D (Sparse4D / BEV) profile needs a calibration dataset with **BEV sensor grouping** and
**world-coordinate ROI / tripwire** definitions. A scene that already has a 2D calibration (the
same physical space, same camera matrices) converts **structurally** (adding fields), not by
re-computation. Do this **before** the VSS deploy so the 3D behavior-analytics service loads
the ROIs / tripwires on startup.

> `<wh_ops>` = `<vss_repo>/deploy/docker/industry-profiles/warehouse-operations/` (see
> `vss_3d_overrides.md`). The calibration datasets live under
> `<wh_ops>/warehouse-2d-app/calibration/sample-data/` (2D source) and
> `<wh_ops>/warehouse-3d-app/calibration/sample-data/` (3D destination). `<name>` is the
> dataset name, the same for both apps.

---

## Source and destination

```
Source: <wh_ops>/warehouse-2d-app/calibration/sample-data/<name>/calibration.json
Dest:   <wh_ops>/warehouse-3d-app/calibration/sample-data/<name>/calibration.json
```

---

## Step 1 — Convert `calibration.json`

Start from the 2D calibration. Per sensor, these fields already exist and are kept as-is:

- `type`, `id`, `origin`, `geoLocation`, `coordinates`, `scaleFactor`
- `translationToGlobalCoordinates`, `attributes`, `place`
- `imageCoordinates`, `worldCoordinates`, `globalCoordinates`
- `intrinsicMatrix` (3×3), `extrinsicMatrix` (3×4), `cameraMatrix` (3×4), `homography` (3×3)

### Add per sensor — a BEV `group`

```json
"group": {
  "name": "bev-sensor-1",
  "alias": "area-1",
  "type": "bev",
  "origin": [<from translationToGlobalCoordinates>],
  "dimensions": [<bounding box of all globalCoordinates points>]
}
```

Compute `origin` from the sensor's `translationToGlobalCoordinates` and `dimensions` from the
bounding box of its `globalCoordinates`. **`group.name`** (`bev-sensor-1`) is the identifier
that binds the three cameras into one BEV fusion group — every sensor in the group shares it.

### Add per sensor — a `region` entry in `place`

```json
{"name": "region", "value": "Region-1"}
```

### Add a top-level `rois` array

The ROI / tripwire are defined in **world coordinates**, which are shared between 2D and 3D
for the same physical scene — so they transfer directly from the 2D calibration. Keep the
**ids** matching `nvpss.conf` (`roi-id-1`), and the object types matching the ATL event map
(restrict **Person**, confine **Forklift**):

```json
"rois": [
  {
    "id": "roi-id-1",
    "roiCoordinates": [
      {"x": <x0>, "y": <y0>},
      {"x": <x0>, "y": <y1>},
      {"x": <x1>, "y": <y1>},
      {"x": <x1>, "y": <y0>}
    ],
    "restrictedObjectTypes": ["Person"],
    "confinedObjectTypes": ["Forklift"],
    "sensors": ["Camera", "Camera_01", "Camera_02"],
    "groups": ["bev-sensor-1"]
  }
]
```

### Add a top-level `tripwires` array

Keep the id matching `nvpss.conf` (`tripwire-id-1`). The `wire` is the trailer-entry line; the
`direction` gives its crossing orientation (OUT = into the trailer, IN = out of it):

```json
"tripwires": [
  {
    "id": "tripwire-id-1",
    "wire": {
      "p1": {"x": <wx>, "y": <wy0>},
      "p2": {"x": <wx>, "y": <wy1>}
    },
    "direction": {
      "p1": {"x": <dx0>, "y": <dy>},
      "p2": {"x": <dx1>, "y": <dy>}
    },
    "sensors": ["Camera", "Camera_01", "Camera_02"],
    "groups": ["bev-sensor-1"]
  }
]
```

> The `roiCoordinates`, `wire`, and `direction` numeric values come straight from the 2D
> calibration of the **same** scene — copy them over unchanged. The ids
> (`roi-id-1`, `tripwire-id-1`) must match the `rule_id`s referenced in `nvpss.conf`, or the
> Safety Core will not map the events (`EVENT_0`…`EVENT_5`).

---

## Step 2 — Copy the BEV floorplan image

The 3D VST BEV visualization needs a top-down floorplan image; copy it from the 2D source
dataset (same scene):

```bash
SRC=<wh_ops>/warehouse-2d-app/calibration/sample-data/<name>
DST=<wh_ops>/warehouse-3d-app/calibration/sample-data/<name>

mkdir -p "$DST/images"
cp "$SRC/images/Top.png"              "$DST/images/"
cp "$SRC/images/imageMetadata.json"  "$DST/images/"
```

> **Gotcha:** make sure `Top.png` lands as a **file**, not a directory (an accidental
> `mkdir -p .../Top.png` creates a dir, and the sensor manager then fails to mount it).
> Verify: `file "$DST/images/Top.png"` must report `PNG image data`.

---

## Step 3 — Select the dataset

Set `SAMPLE_VIDEO_DATASET="<name>"` in the VSS build's `override.env` and put the scene's videos under
`<vss_data>/videos/<name>/`. The calibration file must exist before `up`: for a missing
`calibration.json` Docker creates a directory in its place and perception emits nothing.

---

## Verification

```bash
DST=<wh_ops>/warehouse-3d-app/calibration/sample-data/<name>

# JSON is valid
python3 -c "import json; json.load(open('$DST/calibration.json')); print('valid JSON')"

# required 3D fields present
python3 -c "
import json
d = json.load(open('$DST/calibration.json'))
for s in d['sensors']:
    assert 'group' in s, f'{s[\"id\"]} missing group'
assert len(d.get('rois', [])) > 0,      'no ROIs defined'
assert len(d.get('tripwires', [])) > 0, 'no tripwires defined'
print(f'OK: {len(d[\"sensors\"])} sensors, {len(d[\"rois\"])} ROIs, {len(d[\"tripwires\"])} tripwires')
"

# floorplan is a file, not a directory
file "$DST/images/Top.png"    # expect: PNG image data
```

After VSS is up, confirm the 3D behavior-analytics service actually loaded the ROI(s) — if it
reports **0** ROIs, the top-level `rois` array is missing, or the container needs to be
**recreated** (not just restarted) to pick up the newly bind-mounted `calibration.json`.
