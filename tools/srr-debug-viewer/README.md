# SRR Debug Viewer

Browser-based viewer for SRR `clip_logs` evidence bundles. Pairs the
per-clip MP4 with synchronized PSF state / GT positions / BA Kafka events /
pss.log streams — scrub the video and watch all the logs reveal in
lockstep. Inspired by [webviz.io](https://webviz.io/).

No build step, no dependencies. Just `python3 -m http.server` and open in
a browser. Works offline. (See the screenshot below for the layout.)

---

## Quick start (3 minutes)

You need: **Python 3.6+**, a modern browser (Chrome / Firefox / Safari),
and a SRR test bundle zip.

### 1. Get the viewer

The viewer ships in this repo at `tools/srr-debug-viewer`:

```bash
cd "$(git rev-parse --show-toplevel)/tools/srr-debug-viewer"
```

### 2. Get a test bundle

Two ways depending on where the SRR run lives.

#### 2a. Reference bundles

Pre-baked reference bundles (to be published as GitHub Release assets when the
repo goes public) illustrate the expected output (~2 GB each):

| Zip | Config | Clips |
|---|---|---:|
| `multi-test-20260430-081223.zip` | v1.1 reference (PSF 1.1 image, effective `maxPipelines=2`) | 66 |
| `multi-test-20260503-175731.zip` | v1.2 with `maxPipelines=3` (correct sensor count) | 97 |
| `multi-test-20260504-035555.zip` | v1.2 with `maxPipelines=2` (workaround tested) | 82 |

Unzip into the `data/` folder:

```bash
mkdir -p data
unzip ~/Downloads/multi-test-20260503-175731.zip -d data/
# → data/multi-test-20260503-175731/ now contains summary.md, balanced-10min/, …
```

#### 2b. From a SRR run you just produced on this machine

After running the `hoisa-generate-regression-report` skill or `./scripts/run_multi.sh ...` in `closed-loop-testing/regression-reporter`, the run dir lives under `<regression-reporter>/srr-service/runs/multi-test-YYYYMMDD-HHMMSS/` (or your configured `SRR_RUNS_DIR`). Two options to point the viewer at it — pick one:

**Option 1 — symlink (no disk copy, recommended)**:

```bash
ln -s <regression-reporter>/srr-service/runs/multi-test-YYYYMMDD-HHMMSS data/
# Repeat for additional runs — the landing page lists everything in data/
```

**Option 2 — generate + unzip the self-contained bundle** (use this if you need to share with someone else afterward):

```bash
# In regression-reporter:
docker exec srr python3 -m srr.clip_logs \
  --runs-dir /app/runs/multi-test-YYYYMMDD-HHMMSS \
  --calib /app/calibration.json \
  --zip
# → produces <regression-reporter>/srr-service/runs/multi-test-YYYYMMDD-HHMMSS.zip (~2 GB)

# In srr-debug-viewer:
unzip <regression-reporter>/srr-service/runs/multi-test-YYYYMMDD-HHMMSS.zip -d data/
```

> The viewer's data source is the `clip_logs/` subdir under each scenario. Symlinking the bare run dir works because the SRR aggregator + `clip_logs` already wrote those subdirs in place during the run. The zip route packages everything (raw parquets + MP4s + clip_logs) into a single shareable artifact.

You can drop multiple bundles into `data/` — the landing page will list all of them side by side.

### 3. Launch

```bash
./view.sh
```

This starts `python3 -m http.server` on port 8765 and opens
`http://localhost:8765/` in your default browser. The landing page lists
every run in `data/`. Click a run → see the rendered summary + clip
tables. Click a clip → see the synchronized timeline view.

To stop: `Ctrl+C` in the terminal.

### Different port / no auto-browser

```bash
./view.sh 9000     # port 9000 instead of 8765
```

If `xdg-open` isn't available (no DE / headless), the launcher just prints
the URL — copy + paste into a browser.

---

## What you see

### Landing page (the entry point)

One card per run in `data/`. Each card shows the headline metrics scraped
from `summary.md`:

- Total clips + verdict pills (PASS / NEAR / FAIL)
- **GT expected mute %** — what fraction of frames GT said should mute
- **Mute correct %** — of GT-expected-mute frames, what fraction PSF
  actually muted (the system's safety-state accuracy)
- **Unmute correct %** — same, on the unmute side
- BA ROI entry / TW in / TW out matched (perception detection rate)
- Worst unmute / mute lag

Click anywhere on the card to enter the run, or use the "view full summary"
link at the bottom of each card.

### Run page

Top section: rendered `summary.md` (the headline + per-run rollup table).
Below: one section per scenario (in-roi, psf-edge, psf-clear, balanced,
fast) with a clip table. Each clip row shows verdict, match%,
Mute correct%, Unmute correct%, GT context (%char_in_roi, %fk_trailer),
and BA event match counts.

Diverging values (Mute correct% < 80%, BA matched < total) auto-highlight
red/orange.

Click a clip to drill in.

### Clip page (the main debug view)

![SRR debug viewer — clip page screenshot](assets/srr-debug-viewer.jpeg)

**Scrub the video — all four panels jump to the row matching the current
video time. Press play — all panels scroll together.** The sync key is
`wall_time` (epoch seconds), shared across all four streams.

Each panel:
- **PSF state** — every command from `/safety/command`, `is_muted` flag,
  command sequence number
- **GT positions** — char/forklift world XY at 30 Hz, plus computed
  in_roi / in_trailer / expected_mute flags
- **BA events** — every Kafka event with type, classes, direction, ids,
  with a `±5 s` padding window so you see what fired just before/after
  the clip
- **pss.log slice** — parsed PSF syslog (NVPSB_PSS_DAEMON,
  NVPSB_PSD_CLIENT, nv_atl_client, …)

Header shows clip metadata + verdict + match% + GT context for quick
orientation.

---

## How to share a new run with someone

Run on the SRR test server:

```bash
# inside regression-reporter on the server
docker exec srr python3 -m srr.clip_logs \
  --runs-dir /app/runs/multi-test-XXX \
  --calib /app/calibration.json \
  --zip
# → produces /app/runs/multi-test-XXX.zip (~2 GB)
```

Pull the zip to your laptop:

```bash
rsync -ahP <user>@<HOST_IP>:/path/to/multi-test-XXX.zip .
```

Share the zip (e.g. attach to a GitHub issue / release). Recipients
follow the Quick Start.

---

## Caveats

- **Video codec**: VST records HEVC (H.265). Firefox + most Chromium on
  Linux don't decode HEVC. If `<video>` shows a permanent spinner,
  transcode in place: `./transcode_to_h264.sh data/multi-test-XXX`.
  See [docs/troubleshooting.md](docs/troubleshooting.md).
- **Single-user**: this is a debug tool, not a production dashboard.
  No auth, no rate limits, no concurrent-user testing.
- **Stale cache**: after upgrading the viewer, hard-reload the browser
  (`Ctrl+Shift+R`) to drop cached modules.

---

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | How the viewer is built — vanilla JS, routing, data flow |
| [docs/data-schema.md](docs/data-schema.md) | What the viewer reads — every file format, every column |
| [docs/extending.md](docs/extending.md) | Add columns / panels / pages / themes / runs |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Common issues and fixes |

The data the viewer reads is generated by `srr.clip_logs` in the
[`regression-reporter`](../../closed-loop-testing/regression-reporter) dir
of this repo. To regenerate / rebuild bundles, see that dir's docs.

---

## Repo layout

```
srr-debug-viewer/
├── README.md                    you are here
├── index.html                   entry — boots app.js
├── app.js                       SPA router + global error catch
├── styles.css                   dark theme
├── lib/
│   ├── csv.js                   minimal CSV parser
│   ├── manifest.js              loaders + parsers (summary.md, manifest.json, …)
│   ├── md.js                    minimal markdown → HTML renderer
│   └── time.js                  wall_time ↔ video_t helpers
├── components/
│   ├── landing.js               run grid (auto-discovers data/)
│   ├── browser.js               per-run page
│   ├── clip-view.js             per-clip page
│   └── panel.js                 generic time-keyed table panel
├── data/                        drop unzipped multi-test bundles here
├── docs/                        in-depth docs (architecture / extending / etc.)
├── view.sh                      launcher
└── transcode_to_h264.sh         defensive video transcoder
```

---

## License / contact

Licensed under Apache-2.0 (see the repository `LICENSE`).

Maintainers: Huy To <hnto@nvidia.com>, Huy Bui <hbui@nvidia.com>. For questions, open a GitHub issue.
