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
import re
import threading
import time
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from queue import Queue, Empty
from typing import List, Optional, Callable

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


# Proximity is a separate type from SafetyCommand for the same reason the OPC
# nodes are separate: `is_muted` above is `command_code == 2`, and 0x02 is STOP
# in the proximity table. Reusing SafetyCommand would publish is_muted=True for a
# person in the STOP tier, on a topic whose consumers read it as "loading allowed".
@dataclass
class ProximityDecision:
    """One 0xA5 decision as it arrives from the OPC snapshot."""
    sequence_number: int
    command_code: int
    command_name: str
    # "stop" | "reduce_speed" | "normal", or None when the packet is a heartbeat,
    # an error code or a safe-release message. None is NOT "normal".
    mode: Optional[str] = None
    separation_m: Optional[float] = None
    safety_critical: bool = False
    objects: Optional[List[dict]] = None
    timestamp: str = ""
    source: str = "unknown"
    # The latest packet of ANY kind, heartbeats included: the subscriber's link
    # watchdog. Changes while `sequence_number` stays on a held decision.
    heard_sequence: Optional[int] = None
    heard_at: str = ""

    @property
    def is_motion_decision(self) -> bool:
        return self.mode in ("stop", "reduce_speed", "normal")


class SafetyRosBridge:
    """
    Safety ROS2 Bridge
    
    Publishes safety commands to ROS2 topics:
    - /safety/command (String): JSON with full command details, including a
      `robot_id` that addresses the command. It is null while PSF decides per
      covered zone rather than per truck, and null is what tells every controller
      the decision is theirs; a name would tell all the others to ignore it.
    - /safety/status (Int32): Safety status code (1=MUTED, 2=ACTIVE)
    - /safety/is_alarm (Bool): True if alarm is active
    - /safety/is_muted (Bool): True if safety is muted
    - /<robot>/safety/is_muted (Bool): same value again, one topic per robot in
      `robot_ids`. PSF reasons about camera-covered zones, not about named
      trucks, so these mirrors are about the shape of the interface, not about
      per-truck decisions — they are all equal to /safety/is_muted.

    Proximity (0xA5) publishes on its own topics and touches none of the four
    above, because opcodes 0x02 and 0x07 carry opposite meanings in the two
    command sets:
    - /safety/proximity/mode (String): "stop" | "reduce_speed" | "normal".
      Published ONLY for those three. A heartbeat, an error code or a
      safe-release message is not a motion decision and must not be turned into
      one, so nothing is published on this topic for them — a consumer holding
      the last value is correct, one reading silence as "normal" is not.
    - /safety/proximity/pair (String): JSON with the full decision, including
      both ObjectRecords (id, x, y, z, class, empty), the reconstructed
      separation, the raw opcode and `safety_critical`. This is the topic to
      read for a fault, and the only place object identity reaches ROS.
    
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
        publish_rate_hz: float = 10.0,
        robot_ids: Optional[List[str]] = None
    ):
        """
        Initialize ROS2 bridge
        
        Args:
            input_queue: Queue to receive commands from (direct mode)
            opc_ua_url: OPC UA server URL (OPC UA mode)
            topic_prefix: ROS2 topic prefix
            publish_rate_hz: Publish rate in Hz
            robot_ids: Robots to mirror is_muted to, as /<id><prefix>/is_muted.
                Empty means only the global topic is published.
        """
        self.opc_ua_url = opc_ua_url
        self.topic_prefix = topic_prefix
        self.publish_rate = publish_rate_hz
        self.input_queue = input_queue
        self.robot_ids = list(robot_ids or [])
        
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._last_command: Optional[SafetyCommand] = None
        self._last_proximity: Optional[ProximityDecision] = None
        self._prox_last_seq = -1
        self._prox_last_update = ""
        self._warned_no_prox_node = False
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
        self._proximity_callbacks = []
    
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

    def add_proximity_callback(self, callback: Callable[["ProximityDecision"], None]):
        """Add callback for new proximity decisions"""
        self._proximity_callbacks.append(callback)

    def publish_proximity(self, decision: "ProximityDecision"):
        """Publish a proximity decision to its own two topics.

        Publishes the pair unconditionally and the mode only for the three motion
        levels. The asymmetry is the point: a consumer subscribed to mode may
        assume every message it receives is a motion decision, and a fault has to
        be visible somewhere the consumer cannot mistake for permission to move.
        """
        # The same decision again, republished because a heartbeat arrived.
        repeat = (self._last_proximity is not None
                  and self._last_proximity.sequence_number == decision.sequence_number)
        self._last_proximity = decision

        for callback in self._proximity_callbacks:
            try:
                callback(decision)
            except Exception as e:
                logger.error(f"Proximity callback error: {e}")

        if not HAS_ROS2:
            logger.info(f"[Simulation] Would publish proximity: {decision}")
            return

        try:
            pair_json = json.dumps({
                'sequence': decision.sequence_number,
                'command': decision.command_code,
                'command_name': decision.command_name,
                'mode': decision.mode,
                'separation_m': decision.separation_m,
                'safety_critical': decision.safety_critical,
                'objects': decision.objects or [],
                # Left null for the same reason as /safety/command: the wire
                # carries a uint32 VSS fusion id, controllers filter on a string
                # robot name, and there is no mapping between them. The ids are in
                # `objects` so a consumer can build attribution from x/y; putting a
                # guess here would tell every other controller to ignore a STOP.
                'robot_id': None,
                'timestamp': decision.timestamp,
                'heard_sequence': decision.heard_sequence,
                'heard_at': decision.heard_at,
                'source': decision.source,
                'ros_time': datetime.now(timezone.utc).isoformat(),
            })
            msg_pair = String()
            msg_pair.data = pair_json
            self._publishers['proximity_pair'].publish(msg_pair)

            if decision.is_motion_decision:
                msg_mode = String()
                msg_mode.data = decision.mode
                self._publishers['proximity_mode'].publish(msg_mode)
            elif decision.safety_critical and not repeat:
                # HW_ERROR / SW_ERROR. Not mapped to a mode here: choosing what a
                # faulted PSF means for a moving robot is a safety decision, and it
                # belongs to the consumer that owns the drive, not to this bridge.
                logger.warning(
                    "Proximity fault opcode 0x%02X (%s) — no mode published; "
                    "read /safety/proximity/pair for the fault",
                    decision.command_code, decision.command_name
                )

        except Exception as e:
            logger.error(f"Failed to publish proximity to ROS2: {e}")

    @property
    def last_proximity(self) -> Optional["ProximityDecision"]:
        return self._last_proximity
    
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
            #
            # robot_id addresses the command; null means every truck. That is what
            # robot_controller._command_callback already does with it — a command
            # naming someone else is dropped, one naming nobody is acted on — so
            # null is the honest value while PSF decides per covered zone rather
            # than per named truck. It is spelled out rather than omitted to give
            # per-truck routing one place to be substituted, needing no controller
            # change. Do not fill it from self.robot_ids: that list is who mirrors
            # is_muted, not who a decision is about, and naming one truck here
            # would make every other controller ignore the decision.
            #
            # No traffic routes on it yet either way: `command` below is an integer
            # opcode, and the controller acts only on string keywords.
            cmd_json = json.dumps({
                'sequence': command.sequence_number,
                'command': command.command_code,
                'command_name': command.command_name,
                'status': command.status_code,
                'status_name': command.status_name,
                'timestamp': command.timestamp,
                'source': command.source,
                'robot_id': None,
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
            
            for robot_id in self.robot_ids:
                self._publishers[f"is_muted:{robot_id}"].publish(msg_muted)
            
        except Exception as e:
            logger.error(f"Failed to publish to ROS2: {e}")
    
    @property
    def is_running(self) -> bool:
        return self._running
    
    @property
    def last_command(self) -> Optional[SafetyCommand]:
        return self._last_command
    
    def _robot_muted_topic(self, robot_id: str) -> str:
        """Per-robot mirror of the global is_muted topic.
        
        The prefix is kept so a deployment that renames /safety renames both the
        global topic and the mirrors together. Isaac derives the same name from
        the robot's `name` in robots.yaml, so the string is spelled out once on
        each side and never typed into a config file.
        """
        return f"/{robot_id}{self.topic_prefix}/is_muted"
    
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
            # Proximity: own namespace, own types. Nothing here mirrors is_muted —
            # a three-level motion decision has no faithful Bool encoding, and
            # squeezing REDUCE into one would lose the level that exists to keep a
            # machine moving slowly rather than stopping it.
            self._publishers['proximity_mode'] = self._node.create_publisher(
                String, f"{self.topic_prefix}/proximity/mode", 10
            )
            self._publishers['proximity_pair'] = self._node.create_publisher(
                String, f"{self.topic_prefix}/proximity/pair", 10
            )
            
            logger.info(f"ROS2 publishers created with prefix: {self.topic_prefix}")
            
            # Per-robot mirrors of is_muted. PSF decides for a covered zone, not for a
            # named truck, so every one of these carries the SAME value as the global
            # topic. They exist so subscribers can already bind to a per-robot name;
            # when PSF attributes decisions per truck, this loop is where the per-robot
            # value is substituted.
            #
            # It is the last hop, not the only one. Reaching per-truck attribution also
            # needs PSF to fill the 64-byte ObjectRecords (proximity mode does; ATL
            # passes nullptr at every sendDecisionCommand call site), this bridge to
            # expose command.objects (parsed into CmdPacket.objects today and then
            # dropped — the OPC UA server has no Objects node and SafetyCommand has no
            # such field), a per-robot ground-truth pose source to match detections
            # against (srr_ground_truth.py knows one hardcoded /World/forklift_b), and
            # an empirical frame fit between the packet's VSS world coordinates and
            # Isaac world. The per-robot routing itself will land on a robot_id in the
            # /safety/command JSON below, where the controller-side filter already
            # waits, rather than on these mirrors.
            for robot_id in self.robot_ids:
                topic = self._robot_muted_topic(robot_id)
                self._publishers[f"is_muted:{robot_id}"] = self._node.create_publisher(
                    Bool, topic, 10
                )
            if self.robot_ids:
                logger.info(
                    "Mirroring is_muted to %s — all carry the same global decision "
                    "(PSF does not attribute mute per robot yet)",
                    ", ".join(self._robot_muted_topic(r) for r in self.robot_ids)
                )
            
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
    
    def _poll_proximity(self, opcua_nodes: dict):
        """Read the proximity snapshot node and publish it if it is new.

        Absence is downgraded to one warning rather than an error, unlike the ATL
        StateJson check: only an OPC server older than proximity support lacks
        the node, and that is no reason to stop publishing ATL.
        """
        node = opcua_nodes.get('ProximityStateJson')
        if node is None:
            if not self._warned_no_prox_node:
                logger.warning(
                    "ProximityStateJson OPC node absent — this server predates "
                    "proximity support; %s/proximity/* will not be published",
                    self.topic_prefix
                )
                self._warned_no_prox_node = True
            return

        raw = node.read_value()
        if not raw:
            return
        try:
            st = json.loads(raw)
        except (ValueError, TypeError) as e:
            logger.warning(f"Skipping malformed ProximityStateJson: {e}")
            return

        seq = st.get('sequence')
        if seq is None or st.get('command') is None:
            logger.warning(
                f"Skipping incomplete ProximityStateJson: {raw!r}")
            return

        last_update = st.get('last_update', "")
        if seq == self._prox_last_seq and last_update == self._prox_last_update:
            return

        self.publish_proximity(ProximityDecision(
            sequence_number=seq,
            command_code=st.get('command'),
            command_name=st.get('command_name', "Unknown"),
            mode=st.get('mode'),
            separation_m=st.get('separation_m'),
            safety_critical=bool(st.get('safety_critical', False)),
            objects=st.get('objects') or [],
            timestamp=st.get('timestamp', ""),
            source="opcua",
            heard_sequence=st.get('heard_sequence'),
            heard_at=st.get('heard_at') or "",
        ))
        self._prox_last_seq = seq
        self._prox_last_update = last_update

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
        warned_no_statejson = False
        
        while self._running:
            try:
                # Read ONE atomic snapshot node instead of 7 separate reads that
                # could tear (fresh seq + stale command). NVBug 6512051.
                state_val = opcua_nodes.get('StateJson')

                if state_val is None:
                    # Fail loud (mixed-version deploy): the server is unpatched (no atomic
                    # snapshot node), so is_muted would never be published. Log once instead
                    # of silently doing nothing every poll. NVBug 6512051.
                    if not warned_no_statejson:
                        logger.error(
                            "StateJson OPC node absent — comm-layer server appears unpatched; "
                            "/safety/is_muted will NOT be published. Deploy the matching server."
                        )
                        warned_no_statejson = True
                else:
                    raw = state_val.read_value()
                    error_count = 0
                    if raw:  # skip the empty initial value
                        try:
                            st = json.loads(raw)
                        except (ValueError, TypeError) as e:
                            # Fail safe: skip this publish rather than emit a wrong is_muted
                            logger.warning(f"Skipping malformed StateJson: {e}")
                            st = None
                        if st is not None:
                            seq = st.get('sequence')
                            # Fail safe: an incomplete snapshot (missing sequence/command) is
                            # skipped rather than published with None fields.
                            if seq is None or st.get('command') is None:
                                logger.warning(
                                    f"Skipping incomplete StateJson (missing sequence/command): {raw!r}"
                                )
                            else:
                                last_update = st.get('last_update', "")
                                # Only publish if there's new data
                                if seq != last_sequence or last_update != last_timestamp:
                                    command = SafetyCommand(
                                        sequence_number=seq,
                                        command_code=st.get('command'),
                                        command_name=st.get('command_name', "Unknown"),
                                        status_code=st.get('status'),
                                        status_name=st.get('status_name', "Unknown"),
                                        timestamp=st.get('timestamp', ""),
                                        source="opcua"
                                    )
                                    self.publish_command(command)
                                    last_sequence = seq
                                    last_timestamp = last_update

                # Independent of the ATL block above, and reached whether or not
                # StateJson had anything: under PSF_APP=pxc ATL is silent, so
                # gating proximity on ATL traffic would publish nothing. Its own
                # try: a bad proximity payload must not count towards the
                # errors that end this loop, or slow ATL polling down.
                try:
                    self._poll_proximity(opcua_nodes)
                except Exception as e:
                    logger.warning(f"Skipping proximity poll: {e}")

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


def _parse_robot_ids(raw: str) -> List[str]:
    """Split the --robot-ids string, rejecting anything that is not a ROS name token.
    
    An id that cannot form a valid topic would otherwise become a publisher nobody
    can reach, which looks exactly like the failure this feature exists to make
    visible: a subscriber waiting on a topic that is never published. Better to
    refuse to start.
    """
    ids = [token.strip() for token in raw.split(',') if token.strip()]
    for robot_id in ids:
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', robot_id):
            raise SystemExit(
                f"--robot-ids: '{robot_id}' is not a valid ROS name token "
                f"(letters, digits and underscore; cannot start with a digit)"
            )
    duplicates = {robot_id for robot_id in ids if ids.count(robot_id) > 1}
    if duplicates:
        raise SystemExit(f"--robot-ids: repeated entries {sorted(duplicates)}")
    return ids


ISAAC_SCRIPTS_DIR = os.environ.get("ISAAC_SCRIPTS_DIR", "/app/isaac")


def _mirrors_from_scenario(configs_dir: str, scenario_id: str) -> List[str]:
    """Which robots this scenario's fleet wants a mute mirror for."""
    if ISAAC_SCRIPTS_DIR not in sys.path:
        sys.path.insert(0, ISAAC_SCRIPTS_DIR)
    try:
        from action_graphs.forklift_common import (
            load_and_validate_robots_yaml, load_scenario, robots_needing_mute_mirror)
    except ImportError as exc:
        raise SystemExit(
            f"cannot import the fleet loader from {ISAAC_SCRIPTS_DIR} ({exc}). "
            f"The comm-layer service mounts the Isaac script tree; check that "
            f"mount, or set ISAAC_SCRIPTS_DIR."
        ) from exc
    entry = load_scenario(configs_dir, scenario_id)
    robots, _ = load_and_validate_robots_yaml(
        os.path.join(configs_dir, entry["robots"]))
    return robots_needing_mute_mirror(robots)


def main():
    """Standalone entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(description='Safety ROS2 Bridge')
    parser.add_argument('--opcua', default='opc.tcp://localhost:4840/safety/', help='OPC UA server URL')
    parser.add_argument('--direct', action='store_true', help='Direct mode (start ESL receiver)')
    parser.add_argument('--topic-prefix', default='/safety', help='ROS2 topic prefix')
    parser.add_argument('--rate', type=float, default=10.0, help='Publish rate (Hz)')
    parser.add_argument('--scenario', default='',
                        help='Scenario id; its fleet file says which robots want a '
                             'mirror. Overridden by --robot-ids.')
    parser.add_argument('--configs-dir', default='/app/robots',
                        help='Directory holding scenarios.yaml and the robots configs')
    parser.add_argument('--robot-ids', default='',
                        help='Comma-separated robots to mirror is_muted to. Overrides '
                             '--scenario; empty publishes the global topic only.')
    args = parser.parse_args()
    if args.robot_ids:
        robot_ids = _parse_robot_ids(args.robot_ids)
        source = 'flag'
    elif args.scenario:
        robot_ids = _mirrors_from_scenario(args.configs_dir, args.scenario)
        source = f'SCENARIO={args.scenario}'
    else:
        robot_ids, source = [], 'nothing named a fleet'
    print(f"[mirrors] {', '.join(robot_ids) or '<none>'}  ({source})", flush=True)
    
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
        publish_rate_hz=args.rate,
        robot_ids=robot_ids
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

    def log_proximity(dec: ProximityDecision):
        emoji = {"stop": "🔴", "reduce_speed": "🟡", "normal": "🟢"}.get(dec.mode, "⚪")
        sep = f"{dec.separation_m:.2f}m" if dec.separation_m is not None else "n/a"
        pair = " ".join(
            f"{o.get('role')}[{o.get('id')}]" + ("<empty>" if o.get('empty') else
            f"({o.get('x', 0.0):.2f},{o.get('y', 0.0):.2f})")
            for o in (dec.objects or [])
        )
        mode = dec.mode or "-"
        print(f"ROS2-PXC: Seq#{dec.sequence_number} | {emoji} mode={mode:12s} "
              f"| sep={sep:>7s} | {pair}", flush=True)

    bridge.add_proximity_callback(log_proximity)
    
    try:
        bridge.start(blocking=True)
    except KeyboardInterrupt:
        print("\nShutting down...")
        bridge.stop()


if __name__ == '__main__':
    main()

