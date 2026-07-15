# Architecture

A single-page debug viewer for SRR (Scenario Recorder + Reporter) `clip_logs`
evidence bundles. Vanilla JS, no build step, no dependencies. The whole app
ships as ~30 KB of source files, served over `python3 -m http.server`.

## Why vanilla?

- **No build step** — clone, `python3 -m http.server`, open browser. Works
  offline, works from a USB stick, works inside an air-gapped lab. No npm
  install, no node_modules, no version drift.
- **No backend** — the data is just files on disk. The browser fetches
  `<run>/summary.md`, `<clip>/manifest.json`, etc. directly. Python's
  built-in dir-listing handles run discovery.
- **Audience** — engineers reading evidence for a specific clip. Not a
  high-traffic dashboard. Single-user simplicity > scalability.
- **Lifetime** — debug tool, expected to evolve fast. Vanilla means anyone
  reading the code understands it without React-specific knowledge.

If we ever need React for state management of complex UI flows, we can
swap. For now the cost-benefit favors vanilla.

## Directory layout

```
srr-debug-viewer/
├── index.html              entry — boots app.js
├── app.js                  router + per-route teardown + global error catch
├── styles.css              dark theme, all CSS in one file
├── lib/
│   ├── csv.js              minimal CSV parser (no Papa, no deps)
│   ├── manifest.js         loaders + parsers for summary.md, index.json,
│   │                       per-clip manifest.json, per-clip aggregator .md
│   ├── md.js               minimal markdown → HTML renderer (~100 lines)
│   └── time.js             wall_time ↔ video_t conversions + binary search
├── components/
│   ├── landing.js          run grid (auto-discovers data/<run>/ via dir listing)
│   ├── browser.js          per-run page: rendered summary.md + scenario tables
│   ├── clip-view.js        per-clip page: video + 4 sync panels
│   └── panel.js            generic time-keyed table panel
├── data/                   place clip_logs zips' contents here
│   ├── index.json          (optional) explicit run list — falls back to scrape
│   └── <multi-test-XXX>/   one unzipped multi-test bundle per dir
├── docs/                   (this dir)
├── view.sh                 launcher: starts http.server + opens browser
├── transcode_to_h264.sh    defensive: re-encodes clip MP4s if HEVC fails to play
└── README.md               user-facing quickstart
```

## Routing

Single SPA router in `app.js`. URL params:

| `?run` | `&scenario` | `&clip` | Route |
|---|---|---|---|
| absent | — | — | landing — list of all runs in `data/` |
| `data/multi-test-XXX` | absent | — | run browser — summary.md + scenario tables |
| `data/multi-test-XXX` | `fast-20min` | `scn_0042` | clip view — video + sync panels |

All in-page links use `<a href="?...">` and are intercepted by app.js for
SPA navigation (no full page reload). `popstate` (browser back/forward)
calls `route()` which:

1. Runs the previous route's teardown (cancel rAF, detach video listeners,
   release MP4 buffer).
2. Hides the error overlay.
3. Dispatches to the right component based on URL params.

## Data flow

```
disk (data/<run>/...)
   │
   │  fetch over http.server
   ▼
lib/manifest.js + lib/csv.js + lib/md.js
   │
   │  parsed JSON / CSV rows / HTML
   ▼
components/{landing,browser,clip-view}.js
   │
   │  innerHTML + DOM nodes
   ▼
DOM → user
```

For the clip view, the panel sync loop drives off `video.currentTime`:

```
video element fires `timeupdate` (~4 Hz) + rAF runs every frame
   │
   ▼
seekSync(video.currentTime)
   │
   │  videoToWall(manifest, currentTime) → wall_time
   ▼
each panel.sync(wall_time)
   │
   │  binary search rows for closest wall_time
   ▼
toggle .now class on the matched row + scroll into view
```

All four panels share the wall_time axis. Scrub the video → all panels
jump in lockstep. Press play → all panels scroll together.

## Component contracts

### `landing.js`
- Discovers runs: tries `data/index.json` first, falls back to scraping
  Python http.server's `data/` directory listing (`<a href="multi-test-XXX/">`).
- For each run: fetches `<run>/summary.md`, parses headline metrics
  (`Mute correct%`, `Unmute correct%`, BA detect counts, lags).
- Renders one card per run with action links into the run browser + raw
  summary.md.

### `browser.js`
- Loads `<run>/summary.md` (for the rendered top section + per-clip BA
  detect counts) and per-scenario `<run>/<scen>/clip_logs/index.json`
  (for clip lists).
- Renders rendered summary.md (sans Per-clip detail — duplicates the
  scenario tables) + scenario sections.
- For each clip row, fans out parallel fetches of `<run>/<scen>/reports/<clip>.md`
  (per-clip aggregator report) to enrich with `Mute correct% / Unmute correct%
  / %char_in_roi / %fk_trailer`. Rows render skeleton first then upgrade in
  place when each fetch resolves.

### `clip-view.js`
- Loads `<run>/<scen>/clip_logs/<clip>/manifest.json` for clip metadata.
- Sets `<video src>` → `<run>/<scen>/clip_logs/<clip>/video.mp4`.
- Loads 4 streams in parallel:
  - `psf_timeline.csv` (PSF state per sample)
  - `gt_positions.csv` (Isaac Sim positions per sample)
  - `ba_events.csv` (Kafka BA events ±5 s padding)
  - `pss_log.jsonl` (parsed PSF syslog)
- Mounts each as a `panel.js` instance — generic time-keyed table.
- Wires up sync loop on the video element.
- Registers a teardown that cancels rAF, detaches listeners, releases MP4.

### `panel.js`
- Pure DOM, no framework. Takes rows + column descriptors + clipStart.
- Builds `<table>` with one `<tr>` per row.
- Exposes `sync(wall_time)` that:
  - Binary-searches `rows` for the closest wall_time.
  - Toggles `.now` class on the matched row.
  - `scrollIntoView` if not currently visible (skipped if user is hovering
    the panel — don't fight their manual scroll).

## State management

Mostly URL-driven. The current view is fully a function of URL params, so
back/forward + bookmark / share-link work for free.

The clip view has transient state (the rAF id + closure variables). That's
isolated and torn down on route change via `registerTeardown()`.

No global app state, no observable, no Redux. The browser is the store.

## Error handling

- Global `window.onerror` and `unhandledrejection` listeners surface errors
  in a sticky red overlay at the bottom of the page (with stack trace).
  Closes via the `×` button.
- Per-panel errors don't kill the page — `loadAndBuild` catches and renders
  a `panel error` placeholder with the error message.
- `route()`'s try/catch shows a red empty state with the error message if
  the top-level component throws.

## Performance notes

- **97 clips × 1 markdown fetch each = ~1-2 s on first browser load**, on
  localhost. Browser keeps the table interactive while metric cells fill in
  (`…` placeholder → real value).
- **Panel rendering** is one `<tr>` per CSV row with vanilla `appendChild`.
  831 rows × 4 panels = ~3,300 DOM nodes per clip view. Modern browsers
  handle this without virtualization. If a future panel has 10k+ rows we
  may need windowing.
- **Sync** is a single binary search per panel per video frame (rAF =
  60 Hz worst case). 4 panels × log(831) ≈ 40 comparisons per frame —
  negligible.

## Schema-stability assumption

The viewer relies on stable schemas in `clip_logs/` outputs. The contract
is documented in `data-schema.md`. If `srr.clip_logs` (the Python
generator) changes output, this viewer needs an update.
