# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Proximity Safety Feature (PSF) command packet — 64-byte, identifier 0xA5.

Wire layout is BYTE-IDENTICAL to the ATL packet in safety_commands.py: same
size, same field offsets, same CRC-32 over [0..19] + [24..63]. Mirrors
safety-core/decision-makers/proximity/include/proximity_cmd_pkt.h.

WHY THIS IS A SEPARATE MODULE AND NOT A FLAG ON CmdPacket
---------------------------------------------------------
Because the opcodes COLLIDE WITH OPPOSITE SAFETY MEANINGS, and sharing one enum
would silently invert them:

    opcode   ATL (0xA2)                     proximity (0xA5)
    0x02     MUTE   — allow operation       STOP   — prevent operation
    0x07     UNMUTE — prevent + alarm       NORMAL — standard operation

So decoding a proximity packet through `CommandCode` turns a STOP into "allow
operation" and a NORMAL into "prevent operation + alarm". No exception, no
warning, no CRC failure — the packet is structurally valid and every field
parses. The only thing that distinguishes the two is byte 0, which is why the
identifier must select the ENUM and not merely be admitted to a list of accepted
values. 0x00 HEARTBEAT, 0x01 HW_ERROR and 0x03 SW_ERROR do agree across both.

Proximity additionally defines 0x08/0x09/0x0A (safe-release handshake), which
have no ATL counterpart.

WHAT THE OBJECT RECORDS ACTUALLY CARRY
--------------------------------------
The proximity app fills both records, in copyObjectRecordFromMetadata()
(ProximityControl.cpp:436). ATL fills objects[0] too, with the last forklift
record, for MUTE/UNMUTE (sendDecisionCommandWithObject1, ATLControl.cpp:2182),
but comm-layer's ATL path drops it: StateJson and SafetyCommand carry no
objects. Three consequences worth knowing before trusting a proximity field:

  - ROLES ARE NORMALISED BY SLOT. fillRoleNormalizedObjectRecords() puts the
    first non-person in objects[0] and the first person in objects[1], so slot
    order is a contract: machine then person. Every rule in
    proximity_event_mapping.pb.txt is <machine> x Person, so a scored pair
    always fills both. A machine x machine rule (Forklift x AMR) would leave
    slot 1 empty: separation_m is then None and the OPC payload labels that
    slot `person` with `empty: true`. Do not add one expecting both machines on
    the wire.

  - z IS ALWAYS ZERO. `object->z = 0.0f;` is hardcoded, so the packet carries a
    ground-plane position only. Whatever PSF used internally to compute the
    violation, dz is not on the wire and cannot be recovered from it.

  - metadata CANNOT NAME A ROBOT MODEL. It is `ObjectType` from
    pss_protocol.h:87, which is PERSON=7, VEHICLE=8, OBJECT=9 — a forklift, an
    AMR and a humanoid all land on one value. Attribution to a named robot must
    therefore come from the x/y coordinates, NOT from this field.
"""

import struct
import zlib
from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import IntEnum
from typing import List, Optional

from .safety_commands import (
    COMMAND_NUM_OBJECTS,
    COMMAND_PACKET_SIZE,
    ObjectRecord,
)

PROXIMITY_PACKET_IDENTIFIER = 0xA5


class ProximityCommandCode(IntEnum):
    """Opcodes from proximity_cmd_pkt.h. See the module docstring on 0x02/0x07."""

    HEARTBEAT = 0x00
    HW_ERROR = 0x01
    STOP = 0x02
    SW_ERROR = 0x03
    REDUCE = 0x05
    NORMAL = 0x07
    SAFE_RELEASE_REQUEST = 0x08
    SAFE_RELEASE_ACK = 0x09
    SAFE_RELEASE_DENIED = 0x0A

    @property
    def description(self) -> str:
        return {
            ProximityCommandCode.HEARTBEAT: "HEARTBEAT",
            ProximityCommandCode.HW_ERROR: "HARDWARE ERROR",
            ProximityCommandCode.STOP: "STOP (PREVENT OPERATION)",
            ProximityCommandCode.SW_ERROR: "SOFTWARE ERROR",
            ProximityCommandCode.REDUCE: "REDUCE (SAFE SPEED OPERATION)",
            ProximityCommandCode.NORMAL: "NORMAL (STANDARD OPERATION)",
            ProximityCommandCode.SAFE_RELEASE_REQUEST: "SAFE RELEASE REQUEST",
            ProximityCommandCode.SAFE_RELEASE_ACK: "SAFE RELEASE ACK",
            ProximityCommandCode.SAFE_RELEASE_DENIED: "SAFE RELEASE DENIED",
        }.get(self, f"UNKNOWN ({int(self)})")

    @property
    def mode(self) -> Optional[str]:
        """Topic-safe name for the three motion levels, else None.

        None is meaningful: it separates "PSF expressed a motion decision" from
        heartbeats, error codes and the safe-release handshake. Only the three
        levels below may drive a robot, so anything else must not be published
        as one.
        """
        return {
            ProximityCommandCode.STOP: "stop",
            ProximityCommandCode.REDUCE: "reduce_speed",
            ProximityCommandCode.NORMAL: "normal",
        }.get(self)

    @property
    def is_safety_critical(self) -> bool:
        return self in (
            ProximityCommandCode.STOP,
            ProximityCommandCode.REDUCE,
            ProximityCommandCode.HW_ERROR,
            ProximityCommandCode.SW_ERROR,
        )


# The three motion levels ordered by severity, so a consumer merging several
# pairs can take a worst-case rather than a last-writer-wins result. PSF's own
# computeProximityCommandFromBatch() does the latter: it assigns unconditionally
# in all three branches, so the winner is the last qualifying event by index, and
# an EVENT_10 (the STOP tier) can lose to a later EVENT_8 in the same batch.
# Anything reducing multiple commands to one should sort by this, not by arrival.
PROXIMITY_SEVERITY = {
    ProximityCommandCode.NORMAL: 0,
    ProximityCommandCode.REDUCE: 1,
    ProximityCommandCode.STOP: 2,
}

# A fault outranks every motion level: a truck must not move on a decision the
# decoder cannot read or PSF has declared unsafe.
FAULT_SEVERITY = 3
# Heartbeats and the safe-release handshake carry no decision at all.
NO_DECISION = -1


def packet_severity(pkt: "ProximityCmdPacket") -> int:
    """How much this packet asks of the machine; NO_DECISION for liveness only."""
    cmd = pkt.command
    if not isinstance(cmd, ProximityCommandCode):
        return FAULT_SEVERITY
    if cmd in (ProximityCommandCode.HW_ERROR, ProximityCommandCode.SW_ERROR):
        return FAULT_SEVERITY
    return PROXIMITY_SEVERITY.get(cmd, NO_DECISION)


class ProximityHold:
    """The decision a periodic reader should see: never overwritten unseen by a milder one.

    The OPC node holds one decision and the ROS bridge samples it at 10 Hz, while
    PSF decides at the frame rate. Written by arrival, a STOP followed by a
    NORMAL, or by a mere heartbeat, inside one poll never reaches the gate.

    So a decision at least as severe as the one shown replaces it at once, and a
    milder one waits until the shown one has been visible for `min_visible_s`
    (several polls); `tick()` then promotes the latest decision so a released
    STOP does not outlive PSF's own NORMAL. Heartbeats and the safe-release
    handshake never replace a decision: they are liveness, reported separately.
    """

    def __init__(self, min_visible_s: float = 0.3, clock=None):
        import time as _time

        self.min_visible_s = float(min_visible_s)
        self._clock = clock or _time.monotonic
        self.shown: Optional["ProximityCmdPacket"] = None
        self._shown_sev = NO_DECISION
        self._shown_at = 0.0
        self._latest: Optional["ProximityCmdPacket"] = None

    def offer(self, pkt: "ProximityCmdPacket") -> bool:
        """Take one packet; True if it is now the shown decision."""
        sev = packet_severity(pkt)
        if sev == NO_DECISION:
            return False
        self._latest = pkt
        now = self._clock()
        if (self.shown is None or sev >= self._shown_sev
                or now - self._shown_at >= self.min_visible_s):
            self._show(pkt, sev, now)
            return True
        return False

    def tick(self) -> bool:
        """Promote the latest decision once the shown one has had its time."""
        if self._latest is None or self._latest is self.shown:
            return False
        now = self._clock()
        if now - self._shown_at < self.min_visible_s:
            return False
        self._show(self._latest, packet_severity(self._latest), now)
        return True

    def _show(self, pkt, sev: int, now: float) -> None:
        self.shown, self._shown_sev, self._shown_at = pkt, sev, now


class ObjectClass(IntEnum):
    """`ObjectType` from pss_protocol.h:87 — as coarse as it looks.

    TYPE_0..TYPE_6 occupy 0-6 as unnamed placeholders. Only these three are
    named, and every machine shares one of them, which is why robot attribution
    has to use coordinates.
    """

    PERSON = 7
    VEHICLE = 8
    OBJECT = 9


def classify(metadata: int) -> str:
    try:
        return ObjectClass(metadata).name
    except ValueError:
        return f"TYPE_{metadata}"


@dataclass
class ProximityCmdPacket:
    """64-byte proximity command. `objects[0]` is the machine, `objects[1]` the person."""

    seq: int = 0
    command: ProximityCommandCode = ProximityCommandCode.HEARTBEAT
    ts_seconds: int = 0
    ts_microseconds: int = 0
    objects: List[ObjectRecord] = field(default_factory=list)
    identifier: int = PROXIMITY_PACKET_IDENTIFIER

    def __post_init__(self):
        while len(self.objects) < COMMAND_NUM_OBJECTS:
            self.objects.append(ObjectRecord())
        self.objects = self.objects[:COMMAND_NUM_OBJECTS]

    def pack(self) -> bytes:
        header = struct.pack(
            "<BHBQQ",
            self.identifier & 0xFF,
            self.seq & 0xFFFF,
            int(self.command) & 0xFF,
            self.ts_seconds & 0xFFFFFFFFFFFFFFFF,
            self.ts_microseconds & 0xFFFFFFFFFFFFFFFF,
        )
        payload = b"".join(obj.pack() for obj in self.objects)
        crc = zlib.crc32(header + payload) & 0xFFFFFFFF
        packet = header + struct.pack("<I", crc) + payload
        assert len(packet) == COMMAND_PACKET_SIZE, (
            f"Packet size {len(packet)} != {COMMAND_PACKET_SIZE}")
        return packet

    @classmethod
    def unpack(cls, data: bytes, verify_crc: bool = True) -> Optional["ProximityCmdPacket"]:
        """Parse 64 bytes. None on wrong size, identifier other than 0xA5, or bad CRC."""
        if len(data) != COMMAND_PACKET_SIZE:
            return None

        identifier, seq, command, ts_sec, ts_usec = struct.unpack("<BHBQQ", data[:20])
        if identifier != PROXIMITY_PACKET_IDENTIFIER:
            return None

        if verify_crc:
            crc_received = struct.unpack("<I", data[20:24])[0]
            crc_computed = zlib.crc32(data[0:20] + data[24:64]) & 0xFFFFFFFF
            if crc_received != crc_computed:
                return None

        # An unknown opcode is NOT coerced to HEARTBEAT the way the ATL decoder
        # does it. Here that would turn an unrecognised command into "all
        # normal, carry on"; keeping the raw int lets the caller see and reject
        # it. The int is out of enum range, so `.mode` is unreachable for it.
        try:
            cmd = ProximityCommandCode(command)
        except ValueError:
            cmd = command

        return cls(
            seq=seq,
            command=cmd,
            ts_seconds=ts_sec,
            ts_microseconds=ts_usec,
            objects=[ObjectRecord.unpack(data[24:44]),
                     ObjectRecord.unpack(data[44:64])],
            identifier=identifier,
        )

    @classmethod
    def now(cls, seq: int, command: ProximityCommandCode,
            objects: Optional[List[ObjectRecord]] = None) -> "ProximityCmdPacket":
        ts = datetime.now(timezone.utc)
        return cls(seq=seq, command=command,
                   ts_seconds=int(ts.timestamp()),
                   ts_microseconds=ts.microsecond,
                   objects=objects or [])

    def build_ack(self) -> "ProximityCmdPacket":
        """ACK echoing seq+command with a fresh timestamp.

        Must carry 0xA5: the SDM drops any ACK whose identifier is not
        PROXIMITY_PACKET_IDENTIFIER (ProximityControl.cpp:886), so an ACK built
        by the ATL path would be discarded and every command would look
        unacknowledged.
        """
        return ProximityCmdPacket.now(seq=self.seq, command=self.command, objects=[])

    @property
    def machine(self) -> ObjectRecord:
        return self.objects[0]

    @property
    def person(self) -> ObjectRecord:
        return self.objects[1]

    @property
    def separation_m(self) -> Optional[float]:
        """Ground-plane distance between the pair, or None if a slot is unfilled.

        Horizontal by necessity, not by choice — z is zero on the wire (see the
        module docstring). Derived here rather than read from the packet: PSF
        sends the two positions but not the distance it scored them on, so this
        is a reconstruction and may differ from PSF's own figure if PSF included
        a dz that never reached the wire.
        """
        m, p = self.machine, self.person
        if m.is_empty or p.is_empty:
            return None
        return ((m.x - p.x) ** 2 + (m.y - p.y) ** 2) ** 0.5

    @property
    def timestamp_iso(self) -> str:
        dt = datetime.fromtimestamp(self.ts_seconds, tz=timezone.utc).replace(
            microsecond=self.ts_microseconds % 1_000_000)
        return dt.isoformat()

    def describe_objects(self) -> str:
        """One-line pair dump — the evidence line for frame-fitting and attribution."""
        parts = []
        for role, obj in (("machine", self.machine), ("person", self.person)):
            if obj.is_empty:
                parts.append(f"{role}=<empty>")
            else:
                parts.append(f"{role}[id={obj.object_id} {classify(obj.metadata)}] "
                             f"({obj.x:.2f}, {obj.y:.2f}, {obj.z:.2f})")
        sep = self.separation_m
        if sep is not None:
            parts.append(f"sep={sep:.2f}m")
        return " | ".join(parts)

    def __str__(self) -> str:
        cmd = (self.command.description
               if isinstance(self.command, ProximityCommandCode)
               else f"UNKNOWN (0x{int(self.command):02X})")
        return (f"ProximityCmdPacket(seq={self.seq}, cmd={cmd}, "
                f"ts={self.timestamp_iso})")
