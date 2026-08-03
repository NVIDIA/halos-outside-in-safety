#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Command Handler - Subscribe to /safety/command topic and dispatch commands

Based on VSS Warehouse Blueprint architecture:
- Subscribe: /safety/command (JSON)
- Apply: speed_factor

Command JSON format:
{
    "robot_id": "forklift_1",  // Optional: target specific robot
    "command": "proceed",       // proceed, stop, slow, idle
    "speed_factor": 0.5,       // Optional: 0.0 to 1.0
    "timestamp": 1234567890    // Optional: for ordering
}
"""

import json
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import String
from typing import Callable, Optional, Dict, Any


class CommandHandler(Node):
    """
    Handles incoming safety commands from /safety/command topic
    """
    
    def __init__(self, 
                 robot_id: str = "forklift_1",
                 command_topic: str = "/safety/command",
                 on_command: Optional[Callable] = None):
        super().__init__('command_handler')
        
        self.robot_id = robot_id
        self.on_command = on_command
        self._last_command_time = 0
        
        # QoS for command topic
        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        
        # Subscribe to safety command topic
        self.command_sub = self.create_subscription(
            String,
            command_topic,
            self._command_callback,
            qos
        )
        
        self.get_logger().info(f'CommandHandler initialized for robot: {robot_id}')
        self.get_logger().info(f'  Listening on: {command_topic}')
    
    def _command_callback(self, msg: String):
        """Process incoming command message"""
        try:
            # Parse JSON command
            data = json.loads(msg.data)
            
            # Check if command is for this robot (or all robots if no robot_id specified)
            target_robot = data.get('robot_id', None)
            if target_robot and target_robot != self.robot_id:
                # Command is for a different robot, ignore
                return
            
            # Extract command and parameters
            command = data.get('command', '').lower()
            params = {
                'speed_factor': data.get('speed_factor', 1.0),
                'timestamp': data.get('timestamp', 0),
                'duration': data.get('duration', None),  # Optional: auto-resume after duration
                'reason': data.get('reason', ''),        # Optional: why the command was issued
            }
            
            # Check timestamp ordering (ignore old commands)
            if params['timestamp'] and params['timestamp'] < self._last_command_time:
                self.get_logger().warn(f'Ignoring old command (ts={params["timestamp"]})')
                return
            self._last_command_time = params['timestamp'] or self._last_command_time
            
            # Log the command
            self.get_logger().info(
                f'📥 Command: {command} | speed_factor={params["speed_factor"]:.2f}'
                + (f' | reason: {params["reason"]}' if params['reason'] else '')
            )
            
            # Dispatch to callback
            if self.on_command:
                self.on_command(command, params)
                
        except json.JSONDecodeError as e:
            self.get_logger().error(f'Invalid JSON in command: {e}')
        except Exception as e:
            self.get_logger().error(f'Error processing command: {e}')
    
    def set_command_callback(self, callback: Callable):
        """Set the callback function for commands"""
        self.on_command = callback


class CommandPublisher(Node):
    """
    Utility class for publishing safety commands (for testing/integration)
    """
    
    def __init__(self, command_topic: str = "/safety/command"):
        super().__init__('command_publisher')
        
        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        self.pub = self.create_publisher(String, command_topic, qos)
        self.get_logger().info(f'CommandPublisher ready on {command_topic}')
    
    def send_command(self, 
                     command: str, 
                     robot_id: str = None,
                     speed_factor: float = 1.0,
                     reason: str = ""):
        """Send a command to the safety topic"""
        import time
        
        data = {
            'command': command,
            'speed_factor': speed_factor,
            'timestamp': int(time.time() * 1000),
            'reason': reason,
        }
        if robot_id:
            data['robot_id'] = robot_id
        
        msg = String()
        msg.data = json.dumps(data)
        self.pub.publish(msg)
        
        self.get_logger().info(f'📤 Sent: {command} to {robot_id or "all"}')
    
    def proceed(self, robot_id: str = None):
        """Send proceed command"""
        self.send_command('proceed', robot_id)
    
    def stop(self, robot_id: str = None, reason: str = ""):
        """Send stop command"""
        self.send_command('stop', robot_id, reason=reason)
    
    def slow(self, robot_id: str = None, speed_factor: float = 0.3):
        """Send slow command with speed factor"""
        self.send_command('slow', robot_id, speed_factor=speed_factor)
