# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
UDP Safety Command Receiver — 64-Byte ATL + Proximity Packets (HOISA v1.2)

Receives 64-byte safety commands from PSF via UDP (Black Channel / Comm Layer).

TWO PACKET TYPES ON ONE PORT, TOLD APART BY BYTE 0
--------------------------------------------------
PSF sends ATL packets (0xA2) or proximity packets (0xA5) depending on which
`--app` it was started with. The two are the same size with the same field
offsets, and 0x02/0x07 mean OPPOSITE things in each — so the identifier picks
the decoder and the opcode table together, and the two paths never share a
command object. See common/proximity_commands.py for the collision table.

Before this, a proximity run was silently dropped in full: every packet counted
as `invalid_identifier` and logged "Bad identifier: 0xA5", while the ROS bridge
kept publishing NOP heartbeats with is_muted=False — a safe-looking output for a
system that was emitting STOP.

Changes from 16B version:
  - PACKET_SIZE 16 → 64
  - Identifier byte 0xA2 verification
  - CRC-32 validation (ISO 3309, polynomial 0xEDB88320)
  - UTC timestamp as seconds+microseconds (uint64 each)
  - 2× 20-byte Object Records payload
  - ACK is full 64-byte packet echoing seq+command with fresh timestamp
  - Extended command opcodes (HEARTBEAT, HW_ERROR, MUTE, SW_ERROR, UNMUTE)

Backward-compatible API:
  - Class name SafetyReceiver kept
  - SafetyCommand dataclass fields unchanged
  - Queue and callback interface identical
"""

import logging
import socket
import threading
import sys
import os
from dataclasses import dataclass
from datetime import datetime
from queue import Queue, Full
from typing import Callable, Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from common.safety_commands import (
    CmdPacket,
    CommandCode,
    SafetyCommand,
    SafetyStatus,
    COMMAND_PACKET_SIZE,
    ATL_PACKET_IDENTIFIER,
)
from common.proximity_commands import (
    PROXIMITY_PACKET_IDENTIFIER,
    ProximityCmdPacket,
    ProximityCommandCode,
)
from common.config import UdpReceiverConfig, get_config

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


@dataclass
class ReceiverStats:
    """Statistics for receiver"""
    packets_received: int = 0
    packets_processed: int = 0
    packets_dropped: int = 0
    invalid_identifier: int = 0
    invalid_crc: int = 0
    invalid_size: int = 0
    errors: int = 0
    last_sequence: int = -1
    last_receive_time: Optional[datetime] = None
    # Counted apart from the ATL totals above. Mixing them would hide the case
    # this receiver has actually been in for the whole project: PSF running in
    # proximity mode while every packet is discarded as a bad identifier, with
    # packets_processed sitting at zero and no counter saying why.
    proximity_received: int = 0
    proximity_invalid_crc: int = 0
    proximity_last_sequence: int = -1
    proximity_last_command: Optional[str] = None


class SafetyReceiver:
    """
    64-byte UDP Receiver for ATL Safety Commands (HOISA v1.2).

    Packet format:
      [0]       Identifier 0xA2
      [1-2]     Sequence (uint16 LE)
      [3]       Command opcode
      [4-11]    UTC seconds (uint64 LE)
      [12-19]   UTC microseconds (uint64 LE)
      [20-23]   CRC-32 (uint32 LE, ISO 3309)
      [24-43]   Object Record 0 (20 bytes)
      [44-63]   Object Record 1 (20 bytes)

    Usage:
        receiver = SafetyReceiver(port=12345)
        receiver.start()
        cmd = receiver.get_command()
        receiver.stop()
    """

    PACKET_SIZE = COMMAND_PACKET_SIZE  # 64

    def __init__(
        self,
        port: int = 12345,
        host: str = "0.0.0.0",
        callback: Optional[Callable[[SafetyCommand], None]] = None,
        queue_size: int = 100,
        send_ack: bool = True,
        verify_crc: bool = True,
        config: Optional[UdpReceiverConfig] = None,
        proximity_callback: Optional[Callable[["ProximityCmdPacket"], None]] = None,
    ):
        if config:
            self.host = config.host
            self.port = config.port
        else:
            self.host = host
            self.port = port

        self.callback = callback
        self.send_ack = send_ack
        self.verify_crc = verify_crc
        # Deliberately NOT the same callback as `callback`. A consumer written
        # against SafetyCommand reads `.status` as MUTED/ACTIVE, which for a
        # proximity packet would invert the meaning of 0x02 and 0x07 — see the
        # collision table in common/proximity_commands.py. Anyone wanting
        # proximity has to opt in and accept a ProximityCmdPacket.
        self.proximity_callback = proximity_callback

        self._socket: Optional[socket.socket] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._queue: "Queue[SafetyCommand]" = Queue(maxsize=queue_size)
        self._stats = ReceiverStats()
        self._lock = threading.Lock()

    # ---------------- lifecycle ----------------

    def start(self, blocking: bool = False) -> None:
        if self._running:
            logger.warning("Receiver already running")
            return
        self._setup_socket()
        self._running = True
        if blocking:
            self._receive_loop()
        else:
            self._thread = threading.Thread(target=self._receive_loop, daemon=True)
            self._thread.start()
            logger.info(f"Receiver started in background on port {self.port}")

    def stop(self) -> None:
        self._running = False
        if self._socket:
            try:
                self._socket.close()
            except Exception:
                pass
            self._socket = None
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2)
        logger.info("Receiver stopped")

    def get_command(self, timeout: Optional[float] = None) -> Optional[SafetyCommand]:
        try:
            return self._queue.get(timeout=timeout)
        except Exception:
            return None

    @property
    def stats(self) -> ReceiverStats:
        return self._stats

    @property
    def is_running(self) -> bool:
        return self._running

    # ---------------- internals ----------------

    def _setup_socket(self) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._socket.bind((self.host, self.port))
        self._socket.settimeout(1.0)
        logger.info(f"Socket bound to {self.host}:{self.port}")

    def _receive_loop(self) -> None:
        # Buffer larger than expected packet to detect oversized input
        buf_size = self.PACKET_SIZE * 2
        logger.info(f"Listening for {self.PACKET_SIZE}B safety commands on port {self.port}...")

        while self._running:
            try:
                data, addr = self._socket.recvfrom(buf_size)

                # Must be exactly 64 bytes — reject truncated or oversized
                if len(data) != self.PACKET_SIZE:
                    self._stats.invalid_size += 1
                    logger.warning(f"Invalid packet size: {len(data)} bytes (expected {self.PACKET_SIZE})")
                    continue

                # Identifier selects the DECODER, before any opcode is read.
                # Both packet types are 64 bytes with identical field offsets,
                # so nothing downstream can tell them apart once the header is
                # parsed — byte 0 is the only discriminator that exists.
                if data[0] == PROXIMITY_PACKET_IDENTIFIER:
                    self._process_proximity(data, addr)
                    continue

                pkt = CmdPacket.unpack(data, verify_crc=self.verify_crc)

                if pkt is None:
                    if data[0] != ATL_PACKET_IDENTIFIER:
                        self._stats.invalid_identifier += 1
                        logger.warning(
                            f"Bad identifier: 0x{data[0]:02X} "
                            f"(expected 0x{ATL_PACKET_IDENTIFIER:02X} ATL "
                            f"or 0x{PROXIMITY_PACKET_IDENTIFIER:02X} proximity)")
                    else:
                        self._stats.invalid_crc += 1
                        logger.warning("CRC mismatch — packet discarded")
                    continue

                cmd = SafetyCommand.from_packet(pkt, source_ip=addr[0])
                self._process_command(cmd, addr, pkt)

            except socket.timeout:
                continue
            except OSError as e:
                if self._running:
                    logger.error(f"Socket error: {e}")
                break
            except Exception as e:
                logger.error(f"Receive error: {e}")
                self._stats.errors += 1

    def _process_command(self, command: SafetyCommand, addr: tuple, pkt: CmdPacket) -> None:
        with self._lock:
            self._stats.packets_received += 1
            self._stats.packets_processed += 1
            self._stats.last_sequence = command.sequence_number
            self._stats.last_receive_time = datetime.now()

        if self.send_ack:
            self._send_ack(pkt, addr)

        try:
            self._queue.put_nowait(command)
        except Full:
            self._stats.packets_dropped += 1
            logger.warning("Queue full, dropping packet")

        if self.callback:
            try:
                self.callback(command)
            except Exception as e:
                logger.error(f"Callback error: {e}")

        logger.info(
            f"Received: Seq#{command.sequence_number} | "
            f"{command.command.description} | "
            f"{command.status.emoji} {command.status.description} | ts={command.timestamp_iso}"
        )

    def _process_proximity(self, data: bytes, addr: tuple) -> None:
        """Decode a 0xA5 packet, ACK it, log the pair, hand it to the callback.

        Kept off the ATL queue and out of the ATL callback on purpose. Those
        carry a SafetyCommand whose `status` is MUTED/ACTIVE, and a proximity
        STOP mapped onto that vocabulary reads as "safety muted, loading
        allowed" — the exact inversion this split exists to prevent.
        """
        pkt = ProximityCmdPacket.unpack(data, verify_crc=self.verify_crc)
        if pkt is None:
            with self._lock:
                self._stats.proximity_invalid_crc += 1
            logger.warning("Proximity CRC mismatch — packet discarded")
            return

        is_known = isinstance(pkt.command, ProximityCommandCode)
        with self._lock:
            self._stats.proximity_received += 1
            self._stats.proximity_last_sequence = pkt.seq
            self._stats.proximity_last_command = (
                pkt.command.name if is_known else f"0x{int(pkt.command):02X}")
            self._stats.last_receive_time = datetime.now()

        if self.send_ack:
            try:
                self._socket.sendto(pkt.build_ack().pack(), addr)
            except Exception as e:
                logger.error(f"Proximity ACK send error: {e}")

        if not is_known:
            # Not coerced to a motion level. An unrecognised opcode is a version
            # mismatch between this decoder and PSF, and guessing at it is how a
            # controller ends up acting on a command nobody defined.
            logger.warning(f"Proximity Seq#{pkt.seq}: unknown opcode "
                           f"0x{int(pkt.command):02X} — not published")
            return

        # Heartbeats are the steady state at 10 Hz and would bury everything
        # else; only the motion levels and the error/handshake codes are worth a
        # line. The object dump rides along because the coordinates are the only
        # route to per-robot attribution: `metadata` collapses every machine
        # onto one ObjectType value, so x/y is what distinguishes them.
        if pkt.command != ProximityCommandCode.HEARTBEAT:
            logger.info(
                f"Proximity: Seq#{pkt.seq} | {pkt.command.description} | "
                f"{pkt.describe_objects()} | ts={pkt.timestamp_iso}")

        if self.proximity_callback:
            try:
                self.proximity_callback(pkt)
            except Exception as e:
                logger.error(f"Proximity callback error: {e}")

    def _send_ack(self, received: CmdPacket, addr: tuple) -> None:
        """ACK echoes original seq+command with fresh timestamp + recomputed CRC."""
        try:
            ack_pkt = received.build_ack()
            self._socket.sendto(ack_pkt.pack(), addr)
        except Exception as e:
            logger.error(f"ACK send error: {e}")


# ---------------- standalone entry point ----------------

def main():
    import argparse

    parser = argparse.ArgumentParser(description="64B UDP Safety Receiver (HOISA v1.2)")
    parser.add_argument("-p", "--port", type=int, default=12345, help="UDP port")
    parser.add_argument("--no-ack", action="store_true", help="Disable ACK responses")
    parser.add_argument("--no-crc", action="store_true", help="Skip CRC validation (debug only)")
    args = parser.parse_args()

    print("""
╔══════════════════════════════════════════════════════════╗
║   UDP Safety Receiver — 64-byte packet (HOISA v1.2)      ║
║   Receiving commands from PSF decision system            ║
╚══════════════════════════════════════════════════════════╝
""")

    receiver = SafetyReceiver(
        port=args.port,
        send_ack=not args.no_ack,
        verify_crc=not args.no_crc,
    )

    try:
        receiver.start(blocking=True)
    except KeyboardInterrupt:
        print("\nShutting down...")
        receiver.stop()
        print(f"\nStats: {receiver.stats}")


if __name__ == '__main__':
    main()
