# Documentation

In-depth docs for `srr-debug-viewer`.

| Doc | What's in it |
|---|---|
| [architecture.md](architecture.md) | Why vanilla JS, repo layout, routing, data flow, component contracts, performance notes |
| [data-schema.md](data-schema.md) | What the viewer reads — top-level layout, every file format, every column, time anchoring |
| [extending.md](extending.md) | How to add columns / panels / pages / themes / new runs / new pss-log fields |
| [troubleshooting.md](troubleshooting.md) | Common issues — HEVC video spinner, "no runs found", stale cache, broken markdown render, `…` stuck cells |

Start with **architecture.md** if you want to understand how the viewer
works internally. **data-schema.md** is the contract with `srr.clip_logs`
(the Python generator) — read it before touching parsers. **extending.md**
covers the most common changes (add a column, add a panel). **troubleshooting.md**
is a reference for when something looks wrong.

For the user-facing quickstart (how to download a run + open in browser),
see the top-level [README.md](../README.md).
