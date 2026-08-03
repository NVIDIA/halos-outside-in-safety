#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Step 1: inspect parquet schema + clocks for latency decomposition."""
import sys, json, warnings
from pathlib import Path
import numpy as np, pandas as pd
warnings.filterwarnings("ignore")

RUN = Path(sys.argv[1] if len(sys.argv) > 1 else "/app/runs/multi-test-20260615-043909")
# pick a clip with forklift activity
pq = sorted(RUN.glob("balanced-10min/scenes/scn_*.parquet"))[2]
df = pd.read_parquet(pq)
print("CLIP:", pq.name, "rows:", len(df))
print("\nCOLUMNS:", list(df.columns))

print("\n--- time columns (first 3 rows) ---")
for c in ["arrival_wall_time", "sim_time", "bev_create_time"]:
    if c in df.columns:
        print(f"{c}: {df[c].head(3).tolist()}")

# clock sanity: arrival vs bev_create
if "bev_create_time" in df.columns:
    d = (df["arrival_wall_time"] - df["bev_create_time"]).dropna()
    print(f"\narrival - bev_create (s): median={d.median():.3f} mean={d.mean():.3f} "
          f"p95={d.quantile(.95):.3f} min={d.min():.3f} max={d.max():.3f} n={len(d)}")

# sample JSON payloads
def first_nonempty(col):
    if col not in df.columns:
        return None
    for s in df[col]:
        if isinstance(s, str) and s not in ("", "[]"):
            try:
                j = json.loads(s)
                if j:
                    return j
            except Exception:
                pass
    return None

det = first_nonempty("detections_json")
print("\n--- detections_json sample (1 obj) ---")
print(json.dumps(det[0], indent=2) if det else "none")
print("classes in that frame:", sorted({d.get('class') for d in det}) if det else "n/a")

ba = first_nonempty("ba_events_json")
print("\n--- ba_events_json sample ---")
print(json.dumps(ba[0], indent=2) if ba else "none")

bap = first_nonempty("ba_positions_json")
print("\n--- ba_positions_json sample ---")
print(json.dumps(bap[0], indent=2) if bap else "none")

# range check: are _kafka_ts, bev_create_time, arrival_wall_time same epoch?
kts = []
for s in df["ba_events_json"]:
    if isinstance(s, str) and s not in ("", "[]"):
        for e in json.loads(s):
            if e.get("_kafka_ts"):
                kts.append(e["_kafka_ts"])
print("\n--- epoch ranges ---")
print(f"arrival_wall_time:  {df['arrival_wall_time'].min():.1f} .. {df['arrival_wall_time'].max():.1f}")
if "bev_create_time" in df.columns and df["bev_create_time"].notna().any():
    print(f"bev_create_time:    {df['bev_create_time'].min():.1f} .. {df['bev_create_time'].max():.1f}")
if kts:
    print(f"_kafka_ts:          {min(kts):.1f} .. {max(kts):.1f}  (n={len(kts)})")
