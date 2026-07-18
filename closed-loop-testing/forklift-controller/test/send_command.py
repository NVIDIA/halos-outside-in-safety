#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Test script to send commands to /safety/command topic

Usage:
    python send_command.py proceed              # Start moving
    python send_command.py stop                 # Stop/pause
    python send_command.py slow                 # Slow down (30%)
    python send_command.py slow --speed 0.5    # Slow down to 50%
    python send_command.py proceed --robot forklift_b2  # Target specific robot (ID = robots.yaml name)
    python send_command.py stop --min-subs 2   # Multi-robot broadcast: wait for
                                               # BOTH controllers to be discovered
"""

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import String
import json
import argparse
import time
import sys


class CommandSender(Node):
    def __init__(self, command_topic: str = "/safety/command", min_subs: int = 1):
        super().__init__('command_sender')

        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        self.pub = self.create_publisher(String, command_topic, qos)
        self.topic = command_topic
        self.min_subs = max(1, min_subs)

        # Wait for subscribers to connect (up to 3 seconds). A RELIABLE/VOLATILE
        # message only reaches subscribers already discovered at publish time, so
        # for a broadcast in multi-robot runs pass min_subs = number of controllers.
        self._wait_for_subscribers(timeout=3.0)

        self.get_logger().info(f'CommandSender ready on {command_topic}')

    def _wait_for_subscribers(self, timeout: float = 3.0):
        """Wait until at least self.min_subs subscribers are connected"""
        start = time.time()
        count = 0
        while time.time() - start < timeout:
            count = self.pub.get_subscription_count()
            if count >= self.min_subs:
                self.get_logger().info(f'Found {count} subscriber(s)')
                return True
            time.sleep(0.1)

        self.get_logger().warn(
            f'Only {count}/{self.min_subs} subscriber(s) found on {self.topic} '
            f'after {timeout}s - sending anyway')
        return False
    
    def send(self, command: str, robot_id: str = None, speed_factor: float = 1.0, reason: str = ""):
        """Send a command"""
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
        
        target = robot_id or "all robots"
        self.get_logger().info(f'📤 Sent "{command}" to {target} (speed_factor={speed_factor})')


def main():
    parser = argparse.ArgumentParser(description='Send safety commands to robots')
    parser.add_argument('command', type=str, 
                       choices=['proceed', 'stop', 'slow', 'idle', 'go', 'pause', 'resume', 'reset'],
                       help='Command to send')
    parser.add_argument('--robot', '-r', type=str, default=None,
                       help='Target robot ID (default: all robots)')
    parser.add_argument('--speed', '-s', type=float, default=None,
                       help='Speed factor (0.0 to 1.0)')
    parser.add_argument('--reason', type=str, default='',
                       help='Reason for command')
    parser.add_argument('--topic', '-t', type=str, default='/safety/command',
                       help='Command topic')
    parser.add_argument('--min-subs', type=int, default=1,
                       help='Wait for at least this many subscribers before sending '
                            '(default: 1; use 2 for a broadcast in multi-robot runs '
                            'so a just-started second controller is not missed)')

    args = parser.parse_args()
    
    # Normalize command
    command = args.command.lower()
    if command in ('go', 'resume'):
        command = 'proceed'
    elif command == 'pause':
        command = 'stop'
    
    # Default speed factors
    speed_factor = args.speed
    if speed_factor is None:
        if command == 'slow':
            speed_factor = 0.3
        else:
            speed_factor = 1.0
    
    rclpy.init()
    
    try:
        sender = CommandSender(args.topic, min_subs=args.min_subs)
        sender.send(command, args.robot, speed_factor, args.reason)
        
        # Give time for message to be delivered
        time.sleep(0.2)
        
    except KeyboardInterrupt:
        pass
    finally:
        try:
            rclpy.shutdown()
        except:
            pass


if __name__ == '__main__':
    main()
