# Extending the viewer

Common changes you might want to make. Each section: where, what, gotchas.

## Add a new column to the run browser table

Files: `components/browser.js`, possibly `lib/manifest.js`.

1. Decide the data source. Three options:
   - **Already in the per-scenario `index.json`** (cheap — already loaded).
   - **In `summary.md`'s per-clip table** — extend `parsePerClipTable()` in
     `lib/manifest.js` to capture the new column. One fetch per run.
   - **In the per-clip aggregator report `<scen>/reports/<clip>.md`** — extend
     `parseClipReport()` in `lib/manifest.js`. One fetch per clip (parallel).

2. Update the `<thead>` in `sectionShell()` of `browser.js`.

3. Update `clipRowSkeleton()` to add a `<td class="metric-NEW">…</td>`
   placeholder.

4. In `enrichRow()`, fill `row.querySelector('.metric-NEW').innerHTML = …`.

5. If the value is a percentage you want to highlight when out-of-bounds,
   pass it through `pctCell(value, warnAt)` — it adds the `pct warn` class
   when below threshold.

## Add a new panel to the clip view

Files: `components/clip-view.js`, possibly `lib/manifest.js` if data needs
parsing.

1. If the panel reads a CSV, add a new `loadAndBuild()` call to
   `mountPanels()`'s `tasks` array, passing the URL relative to `baseUrl`
   and a column descriptor list.

2. If the panel reads a different format (JSONL, raw text), use
   `loadPssAndBuild` as a template — fetch + parse + `createPanel()`.

3. Each row must have a `wall_time` field (number, epoch seconds) for
   sync to work. If your source uses a different time field, map it to
   `wall_time` during parse.

4. If you want the panel to highlight rows by some condition (e.g. "PSF
   muted" rows in red), use the `fmt` callback in the column descriptor:
   ```js
   { key: 'is_muted', label: 'mute',
     fmt: v => v ? '<span class="warn">MUTE</span>' : 'unmute' }
   ```
   `fmt` returns HTML — escape it yourself if untrusted.

## Add a new top-level page

Files: `app.js`, new `components/<page>.js`.

1. Create `components/<page>.js` exporting `render(main, header, …)`.

2. Import it in `app.js`. Add a route condition before the existing ones:
   ```js
   if (params.has('mypage')) {
     await mypage.render(main, header);
   } else if (scenario && clip) {
     ...
   }
   ```

3. Add navigation. Use `<a href="?mypage=1">` so SPA navigation kicks in.
   Link from landing or browser pages as appropriate.

## Add a new run

Drop a clip_logs bundle (unzipped multi-test dir) into `data/`:

```bash
unzip ~/Downloads/multi-test-XXXX.zip -d data/
```

The landing page picks it up via dir-listing scrape on next refresh. No
code change needed.

To force a specific order (rather than alphabetical), edit `data/index.json`
to list runs explicitly:

```json
{
  "runs": [
    "multi-test-20260504-035555",
    "multi-test-20260503-175731",
    "multi-test-20260430-081223"
  ]
}
```

If `data/index.json` exists, dir scrape is skipped.

## Change the theme

Edit `styles.css`. The `:root` block at the top defines the palette:

```css
:root {
  --bg: #0e1116;        /* page bg */
  --bg2: #161b22;       /* panel bg */
  --bg3: #21262d;       /* hover / table-header bg */
  --fg: #c9d1d9;        /* primary text */
  --fg2: #8b949e;       /* muted text */
  --accent: #58a6ff;    /* links + accent */
  --pass: #3fb950;
  --near: #d29922;
  --fail: #f85149;
  --row-hi: #1f6feb40;  /* "now" row highlight */
  --border: #30363d;
}
```

Swap to a light theme by changing the variables — no other CSS changes
needed.

## Add a new pss.log module to the safety-modules filter

The pss.log JSONL filter is set in `srr.clip_logs` (Python — `regression-reporter`
dir, not this one). Extend the `SAFETY_MODULES` set there, then re-run
`clip_logs` on existing run dirs. The viewer doesn't filter — it just
reads the pre-filtered JSONL.

## Surface a new pss.log field in the panel

Files: `components/clip-view.js`.

`loadPssAndBuild` builds the panel with these columns:
```
{ key: 'wall_time', label: 't (rel)', fmt: relSec },
{ key: 'module',    label: 'module' },
{ key: 'endpoint',  label: 'endpoint' },
{ key: 'msg',       label: 'msg' },
```

`pss_log.jsonl` rows have exactly: `wall_time, iso, module, endpoint, msg`.
To add a column you'd need to also extend `srr.clip_logs._slice_pss_log`
in Python to extract more fields — then add the column descriptor here.

## Add a new pre-flight check / health gate

The viewer doesn't currently health-check before loading. If you wanted
something like "warn if BA detect rate < 50%":

1. In `landing.js`, after `loadRunPreview`, compute the warning condition
   from `r.baRoiMatched / r.baRoiTotal`.

2. Render a banner inside the card: e.g. add a `<div class="warn-banner">
   ⚠ Ghost result risk — perception detect rate 5%</div>` above the
   summary table.

3. Add CSS `.warn-banner { background: #2a1414; … }`.

The aggregator's ghost-result detector already flags this in the rendered
summary.md as a banner — the landing card could surface that flag for
quicker visual scanning across runs.

## Generate a new run bundle

The viewer reads what `srr.clip_logs` writes. The generator lives in
`regression-reporter/srr-service/srr/clip_logs.py`. Run it after a multi-test:

```bash
cd /path/to/regression-reporter/srr-service
docker exec srr python3 -m srr.clip_logs \
  --runs-dir /app/runs/multi-test-XXX \
  --calib /app/calibration.json \
  --zip
```

This produces:
- `<run>/<scen>/clip_logs/` per-scenario clip folders (consumed by viewer)
- `runs/<run-name>.zip` self-contained archive (drop into `data/` after unzip)

See the docstring in `clip_logs.py` for full options (`--fails-only`,
`--no-mp4`, `--scenarios`, `--pad`, etc.).

## When to abandon vanilla and reach for a framework

Signals it's time:
- Need real-time updates from a backend (websocket, etc.).
- Need cross-component shared state that URL params don't cover.
- Need user accounts / auth / multi-tenancy.
- Need deep keyboard shortcuts + focus management for a power-user UX.

Until then, vanilla wins on simplicity. Keep diff sizes small and bias
toward direct DOM manipulation over abstractions.
