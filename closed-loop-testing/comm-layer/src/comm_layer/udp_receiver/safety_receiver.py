# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
UDP Safety Command Receiver — 64-Byte ATL Packet (HOISA v1.2)

Receives 64-byte safety commands from PSF via UDP (Black Channel / Comm Layer).

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

                pkt = CmdPacket.unpack(data, verify_crc=self.verify_crc)

                if pkt is None:
                    if data[0] != ATL_PACKET_IDENTIFIER:
                        self._stats.invalid_identifier += 1
                        logger.warning(f"Bad identifier: 0x{data[0]:02X} (expected 0x{ATL_PACKET_IDENTIFIER:02X})")
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
