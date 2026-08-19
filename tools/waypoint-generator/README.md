# Forklift Waypoint Generator

![Waypoint Generator Demo](assets/waypoint-generator.png)

A web-based tool to visually create waypoints for forklift navigation on the warehouse floor plan.

## Features

- **Visual waypoint editing** on a plan-view render of the warehouse
- **Multiple maps**, one per Isaac scene, picked from a dropdown; adding one needs
  no code change
- **Pan/Zoom** navigation (Alt+Drag or Middle Mouse to pan, Scroll to zoom)
- **Origin pose selection** for odom frame reference — position and start heading
- **Heading control** via Shift+Scroll or slider, for waypoints and the origin alike
- **Drag-and-drop** waypoint reordering
- **Export** to JSON (waypoints plus interpolated poses for curve following)
- **Import** existing waypoint files (JSON or YAML)
- **Path Management System** - Save, open, delete, and rename paths with Redux store 
- **Auto-save** to localStorage for persistent storage

## Quick Start

Requires Node.js 20.19+ or 22.12+ (Vite 7 requirement).

```bash
# Install dependencies
npm install

# Start development server
npm run dev

# Open http://localhost:5173
```

## Usage

### Basic Workflow

1. **Set Origin**: Click "Set Origin" mode and click on the map where the forklift starts (or use "Use Default Forklift Start"). Set the start heading the same way as for a waypoint — Shift+Scroll or the sidebar slider — before clicking; the preview body shows what the click will commit. Turning the slider while in this mode re-aims an origin already placed, without discarding the waypoints drawn from it. Leave the heading at 0 unless you know otherwise — see [Which way the truck faces](#which-way-the-truck-faces).

2. **Add Waypoints**: Switch to "Add Waypoint" mode, then click on the map to add waypoints. Use Shift+Scroll to adjust heading before clicking.

3. **Edit Waypoints**: Click on waypoints to select, drag to move, or use the sidebar to edit coordinates directly.

4. **Save Path**: Click the 💾 **Save** button in the header to save your path for later use.

5. **Export**: Download or copy the JSON waypoint file.

### Path Management
![Path Manager](assets/path-manager.png)
- **📁 Paths Button**: Open Path Manager to view all saved paths
- **Create New Path**: Save your current work with a name
- **Open Existing Path**: Load previously saved paths
- **Rename Path**: Double-click on path name or use the ✏️ button
- **Delete Path**: Remove paths you no longer need with the 🗑️ button
- **Search Paths**: Filter paths by name in the Path Manager

## Coordinate Systems

- **Pixel**: Image coordinates (each map's plan view is 1920x1080)
- **World**: Isaac Sim global coordinates (meters)
- **Odom**: Relative to origin point

### Which way the truck faces

The forklift controller runs with pose inversion on — the truck's `drive:` block
in the robots config states `no_invert: false` and `heading_offset: 180.0` — so
it mirrors the whole path 180 degrees about the origin before driving it, and
adds that offset to the truck's measured heading. Both values follow from the
asset, so they sit on the model rather than on each truck. **A path drawn heading
east here is driven heading west.** That is not a bug to fix in this tool; it is
how the forklift asset's forward axis is reconciled with the odom frame, and the
paths under `forklift-controller/waypoints/` are all drawn that way.

The consequence is that the origin's heading here is **not** the truck's yaw in
the scene. The maps' `defaultForkliftStart.theta_deg` is 180 because that is
where the prim points, but the origin heading that makes a path drivable is 0.
"Use Default Forklift Start" therefore takes only the position from the map
config and leaves the heading at 0. Setting it to 180 to "match the scene" puts
the opening pose 180 degrees away from the spawned truck, and it spins on the
spot instead of pulling away.

Turn the origin off 0 only for a truck that genuinely starts at an angle to the
aisle, and check the first exported pose against where the truck stands before
running it.

## Output Format

The JSON export (including `poses`) is the input format of the SIL forklift controller: `closed-loop-testing/forklift-controller/robot_controller.py` loads it via `--path <file>.json`. In the compose deployment the whole `closed-loop-testing/forklift-controller/waypoints/` tree is mounted and the controller reads `<map id>/<ROBOT_ID>.json`, the map coming from `SCENARIO` — so an exported path replaces the file of the same robot name under the map it was drawn on. The `map` block this tool writes records that id, and `deployments/scripts/preflight.py` checks the file's `origin` against where the truck actually stands.

### JSON (for the forklift controller)
```json
{
  "map": {
    "id": "warehouse_40x20",
    "scene": "sil/scenes/warehouse_40x20_two_loading_dock.usd"
  },
  "origin": {
    "world_x": 1.0,
    "world_y": -13.39,
    "theta_deg": 0
  },
  "waypoints": [
    {
      "x": 12.44,
      "y": -0.01,
      "theta_deg": 0,
      "reverse": false,
      "world_x": 13.44,
      "world_y": -13.40,
      "velocity": 0.5,
      "note": "Waypoint 1"
    }
  ],
  "poses": [
    {"x": 1.0, "y": -13.39, "theta": 0.0},
    {"x": 1.5, "y": -13.39, "theta": 0.0}
  ],
  "segments": [
    {
      "from_waypoint": 0,
      "to_waypoint": 1,
      "reverse": false,
      "pose_count": 21
    }
  ]
}
```

**Fields:**
- `map`: Which warehouse the coordinates belong to. The controller ignores it, but
  it is what lets a person — or the Import button — tell a 20x20 path from a 40x20
  one. The two scenes place their forklift barely a metre apart, so without this
  a path from the wrong warehouse reads as perfectly plausible. Importing a file
  whose `map.id` is not the open map offers to switch first.
- `origin`: Robot starting pose in world coordinates. The controller and
  `preflight.py` read only `world_x`/`world_y`, but `theta_deg` is what aimed the
  first Bezier control point, so the opening segment's `poses` cannot be
  reproduced without it. A file whose `origin` has no `theta_deg` predates the
  field and is read back as 0, which is how it was written.
- `waypoints`: User-defined waypoints with odom (x, y) and world coordinates
- `poses`: Intermediate points for smooth curved path (consumed by the forklift controller)
- `segments`: Path segment info with reverse flag for each waypoint pair

## Controls

| Action | Control |
|--------|---------|
| Pan view | Alt+Drag or Middle Mouse |
| Zoom | Scroll |
| Add waypoint | Left Click (in Add mode) |
| Set origin | Left Click (in Set Origin mode) |
| Adjust heading | Shift+Scroll (in Add or Set Origin mode) |
| Select waypoint | Click on waypoint |
| Move waypoint | Drag waypoint |

## Maps

A map is a plan-view render of one Isaac scene plus the calibration that turns
its pixels into that scene's world metres. Each one is a folder:

```
public/maps/maps.json               the list, in the order the UI shows
public/maps/<id>/map.png            the plan view, 1920x1080
public/maps/<id>/config.json        scale, translation, forklift start
```

Two ship with the tool:

| Map id | Isaac scene | Scale |
|---|---|---|
| `warehouse_40x20` | `warehouse_40x20_two_loading_dock.usd` | 26.83 px/m |
| `warehouse_20x20` | `indicator_warehouse_20x20_layout_overflow_test.usd` | 49.51 px/m |

Pick one from the dropdown in the header. `warehouse_40x20` opens by default
(`"default": true` in `maps.json`), and the choice is written into the URL so a
reload or a shared link comes back to the same warehouse:

```
http://localhost:5173/?map=warehouse_20x20
```

An unknown id fails with the list of known ids rather than falling back, so a
typo cannot quietly draw on the wrong warehouse.

Switching maps clears the current drawing. Waypoints are metres in one scene's
world frame, so carrying them across would silently place them at coordinates
nobody chose.

### Paths remember their map

Each saved path records the `mapId` it was drawn on, and the Path Manager lists
the ones belonging to the open map. Opening a path from another map switches to
that map, so its coordinates are always read with the calibration they were
drawn with.

Paths saved before this existed are shown as **map not recorded** rather than
being assigned a guess: the tool was retargeted from the 20x20 scene to the
40x20 one in place, so an old path could belong to either and nothing stored in
it says which. Saving such a path labels it with the map that is open.

### Adding a map

Create `public/maps/<id>/` with the plan view and a `config.json`, then add the
id to `maps.json`. No code changes and no rebuild — `npm run dev` picks it up on
reload. Nothing is overwritten, so existing maps keep working.

The calibration fields:

| Field | Meaning |
|---|---|
| `scaleFactor` | Pixels per metre |
| `translationToGlobalCoordinates` | World origin offset, in metres |
| `defaultForkliftStart` | Where "Use Default Forklift Start" puts the origin. Its `theta_deg` records the prim's yaw for reference only — the button does not apply it, see [Which way the truck faces](#which-way-the-truck-faces) |
| `forkliftDimensions` | Body drawn on the canvas, in metres |
| `scene` | The scene USD this plan view was rendered from |

The formula is `pixelX = (worldX + transX) * scale` and
`pixelY = (-worldY + transY) * scale`. Take `defaultForkliftStart` from the
forklift prim's transform in the scene, or from its `spawn:` block in
`sil/configs/robots-*.yaml`.

The 20x20 values come from the [VSS blueprint sample data](https://github.com/NVIDIA-AI-Blueprints/video-search-and-summarization/tree/v3.2.1/deploy/docker/industry-profiles/warehouse-operations/warehouse-2d-app/calibration/sample-data/warehouse-loading-dock-3cams-synthetic).
That calibration measures pixel Y from the image *bottom*, so `transY` has to be
converted for this tool's top-anchored formula; each `config.json` records the
conversion it used in its `note`.

This tool deliberately does not use ROS occupancy maps
(`resolution`/`origin`/`imageHeight` from `map_server`). Plan-view renders read
better for hand-placing waypoints, and the scale calibration above is what goes
with them.
