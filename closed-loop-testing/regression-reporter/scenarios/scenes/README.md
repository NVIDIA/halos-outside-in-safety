# Scene NavMesh

SRR ships **no custom USD scene**. It runs against the stock warehouse scene at
`isaac-sim/sil/scenes/indicator_warehouse_20x20_layout_overflow_test.usd`, and
the `/gt/*` publishers are added at run time via `--srr-gt` (see
`action_graphs/srr_ground_truth.py`) — nothing is baked into the USD.

This folder only owns the exported NavMesh for that scene.

## What's here

| File | Purpose |
|---|---|
| `navmesh.json` | Exported NavMesh triangle data for the stock scene (produced by [`../isaac-scripts/navmesh/export_navmesh.py`](../isaac-scripts/navmesh/export_navmesh.py)). Input to [`../tools/randomize_paths.py`](../tools/randomize_paths.py) `--navmesh`. Kept beside the scenarios because the NavMesh is a derived property of a specific scene — regenerate it if the scene changes. |

## Re-exporting the NavMesh

If the stock scene's geometry changes, re-bake + export in Isaac Sim's Script
Editor (see [`../isaac-scripts/README.md`](../isaac-scripts/README.md)):

1. `navmesh/clear_cache_and_rebake.py` — sync re-bake
2. `navmesh/export_navmesh.py` — writes `navmesh.json`

Then regenerate all behavior trees with `../tools/randomize_paths.py`; waypoints
are baked against the NavMesh and must be re-sampled.
