#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Simple 64-byte packet sender for testing Black Channel Layer (HOISA v1.2).

Usage:
    python send_packet.py                               # Default: seq=1, cmd=MUTE, port=12345
    python send_packet.py 1 2                           # Seq#1, cmd=2 (MUTE)
    python send_packet.py 5 7 12346                     # Seq#5, cmd=7 (UNMUTE) to port 12346
    python send_packet.py --loop                        # Alternate MUTE/UNMUTE continuously
    python send_packet.py --bad-crc                     # Negative test: corrupt CRC
    python send_packet.py --bad-id                      # Negative test: wrong identifier
    python send_packet.py 9 0 12346 --proximity stop --machine 40,7.8,-13.4 --person 13,9.2,-12.1
                                                        # 0xA5 proximity packet, Seq#9
"""

import argparse
import os
import sys
import socket
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from common.safety_commands import CmdPacket, CommandCode, ObjectRecord
from common.proximity_commands import ObjectClass, ProximityCmdPacket, ProximityCommandCode

PROXIMITY_COMMANDS = {
    "stop": ProximityCommandCode.STOP,
    "reduce": ProximityCommandCode.REDUCE,
    "normal": ProximityCommandCode.NORMAL,
    "heartbeat": ProximityCommandCode.HEARTBEAT,
    "hw_error": ProximityCommandCode.HW_ERROR,
    "sw_error": ProximityCommandCode.SW_ERROR,
}


def _object_arg(metadata: int):
    """argparse type for ID,X,Y[,Z] — one proximity object record."""
    def parse(text: str) -> ObjectRecord:
        parts = text.split(",")
        if len(parts) not in (3, 4):
            raise argparse.ArgumentTypeError(f"expected ID,X,Y[,Z], got {text!r}")
        try:
            oid, *xyz = int(parts[0]), *map(float, parts[1:])
        except ValueError as e:
            raise argparse.ArgumentTypeError(f"{text!r}: {e}")
        x, y, z = (xyz + [0.0])[:3]
        return ObjectRecord(object_id=oid, x=x, y=y, z=z, metadata=metadata)
    return parse


def send_packet(seq: int, cmd, port: int = 12345,
                host: str = "127.0.0.1",
                bad_identifier: bool = False,
                bad_crc: bool = False,
                objects: list = None) -> None:
    # Byte 0 follows the opcode's type: 0xA5 for a ProximityCommandCode, 0xA2 otherwise.
    packet_cls = ProximityCmdPacket if isinstance(cmd, ProximityCommandCode) else CmdPacket
    pkt = packet_cls.now(seq=seq, command=cmd, objects=objects or [])
    data = pkt.pack()

    if bad_identifier:
        data = bytes([0x00]) + data[1:]
    if bad_crc:
        data = data[:20] + bytes([0xFF, 0xFF, 0xFF, 0xFF]) + data[24:]

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(data, (host, port))
        flags = []
        if bad_identifier:
            flags.append("bad-id")
        if bad_crc:
            flags.append("bad-crc")
        tag = f" [{','.join(flags)}]" if flags else ""
        print(f"Sent: Seq#{seq} | {cmd.description} | size={len(data)}B → {host}:{port}{tag}")
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser(description="Send 64-byte ATL or proximity command packet")
    parser.add_argument("seq", nargs="?", type=int, default=1, help="Sequence number (default: 1)")
    parser.add_argument("cmd", nargs="?", type=int, default=2,
                        help="Command opcode: 0=HEARTBEAT, 1=HW_ERROR, 2=MUTE, 3=SW_ERROR, 7=UNMUTE (default: 2)")
    parser.add_argument("port", nargs="?", type=int, default=12345, help="UDP port (default: 12345)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--loop", action="store_true",
                        help="Alternate MUTE/UNMUTE every interval")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--bad-id", action="store_true", help="Send with wrong identifier")
    parser.add_argument("--bad-crc", action="store_true", help="Send with wrong CRC")
    parser.add_argument("--with-objects", action="store_true",
                        help="Populate 2 sample object records")
    parser.add_argument("--proximity", choices=sorted(PROXIMITY_COMMANDS),
                        help="Send a 0xA5 proximity packet with this opcode instead "
                             "(the positional cmd is ignored)")
    parser.add_argument("--machine", type=_object_arg(ObjectClass.OBJECT),
                        help="Proximity object record 0: ID,X,Y[,Z]")
    parser.add_argument("--person", type=_object_arg(ObjectClass.PERSON),
                        help="Proximity object record 1: ID,X,Y[,Z]")
    args = parser.parse_args()

    if args.proximity:
        if args.loop or args.with_objects:
            parser.error("--proximity does not combine with --loop or --with-objects")
        objects = [args.machine or ObjectRecord(), args.person or ObjectRecord()]
        send_packet(args.seq, PROXIMITY_COMMANDS[args.proximity], port=args.port,
                    host=args.host, bad_identifier=args.bad_id, bad_crc=args.bad_crc,
                    objects=objects)
        return
    if args.machine or args.person:
        parser.error("--machine / --person need --proximity")

    print("""
╔══════════════════════════════════════════════════════════════╗
║        64B ATL Command Packet Sender (HOISA v1.2)           ║
╚══════════════════════════════════════════════════════════════╝
""")

    objects = None
    if args.with_objects:
        objects = [
            ObjectRecord(object_id=1, x=1.2, y=3.4, z=0.0, metadata=0x01),
            ObjectRecord(object_id=2, x=5.6, y=7.8, z=0.0, metadata=0x02),
        ]

    if args.loop:
        print(f"Sending alternating MUTE/UNMUTE every {args.interval}s → {args.host}:{args.port}")
        seq = 0
        commands = [CommandCode.MUTE, CommandCode.UNMUTE]
        try:
            while True:
                send_packet(seq, commands[seq % 2], port=args.port, host=args.host,
                            bad_identifier=args.bad_id, bad_crc=args.bad_crc, objects=objects)
                seq += 1
                time.sleep(args.interval)
        except KeyboardInterrupt:
            print("\nStopped")
        return

    try:
        cmd = CommandCode(args.cmd)
    except ValueError:
        print(f"ERROR: unknown command code {args.cmd}. Valid: 0, 1, 2, 3, 7")
        sys.exit(1)

    send_packet(args.seq, cmd, port=args.port, host=args.host,
                bad_identifier=args.bad_id, bad_crc=args.bad_crc, objects=objects)


if __name__ == '__main__':
    main()
