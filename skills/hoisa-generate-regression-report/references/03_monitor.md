# Phase 3 — Live Clip Monitor (during recording)

The recording window (`/srr/record true` → false) is otherwise silent — for demo purposes we want a per-clip log line at the moment of each forklift tripwire crossing.

---

## Architecture

A background Python process subscribes to `/gt/forklift/tf` via rclpy with `BEST_EFFORT` QoS (must match Isaac's publisher), tracks `forklift_x`, and prints one line on each high-edge / low-edge crossing of `TW_X = 9.574`.

Script: [live_clip_monitor.py](../../../closed-loop-testing/regression-reporter/scripts/live_clip_monitor.py)

---

## Output format

Each crossing emits exactly one line to stdout:

```
  [MM:SS] Clip K: forklift <entered|exited> trailer (x=<x.xx>, t=<MM:SS>)
```

- `MM:SS` (left) — wall-clock time since monitor start
- `K` — 1-indexed clip count (starts at 1, increments on every crossing)
- `entered` / `exited` — direction of the crossing (forklift_x went above/below TW_X)
- `x=<x.xx>` — exact forklift_x at the crossing frame
- `t=<MM:SS>` — sim-time of the crossing within the recording

The orchestrator pipes this stdout into the main demo log so the user sees the lines interleaved with phase headers.

---

## Why a separate process

- Cannot run rclpy spin loop in the orchestrator bash — would block.
- Cannot use SRR's existing rclpy node — that's inside the docker container, would need a separate ROS DDS bridge to host stdout.
- Spawning a fresh host-side rclpy subscriber is the cleanest option.

Requirements:
- `rclpy` available. ROS distribution differs by location:
  - **host:** typically `/opt/ros/jazzy/setup.bash`
  - **srr container:** `/opt/ros/jazzy/setup.bash`

  Use a robust source pattern that works either way:
  ```bash
  for d in jazzy humble; do
    [ -f /opt/ros/$d/setup.bash ] && source /opt/ros/$d/setup.bash && break
  done
  ```
- `ROS_DOMAIN_ID=74` (matches Isaac + comm-layer + srr).
- BEST_EFFORT QoS or messages will be silently dropped.

> **Known limitation:** host rclpy subscriber does not receive
> `/gt/forklift/tf` messages even when Isaac publishes — host↔container
> DDS bridging issue. The 2026-05-03 run had monitor reporting 0
> boundaries while parquet contained 8 forklift TW crossings. Offline
> `tw_split` produces the canonical clip count from the parquet, so this
> only affects the demo log's per-crossing line, not analysis output.
> Fix belongs at Halos compose level (matched cyclonedds.xml or
> `network_mode: host`) — out of scope for this skill.

---

## Start / stop

Start (Phase 2 step 3f, BEFORE `/srr/record true`):

```bash
for d in jazzy humble; do
  [ -f /opt/ros/$d/setup.bash ] && source /opt/ros/$d/setup.bash && break
done
export ROS_DOMAIN_ID=74

nohup python3 ../scripts/live_clip_monitor.py --tw-x 9.574 --label "$LABEL" \
  > /tmp/live-clip-${TIMESTAMP}-${LABEL}.log 2>&1 &
LIVE_PID=$!
```

Stop (Phase 2 step 3j, AFTER `/srr/record false`):

```bash
kill -TERM $LIVE_PID 2>/dev/null
wait $LIVE_PID 2>/dev/null
```

The monitor handles SIGTERM cleanly and prints a final summary line:
```
  [MM:SS] Live monitor stopped. <K> total clip boundaries detected.
```

---

## Optional: live agent heartbeat

Every 60 s during the recording window, the orchestrator can spawn a tiny Explore agent to verify `/gt/forklift/tf` is still streaming:

```
Run `ros2 topic hz /gt/forklift/tf --window 30` for one second.
Report the rate. Expected ~30 Hz. Flag if < 5 Hz or no message.
```

This catches dead Isaac scenarios mid-recording without waiting for the final aggregator to fail.
