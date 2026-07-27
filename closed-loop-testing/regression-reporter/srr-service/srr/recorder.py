# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Append-mode parquet writer. Rotates file on every open() so each
/srr/record true → false window produces one parquet.
"""
import json
import time
from pathlib import Path
from typing import Optional

import pyarrow as pa
import pyarrow.parquet as pq

from .schema import Frame


class Recorder:
    def __init__(self, out_dir: str = "runs") -> None:
        self.out = Path(out_dir)
        self.out.mkdir(parents=True, exist_ok=True)
        self.path: Optional[Path] = None
        self._writer: Optional[pq.ParquetWriter] = None
        self._buf: list[Frame] = []
        self._total_rows = 0

    def open(self) -> None:
        ts = time.strftime("%Y%m%d-%H%M%S")
        self.path = self.out / f"run-{ts}.parquet"
        self._writer = None
        self._buf.clear()
        self._total_rows = 0

    def append(self, f: Frame) -> None:
        self._buf.append(f)
        if len(self._buf) >= 256:
            self._flush_to_writer()

    def _flush_to_writer(self) -> None:
        if not self._buf:
            return
        rows = [
            {
                "arrival_wall_time": f.arrival_wall_time,
                "sim_time": f.sim_time,
                "char_0_x": f.char_0_xy[0] if f.char_0_xy else None,
                "char_0_y": f.char_0_xy[1] if f.char_0_xy else None,
                "char_1_x": f.char_1_xy[0] if f.char_1_xy else None,
                "char_1_y": f.char_1_xy[1] if f.char_1_xy else None,
                "char_2_x": f.char_2_xy[0] if f.char_2_xy else None,
                "char_2_y": f.char_2_xy[1] if f.char_2_xy else None,
                "forklift_x": f.forklift_xy[0] if f.forklift_xy else None,
                "forklift_y": f.forklift_xy[1] if f.forklift_xy else None,
                "psf_command": f.psf_command,
                "is_muted": f.is_muted,
                "ba_events_json": json.dumps(f.ba_events),
                # Phase 2 — None stays None (Phase 1 parquet has nulls here).
                "detections_json": json.dumps(f.detections) if f.detections is not None else None,
                "tracker_state_json": json.dumps(f.tracker_state) if f.tracker_state is not None else None,
                "bev_frame_id": f.bev_frame_id,
                "bev_create_time": f.bev_create_time,
                "ba_positions_json": json.dumps(f.ba_positions) if f.ba_positions is not None else None,
            }
            for f in self._buf
        ]
        self._buf.clear()
        self._total_rows += len(rows)
        table = pa.Table.from_pylist(rows)
        if self._writer is None:
            self._writer = pq.ParquetWriter(self.path, table.schema, compression="snappy")
        self._writer.write_table(table)

    def flush(self) -> int:
        """Flush the buffer, close the writer, and return the CUMULATIVE number of
        rows written to this parquet (not just the final in-memory buffer)."""
        self._flush_to_writer()
        if self._writer is not None:
            self._writer.close()
            self._writer = None
        return self._total_rows
