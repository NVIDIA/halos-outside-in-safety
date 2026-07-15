# Troubleshooting

Common issues and what to check.

## "No runs found under data/"

The landing page can't enumerate any runs.

1. **Did you actually unzip into `data/`?** A common mistake is dragging
   the zip itself into `data/`. The viewer needs unzipped multi-test
   directories at `data/multi-test-YYYYMMDD-HHMMSS/`.

   ```bash
   ls data/                     # should list multi-test-* dirs
   ls data/multi-test-XXX/      # should contain summary.md, balanced-10min/, etc.
   ```

2. **Are you running from the right cwd?** `view.sh` does `cd "$VIEWER_DIR"`
   before starting the server. If you're starting Python's http.server by
   hand, do it from the viewer repo root, not from inside `data/`.

3. **Is the server actually serving the `data/` listing?** Test with:
   ```bash
   curl -s http://localhost:8765/data/ | head -10
   ```
   Should be an HTML directory listing. If you get a 404, the server's cwd
   is wrong.

4. **Stale browser cache.** After updating viewer files, hard-reload with
   `Ctrl+Shift+R` (Windows/Linux) or `Cmd+Shift+R` (Mac).

5. **Force the run list.** Create `data/index.json`:
   ```json
   { "runs": ["multi-test-20260503-175731"] }
   ```
   If this file exists, the dir-scrape fallback is skipped.

## Video shows a spinner forever / never plays

Symptom: clip view loads, all 4 panels populate, but the `<video>` element
stays on the loading spinner indefinitely. Browser DevTools Network tab
might show the MP4 download succeeded.

**Cause**: VST replay encodes per-clip MP4s as **HEVC (H.265)**. Firefox on
Linux has no software HEVC decoder by default. Most Chromium builds on
Linux don't ship HEVC either (license patent thicket). macOS Safari +
Chrome usually do support HEVC.

**Verify**: `ffprobe data/multi-test-XXX/.../video.mp4` — look for
`Stream #0:0 ... Video: hevc`.

**Workarounds**:

1. **Quick** — open the MP4 in `mpv` or `vlc` outside the browser. It plays.
2. **Permanent — transcode to H.264**:
   ```bash
   ./transcode_to_h264.sh data/multi-test-XXX
   ```
   Re-encodes every `clip_logs/*/video.mp4` in place to H.264. Takes ~3-10
   minutes per run depending on CPU + clip count. The viewer just needs a
   refresh after.
3. **For new runs** — change `vst_video.py` (in the SRR pipeline repo) to
   request H.264 from VST. Long-term fix.

## Page hangs after navigating back

Symptom: open a clip, click the breadcrumb to go back, page hangs on
loading state forever.

**Cause**: with very large multi-test bundles, the `<video>` element from
the clip view might still be holding a buffer when navigation fires. The
SPA teardown registered in `clip-view.js` cancels rAF + detaches listeners
+ `video.removeAttribute('src'); video.load();` which should release the
buffer — but timing edge cases exist.

**Fix**:
1. Hard-reload (`Ctrl+Shift+R`) — clears any stale module state.
2. If reproducible, check DevTools Console for errors. The error overlay
   should also surface anything that crashed.
3. Worst case: restart the http.server (kill + `./view.sh`).

This was a real bug before navigation became SPA-style: full page reloads
with module re-import — caching quirks made it look like the page hung
when it had actually loaded but without the new JS. Fixed via SPA nav.
If you see it again, it's likely something new.

## Console: `Cannot use import statement outside a module`

Means a JS file is being loaded as classic script, not module.

- `index.html` must reference `app.js` with `<script type="module" …>`.
  All `import …` statements work only in module context.
- If you `node --check` the JS files, you'll get this — Node defaults to
  CJS without a `"type": "module"` package.json. That's fine; the browser
  doesn't care. Use `node --input-type=module` to syntax-check from CLI.

## Per-clip metric cells stuck at `…`

Symptom: scenario tables have `…` placeholders that never update.

**Cause**: the per-clip aggregator report fetch (`<scen>/reports/<clip>.md`)
failed for that clip — file missing or HTTP error.

**Verify**: open DevTools Network tab, filter for `.md`, look for 404s or
500s. Or check directly:
```bash
ls data/multi-test-XXX/balanced-10min/reports/scn_*.md | head
```

If the report file is missing, the aggregator never wrote it (skipped or
errored). Re-run the aggregator on that run dir, or accept the gap (one
row stays `n/a`).

## `summary.md` rendering looks broken — random asterisks, weird tables

The minimal markdown renderer (`lib/md.js`) handles only the constructs
the SRR aggregator emits. If a future aggregator adds something new
(code blocks, blockquotes, images), it'll show as raw text.

**Check** which markdown construct broke:
- View the source of the rendered HTML (DevTools Elements). If bold `**X**`
  shows literal `**X**`, the inline regex in `md.js` didn't match — likely
  an unusual character in `X`.
- If a table renders as plaintext, the alignment row check might have
  failed.

**Fix**: extend `md.js`. Keep the renderer minimal — the goal is the SRR
output, not arbitrary Markdown.

## Browser DevTools shows 404s for `manifest.json` or `index.json`

The path the viewer requested doesn't exist. Look at the failed URL:

- `data/multi-test-XXX/balanced-10min/clip_logs/scn_0000/manifest.json`
  → This file should exist if `clip_logs` ran. Check on disk.
- `data/multi-test-XXX/balanced-10min/clip_logs/index.json`
  → Same — written by `clip_logs` after processing the scenario.

If files are missing, the bundle is incomplete. Re-run `srr.clip_logs`
on the source run dir + re-zip + re-distribute.

## Server permission errors

`view.sh` starts `python3 -m http.server` from the viewer repo root.
If `data/multi-test-XXX/` is owned by `root:root` (e.g. just-unzipped from
a docker-written bundle), Python can serve read-only fine — no fix needed.
But if your `clip_logs.py` re-runs in-place with root in the container,
you might hit chown issues. Fix:
```bash
docker exec srr chown -R "$(id -u):$(id -g)" /app/runs/multi-test-XXX
```
(Replace with your uid:gid — `id -u` / `id -g`.)

## Want raw pss.log lines but the panel is filtered

The viewer's pss.log panel shows only the parsed `pss_log.jsonl` (safety
modules only). For the unfiltered raw text:
```bash
less data/multi-test-XXX/<scenario>/clip_logs/<clip>/pss_log.txt
```

`pss_log.txt` is included in the bundle but the viewer doesn't render it
to keep the panel manageable. Open it in any text viewer for the full
syslog slice.

## Where to file bugs

Open a GitHub issue. Include: viewer URL, screenshot of the issue, browser
console output (F12 → Console).
