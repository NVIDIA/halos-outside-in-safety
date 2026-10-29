<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Safety-latch manager

Watches the ATL SDM's log, works out why the safe state latched, and releases it once the
fault has recovered and the trailer is verifiably clear. Faults that cannot be judged from
the log are handed to an operator instead. The SDM is never modified, the manager only
reads its log and asks `cmd_rx` to send a release.

## How it works

The manager follows the SDM log (a container, a file, or stdin), matching known onset and
recovery lines. During start-up it replays history to rebuild state without acting, then
arms itself once the stream reaches the present.

A release has two possible triggers, the manager deciding for itself, or an operator pressing the button, and both funnel through the same gates before anything is sent:

```
                           browser   (panel.js in the VST page)
                              │                 ▲
                        press │                 │ card + blocking reason
                              ▼                 │  HTTP :8771
                             safety_latch_panel.py
                              │                 ▲
            writes "release"  │                 │ reads snapshot
                              ▼                 │
         (FIFO) /run/atl-latch/release      /run/atl-latch/status.json
                              │                 ▲
                              │                 │ publishes
                              ▼                 │
   ┌──────────────────────► safety_latch_manager.py
   │     log lines             │
   │                           │   GATES: latched, armed, trailer clear,
   │                           │   no camera FAILED  (+ cause recovered
   │                           │   and dwell, on the automatic trigger)
   │                           ▼
   │                /tmp/cmd_rx_release (FIFO)
   │                           │
   │                           ▼
   │                        cmd_rx
   │                           │   CMD_SAFE_RELEASE_REQUEST (UDP)
   │                           ▼
   └───────────────────────  ATL SDM
            accept / deny come back as log lines
```

Note the single exit: an operator press does not go to `cmd_rx` or the SDM directly, it
enters the manager as a request and is checked like any other release. The only difference
from the automatic trigger is that a press does not wait for the cause to recover or for the
dwell, since the operator is standing at the trailer — every other gate still applies.


| File                      | Role                                                                                                                                       |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `safety_latch_manager.py` | Reads the log, decides, sends releases. Publishes `status.json`.                                                                           |
| `safety_latch_panel.py`   | Serves the operator card as a widget injected into the VST page. Reads `status.json`, forwards presses to the /run/atl-latch/release FIFO. |


Both share a state directory, `/run/atl-latch` by default (`ATL_LATCH_STATE_DIR` to
override): `status.json` is the snapshot the panel renders, and `release` is the FIFO a
press arrives on.

### Latch causes

| Cause                                              | Released automatically?                           |
| -------------------------------------------------- | ------------------------------------------------- |
| `sensors` — all safety cameras failed              | yes, once every failed pipeline reports RECOVERED |
| `gateway_hb` — gateway heartbeat fault             | yes                                               |
| `decision_freshness` — DecisionRequest timeout     | yes                                               |
| `decision_sequence` — DecisionRequest sequence gap | yes                                               |
| `count_drift` — person count untrustworthy         | no, operator only                                 |
| `pss` — PSS error mode or SW_FAIL                  | no, operator only                                 |


The first four log both an onset and a recovery, so recovery is observable. The last two log no recovery at all, so nothing can be waited for. A latch whose cause never appears in the log is declared unreadable after 30s and also goes to the operator.

### What auto-release requires

All of these, re-checked every 5s:

1. The manager is armed (log stream has reached the present)
2. A cause was identified, and every identified cause is auto-releasable
3. Every cause's recovery line has been seen
4. No safety camera is still FAILED
5. The dwell has elapsed (10s after the last recovery)
6. The forklift position is confirmed by an observed EVENT_0/EVENT_1 crossing
7. The trailer is clear: forklift out, persons 0, no restricted-area violation

Step 6 is there because the scene state can be stale by the time a fault clears; a crossing
is the evidence that resynchronises the manager's own belief.

## Requirements

Runs on the host beside the SDM. Python 3 standard library only.

- Read access to the log source. `--docker-container` needs Docker access; `--log-file`
needs the file readable.
- Write access to the state directory. `/run` is root-owned on a stock image, so either run
as root or pre-create it:
`sudo mkdir -p /run/atl-latch && sudo chown $(id -un):$(id -gn) /run/atl-latch`
- `cmd_rx` running with its release FIFO at `/tmp/cmd_rx_release`.

## Start the manager

Following a container:

```bash
python3 ./safety_latch_manager.py --docker-container nv-psf
```

Following a log file:

```bash
python3 ./safety_latch_manager.py --log-file /var/log/psf/atl_sdm.log
```

Either can be started **before** the SDM. The manager retries a source that is not up yet for up to 5 minutes, then exits non-zero so a supervisor restarts it. Add `--dry-run` to watch and decide without sending anything.

On start-up it replays the last 40000 log lines (~10 minutes) to catch a latch that began
before it did, and separately scans up to 200000 lines for the most recent scene snapshot.
Nothing is released during the replay.

## Start the panel

```bash
python3 ./safety_latch_panel.py --bind 0.0.0.0:8771
```

Then paste this in the VST page's browser console to inject the card:

```js
var s=document.createElement('script');s.src='http://'+location.hostname+':8771/panel.js';document.head.appendChild(s);
```

The card shows the latch state, the cause, what is blocking a release, and a release
button. `--bind` defaults to `127.0.0.1:8771`, which the browser on another machine cannot
reach — bind `0.0.0.0` for the demo rig. The port must match the snippet.

## Operator release

A press is refused outright, with no override, when anyone or anything is in the trailer, a restricted-area violation is active, a camera is still FAILED, or there is no latch to clear. The panel shows the reason.

Otherwise the press is accepted, and it does **not** wait for the cause to recover or for
the dwell. A press is required **twice** when the manager cannot vouch for the scene: the
forklift position is unconfirmed since the latch began, or a scene term has never been
observed. The prompt says what to check before pressing again.

A release can also be sent from a shell. Straight to `cmd_rx`, which is what the manager
itself writes to:

```bash
echo release > /tmp/cmd_rx_release
```

This **bypasses the manager entirely** — no occupancy check, no camera check, no two-press
prompt. To go through the manager's policy instead, write to its request FIFO, which is
what the panel button does:

```bash
echo release > /run/atl-latch/release
```

## Useful options

```bash
--dry-run              decide and log, send nothing
--tail N               log history to replay (default 40000)
--wait-source-sec SEC  retry a source that is not up yet (default 300; 0 fails at once)
--dwell-sec SEC        settle time after a cause recovers (default 10)
--recheck-sec SEC      how often the gate is re-evaluated (default 5)
--identify-sec SEC     how long to wait for a cause line before giving up (default 30)
--max-attempts N       release attempts before handing to the operator (default 20)
--release-fifo PATH    cmd_rx FIFO (default /tmp/cmd_rx_release)
```

`--help` lists the rest. Exit code 2 means the log source could not be read or produced
nothing recognisable — restart it; 0 is a clean stop.