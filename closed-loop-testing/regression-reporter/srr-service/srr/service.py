# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
SRR rclpy node — TF buffers per character + forklift, /safety/* subscribers,
periodic sampling timer that produces Frame rows and pushes to recorder.

Phase 1 demo. Service control via /srr/record (SetBool: data=True start, False stop).
"""
import os
import time
import threading
from typing import Optional

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy

from tf2_ros import Buffer
from tf2_msgs.msg import TFMessage
from std_msgs.msg import String, Bool
from rosgraph_msgs.msg import Clock
from std_srvs.srv import Trigger, SetBool


# Isaac's ROS2PublishTransformTree publishes BEST_EFFORT. Default subscriber
# QoS is RELIABLE → mismatch → tf2 Buffer never receives updates → recorder
# samples the same first transform forever (parquet shows std=0, unique=1).
# Use BEST_EFFORT explicitly here.
TF_QOS = QoSProfile(
    reliability=ReliabilityPolicy.BEST_EFFORT,
    history=HistoryPolicy.KEEP_LAST,
    depth=50,
)

from .schema import Frame
from .recorder import Recorder
from .kafka_consumer import (
    KafkaConsumerThread,
    parse_mdx_bev_bytes,
    parse_mdx_behavior_bytes,
)


CHAR_TOPICS = ["/gt/character_0/tf", "/gt/character_1/tf", "/gt/character_2/tf"]
FORKLIFT_TOPIC = "/gt/forklift/tf"
WORLD_FRAME = "world"   # confirmed via ros2 topic echo: parent frame_id is lowercase
FORKLIFT_CHILD = "body"   # /gt/forklift/tf publishes both 'body' and 'lift'; we want body
SAMPLE_HZ = 30
BA_POS_TTL_S = 1.0   # drop BA track positions not updated within this window


class SRRNode(Node):
    def __init__(self) -> None:
        super().__init__("srr_service")
        out_dir = os.environ.get("SRR_OUT_DIR", "/app/runs")
        self.recorder = Recorder(out_dir=out_dir)
        self.recording = False

        self._tf_subs: list = []  # track subscriptions so we can destroy them on /srr/record start
        # /gt/character_N/tf carries a single world->character transform whose
        # child frame is the IRA-assigned asset name (e.g. female_adult_medical_01),
        # which varies by spawn seed — NOT the legacy baked "SkelRoot". Latch it
        # per-topic from the stream so sampling never hardcodes a frame name.
        self._char_child: list[Optional[str]] = [None for _ in CHAR_TOPICS]
        self.char_buffers = [Buffer() for _ in CHAR_TOPICS]
        self.forklift_buf = Buffer()
        for i, (buf, topic) in enumerate(zip(self.char_buffers, CHAR_TOPICS)):
            self._wire_tf(topic, buf, char_slot=i)
        self._wire_tf(FORKLIFT_TOPIC, self.forklift_buf)

        self._last_command: Optional[str] = None
        self._last_muted: Optional[bool] = None
        self.create_subscription(String, "/safety/command", self._cb_command, 10)
        self.create_subscription(Bool, "/safety/is_muted", self._cb_muted, 10)

        self._sim_time: Optional[float] = None
        self.create_subscription(Clock, "/clock", self._cb_clock, 10)

        self._ba_buffer: list[dict] = []
        self._ba_lock = threading.Lock()
        kafka_topic = os.environ.get("KAFKA_TOPIC", "mdx-events")
        KafkaConsumerThread(kafka_topic, self._on_ba_event).start()

        # Phase 2 — 3D detector + tracker output (mdx-bev). Unlike ba_events
        # (accumulated between ticks), detections are a per-frame *snapshot*:
        # we keep only the latest decoded frame and stamp it onto each sample.
        # Empty BEV_TOPIC disables Phase 2 (pure Phase 1 deployments).
        self._bev_lock = threading.Lock()
        self._last_bev: Optional[dict] = None
        bev_topic = os.environ.get("BEV_TOPIC", "mdx-bev")
        if bev_topic:
            KafkaConsumerThread(bev_topic, self._on_bev, parser=parse_mdx_bev_bytes).start()

        # Phase 2 — BA trajectories (mdx-behavior). Each message is one track's
        # current BA-reported position. Keep latest per track_id; a tick
        # snapshots the tracks updated within BA_POS_TTL_S to drop stale tracks.
        self._bapos_lock = threading.Lock()
        self._ba_pos: dict[str, dict] = {}
        behavior_topic = os.environ.get("BEHAVIOR_TOPIC", "mdx-behavior")
        if behavior_topic:
            KafkaConsumerThread(behavior_topic, self._on_behavior,
                                parser=parse_mdx_behavior_bytes).start()

        self.create_service(SetBool, "/srr/record", self._svc_record)
        self.create_service(Trigger, "/srr/flush", self._svc_flush)

        self.create_timer(1.0 / SAMPLE_HZ, self._sample_tick)

        self.get_logger().info(
            f"SRR service up; sample_hz={SAMPLE_HZ}, out_dir={out_dir}, "
            f"kafka_topic={kafka_topic}, bev_topic={bev_topic or '(disabled)'}, "
            f"behavior_topic={behavior_topic or '(disabled)'}"
        )
        self.get_logger().info("Awaiting: ros2 service call /srr/record std_srvs/srv/SetBool '{data: true}'")

    # --- Subscriptions ---
    def _wire_tf(self, topic: str, buf: Buffer, char_slot: Optional[int] = None) -> None:
        def cb(msg: TFMessage) -> None:
            for tr in msg.transforms:
                buf.set_transform(tr, "default_authority")
                # For character topics, latch the world-parented child frame name
                # (the IRA asset-named SkelRoot) so _sample_tick can look it up
                # without hardcoding a per-asset / per-seed frame name.
                if char_slot is not None and tr.header.frame_id == WORLD_FRAME:
                    self._char_child[char_slot] = tr.child_frame_id
        sub = self.create_subscription(TFMessage, topic, cb, TF_QOS)
        self._tf_subs.append(sub)

    def _cb_command(self, msg: String) -> None:
        self._last_command = msg.data

    def _cb_muted(self, msg: Bool) -> None:
        self._last_muted = msg.data

    def _cb_clock(self, msg: Clock) -> None:
        self._sim_time = msg.clock.sec + msg.clock.nanosec * 1e-9

    def _on_ba_event(self, evt: dict) -> None:
        with self._ba_lock:
            self._ba_buffer.append(evt)

    def _on_bev(self, evt: dict) -> None:
        # Replace (not append): only the most recent detector frame matters
        # for a given 30 Hz sample. Ignore decode-error frames.
        if evt.get("_parse_error") or evt.get("_unparseable"):
            return
        with self._bev_lock:
            self._last_bev = evt

    def _on_behavior(self, evt: dict) -> None:
        tid = evt.get("track_id")
        if not tid or evt.get("_parse_error") or evt.get("_unparseable"):
            return
        with self._bapos_lock:
            self._ba_pos[tid] = {
                "track_id": tid,
                "x": evt.get("x"), "y": evt.get("y"), "z": evt.get("z"),
                "speed": evt.get("speed"), "direction": evt.get("direction"),
                "_t": time.time(),  # arrival wall time, for TTL staleness only
            }

    # --- Services ---
    def _svc_record(self, req: SetBool.Request, resp: SetBool.Response) -> SetBool.Response:
        self.recording = bool(req.data)
        if self.recording:
            # Destroy old subscriptions FIRST — otherwise every /srr/record
            # start accumulates 4 more subscriptions per topic, the rclpy
            # executor congests, and the 30 Hz sample timer drops samples
            # (observed: 2.1 min of data captured during a 10-min recording
            # after 3 prior /record cycles).
            for old_sub in self._tf_subs:
                try:
                    self.destroy_subscription(old_sub)
                except Exception as e:
                    self.get_logger().warn(f"could not destroy old tf sub: {e}")
            self._tf_subs.clear()
            # Fresh buffers — drops stale transforms from previous Isaac
            # session (where sim_time may have been larger; otherwise tf2
            # rejects new transforms as TF_OLD_DATA).
            self._char_child = [None for _ in CHAR_TOPICS]
            self.char_buffers = [Buffer() for _ in CHAR_TOPICS]
            self.forklift_buf = Buffer()
            # Also clear latched safety state — otherwise a PSF feed that died
            # before this session inherits the previous session's last value
            # forever and the clip still scores as 100% PSF coverage.
            self._last_command = None
            self._last_muted = None
            for i, (buf, topic) in enumerate(zip(self.char_buffers, CHAR_TOPICS)):
                self._wire_tf(topic, buf, char_slot=i)
            self._wire_tf(FORKLIFT_TOPIC, self.forklift_buf)
            self.recorder.open()
            self.get_logger().info(
                f"Recording started → {self.recorder.path}; "
                f"destroyed old tf subs + reset buffers")
            resp.message = f"recording → {self.recorder.path}"
        else:
            n = self.recorder.flush()
            self.get_logger().info(f"Recording stopped, {n} total rows written")
            resp.message = f"stopped, {n} total rows written"
        resp.success = True
        return resp

    def _svc_flush(self, req: Trigger.Request, resp: Trigger.Response) -> Trigger.Response:
        n = self.recorder.flush()
        resp.success = True
        resp.message = f"flushed; {n} rows written total"
        return resp

    # --- Sampling ---
    def _lookup_xy(self, buf: Buffer, child_frame: str) -> Optional[tuple[float, float]]:
        try:
            t = buf.lookup_transform(WORLD_FRAME, child_frame, rclpy.time.Time())
            return (t.transform.translation.x, t.transform.translation.y)
        except Exception:
            return None

    def _char_xy(self, idx: int) -> Optional[tuple[float, float]]:
        """World (x, y) of SRR character `idx`. The child frame is the
        IRA-assigned asset name latched from /gt/character_N/tf (see _wire_tf);
        None until the first transform for that character arrives."""
        child = self._char_child[idx]
        if child is None:
            return None
        return self._lookup_xy(self.char_buffers[idx], child)

    def _sample_tick(self) -> None:
        if not self.recording:
            return
        with self._ba_lock:
            ba_events = self._ba_buffer[:]
            self._ba_buffer.clear()
        with self._bev_lock:
            bev = self._last_bev
        detections = tracker_state = bev_frame_id = bev_create_time = None
        if bev is not None:
            detections = bev.get("detections", [])
            tracker_state = [d["track_id"] for d in detections]
            bev_frame_id = bev.get("frame_id")
            bev_create_time = bev.get("create_time")
        now = time.time()
        with self._bapos_lock:
            ba_positions = [
                {k: v for k, v in p.items() if k != "_t"}
                for p in self._ba_pos.values()
                if now - p["_t"] <= BA_POS_TTL_S
            ] or None
        frame = Frame(
            arrival_wall_time=time.time(),
            sim_time=self._sim_time,
            char_0_xy=self._char_xy(0),
            char_1_xy=self._char_xy(1),
            char_2_xy=self._char_xy(2),
            forklift_xy=self._lookup_xy(self.forklift_buf, FORKLIFT_CHILD),
            psf_command=self._last_command,
            is_muted=self._last_muted,
            ba_events=ba_events,
            detections=detections,
            tracker_state=tracker_state,
            bev_frame_id=bev_frame_id,
            bev_create_time=bev_create_time,
            ba_positions=ba_positions,
        )
        self.recorder.append(frame)


def main() -> None:
    rclpy.init()
    node = SRRNode()
    try:
        rclpy.spin(node)
    finally:
        node.recorder.flush()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
