#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
Robot Controller - Integrates Command Handler, State Machine, and Waypoint Follower

Based on VSS Warehouse Blueprint architecture:
- RobotController(forklift_b)
- RobotController(forklift_b2)
- etc.

This is the main controller that:
1. Receives commands via /safety/command
2. Manages robot state (IDLE, MOVING, PAUSED, SLOW)
3. Controls waypoint following with speed adjustment
4. Publishes cmd_vel with applied speed_factor
"""

import fleet_config
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from geometry_msgs.msg import Twist, PoseStamped
from nav_msgs.msg import Odometry, Path
from std_msgs.msg import String
from visualization_msgs.msg import Marker, MarkerArray
from std_msgs.msg import ColorRGBA
from rosgraph_msgs.msg import Clock

import json
import os
import math
import time
import argparse
import signal
from typing import Optional
from datetime import datetime

from state_machine import StateMachine, RobotState


class RobotController(Node):
    """
    Single robot controller with state machine and command handling
    """
    
    def __init__(self,
                 robot_id: str = "forklift_b",
                 path_file: Optional[str] = None,
                 base_linear_speed: float = 0.5,
                 base_angular_speed: float = 0.4,
                 loop: bool = False,
                 invert_poses: bool = True,
                 heading_offset: float = math.pi,
                 topics: Optional[dict] = None,
                 end_tolerance: float = 1.5,
                 end_pose_count: int = 3,
                 spiral_timeout: float = 15.0):
        # Node name must be unique per robot: N controller instances (one
        # container per robot) with the same name collide on parameter
        # services and rosout.
        super().__init__(f'{robot_id}_controller')
        
        self.robot_id = robot_id
        self.base_linear_speed = base_linear_speed
        self.base_angular_speed = base_angular_speed
        self.loop = loop
        self.invert_poses = invert_poses
        self.heading_offset = heading_offset
        
        # State machine
        self.state_machine = StateMachine(logger=self.get_logger())
        self.state_machine.set_on_state_change(self._on_state_change)
        
        # Waypoint following state
        self.poses = []
        self.segments = []
        self.origin = {'x': 0, 'y': 0}
        self.current_pose_idx = 0
        self.robot_x = 0.0
        self.robot_y = 0.0
        self.robot_theta = 0.0
        self.odom_received = False
        self.completed = False
        
        # Tolerances
        self.position_tolerance = 0.35
        self.angle_tolerance = math.radians(15)
        
        # End-of-path tolerance (larger tolerance for last few poses)
        self.end_tolerance = end_tolerance  # meters
        self.end_pose_count = end_pose_count  # how many poses from end
        
        # Skip-ahead logic
        self.min_distance_to_pose = float('inf')
        self.distance_increasing_count = 0
        
        # Spiral detection - auto-complete if stuck too long
        self._pose_start_time = None
        self._spiral_timeout = spiral_timeout  # seconds - if stuck at same pose for this long
        self._spiral_distance_threshold = 2.0  # meters - only auto-complete if within this distance
        
        # Simulation state tracking (via /clock topic)
        self._last_sim_time = None
        self._last_clock_wall_time = 0.0
        self._sim_running = False
        self._clock_timeout = 2.0  # seconds without /clock = sim stopped
        self._prev_robot_x = None
        self._prev_robot_y = None
        self._odom_jump_threshold = 5.0  # meters - detect teleport/reset
        
        # QoS profiles
        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
        latched_qos = QoSProfile(
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL
        )
        
        # ===== Topic names =====
        # From the robot's own block in the fleet file, which is also where Isaac
        # reads them: one declaration, so the two sides cannot disagree about
        # what this truck listens on. Falls back to the namespaced convention
        # when there is no fleet file (a run started by hand).
        topics = topics or {}
        cmd_vel_topic = topics.get('cmd_vel') or f'/{robot_id}/cmd_vel'
        odom_topic = topics.get('odom') or f'/{robot_id}/odom'
        state_topic = f'/{robot_id}/state'
        path_topic = f'/{robot_id}/planned_path'
        marker_topic = f'/{robot_id}/markers'
        
        # ===== Publishers =====
        self.cmd_pub = self.create_publisher(Twist, cmd_vel_topic, qos)
        self.state_pub = self.create_publisher(String, state_topic, qos)
        self.path_pub = self.create_publisher(Path, path_topic, latched_qos)
        self.marker_pub = self.create_publisher(MarkerArray, marker_topic, qos)
        
        # ===== Subscribers =====
        self.odom_sub = self.create_subscription(
            Odometry, odom_topic, self._odom_callback, qos)
        
        # Safety command (global - handles all robots)
        self.command_sub = self.create_subscription(
            String, '/safety/command', self._command_callback, qos)
        
        # Simulation clock (detect stop/play/reset)
        self.clock_sub = self.create_subscription(
            Clock, '/clock', self._clock_callback, 10)
        
        # ===== Timers =====
        # Control loop (20Hz)
        self.control_timer = self.create_timer(0.05, self._control_loop)
        # State publisher (5Hz)
        self.state_timer = self.create_timer(0.2, self._publish_state)
        # Visualization (5Hz)
        self.viz_timer = self.create_timer(0.2, self._publish_visualization)
        
        # Load path if provided
        if path_file:
            self.load_path(path_file)
        
        self.get_logger().info(f'═══════════════════════════════════════')
        self.get_logger().info(f'RobotController initialized: {robot_id}')
        self.get_logger().info(f'  State: {self.state_machine.state.value}')
        self.get_logger().info(f'  Base speed: {base_linear_speed} m/s')
        self.get_logger().info(f'  Command topic: /safety/command')
        self.get_logger().info(f'  Odom topic: {odom_topic}')
        self.get_logger().info(f'  Cmd_vel topic: {cmd_vel_topic}')
        self.get_logger().info(f'  End tolerance: {self.end_tolerance}m for last {self.end_pose_count} poses')
        self.get_logger().info(f'  Spiral timeout: {self._spiral_timeout}s')
        if self.poses:
            self.get_logger().info(f'  Loaded {len(self.poses)} poses')
        self.get_logger().info(f'═══════════════════════════════════════')
    
    def load_path(self, path_file: str):
        """Load waypoints from JSON file"""
        try:
            with open(path_file, 'r') as f:
                data = json.load(f)
            
            # Get origin
            if 'origin' in data:
                self.origin = {
                    'x': data['origin'].get('world_x', 0),
                    'y': data['origin'].get('world_y', 0)
                }
            
            # Load poses
            if 'poses' in data and len(data['poses']) > 0:
                for p in data['poses']:
                    odom_x = p['x'] - self.origin['x']
                    odom_y = p['y'] - self.origin['y']
                    theta = float(p.get('theta', 0))
                    
                    if self.invert_poses:
                        odom_x = -odom_x
                        odom_y = -odom_y
                        theta = self._normalize_angle(theta + math.pi)
                    
                    self.poses.append({
                        'x': odom_x,
                        'y': odom_y,
                        'theta': theta
                    })
            
            # Load segments
            if 'segments' in data:
                self.segments = data['segments']
            
            self.get_logger().info(f'Loaded path: {len(self.poses)} poses')
            
        except Exception as e:
            self.get_logger().error(f'Failed to load path: {e}')
    
    def _command_callback(self, msg: String):
        """Handle incoming safety commands"""
        try:
            data = json.loads(msg.data)
            
            # Only null broadcasts; "" or 0 is a malformed address, not everyone.
            target_robot = data.get('robot_id', None)
            if target_robot is not None and target_robot != self.robot_id:
                return  # Not for us
            
            raw_cmd = data.get('command', '')

            # The comm-layer / PSF publishes its ATL safety *decision* here as an
            # integer opcode (HEARTBEAT=0, HW_ERROR=1, MUTE=2, SW_ERROR=3, UNMUTE=7).
            # That is the safety output for a real machine — NOT a drive command for
            # this simulation forklift, which is just a stimulus generator and must
            # keep following its waypoint loop so PSF always has a moving target.
            # Only explicit string keywords from test/send_command.py
            # (proceed/stop/slow/reset) drive the state machine.
            if not isinstance(raw_cmd, str):
                return
            command = raw_cmd.strip().lower()
            if not command:
                return

            raw_speed_factor = data.get('speed_factor', 1.0)
            try:
                speed_factor = float(raw_speed_factor)
            except (TypeError, ValueError):
                self.get_logger().warn(
                    f'Invalid speed_factor {raw_speed_factor!r}; using 1.0')
                speed_factor = 1.0

            params = {
                'speed_factor': speed_factor,
                'reason': data.get('reason', data.get('command_name', '')),
            }
            
            self.get_logger().info(
                f'📥 [{self.robot_id}] Command: {command} | '
                f'speed_factor={params["speed_factor"]:.2f}'
            )
            
            # Handle reset command directly (not a state machine command)
            if command == 'reset':
                # The early return below skips state_machine.handle_command, so
                # apply the speed factor here (params default 1.0 restores full
                # speed; honors `reset --speed X`). Deliberately NOT inside
                # _reset_path: that path is shared with the sim-reset/odom-jump
                # auto-recovery, which must not wipe an operator's `slow` factor.
                self.state_machine.set_speed_factor(params['speed_factor'])
                self._reset_path()
                return
            
            # Handle command via state machine
            self.state_machine.handle_command(command, params)
            
        except json.JSONDecodeError as e:
            self.get_logger().error(f'Invalid command JSON: {e}')
        except Exception as e:
            self.get_logger().error(f'Command error: {e}')
    
    def _clock_callback(self, msg: Clock):
        """Monitor /clock to detect simulation stop/play/reset"""
        sim_time = msg.clock.sec + msg.clock.nanosec * 1e-9
        wall_now = time.time()
        
        was_running = self._sim_running
        clock_gap = wall_now - self._last_clock_wall_time if self._last_clock_wall_time > 0 else 0
        
        # Detect sim reset: time jumped backward significantly
        if self._last_sim_time is not None and sim_time < self._last_sim_time - 0.5:
            self.get_logger().warn(
                f'🔄 SIM RESET detected (time {self._last_sim_time:.1f}s → {sim_time:.1f}s) - restarting path')
            self._reset_path()
        
        # Detect sim resumed after long gap (Stop → Play)
        elif was_running and clock_gap > self._clock_timeout:
            self.get_logger().warn(
                f'▶️ SIM RESUMED after {clock_gap:.1f}s pause - restarting path')
            self._reset_path()
        
        self._last_sim_time = sim_time
        self._last_clock_wall_time = wall_now
        self._sim_running = True
    
    def _reset_path(self):
        """Reset path following - find nearest heading-compatible pose"""
        best_idx = 0

        if self.odom_received and self.poses:
            best_score = float('inf')
            for i, p in enumerate(self.poses):
                dx = float(p['x']) - self.robot_x
                dy = float(p['y']) - self.robot_y
                dist = math.sqrt(dx * dx + dy * dy)
                heading_diff = abs(self._normalize_angle(float(p['theta']) - self.robot_theta))
                # Weight: distance + heavy heading penalty (1 radian ~ 3 meters)
                score = dist + heading_diff * 3.0
                if score < best_score:
                    best_score = score
                    best_idx = i

            self.get_logger().info(
                f'🔁 Path reset: nearest heading-compatible pose = {best_idx} '
                f'(score={best_score:.2f}, robot=({self.robot_x:.1f},{self.robot_y:.1f},{math.degrees(self.robot_theta):.0f}°))')
        else:
            self.get_logger().info('🔁 Path reset to beginning (no odom)')

        self.current_pose_idx = best_idx
        self.completed = False
        self.min_distance_to_pose = float('inf')
        self.distance_increasing_count = 0
        self._pose_start_time = None
        self._prev_robot_x = None
        self._prev_robot_y = None
        self._sim_running = False
        self._last_sim_time = None
        self._last_clock_wall_time = 0.0
        
        self._stop_robot()
        self.state_machine.start()
    
    def _odom_callback(self, msg: Odometry):
        """Update robot position from odometry"""
        new_x = msg.pose.pose.position.x
        new_y = msg.pose.pose.position.y
        
        # Fallback: detect odom position jump (teleport = sim reset)
        if self._prev_robot_x is not None and self.odom_received:
            jump = math.sqrt(
                (new_x - self._prev_robot_x)**2 +
                (new_y - self._prev_robot_y)**2)
            if jump > self._odom_jump_threshold:
                self.get_logger().warn(
                    f'⚡ ODOM JUMP detected ({jump:.1f}m) - restarting path')
                self._prev_robot_x = new_x
                self._prev_robot_y = new_y
                self._reset_path()
                return
        
        self._prev_robot_x = new_x
        self._prev_robot_y = new_y
        self.robot_x = new_x
        self.robot_y = new_y
        
        # Extract yaw from quaternion
        q = msg.pose.pose.orientation
        siny_cosp = 2.0 * (q.w * q.z + q.x * q.y)
        cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z)
        raw_theta = math.atan2(siny_cosp, cosy_cosp)
        
        # Apply heading offset
        self.robot_theta = self._normalize_angle(raw_theta + self.heading_offset)
        
        if not self.odom_received:
            self.odom_received = True
            self.get_logger().info(
                f'Odom received: ({self.robot_x:.2f}, {self.robot_y:.2f}, '
                f'{math.degrees(self.robot_theta):.1f}°)')
    
    def _on_state_change(self, old_state: RobotState, new_state: RobotState):
        """Callback when state changes"""
        # If paused or idle, immediately stop
        if new_state in (RobotState.PAUSED, RobotState.IDLE):
            self._stop_robot()
    
    def _control_loop(self):
        """Main control loop - follows waypoints with speed factor applied"""
        # Check if simulation is running (via /clock)
        if self._last_clock_wall_time > 0:
            clock_age = time.time() - self._last_clock_wall_time
            if clock_age > self._clock_timeout and self._sim_running:
                self._sim_running = False
                self._stop_robot()
                self.get_logger().warn('⏸️ SIM STOPPED (no /clock) - pausing robot')
            if not self._sim_running:
                return
        
        if not self.odom_received:
            return
        
        if self.state_machine.is_stopped:
            return  # Don't move if paused or idle
        
        if self.completed or not self.poses:
            return
        
        if self.current_pose_idx >= len(self.poses):
            if self.loop:
                self.current_pose_idx = 0
                self.get_logger().info('Loop: Restarting path')
            else:
                if not self.completed:
                    self._stop_robot()
                    self.completed = True
                    self.state_machine.transition_to(RobotState.IDLE)
                    self.get_logger().info('✓ Path completed!')
                return
        
        # Get current target
        target = self.poses[self.current_pose_idx]
        target_x = float(target['x'])
        target_y = float(target['y'])
        
        # Get segment info (reverse?)
        segment = self._get_segment_for_pose(self.current_pose_idx)
        is_reverse = segment.get('reverse', False)
        
        # Calculate distance and angle
        dx = target_x - self.robot_x
        dy = target_y - self.robot_y
        distance = math.sqrt(dx * dx + dy * dy)
        angle_to_target = math.atan2(dy, dx)
        
        # SMART SKIP: When robot is far from target, find the closest pose AHEAD on the path
        # This prevents spiraling when robot overshoots
        if distance > 1.0 and self.current_pose_idx < len(self.poses) - 3:
            current_segment = self._get_segment_for_pose(self.current_pose_idx)
            best_idx = self.current_pose_idx
            best_dist = distance
            
            # Look up to 10 poses ahead within same segment
            for check_idx in range(self.current_pose_idx + 1, min(self.current_pose_idx + 10, len(self.poses))):
                check_seg = self._get_segment_for_pose(check_idx)
                if check_seg.get('reverse') != current_segment.get('reverse'):
                    break  # Don't skip across segment boundary
                
                check_pose = self.poses[check_idx]
                check_dist = math.sqrt(
                    (float(check_pose['x']) - self.robot_x)**2 +
                    (float(check_pose['y']) - self.robot_y)**2
                )
                if check_dist < best_dist:
                    best_dist = check_dist
                    best_idx = check_idx
            
            # If found a closer pose ahead, skip to it
            if best_idx > self.current_pose_idx:
                self.get_logger().warn(
                    f'⏭️ SKIP AHEAD {self.current_pose_idx}→{best_idx} (closer: {distance:.1f}m → {best_dist:.1f}m)')
                self.current_pose_idx = best_idx
                self.min_distance_to_pose = float('inf')
                return  # Re-evaluate with new target
        
        # Original skip logic for when we passed a pose
        if distance < self.min_distance_to_pose:
            self.min_distance_to_pose = distance
            self.distance_increasing_count = 0
        elif distance > self.min_distance_to_pose + 0.2:
            self.distance_increasing_count += 1
        
        should_skip = (self.distance_increasing_count > 20 and
                      self.min_distance_to_pose < 0.5 and
                      distance > self.min_distance_to_pose + 0.3 and
                      self.current_pose_idx < len(self.poses) - 1)
        
        # === END-OF-PATH TOLERANCE ===
        # Use larger tolerance for last few poses (easier completion at high speeds)
        poses_remaining = len(self.poses) - self.current_pose_idx
        if poses_remaining <= self.end_pose_count:
            current_tolerance = self.end_tolerance
        else:
            current_tolerance = self.position_tolerance
        
        # === SPIRAL DETECTION - auto-complete if stuck too long ===
        # Track how long we've been trying to reach current pose
        current_time = self.get_clock().now().nanoseconds / 1e9
        if self._pose_start_time is None:
            self._pose_start_time = current_time
        
        time_at_pose = current_time - self._pose_start_time
        
        # If stuck at last pose for too long and within threshold distance, force complete
        is_last_pose = (self.current_pose_idx == len(self.poses) - 1)
        spiral_detected = (is_last_pose and 
                          time_at_pose > self._spiral_timeout and 
                          distance < self._spiral_distance_threshold)
        
        if spiral_detected:
            self.get_logger().warn(
                f'🔄 SPIRAL DETECTED: stuck at final pose for {time_at_pose:.1f}s, '
                f'distance={distance:.2f}m - AUTO-COMPLETING')
            self._stop_robot()
            self.completed = True
            self.state_machine.transition_to(RobotState.IDLE)
            self.get_logger().info('✓ Path completed (spiral auto-complete)!')
            return
        
        # Check if reached pose
        if distance < current_tolerance or should_skip:
            old_idx = self.current_pose_idx
            self.current_pose_idx += 1
            self.min_distance_to_pose = float('inf')
            self.distance_increasing_count = 0
            self._pose_start_time = None  # Reset spiral timer for new pose
            
            if should_skip:
                self.get_logger().warn(f'⏭️ SKIPPING pose {old_idx} (passed it, got within {self.min_distance_to_pose:.2f}m)')
            
            # Check segment transition
            new_segment = self._get_segment_for_pose(self.current_pose_idx) if self.current_pose_idx < len(self.poses) else None
            new_is_reverse = new_segment.get('reverse', False) if new_segment else False
            
            if new_is_reverse != is_reverse:
                self.get_logger().warn(
                    f'🔄 SEGMENT TRANSITION at pose {self.current_pose_idx}: '
                    f'{"FORWARD→REVERSE" if new_is_reverse else "REVERSE→FORWARD"}')
            
            if self.current_pose_idx % 5 == 0 or new_is_reverse != is_reverse:
                progress = self.current_pose_idx / len(self.poses) * 100
                self.get_logger().info(
                    f'[{progress:.0f}%] pose {old_idx}→{self.current_pose_idx} '
                    f'robot=({self.robot_x:.2f},{self.robot_y:.2f}) '
                    f'target=({target_x:.2f},{target_y:.2f}) {"REV" if is_reverse else "FWD"}')
            return
        
        # Pure pursuit - find lookahead point
        lookahead_dist = max(0.8, self.base_linear_speed * 1.5)
        carrot_idx = self.current_pose_idx
        accumulated_dist = 0.0
        current_segment = self._get_segment_for_pose(self.current_pose_idx)
        
        for i in range(self.current_pose_idx, min(self.current_pose_idx + 15, len(self.poses) - 1)):
            next_seg = self._get_segment_for_pose(i + 1)
            if next_seg.get('reverse') != current_segment.get('reverse'):
                break
            
            cdx = float(self.poses[i+1]['x']) - float(self.poses[i]['x'])
            cdy = float(self.poses[i+1]['y']) - float(self.poses[i]['y'])
            accumulated_dist += math.sqrt(cdx*cdx + cdy*cdy)
            carrot_idx = i + 1
            if accumulated_dist >= lookahead_dist:
                break
        
        # Calculate heading to carrot
        carrot = self.poses[carrot_idx]
        carrot_dx = float(carrot['x']) - self.robot_x
        carrot_dy = float(carrot['y']) - self.robot_y
        angle_for_heading = math.atan2(carrot_dy, carrot_dx)
        
        # Desired heading
        if is_reverse:
            desired_heading = self._normalize_angle(angle_for_heading + math.pi)
        else:
            desired_heading = angle_for_heading
        
        heading_error = self._normalize_angle(desired_heading - self.robot_theta)
        
        # Create command with speed factor applied
        cmd = Twist()
        speed_factor = self.state_machine.speed_factor
        
        angular_gain = 1.2
        cmd.angular.z = heading_error * angular_gain
        
        # Apply speed factor to linear speed
        base_speed = -self.base_linear_speed if is_reverse else self.base_linear_speed
        cmd.linear.x = base_speed * speed_factor
        
        # Clamp angular velocity
        max_angular = self.base_angular_speed
        cmd.angular.z = max(-max_angular, min(max_angular, cmd.angular.z))
        
        # Slow down near target
        if distance < 0.5:
            cmd.linear.x *= max(0.4, distance / 0.5)
        
        # Slow down for large heading errors
        heading_error_deg = abs(math.degrees(heading_error))
        if heading_error_deg > 60:
            cmd.linear.x *= 0.2
        elif heading_error_deg > 30:
            cmd.linear.x *= 0.4
        
        # Minimum forward speed to maintain steering authority
        min_forward = 0.15
        if is_reverse:
            if cmd.linear.x > -min_forward:
                cmd.linear.x = -min_forward
        else:
            if cmd.linear.x < min_forward:
                cmd.linear.x = min_forward
        
        self.cmd_pub.publish(cmd)
        
        # Periodic status log (every 2 seconds)
        current_sec = int(self.get_clock().now().nanoseconds / 1e9)
        if not hasattr(self, '_last_status_sec') or current_sec != self._last_status_sec:
            self._last_status_sec = current_sec
            if current_sec % 2 == 0:
                self.get_logger().info(
                    f'📍 pose={self.current_pose_idx} {"🔙REV" if is_reverse else "▶FWD"} '
                    f'dist={distance:.2f}m cmd=({cmd.linear.x:.2f},{cmd.angular.z:.2f}) '
                    f'robot=({self.robot_x:.2f},{self.robot_y:.2f},{math.degrees(self.robot_theta):.0f}°)')
    
    def _get_segment_for_pose(self, pose_idx: int) -> dict:
        """Get segment info for a pose index"""
        if not self.segments:
            return {'reverse': False}
        
        cumulative = 0
        for seg in self.segments:
            cumulative += seg.get('pose_count', 0)
            if pose_idx < cumulative:
                return seg
        return {'reverse': False}
    
    def _normalize_angle(self, angle: float) -> float:
        """Normalize angle to [-pi, pi]"""
        while angle > math.pi:
            angle -= 2 * math.pi
        while angle < -math.pi:
            angle += 2 * math.pi
        return angle
    
    def _stop_robot(self):
        """Stop the robot"""
        cmd = Twist()
        self.cmd_pub.publish(cmd)
    
    def _publish_state(self):
        """Publish current state"""
        state_msg = String()
        state_data = {
            'robot_id': self.robot_id,
            'state': self.state_machine.state.value,
            'speed_factor': self.state_machine.speed_factor,
            'pose_idx': self.current_pose_idx,
            'total_poses': len(self.poses),
            'completed': self.completed,
        }
        state_msg.data = json.dumps(state_data)
        self.state_pub.publish(state_msg)
    
    def _publish_visualization(self):
        """Publish visualization markers"""
        if not self.poses:
            return
        
        now = self.get_clock().now().to_msg()
        markers = MarkerArray()
        
        # Current state text
        text_marker = Marker()
        text_marker.header.stamp = now
        text_marker.header.frame_id = 'odom'
        text_marker.ns = 'state'
        text_marker.id = 0
        text_marker.type = Marker.TEXT_VIEW_FACING
        text_marker.action = Marker.ADD
        text_marker.pose.position.x = float(self.robot_x)
        text_marker.pose.position.y = float(self.robot_y)
        text_marker.pose.position.z = 2.0
        text_marker.scale.z = 0.5
        
        state = self.state_machine.state
        state_colors = {
            RobotState.IDLE: (0.5, 0.5, 0.5),     # Gray
            RobotState.MOVING: (0.0, 1.0, 0.0),   # Green
            RobotState.PAUSED: (1.0, 0.0, 0.0),   # Red
            RobotState.SLOW: (1.0, 1.0, 0.0),     # Yellow
        }
        color = state_colors.get(state, (1.0, 1.0, 1.0))
        
        text_marker.text = f"{self.robot_id}\n{state.value}\nSpeed: {self.state_machine.speed_factor:.0%}"
        text_marker.color = ColorRGBA(r=color[0], g=color[1], b=color[2], a=1.0)
        markers.markers.append(text_marker)
        
        self.marker_pub.publish(markers)


def main():
    parser = argparse.ArgumentParser(description='Robot Controller with State Machine')
    parser.add_argument('--robot-id', type=str, default='forklift_b',
                       help='Robot ID for namespacing')
    parser.add_argument('--path', type=str, default=None,
                       help='JSON file with waypoints/poses (overrides --map)')
    parser.add_argument('--scenario', type=str, default=None,
                       help='Scenario id; supplies the robots config and map when those are not given')
    parser.add_argument('--configs-dir', type=str, default='/app/robots',
                       help='Directory holding scenarios.yaml and the robots configs')
    parser.add_argument('--robots-config', type=str, default=None,
                       help='robots.yaml Isaac was launched with; supplies this robot\'s drive: block')
    parser.add_argument('--waypoints-root', type=str, default='/app/waypoints',
                       help='Directory holding <map id>/ waypoint sets')
    parser.add_argument('--map', type=str, default=None,
                       help='Map id under --waypoints-root, e.g. warehouse_20x20')
    # Drive knobs default to None so an absent flag is distinguishable from an
    # explicit one: entrypoint.sh passes a flag only when its env var is set,
    # which is what lets robots.yaml fill the rest. See fleet_config.py.
    parser.add_argument('--speed', type=float, default=None,
                       help='Base linear speed (m/s)')
    parser.add_argument('--angular-speed', type=float, default=None,
                       help='Base angular speed (rad/s)')
    parser.add_argument('--loop', action='store_true', default=None,
                       help='Loop the path')
    parser.add_argument('--no-invert', action='store_true', default=None,
                       help='Disable pose inversion')
    parser.add_argument('--heading-offset', type=float, default=None,
                       help='Heading offset in degrees')
    parser.add_argument('--end-tolerance', type=float, default=None,
                       help='Position tolerance for last few poses (meters)')
    parser.add_argument('--end-pose-count', type=int, default=None,
                       help='Number of poses from end to use larger tolerance')
    parser.add_argument('--spiral-timeout', type=float, default=None,
                       help='Timeout (seconds) for spiral auto-complete at final pose')

    args, unknown = parser.parse_known_args()

    # A scenario names the fleet file and the map; an explicit flag still wins.
    robots_config, waypoints_map = args.robots_config, args.map
    if args.scenario:
        scenario = fleet_config.load_scenario(args.configs_dir, args.scenario)
        if not robots_config and scenario.get('robots'):
            robots_config = os.path.join(args.configs_dir, scenario['robots'])
        waypoints_map = waypoints_map or scenario.get('waypoints')
        print(f"[scenario] {args.scenario}: robots={scenario.get('robots')} "
              f"waypoints={scenario.get('waypoints')}"
              f"{' EXPERIMENTAL' if scenario.get('experimental') else ''}", flush=True)

    # env (flag) > robots.yaml > built-in default, per knob.
    robot_block = None
    if robots_config:
        fleet = fleet_config.load_fleet(robots_config)
        robot_block = fleet_config.find_robot(fleet, args.robot_id, robots_config)
    drive, drive_sources = fleet_config.resolve_drive({
        'speed': args.speed,
        'angular_speed': args.angular_speed,
        'heading_offset': args.heading_offset,
        'loop': args.loop,
        'no_invert': args.no_invert,
        'end_tolerance': args.end_tolerance,
        'end_pose_count': args.end_pose_count,
        'spiral_timeout': args.spiral_timeout,
    }, robot_block)
    print(f"[drive] {fleet_config.format_sources(drive, drive_sources)}", flush=True)

    topics = fleet_config.resolve_topics(robot_block, args.robot_id)
    print(f"[topics] cmd_vel={topics['cmd_vel']} odom={topics['odom']}", flush=True)

    path_file = args.path
    if not path_file and waypoints_map:
        path_file = fleet_config.resolve_waypoint_file(
            args.waypoints_root, waypoints_map, args.robot_id)
        print(f"[waypoints] {path_file}", flush=True)

    rclpy.init(args=unknown)

    # Translate SIGTERM (e.g. `docker stop`) into KeyboardInterrupt so the
    # finally block runs and publishes a zero-velocity stop before exit.
    # SIGINT (Ctrl+C) already raises KeyboardInterrupt by default.
    def _request_shutdown(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _request_shutdown)

    node = None
    try:
        node = RobotController(
            robot_id=args.robot_id,
            path_file=path_file,
            base_linear_speed=drive['speed'],
            base_angular_speed=drive['angular_speed'],
            loop=drive['loop'],
            invert_poses=not drive['no_invert'],
            heading_offset=math.radians(drive['heading_offset']),
            topics=topics,
            end_tolerance=drive['end_tolerance'],
            end_pose_count=drive['end_pose_count'],
            spiral_timeout=drive['spiral_timeout']
        )
        
        # Auto-start moving
        node.state_machine.start()
        
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print(f'Error: {e}')
        import traceback
        traceback.print_exc()
    finally:
        if node is not None:
            # Fail safe: publish a zero Twist and flush it before teardown so the
            # forklift does not hold its last cmd_vel after the process exits.
            try:
                node._stop_robot()
                for _ in range(5):
                    rclpy.spin_once(node, timeout_sec=0.02)
            except Exception:
                pass
            node.destroy_node()
        try:
            rclpy.shutdown()
        except Exception:
            pass


if __name__ == '__main__':
    main()
