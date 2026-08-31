# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
OPC UA Server (Non-Safe Black Channel)

Exposes commands via OPC UA protocol (NOT OPC UA Safety).
This is a simple data exchange mechanism, not safety-certified.

IMPORTANT:
- This is NOT "OPC UA Safety" (which doesn't exist in open-source OPC UA)
- This is NOT a safety protocol - just black channel communication
- Real safety protocols (ESL/FSoE/Profisafe) are future work

Based on architecture diagram:
- Receives commands from UDP receiver (simple, non-safe)
- Exposes as OPC UA nodes for data exchange
- Clients (like ROS2 Bridge) read from these nodes
"""

import logging
import threading
import time
import json
from datetime import datetime
from queue import Queue, Empty
from typing import Optional

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from common.safety_commands import SafetyCommand, CommandCode, SafetyStatus
from common.proximity_commands import ProximityCmdPacket, ProximityCommandCode, classify
from common.config import OpcUaConfig, get_config

# Setup logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Suppress verbose asyncua library logging
logging.getLogger('asyncua').setLevel(logging.WARNING)
logging.getLogger('asyncua.server').setLevel(logging.WARNING)
logging.getLogger('asyncua.server.address_space').setLevel(logging.ERROR)
logging.getLogger('asyncua.server.internal_server').setLevel(logging.WARNING)
logging.getLogger('asyncua.server.binary_server_asyncio').setLevel(logging.WARNING)
logging.getLogger('asyncua.server.uaprocessor').setLevel(logging.WARNING)
logging.getLogger('asyncua.uaprotocol').setLevel(logging.WARNING)

# Try to import asyncua library (using sync wrapper for backward compatibility)
# Note: Using asyncua instead of opcua to fix CVE-2022-25304
try:
    from asyncua.sync import Server
    from asyncua import ua
    HAS_OPCUA = True
except ImportError:
    HAS_OPCUA = False
    logger.warning("asyncua library not installed. Install with: pip install asyncua")


class SafetyOpcUaServer:
    """
    OPC UA Server (Non-Safe)
    
    Exposes commands as OPC UA nodes for data exchange.
    NOT a safety protocol - just black channel communication.
    
    OPC UA nodes exposed:
    - Safety.Command: Current command code (int)
    - Safety.CommandName: Command name (string)
    - Safety.Sequence: Sequence number (int)
    - Safety.Status: Safety status (int)
    - Safety.StatusName: Status description (string)
    - Safety.Timestamp: Last update timestamp (string)
    - Safety.IsAlarm: Whether alarm is active (bool)

    Proximity (0xA5) lives on its own nodes and shares none of the above:
    - Safety.ProximityCommand / ProximityCommandName: opcode + description
    - Safety.ProximityMode: "stop" | "reduce_speed" | "normal", "" if not a
      motion decision
    - Safety.ProximitySeparation: ground-plane metres, -1.0 if a slot is unfilled
    - Safety.ProximityStateJson: atomic snapshot incl. both ObjectRecords
    
    Usage:
        from udp_receiver import SafetyReceiver
        from opc_ua import SafetyOpcUaServer
        
        receiver = SafetyReceiver(port=12345)
        opc_server = SafetyOpcUaServer(input_queue=receiver._queue)
        
        receiver.start()
        opc_server.start()
    """
    
    def __init__(
        self,
        input_queue: Optional[Queue] = None,
        endpoint: str = "opc.tcp://0.0.0.0:4840/safety/",
        server_name: str = "Safety OPC UA Server",
        namespace: str = "http://nvidia.com/safety",
        config: Optional[OpcUaConfig] = None,
        proximity_queue: Optional[Queue] = None
    ):
        """
        Initialize OPC UA server
        
        Args:
            input_queue: Queue to receive SafetyCommand objects from
            endpoint: OPC UA endpoint URL
            server_name: Server name
            namespace: OPC UA namespace
            config: Optional OpcUaConfig object
            proximity_queue: Queue of ProximityCmdPacket. SEPARATE from
                input_queue on purpose: 0x02 is STOP here and MUTE there, so a
                proximity packet drained through update_command() would write
                IsMuted=True for a person inside 1 m. See the collision table in
                common/proximity_commands.py.
        """
        if not HAS_OPCUA:
            raise ImportError("asyncua library not installed. Install with: pip install asyncua")
        
        if config:
            self.endpoint = config.endpoint
            self.server_name = config.server_name
            self.namespace = config.namespace
        else:
            self.endpoint = endpoint
            self.server_name = server_name
            self.namespace = namespace
        
        self.input_queue = input_queue or Queue()
        self.proximity_queue = proximity_queue
        
        self._server: Optional[Server] = None
        self._running = False
        self._thread: Optional[threading.Thread] = None
        
        # OPC UA nodes
        self._nodes = {}
        self._last_command: Optional[SafetyCommand] = None
        self._last_proximity: Optional[ProximityCmdPacket] = None
    
    def start(self, blocking: bool = False):
        """Start the OPC UA server"""
        if self._running:
            logger.warning("OPC UA server already running")
            return
        
        self._setup_server()
        self._running = True
        
        if blocking:
            self._run_loop()
        else:
            self._thread = threading.Thread(target=self._run_loop, daemon=True)
            self._thread.start()
            logger.info(f"OPC UA server started at {self.endpoint}")
    
    def stop(self):
        """Stop the OPC UA server"""
        self._running = False
        
        if self._server:
            try:
                self._server.stop()
            except:
                pass
            self._server = None
        
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2)
        
        logger.info("OPC UA server stopped")
    
    def update_command(self, command: SafetyCommand):
        """Update OPC UA nodes with new command"""
        if not self._running or not self._nodes:
            return
        
        try:
            # asyncua uses write_value() with ua.Variant for strict type checking
            self._nodes['command'].write_value(
                ua.Variant(int(command.command.value), ua.VariantType.Int32)
            )
            self._nodes['command_name'].write_value(
                ua.Variant(str(command.command.description), ua.VariantType.String)
            )
            self._nodes['sequence'].write_value(
                ua.Variant(int(command.sequence_number), ua.VariantType.Int32)
            )
            self._nodes['status'].write_value(
                ua.Variant(int(command.status.value), ua.VariantType.Int32)
            )
            self._nodes['status_name'].write_value(
                ua.Variant(str(command.status.description), ua.VariantType.String)
            )
            self._nodes['timestamp'].write_value(
                ua.Variant(f"{command.timestamp}.{command.microseconds}", ua.VariantType.String)
            )
            self._nodes['is_alarm'].write_value(
                ua.Variant(command.command == CommandCode.UNMUTE, ua.VariantType.Boolean)
            )
            self._nodes['is_muted'].write_value(
                ua.Variant(command.command == CommandCode.MUTE, ua.VariantType.Boolean)
            )
            self._nodes['last_update'].write_value(
                ua.Variant(datetime.now().isoformat(), ua.VariantType.String)
            )
            # Atomic commit point: one coherent snapshot the bridge reads as a unit.
            # Written AFTER the per-field nodes so its content is always consistent.
            self._nodes['state_json'].write_value(
                ua.Variant(json.dumps({
                    'sequence': int(command.sequence_number),
                    'command': int(command.command.value),
                    'command_name': str(command.command.description),
                    'status': int(command.status.value),
                    'status_name': str(command.status.description),
                    'timestamp': f"{command.timestamp}.{command.microseconds}",
                    'last_update': datetime.now().isoformat(),
                }), ua.VariantType.String)
            )

            self._last_command = command
            logger.debug(f"Updated OPC UA nodes: {command}")
            
        except Exception as e:
            logger.error(f"Failed to update OPC UA nodes: {e}")

    @staticmethod
    def _object_payload(role: str, obj) -> dict:
        """One ObjectRecord as JSON, with `empty` kept rather than dropped.

        An unfilled slot decodes to id=0 at (0, 0), which is a legal position in
        this scene's frame, so a consumer cannot tell it apart from a real object
        at the origin. fillObjectRecords() memsets all 40 bytes on the periodic
        re-assert path (request == nullptr), so this happens on every repeat of a
        held command, not just at startup.
        """
        return {
            'role': role,
            'id': int(obj.object_id),
            'x': float(obj.x),
            'y': float(obj.y),
            'z': float(obj.z),
            'class': classify(obj.metadata),
            'empty': bool(obj.is_empty),
        }

    def update_proximity(self, packet: ProximityCmdPacket):
        """Update the proximity OPC UA nodes from a 0xA5 packet.

        Writes NOTHING on the ATL nodes. IsMuted/IsAlarm are defined by the ATL
        opcode table and proximity inverts two of its entries, so the two command
        sets share this server but never share a node.
        """
        if not self._running or not self._nodes:
            return

        try:
            cmd = packet.command
            is_enum = isinstance(cmd, ProximityCommandCode)
            code = int(cmd)
            name = cmd.description if is_enum else f"UNKNOWN (0x{code:02X})"
            # None for heartbeats, error codes and the safe-release handshake.
            # Deliberately NOT coerced to "normal": only the three motion levels
            # may drive a robot, and a heartbeat is not a permission to move.
            mode = cmd.mode if is_enum else None
            sep = packet.separation_m

            self._nodes['proximity_command'].write_value(
                ua.Variant(code, ua.VariantType.Int32)
            )
            self._nodes['proximity_command_name'].write_value(
                ua.Variant(str(name), ua.VariantType.String)
            )
            # Empty string, not "unknown": the mode topic must stay a closed set
            # of three values, so absence is spelled as absence.
            self._nodes['proximity_mode'].write_value(
                ua.Variant(str(mode or ""), ua.VariantType.String)
            )
            self._nodes['proximity_separation'].write_value(
                ua.Variant(float(sep) if sep is not None else -1.0,
                           ua.VariantType.Double)
            )
            # Atomic commit point, same contract as StateJson: written LAST so a
            # reader that takes this one node never pairs a fresh sequence with a
            # stale pair. The bridge reads only this node.
            self._nodes['proximity_state_json'].write_value(
                ua.Variant(json.dumps({
                    'sequence': int(packet.seq),
                    'command': code,
                    'command_name': str(name),
                    'mode': mode,
                    'separation_m': sep,
                    'safety_critical': bool(cmd.is_safety_critical) if is_enum else True,
                    'objects': [
                        self._object_payload('machine', packet.machine),
                        self._object_payload('person', packet.person),
                    ],
                    'timestamp': packet.timestamp_iso,
                    'last_update': datetime.now().isoformat(),
                }), ua.VariantType.String)
            )

            self._last_proximity = packet
            logger.debug(f"Updated proximity OPC UA nodes: {packet}")

        except Exception as e:
            logger.error(f"Failed to update proximity OPC UA nodes: {e}")

    @property
    def is_running(self) -> bool:
        """Check if server is running"""
        return self._running

    @property
    def last_proximity(self) -> Optional[ProximityCmdPacket]:
        """Get last received proximity packet"""
        return self._last_proximity
    
    @property
    def last_command(self) -> Optional[SafetyCommand]:
        """Get last received command"""
        return self._last_command
    
    def _setup_server(self):
        """Setup OPC UA server and nodes"""
        self._server = Server()
        self._server.set_endpoint(self.endpoint)
        self._server.set_server_name(self.server_name)
        
        # Register namespace
        idx = self._server.register_namespace(self.namespace)
        
        # Get objects node (asyncua uses nodes.objects instead of get_objects_node())
        objects = self._server.nodes.objects
        
        # Create Safety folder
        safety_folder = objects.add_folder(idx, "Safety")
        
        # Create nodes
        self._nodes['command'] = safety_folder.add_variable(
            idx, "Command", 0, ua.VariantType.Int32
        )
        self._nodes['command_name'] = safety_folder.add_variable(
            idx, "CommandName", "UNKNOWN", ua.VariantType.String
        )
        self._nodes['sequence'] = safety_folder.add_variable(
            idx, "Sequence", 0, ua.VariantType.Int32
        )
        self._nodes['status'] = safety_folder.add_variable(
            idx, "Status", 0, ua.VariantType.Int32
        )
        self._nodes['status_name'] = safety_folder.add_variable(
            idx, "StatusName", "UNKNOWN", ua.VariantType.String
        )
        self._nodes['timestamp'] = safety_folder.add_variable(
            idx, "Timestamp", "", ua.VariantType.String
        )
        self._nodes['is_alarm'] = safety_folder.add_variable(
            idx, "IsAlarm", False, ua.VariantType.Boolean
        )
        self._nodes['is_muted'] = safety_folder.add_variable(
            idx, "IsMuted", False, ua.VariantType.Boolean
        )
        self._nodes['last_update'] = safety_folder.add_variable(
            idx, "LastUpdate", "", ua.VariantType.String
        )
        # Atomic snapshot: the ROS bridge reads this ONE node so it can never
        # pair a fresh sequence with a stale command (NVBug 6512051 tear).
        self._nodes['state_json'] = safety_folder.add_variable(
            idx, "StateJson", "", ua.VariantType.String
        )

        # --- Proximity (0xA5) ------------------------------------------------
        # Flat under Safety rather than in a sub-folder: the ROS bridge builds its
        # node map from safety_folder.get_children() and calls read_value() on each
        # child, so a folder child would raise on every poll.
        #
        # Disjoint from the ATL nodes above, not an extension of them. Sharing
        # IsMuted would publish "loading allowed" for a person inside 1 m, because
        # proximity STOP and ATL MUTE are both opcode 0x02.
        self._nodes['proximity_command'] = safety_folder.add_variable(
            idx, "ProximityCommand", 0, ua.VariantType.Int32
        )
        self._nodes['proximity_command_name'] = safety_folder.add_variable(
            idx, "ProximityCommandName", "UNKNOWN", ua.VariantType.String
        )
        # One of "stop" / "reduce_speed" / "normal", or "" for anything that is not
        # a motion decision (heartbeat, HW/SW error, safe-release handshake).
        self._nodes['proximity_mode'] = safety_folder.add_variable(
            idx, "ProximityMode", "", ua.VariantType.String
        )
        # Reconstructed ground-plane distance, -1.0 when either slot is unfilled.
        # PSF scores with dz but sends z = 0, so this is not the figure it decided on.
        self._nodes['proximity_separation'] = safety_folder.add_variable(
            idx, "ProximitySeparation", -1.0, ua.VariantType.Double
        )
        self._nodes['proximity_state_json'] = safety_folder.add_variable(
            idx, "ProximityStateJson", "", ua.VariantType.String
        )

        # Make nodes readable
        for node in self._nodes.values():
            node.set_writable()
        
        logger.info(f"OPC UA nodes created under namespace {idx}")
    
    def _run_loop(self):
        """Main server loop"""
        try:
            self._server.start()
            logger.info(f"OPC UA server listening at {self.endpoint}")
            
            while self._running:
                # Both queues are drained non-blocking, then the loop sleeps only
                # when both were empty. A blocking get() on one queue would add its
                # timeout to the other's latency, and proximity arrives at up to
                # 10 Hz while ATL is silent for minutes at a time.
                idle = True

                try:
                    command = self.input_queue.get_nowait()
                    self.update_command(command)
                    idle = False
                except Empty:
                    pass
                except Exception as e:
                    logger.error(f"Error processing command: {e}")

                if self.proximity_queue is not None:
                    try:
                        packet = self.proximity_queue.get_nowait()
                        self.update_proximity(packet)
                        idle = False
                    except Empty:
                        pass
                    except Exception as e:
                        logger.error(f"Error processing proximity packet: {e}")

                if idle:
                    time.sleep(0.01)
                
        except Exception as e:
            logger.error(f"OPC UA server error: {e}")
        finally:
            if self._server:
                self._server.stop()


def main():
    """Standalone entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(description='OPC UA Server (Non-Safe)')
    parser.add_argument('-e', '--endpoint', default='opc.tcp://0.0.0.0:4840/safety/', help='OPC UA endpoint')
    parser.add_argument('-p', '--port', type=int, default=12345, help='UDP port')
    args = parser.parse_args()
    
    print("""
╔══════════════════════════════════════════════════════════╗
║     OPC UA Server (Non-Safe Black Channel)               ║
║     Simple data exchange - NOT OPC UA Safety             ║
║     Exposing commands via OPC UA protocol                ║
╚══════════════════════════════════════════════════════════╝
""")
    
    if not HAS_OPCUA:
        print("WARNING: asyncua library not installed!")
        print("  Install with: pip install asyncua")
        return
    
    # Start UDP receiver to get commands
    from udp_receiver.safety_receiver import SafetyReceiver

    # Proximity packets arrive on the receiver thread. Bounded and non-blocking so
    # a stalled OPC write can never back-pressure the receive loop — that loop also
    # sends the ACKs, and cmd_rx.cpp treats a missing ACK as an unacknowledged
    # command. Dropping the oldest is right for a state signal: only the newest
    # decision is actionable, and PSF re-asserts the held command periodically.
    proximity_queue: Queue = Queue(maxsize=100)

    def on_proximity(packet):
        try:
            proximity_queue.put_nowait(packet)
        except Exception:
            try:
                proximity_queue.get_nowait()
                proximity_queue.put_nowait(packet)
            except Exception:
                logger.warning("Proximity queue full, dropped Seq#%s", packet.seq)

    print(f"\nStarting UDP Receiver on port {args.port}...")
    receiver = SafetyReceiver(port=args.port, proximity_callback=on_proximity)
    receiver.start()
    print(f"UDP Receiver listening on port {args.port}")
    
    # Start OPC UA server
    server = SafetyOpcUaServer(
        input_queue=receiver._queue,
        endpoint=args.endpoint,
        proximity_queue=proximity_queue
    )
    
    try:
        server.start(blocking=True)
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
        receiver.stop()


if __name__ == '__main__':
    main()

