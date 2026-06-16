# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Safety Command Definitions for Communication Layer — 64-Byte ATL Packet (HOISA v1.2)

Packet layout (64 bytes total):
  [0]       Identifier        uint8    magic 0xA2
  [1-2]     Sequence Number   uint16   little-endian
  [3]       Command           uint8    opcode
  [4-11]    Timestamp sec UTC uint64   seconds since epoch
  [12-19]   Timestamp usec    uint64   microseconds component
  [20-23]   CRC-32            uint32   ISO 3309, polynomial 0xEDB88320
  [24-43]   Object Record 0   20 bytes
  [44-63]   Object Record 1   20 bytes

CRC is computed over bytes [0..19] + [24..63] (excludes the CRC field itself).

Backward compatibility:
  - CommandCode.UNMUTE_ALARM kept as alias for UNMUTE (same opcode 0x07)
  - CommandCode.NOP kept as alias for HEARTBEAT (previously 0x00 NOP in 16B)
  - SafetyCommand dataclass keeps same field names (sequence_number, command, status,
    timestamp, microseconds, source_ip, raw_header). New field `objects` (List[ObjectRecord]).
"""

import struct
import zlib
from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import IntEnum
from typing import List, Optional


# ------------------- Constants -------------------

ATL_PACKET_IDENTIFIER = 0xA2
COMMAND_HEADER_SIZE = 24
OBJECT_SIZE = 20
COMMAND_NUM_OBJECTS = 2
COMMAND_PACKET_SIZE = COMMAND_HEADER_SIZE + (COMMAND_NUM_OBJECTS * OBJECT_SIZE)  # 64

ACK_TIMEOUT_SECONDS = 3.0


# ------------------- Command opcodes -------------------

class CommandCode(IntEnum):
    """
    Safety command codes from PSF decision system (HOISA v1.2).

      CMD_HEARTBEAT = 0x00
      CMD_HW_ERROR  = 0x01
      CMD_MUTE      = 0x02  (Allow operation — safety muted)
      CMD_SW_ERROR  = 0x03
      CMD_UNMUTE    = 0x07  (Prevent operation — safety active + alarm)
    """
    HEARTBEAT = 0x00
    HW_ERROR  = 0x01
    MUTE      = 0x02
    SW_ERROR  = 0x03
    UNMUTE    = 0x07

    @property
    def description(self) -> str:
        return {
            CommandCode.HEARTBEAT: "HEARTBEAT",
            CommandCode.HW_ERROR:  "HARDWARE ERROR",
            CommandCode.MUTE:      "MUTE (ALLOW OPERATION)",
            CommandCode.SW_ERROR:  "SOFTWARE ERROR",
            CommandCode.UNMUTE:    "UNMUTE (PREVENT OPERATION)",
        }.get(self, f"UNKNOWN ({int(self)})")

    @property
    def is_safety_critical(self) -> bool:
        return self in (CommandCode.MUTE, CommandCode.UNMUTE,
                        CommandCode.HW_ERROR, CommandCode.SW_ERROR)

    # Backward-compat alias methods (not used in 64B but kept so old code doesn't break)
    @classmethod
    def from_header(cls, header: int) -> 'CommandCode':
        """Legacy 16B decode — no longer valid for 64B. Returns HEARTBEAT."""
        return cls.HEARTBEAT


# Backward-compat aliases for 16B code paths
CommandCode.UNMUTE_ALARM = CommandCode.UNMUTE    # 0x07 — same opcode
CommandCode.NOP = CommandCode.HEARTBEAT          # 0x00 — old NOP, new HEARTBEAT


class SafetyStatus(IntEnum):
    UNKNOWN   = 0
    MUTED     = 1
    ACTIVE    = 2
    NO_CHANGE = 3       # Backward-compat (was for NOP)
    HEARTBEAT = 4
    ERROR     = 5

    @classmethod
    def from_command(cls, cmd: CommandCode) -> "SafetyStatus":
        if cmd == CommandCode.MUTE:
            return cls.MUTED
        elif cmd == CommandCode.UNMUTE:
            return cls.ACTIVE
        elif cmd == CommandCode.HEARTBEAT:
            return cls.HEARTBEAT
        elif cmd in (CommandCode.HW_ERROR, CommandCode.SW_ERROR):
            return cls.ERROR
        return cls.UNKNOWN

    @property
    def emoji(self) -> str:
        return {
            SafetyStatus.MUTED:     "🟢",
            SafetyStatus.ACTIVE:    "🟡",
            SafetyStatus.HEARTBEAT: "💓",
            SafetyStatus.ERROR:     "🔴",
            SafetyStatus.NO_CHANGE: "⚪",
            SafetyStatus.UNKNOWN:   "❓",
        }.get(self, "❓")

    @property
    def description(self) -> str:
        return {
            SafetyStatus.MUTED:     "Safety muted - Loading allowed",
            SafetyStatus.ACTIVE:    "Safety active + Alarm on",
            SafetyStatus.HEARTBEAT: "Heartbeat",
            SafetyStatus.ERROR:     "Error",
            SafetyStatus.NO_CHANGE: "No change",
            SafetyStatus.UNKNOWN:   "Unknown status",
        }.get(self, "Unknown")


# ------------------- Object Record (20 bytes) -------------------

@dataclass
class ObjectRecord:
    object_id: int = 0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    metadata: int = 0

    def pack(self) -> bytes:
        return struct.pack("<IfffI", self.object_id, self.x, self.y, self.z, self.metadata)

    @classmethod
    def unpack(cls, data: bytes) -> "ObjectRecord":
        if len(data) != OBJECT_SIZE:
            raise ValueError(f"ObjectRecord needs {OBJECT_SIZE} bytes, got {len(data)}")
        obj_id, x, y, z, meta = struct.unpack("<IfffI", data)
        return cls(obj_id, x, y, z, meta)

    @property
    def is_empty(self) -> bool:
        return (self.object_id == 0 and self.x == 0.0 and self.y == 0.0
                and self.z == 0.0 and self.metadata == 0)


# ------------------- CmdPacket (64 bytes) -------------------

@dataclass
class CmdPacket:
    seq: int = 0
    command: CommandCode = CommandCode.HEARTBEAT
    ts_seconds: int = 0
    ts_microseconds: int = 0
    objects: List[ObjectRecord] = field(default_factory=list)
    identifier: int = ATL_PACKET_IDENTIFIER

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
        )  # 20 bytes
        payload = b"".join(obj.pack() for obj in self.objects)  # 40 bytes
        crc = zlib.crc32(header + payload) & 0xFFFFFFFF
        crc_bytes = struct.pack("<I", crc)
        packet = header + crc_bytes + payload
        assert len(packet) == COMMAND_PACKET_SIZE, f"Packet size {len(packet)} != {COMMAND_PACKET_SIZE}"
        return packet

    @classmethod
    def unpack(cls, data: bytes, verify_crc: bool = True) -> Optional["CmdPacket"]:
        """
        Parse 64-byte packet. Returns None if:
          - Wrong size
          - Bad identifier (not 0xA2)
          - CRC mismatch (if verify_crc=True)
        """
        if len(data) != COMMAND_PACKET_SIZE:
            return None

        identifier, seq, command, ts_sec, ts_usec = struct.unpack("<BHBQQ", data[:20])
        if identifier != ATL_PACKET_IDENTIFIER:
            return None

        crc_received = struct.unpack("<I", data[20:24])[0]
        if verify_crc:
            crc_computed = zlib.crc32(data[0:20] + data[24:64]) & 0xFFFFFFFF
            if crc_received != crc_computed:
                return None

        try:
            cmd = CommandCode(command)
        except ValueError:
            cmd = CommandCode.HEARTBEAT

        obj0 = ObjectRecord.unpack(data[24:44])
        obj1 = ObjectRecord.unpack(data[44:64])

        return cls(
            seq=seq,
            command=cmd,
            ts_seconds=ts_sec,
            ts_microseconds=ts_usec,
            objects=[obj0, obj1],
            identifier=identifier,
        )

    @classmethod
    def now(cls, seq: int, command: CommandCode,
            objects: Optional[List[ObjectRecord]] = None) -> "CmdPacket":
        now = datetime.now(timezone.utc)
        return cls(
            seq=seq, command=command,
            ts_seconds=int(now.timestamp()),
            ts_microseconds=now.microsecond,
            objects=objects or [],
        )

    @property
    def status(self) -> SafetyStatus:
        return SafetyStatus.from_command(self.command)

    @property
    def timestamp_iso(self) -> str:
        dt = datetime.fromtimestamp(self.ts_seconds, tz=timezone.utc).replace(
            microsecond=self.ts_microseconds)
        return dt.isoformat()

    def build_ack(self) -> "CmdPacket":
        """Build an ACK packet echoing this packet's seq+command with a fresh timestamp."""
        return CmdPacket.now(seq=self.seq, command=self.command, objects=[])

    def __str__(self) -> str:
        return (f"CmdPacket(seq={self.seq}, cmd={self.command.description}, "
                f"ts={self.timestamp_iso}, status={self.status.emoji})")


# ------------------- SafetyCommand (backward-compat view) -------------------

@dataclass
class SafetyCommand:
    """Decoded safety command — backward-compat surface for existing consumers."""
    sequence_number: int
    command: CommandCode
    status: SafetyStatus
    timestamp: str                     # HH:MM:SS.usec string (back-compat format)
    microseconds: int                  # kept for back-compat
    source_ip: Optional[str] = None
    raw_header: Optional[int] = None   # kept for back-compat; not meaningful for 64B
    # New fields (64B)
    ts_seconds: int = 0
    timestamp_iso: str = ""
    objects: List[ObjectRecord] = field(default_factory=list)

    @property
    def is_critical(self) -> bool:
        return self.command.is_safety_critical

    @classmethod
    def from_packet(cls, pkt: CmdPacket, source_ip: Optional[str] = None) -> "SafetyCommand":
        # Build back-compat HH:MM:SS string
        dt = datetime.fromtimestamp(pkt.ts_seconds, tz=timezone.utc)
        hms = dt.strftime("%H:%M:%S")
        return cls(
            sequence_number=pkt.seq,
            command=pkt.command,
            status=pkt.status,
            timestamp=hms,
            microseconds=pkt.ts_microseconds,
            source_ip=source_ip,
            raw_header=pkt.identifier,
            ts_seconds=pkt.ts_seconds,
            timestamp_iso=pkt.timestamp_iso,
            objects=pkt.objects,
        )

    def __str__(self) -> str:
        return (f"SafetyCommand(seq={self.sequence_number}, "
                f"cmd={self.command.description}, "
                f"status={self.status.emoji} {self.status.description}, "
                f"ts={self.timestamp_iso})")


# ------------------- Legacy constants (backward-compat) -------------------

CMD_NOP = CommandCode.HEARTBEAT
CMD_MUTE = CommandCode.MUTE
CMD_UNMUTE_ALARM = CommandCode.UNMUTE
ACK_CODE = 0x07  # legacy; not used in 64B (ACK is full packet echoing cmd)
