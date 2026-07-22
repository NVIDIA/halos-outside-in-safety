# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Tripwire geometry — side-of-wire tests driven by calibration.json.

A tripwire in the VSS calibration is a segment (``wire.p1 → wire.p2``) plus a
``direction`` segment (``direction.p1 → direction.p2``) that crosses it. The
direction arrow defines the "entry" sense: movement with the arrow is the
entry crossing (BA labels it ``Right``), and the side of the wire that
``direction.p2`` (the arrow head) lies on is the "inside" side — for the
warehouse scene, the trailer.

This replaces the previous hardcoded assumption of a vertical wire with
inside = +x (``fx > tw_x``): the wire may have any orientation, and which side
is "inside" comes from the calibration instead of from code.
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class Tripwire:
    x1: float
    y1: float
    x2: float
    y2: float
    inside_sign: float  # normalizes side() so that inside is always positive

    # ---- construction ----

    @staticmethod
    def _raw_side(x1: float, y1: float, x2: float, y2: float,
                  px: float, py: float) -> float:
        """Cross product (wire vector) × (p1→point): sign = side of the line."""
        return (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)

    @classmethod
    def from_calib_dict(cls, tw: dict) -> "Tripwire":
        """Build from one entry of calibration.json ``sensors[i].tripwires``."""
        w = tw["wire"]
        x1, y1 = float(w["p1"]["x"]), float(w["p1"]["y"])
        x2, y2 = float(w["p2"]["x"]), float(w["p2"]["y"])
        s = 0.0
        d = tw.get("direction")
        if d:
            # The arrow head (direction.p2) sits on the inside of the wire.
            s = cls._raw_side(x1, y1, x2, y2, float(d["p2"]["x"]), float(d["p2"]["y"]))
        if s == 0.0:
            # No usable direction (missing, or degenerate: head exactly on the
            # wire line) — fall back to the legacy convention, inside = +x.
            s = cls._raw_side(x1, y1, x2, y2, max(x1, x2) + 1.0, (y1 + y2) / 2.0)
        return cls(x1, y1, x2, y2, 1.0 if s > 0 else -1.0)

    @classmethod
    def legacy(cls, tw_x: float, y_min: float, y_max: float) -> "Tripwire":
        """Vertical wire at x = tw_x with inside = +x (the old hardcoded model)."""
        return cls.from_calib_dict(
            {"wire": {"p1": {"x": tw_x, "y": y_min}, "p2": {"x": tw_x, "y": y_max}}}
        )

    # ---- queries ----

    def side(self, px: float, py: float) -> float:
        """Signed side of the (infinite) wire line; > 0 = inside half-plane."""
        return self.inside_sign * self._raw_side(self.x1, self.y1, self.x2, self.y2, px, py)

    def is_past(self, px: float, py: float) -> bool:
        """Point in the inside half-plane (no wire-extent check). This is the
        clip-boundary test tw_split uses — same semantics as the old
        ``fx > tw_x`` (which also ignored the wire's extent)."""
        return self.side(px, py) > 0.0

    def is_inside(self, px: float, py: float) -> bool:
        """Inside half-plane AND within the wire's extent (the old test also
        required y between the wire endpoints)."""
        return self.is_past(px, py) and 0.0 <= self._projection(px, py) <= 1.0

    def line_distance(self, px: float, py: float) -> float:
        """Perpendicular distance to the infinite wire line — same semantics
        as the old ``abs(x - tw_x)`` used for the trailer-boundary slice."""
        length = math.hypot(self.x2 - self.x1, self.y2 - self.y1)
        if length <= 0.0:
            return math.hypot(px - self.x1, py - self.y1)
        return abs(self._raw_side(self.x1, self.y1, self.x2, self.y2, px, py)) / length

    def _projection(self, px: float, py: float) -> float:
        """Normalized projection of the point onto the wire (0 = p1, 1 = p2)."""
        vx, vy = self.x2 - self.x1, self.y2 - self.y1
        length_sq = vx * vx + vy * vy
        if length_sq <= 0.0:
            return 0.0
        return ((px - self.x1) * vx + (py - self.y1) * vy) / length_sq

    # ---- legacy accessors (report text / JSON keys kept for compat) ----

    @property
    def x_ref(self) -> float:
        """p1.x — what the old code called TW_X (reports/JSON keep this key)."""
        return self.x1

    def __str__(self) -> str:
        return (f"wire ({self.x1:.3f},{self.y1:.3f})→({self.x2:.3f},{self.y2:.3f})"
                f" inside={'+' if self.inside_sign > 0 else '-'}side")
