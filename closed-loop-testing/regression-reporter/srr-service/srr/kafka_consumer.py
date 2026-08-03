# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Background thread reading Kafka topics. Calls a sink fn per message.

Two payload families are handled:

* **mdx-events** (Phase 1) — VSS BA events, protobuf-framed binary. We regex
  over the raw protobuf bytes to extract the human-readable fields (event type,
  class, ROI/TW ids, direction).
* **mdx-bev** (Phase 2) — the 3D detector + tracker per-frame output, encoded
  with the proper mdx ``nv.Frame`` protobuf schema (vendored as
  ``mdx_schema_pb2``). ``parse_mdx_bev_bytes`` decodes it into per-object
  detections {track_id, class, confidence, world x/y/z}.

To support the binary protobuf payloads the consumer keeps the **raw bytes**
(``value_deserializer=None``) and lets the per-topic ``parser`` decode them.
"""
import json
import os
import re
import threading
from typing import Callable, Optional

from kafka import KafkaConsumer


_RX_CREATE_TIME = re.compile(rb"CreateTime:(\d+)")
_RX_IDS = re.compile(rb"(roi-id-[0-9]+|tripwire-id-[0-9]+)")
_CLASSES = (b"Forklift", b"Person", b"Humanoid", b"Pallet")
_DIRECTIONS = (b"Rightp", b"Leftp")


def parse_mdx_event_bytes(raw: bytes) -> dict:
    """Pattern-match an mdx-events protobuf message from raw protobuf bytes."""
    if not isinstance(raw, (bytes, bytearray)):
        raw = str(raw).encode("utf-8", errors="replace")

    out: dict = {}
    m = _RX_CREATE_TIME.search(raw)
    if m:
        # CreateTime is in microseconds in VSS protos
        out["create_time"] = int(m.group(1)) / 1e6
    types = []
    if b"ROIEvent" in raw:
        types.append("ROI")
    if b"TripEvent" in raw:
        types.append("TW")
    if types:
        out["event_types"] = types
    classes = [c.decode() for c in _CLASSES if c in raw]
    if classes:
        out["classes"] = classes
    if b"TripEvent" in raw:
        for tag in _DIRECTIONS:
            if tag in raw:
                out["direction"] = tag[:-1].decode()
                break
    ids = sorted({m.group(1).decode() for m in _RX_IDS.finditer(raw)})
    if ids:
        out["ids"] = ids
    return out


def _default_event_parser(raw_bytes: bytes) -> dict:
    """Phase 1 path: try JSON, else regex the mdx-events protobuf bytes."""
    try:
        parsed = json.loads(raw_bytes)
        if isinstance(parsed, dict):
            return parsed
    except Exception:
        pass
    evt = parse_mdx_event_bytes(raw_bytes)
    if not evt:
        # truly unparseable; keep a tiny breadcrumb
        evt = {"_raw_first_60b_hex": raw_bytes[:60].hex()}
    return evt


# Lazy import so Phase 1 (mdx-events only) does not require the protobuf dep.
_BEV_FRAME = None
_BEHAVIOR_CLS = None
# Behavior.id looks like "bev-sensor-1 #-# 121" — the trailing int is the track id.
_RX_BEHAVIOR_TID = re.compile(r"#-#\s*(\d+)\s*$")


def _bev_frame_cls():
    global _BEV_FRAME
    if _BEV_FRAME is None:
        from . import mdx_schema_pb2 as _pb
        _BEV_FRAME = _pb.Frame
    return _BEV_FRAME


def _behavior_cls():
    global _BEHAVIOR_CLS
    if _BEHAVIOR_CLS is None:
        from . import mdx_ext_pb2 as _ext
        _BEHAVIOR_CLS = _ext.Behavior
    return _BEHAVIOR_CLS


def parse_mdx_bev_bytes(raw: bytes) -> dict:
    """Decode an mdx-bev ``nv.Frame`` protobuf into per-object detections.

    The 3D world pose lives in ``object.bbox3d.coordinates`` laid out as
    ``[cx, cy, cz, w, l, h, _, _, yaw, vx, vy, vz]`` (BEV fusion output). The
    flat ``object.coordinate`` field is left zeroed by the 3D pipeline, so we
    read the centre from ``bbox3d`` and fall back to ``coordinate`` only if the
    3D box is absent.

    Returns ``{"_bev": True, "create_time", "frame_id", "sensor_id",
    "detections": [{track_id, class, conf, x, y, z, w, l, h, yaw}]}``.
    """
    if not isinstance(raw, (bytes, bytearray)):
        return {"_bev": True, "detections": [], "_unparseable": True}
    try:
        frame = _bev_frame_cls()()
        frame.ParseFromString(bytes(raw))
    except Exception as e:  # noqa: BLE001 — keep the recorder alive on a bad frame
        return {"_bev": True, "detections": [], "_parse_error": str(e)}

    dets: list[dict] = []
    for o in frame.objects:
        c = list(o.bbox3d.coordinates)
        if len(c) >= 3:
            x, y, z = c[0], c[1], c[2]
            w = c[3] if len(c) > 3 else None
            l = c[4] if len(c) > 4 else None
            h = c[5] if len(c) > 5 else None
            yaw = c[8] if len(c) > 8 else None
        else:
            x, y, z = o.coordinate.x, o.coordinate.y, o.coordinate.z
            w = l = h = yaw = None
        dets.append({
            "track_id": o.id,
            "class": o.type,
            "conf": round(float(o.confidence), 4),
            "x": round(float(x), 4),
            "y": round(float(y), 4),
            "z": round(float(z), 4),
            "w": round(float(w), 4) if w is not None else None,
            "l": round(float(l), 4) if l is not None else None,
            "h": round(float(h), 4) if h is not None else None,
            "yaw": round(float(yaw), 5) if yaw is not None else None,
        })
    return {
        "_bev": True,
        "create_time": frame.timestamp.seconds + frame.timestamp.nanos / 1e9,
        "frame_id": frame.id,
        "sensor_id": frame.sensorId,
        "detections": dets,
    }


def parse_mdx_behavior_bytes(raw: bytes) -> dict:
    """Decode an mdx-behavior ``nv.Behavior`` (BA trajectory for one track).

    Each message is one object's smoothed trajectory over a short window. We
    take the most recent point (``smoothLocations`` last, falling back to raw
    ``locations``) as that track's current BA-reported position, and parse the
    track id from the ``id`` string ("bev-sensor-1 #-# 121" → 121).

    Returns ``{"_behavior": True, "track_id", "x", "y", "z", "speed",
    "direction", "create_time"}`` (``track_id`` None if unparseable).
    """
    if not isinstance(raw, (bytes, bytearray)):
        return {"_behavior": True, "_unparseable": True}
    try:
        b = _behavior_cls()()
        b.ParseFromString(bytes(raw))
    except Exception as e:  # noqa: BLE001
        return {"_behavior": True, "_parse_error": str(e)}

    pts = b.smoothLocations.coordinates or b.locations.coordinates
    if not pts or not list(pts[-1].point):
        return {"_behavior": True, "track_id": None}
    p = list(pts[-1].point)
    m = _RX_BEHAVIOR_TID.search(b.id or "")
    return {
        "_behavior": True,
        "track_id": m.group(1) if m else None,
        "x": round(float(p[0]), 4),
        "y": round(float(p[1]), 4),
        "z": round(float(p[2]), 4) if len(p) > 2 else None,
        "speed": round(float(b.speed), 4),
        "direction": b.direction or None,
        "create_time": b.end.seconds + b.end.nanos / 1e9,
    }


class KafkaConsumerThread(threading.Thread):
    def __init__(self, topic: str, sink, brokers: Optional[str] = None,
                 parser: Optional[Callable[[bytes], dict]] = None) -> None:
        super().__init__(daemon=True)
        self.topic = topic
        self.sink = sink
        self.parser = parser or _default_event_parser
        self.brokers = brokers or os.environ.get("KAFKA_BROKERS", "localhost:9092")

    def run(self) -> None:
        try:
            consumer = KafkaConsumer(
                self.topic,
                bootstrap_servers=self.brokers,
                auto_offset_reset="latest",
                enable_auto_commit=True,
                # Keep RAW bytes — the protobuf payloads (mdx-events, mdx-bev)
                # are corrupted by a utf-8 round-trip. Per-topic `parser` decodes.
                value_deserializer=None,
                group_id=f"srr-{self.topic}",
            )
        except Exception as e:
            print(f"[kafka] connect to {self.brokers} failed: {e}", flush=True)
            return
        print(f"[kafka] subscribed to {self.topic} on {self.brokers}", flush=True)
        msg_count = 0
        for msg in consumer:
            msg_count += 1
            raw = msg.value
            raw_bytes = raw.encode("utf-8", errors="replace") if isinstance(raw, str) else bytes(raw)
            evt = self.parser(raw_bytes)
            evt["_kafka_ts"] = msg.timestamp / 1000.0
            self.sink(evt)
            if msg_count % 500 == 0:
                n_det = len(evt.get("detections", [])) if isinstance(evt, dict) else 0
                print(f"[kafka] {self.topic}: processed {msg_count} msgs (last_dets={n_det})", flush=True)
