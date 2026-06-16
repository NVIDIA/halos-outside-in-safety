# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Safety ROS2 Bridge

Bridges safety commands to ROS2 topics.
Can receive commands from:
1. OPC UA Server (via OPC UA client)
2. Direct queue from ESL Receiver

Based on architecture diagram:
- Receives commands from Communication Layer (OPC UA)
- Publishes to ROS2 topics
- ROS controlled forklift subscribes to these topics
"""

import logging
import threading
import time
import json
from dataclasses import dataclass
from datetime import datetime
from queue import Queue, Empty
from typing import Optional, Callable

import sys
import os

# Setup logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Suppress verbose asyncua library logging
logging.getLogger('asyncua').setLevel(logging.WARNING)
logging.getLogger('asyncua.client').setLevel(logging.WARNING)
logging.getLogger('asyncua.client.ua_client').setLevel(logging.WARNING)
logging.getLogger('asyncua.uaprotocol').setLevel(logging.WARNING)

# Try to import ROS2
try:
    import rclpy
    from rclpy.node import Node
    from std_msgs.msg import String, Bool, Int32
    HAS_ROS2 = True
except ImportError:
    HAS_ROS2 = False
    logger.warning("ROS2 not available. Running in simulation mode.")

# Try to import OPC UA client (using asyncua.sync for backward compatibility)
# Note: Using asyncua instead of opcua to fix CVE-2022-25304
try:
    from asyncua.sync import Client
    HAS_OPCUA = True
except ImportError:
    HAS_OPCUA = False
    logger.warning("asyncua library not available.")


# Simple dataclass for safety commands (avoid dependency on comm_layer)
@dataclass
class SafetyCommand:
    """Represents a safety command"""
    sequence_number: int
    command_code: int
    command_name: str
    status_code: int
    status_name: str
    timestamp: str = ""
    source: str = "unknown"
    
    @property
    def is_muted(self) -> bool:
        return self.command_code == 2
    
    @property
    def is_alarm(self) -> bool:
        return self.command_code == 7


class SafetyRosBridge:
    """
    Safety ROS2 Bridge
    
    Publishes safety commands to ROS2 topics:
    - /safety/command (String): JSON with full command details
    - /safety/status (Int32): Safety status code (1=MUTED, 2=ACTIVE)
    - /safety/is_alarm (Bool): True if alarm is active
    - /safety/is_muted (Bool): True if safety is muted
    
    Usage:
        # OPC UA mode
        bridge = SafetyRosBridge(opc_ua_url="opc.tcp://localhost:4840/safety/")
        bridge.start()
        
        # Direct mode (from queue)
        bridge = SafetyRosBridge(input_queue=some_queue)
        bridge.start()
    """
    
    def __init__(
        self,
        input_queue: Optional[Queue] = None,
        opc_ua_url: Optional[str] = None,
        topic_prefix: str = "/safety",
        publish_rate_hz: float = 10.0
    ):
        """
        Initialize ROS2 bridge
        
        Args:
            input_queue: Queue to receive commands from (direct mode)
            opc_ua_url: OPC UA server URL (OPC UA mode)
            topic_prefix: ROS2 topic prefix
            publish_rate_hz: Publish rate in Hz
        """
        self.opc_ua_url = opc_ua_url
        self.topic_prefix = topic_prefix
        self.publish_rate = publish_rate_hz
        self.input_queue = input_queue
        
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._last_command: Optional[SafetyCommand] = None
        self._mode = "unknown"
        
        # Track actual muted/alarm state (persists across "No Operation" commands)
        self._current_is_muted = False
        self._current_is_alarm = False
        
        # ROS2 node
        self._node = None
        self._publishers = {}
        
        # OPC UA client
        self._opc_client = None
        
        # Callbacks
        self._command_callbacks = []
    
    def start(self, blocking: bool = False):
        """Start the ROS2 bridge"""
        if self._running:
            logger.warning("ROS2 bridge already running")
            return
        
        # Determine mode
        if self.opc_ua_url and HAS_OPCUA:
            self._mode = "opcua"
            logger.info(f"Starting in OPC UA mode: {self.opc_ua_url}")
        elif self.input_queue:
            self._mode = "direct"
            logger.info("Starting in direct mode (from queue)")
        else:
            self._mode = "simulation"
            logger.warning("No input source, running in simulation mode")
        
        self._setup_ros2()
        self._running = True
        
        if blocking:
            self._run_loop()
        else:
            self._thread = threading.Thread(target=self._run_loop, daemon=True)
            self._thread.start()
            logger.info("ROS2 bridge started in background")
    
    def stop(self):
        """Stop the ROS2 bridge"""
        self._running = False
        
        if self._opc_client:
            try:
                self._opc_client.disconnect()
            except:
                pass
        
        if self._node and HAS_ROS2:
            try:
                self._node.destroy_node()
                rclpy.shutdown()
            except:
                pass
        
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2)
        
        logger.info("ROS2 bridge stopped")
    
    def add_command_callback(self, callback: Callable[[SafetyCommand], None]):
        """Add callback for new commands"""
        self._command_callbacks.append(callback)
    
    def publish_command(self, command: SafetyCommand):
        """Publish command to ROS2 topics"""
        self._last_command = command
        
        # Update tracked state only for actual MUTE/UNMUTE commands (not "No Operation")
        # Command codes: 2 = MUTE, 7 = UNMUTE + ALARM, 0 = No Operation
        if command.command_code == 2:  # MUTE
            self._current_is_muted = True
            self._current_is_alarm = False
        elif command.command_code == 7:  # UNMUTE + ALARM
            self._current_is_muted = False
            self._current_is_alarm = True
        # For "No Operation" (code 0), keep previous state
        
        # Call callbacks with current tracked state
        for callback in self._command_callbacks:
            try:
                callback(command, self._current_is_muted, self._current_is_alarm)
            except TypeError:
                # Fallback for callbacks that don't accept extra args
                try:
                    callback(command)
                except Exception as e:
                    logger.error(f"Callback error: {e}")
            except Exception as e:
                logger.error(f"Callback error: {e}")
        
        if not HAS_ROS2:
            logger.info(f"[Simulation] Would publish: {command}")
            return
        
        try:
            # Publish full command as JSON (use tracked state, not command properties)
            cmd_json = json.dumps({
                'sequence': command.sequence_number,
                'command': command.command_code,
                'command_name': command.command_name,
                'status': command.status_code,
                'status_name': command.status_name,
                'timestamp': command.timestamp,
                'source': command.source,
                'is_muted': self._current_is_muted,
                'is_alarm': self._current_is_alarm,
                'ros_time': datetime.now().isoformat()
            })
            
            msg_command = String()
            msg_command.data = cmd_json
            self._publishers['command'].publish(msg_command)
            
            # Publish status (use tracked state for status too)
            msg_status = Int32()
            # Status: 1=MUTED, 2=ACTIVE+ALARM, 3=NO_CHANGE
            if command.command_code == 0:  # No Operation
                msg_status.data = 3  # No change
            else:
                msg_status.data = command.status_code
            self._publishers['status'].publish(msg_status)
            
            # Publish flags (use tracked state)
            msg_alarm = Bool()
            msg_alarm.data = self._current_is_alarm
            self._publishers['is_alarm'].publish(msg_alarm)
            
            msg_muted = Bool()
            msg_muted.data = self._current_is_muted
            self._publishers['is_muted'].publish(msg_muted)
            
        except Exception as e:
            logger.error(f"Failed to publish to ROS2: {e}")
    
    @property
    def is_running(self) -> bool:
        return self._running
    
    @property
    def last_command(self) -> Optional[SafetyCommand]:
        return self._last_command
    
    def _setup_ros2(self):
        """Setup ROS2 node and publishers"""
        if not HAS_ROS2:
            logger.warning("ROS2 not available, running in simulation mode")
            return
        
        try:
            rclpy.init()
            self._node = rclpy.create_node('safety_ros_bridge')
            
            # Create publishers
            self._publishers['command'] = self._node.create_publisher(
                String, f"{self.topic_prefix}/command", 10
            )
            self._publishers['status'] = self._node.create_publisher(
                Int32, f"{self.topic_prefix}/status", 10
            )
            self._publishers['is_alarm'] = self._node.create_publisher(
                Bool, f"{self.topic_prefix}/is_alarm", 10
            )
            self._publishers['is_muted'] = self._node.create_publisher(
                Bool, f"{self.topic_prefix}/is_muted", 10
            )
            
            logger.info(f"ROS2 publishers created with prefix: {self.topic_prefix}")
            
        except Exception as e:
            logger.error(f"Failed to setup ROS2: {e}")
    
    def _run_loop(self):
        """Main bridge loop"""
        interval = 1.0 / self.publish_rate
        
        if self._mode == "opcua":
            self._run_opcua_loop(interval)
        elif self._mode == "direct":
            self._run_direct_loop(interval)
        else:
            self._run_simulation_loop(interval)
    
    def _run_direct_loop(self, interval: float):
        """Run loop reading from direct queue"""
        logger.info("Running direct queue loop...")
        
        while self._running:
            try:
                cmd = self.input_queue.get(timeout=interval)
                # Convert to SafetyCommand if needed
                if hasattr(cmd, 'command'):
                    command = SafetyCommand(
                        sequence_number=cmd.sequence_number,
                        command_code=cmd.command.value,
                        command_name=cmd.command.description,
                        status_code=cmd.status.value,
                        status_name=cmd.status.description,
                        timestamp=f"{cmd.timestamp}.{cmd.microseconds}",
                        source="direct"
                    )
                else:
                    command = cmd
                self.publish_command(command)
            except Empty:
                pass
            except Exception as e:
                logger.error(f"Error in direct loop: {e}")
            
            if HAS_ROS2 and self._node:
                rclpy.spin_once(self._node, timeout_sec=0.001)
    
    def _run_opcua_loop(self, interval: float):
        """Run loop reading from OPC UA server"""
        logger.info(f"Running OPC UA client loop, connecting to {self.opc_ua_url}...")
        
        opcua_nodes = {}
        
        try:
            self._opc_client = Client(self.opc_ua_url)
            self._opc_client.connect()
            logger.info("Connected to OPC UA server")
            
            # Find Safety folder (asyncua uses nodes.objects and read_browse_name())
            objects = self._opc_client.nodes.objects
            safety_folder = None
            for child in objects.get_children():
                name = child.read_browse_name().Name
                if name == "Safety":
                    safety_folder = child
                    break
            
            if safety_folder is None:
                logger.error("Could not find Safety folder in OPC UA server")
                return
            
            # Get node references
            for child in safety_folder.get_children():
                name = child.read_browse_name().Name
                opcua_nodes[name] = child
            
            logger.info(f"Found {len(opcua_nodes)} OPC UA nodes")
            
        except Exception as e:
            logger.error(f"Failed to connect to OPC UA server: {e}")
            return
        
        last_sequence = -1
        last_timestamp = ""
        error_count = 0
        max_errors = 5
        
        while self._running:
            try:
                # Read values from OPC UA nodes
                command_val = opcua_nodes.get('Command')
                command_name_val = opcua_nodes.get('CommandName')
                sequence_val = opcua_nodes.get('Sequence')
                status_val = opcua_nodes.get('Status')
                status_name_val = opcua_nodes.get('StatusName')
                timestamp_val = opcua_nodes.get('Timestamp')
                last_update_val = opcua_nodes.get('LastUpdate')
                
                if all([command_val, sequence_val, status_val]):
                    cmd_code = command_val.read_value()
                    cmd_name = command_name_val.read_value() if command_name_val else "Unknown"
                    seq = sequence_val.read_value()
                    status_code = status_val.read_value()
                    status_name = status_name_val.read_value() if status_name_val else "Unknown"
                    timestamp = timestamp_val.read_value() if timestamp_val else ""
                    last_update = last_update_val.read_value() if last_update_val else ""
                    
                    error_count = 0
                    
                    # Only publish if there's new data
                    if seq != last_sequence or last_update != last_timestamp:
                        command = SafetyCommand(
                            sequence_number=seq,
                            command_code=cmd_code,
                            command_name=cmd_name,
                            status_code=status_code,
                            status_name=status_name,
                            timestamp=timestamp,
                            source="opcua"
                        )
                        
                        self.publish_command(command)
                        last_sequence = seq
                        last_timestamp = last_update
                
                time.sleep(interval)
                
                if HAS_ROS2 and self._node:
                    rclpy.spin_once(self._node, timeout_sec=0.001)
                    
            except (BrokenPipeError, ConnectionResetError, OSError) as e:
                error_count += 1
                if error_count >= max_errors:
                    logger.error(f"OPC UA connection lost: {e}")
                    break
                time.sleep(1)
            except Exception as e:
                error_count += 1
                if error_count >= max_errors:
                    logger.error(f"Too many errors: {e}")
                    break
                time.sleep(0.5)
    
    def _run_simulation_loop(self, interval: float):
        """Run simulation loop"""
        logger.info("Running in simulation mode...")
        
        seq = 0
        while self._running:
            cmd = SafetyCommand(
                sequence_number=seq % 32,
                command_code=2 if seq % 2 == 0 else 7,
                command_name="MUTE" if seq % 2 == 0 else "UNMUTE",
                status_code=1 if seq % 2 == 0 else 2,
                status_name="Safety muted" if seq % 2 == 0 else "Safety active",
                timestamp=datetime.now().strftime("%H%M%S"),
                source="simulation"
            )
            
            self.publish_command(cmd)
            seq += 1
            time.sleep(2.0)
            
            if HAS_ROS2 and self._node:
                rclpy.spin_once(self._node, timeout_sec=0.001)


def main():
    """Standalone entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(description='Safety ROS2 Bridge')
    parser.add_argument('--opcua', default='opc.tcp://localhost:4840/safety/', help='OPC UA server URL')
    parser.add_argument('--direct', action='store_true', help='Direct mode (start ESL receiver)')
    parser.add_argument('--topic-prefix', default='/safety', help='ROS2 topic prefix')
    parser.add_argument('--rate', type=float, default=10.0, help='Publish rate (Hz)')
    args = parser.parse_args()
    
    print("""
╔══════════════════════════════════════════════════════════╗
║     Safety ROS2 Bridge                                   ║
║     Publishing safety commands to ROS2 topics            ║
╚══════════════════════════════════════════════════════════╝
""")
    
    receiver = None
    
    if args.direct:
        print("WARNING: Direct mode requires comm_layer package")
        print("  Use --opcua mode instead")
        return
    
    # OPC UA mode
    bridge = SafetyRosBridge(
        opc_ua_url=args.opcua,
        topic_prefix=args.topic_prefix,
        publish_rate_hz=args.rate
    )
    
    # Add logging callback (receives tracked state from bridge)
    def log_command(cmd: SafetyCommand, is_muted: bool, is_alarm: bool):
        # Show status with color emoji based on tracked state
        # Simple 1-line format with flush=True for real-time output
        emoji = "🟢" if is_muted else "🟡"
        mute_str = "true" if is_muted else "false"
        alarm_str = "true" if is_alarm else "false"
        # Determine status code based on command
        status_code = 3 if cmd.command_code == 0 else cmd.status_code
        # Short command name
        if cmd.command_code == 0:
            cmd_short = "NOP"
        elif cmd.command_code == 2:
            cmd_short = "MUTE"
        elif cmd.command_code == 7:
            cmd_short = "UNMUTE"
        else:
            cmd_short = f"CMD{cmd.command_code}"
        muted_str = "MUTED" if is_muted else "UNMUTED"
        print(f"ROS2: Seq#{cmd.sequence_number:02d} | {cmd_short:6s} | {emoji} is_muted={is_muted} | State: {muted_str}", flush=True)
    
    bridge.add_command_callback(log_command)
    
    try:
        bridge.start(blocking=True)
    except KeyboardInterrupt:
        print("\nShutting down...")
        bridge.stop()


if __name__ == '__main__':
    main()

