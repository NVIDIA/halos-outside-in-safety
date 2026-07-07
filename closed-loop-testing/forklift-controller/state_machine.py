#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
State Machine for Robot Controller
States: IDLE, MOVING (PROCEED), PAUSED (YIELD), SLOW (BUFFER)

Based on VSS Warehouse Blueprint architecture
"""

from enum import Enum
from typing import Callable, Optional
import rclpy
from rclpy.node import Node


class RobotState(Enum):
    """Robot operational states"""
    IDLE = "IDLE"           # Robot is idle, waiting for commands
    MOVING = "MOVING"       # Robot is moving at normal speed (PROCEED)
    PAUSED = "PAUSED"       # Robot is paused/stopped (YIELD)
    SLOW = "SLOW"           # Robot is moving at reduced speed (BUFFER)


class StateMachine:
    """
    State machine for robot control
    
    Transitions:
    - IDLE -> MOVING: Start command received
    - MOVING -> PAUSED: Stop/yield command
    - MOVING -> SLOW: Slow down command
    - PAUSED -> MOVING: Proceed command
    - SLOW -> MOVING: Proceed command
    - SLOW -> PAUSED: Stop command
    - Any -> IDLE: Reset/stop command
    """
    
    # Speed factors for each state
    SPEED_FACTORS = {
        RobotState.IDLE: 0.0,
        RobotState.MOVING: 1.0,
        RobotState.PAUSED: 0.0,
        RobotState.SLOW: 0.3,  # 30% speed
    }
    
    def __init__(self, logger=None):
        self._state = RobotState.IDLE
        self._speed_factor = 1.0  # Custom speed factor from command
        self._on_state_change: Optional[Callable] = None
        self._logger = logger
    
    @property
    def state(self) -> RobotState:
        return self._state
    
    @property
    def speed_factor(self) -> float:
        """Get current speed factor based on state and custom factor"""
        base_factor = self.SPEED_FACTORS[self._state]
        return base_factor * self._speed_factor
    
    @property
    def is_moving(self) -> bool:
        """Check if robot should be moving"""
        return self._state in (RobotState.MOVING, RobotState.SLOW)
    
    @property
    def is_stopped(self) -> bool:
        """Check if robot should be stopped"""
        return self._state in (RobotState.IDLE, RobotState.PAUSED)
    
    def set_on_state_change(self, callback: Callable):
        """Set callback for state changes"""
        self._on_state_change = callback
    
    def _log(self, msg: str):
        if self._logger:
            self._logger.info(msg)
    
    def transition_to(self, new_state: RobotState) -> bool:
        """
        Transition to a new state
        Returns True if transition was successful
        """
        old_state = self._state
        
        # All transitions are allowed for now
        self._state = new_state
        
        if old_state != new_state:
            self._log(f"🔄 State: {old_state.value} → {new_state.value} (speed_factor={self.speed_factor:.2f})")
            if self._on_state_change:
                self._on_state_change(old_state, new_state)
        
        return True
    
    def set_speed_factor(self, factor: float):
        """Set custom speed factor (0.0 to 1.0)"""
        self._speed_factor = max(0.0, min(1.0, factor))
        self._log(f"⚡ Speed factor set to {self._speed_factor:.2f}")
    
    def handle_command(self, command: str, params: dict = None) -> bool:
        """
        Handle a command string and transition state accordingly
        
        Commands:
        - "proceed" / "start" / "go": Start moving
        - "stop" / "yield" / "pause": Stop/pause
        - "slow" / "buffer": Slow down
        - "idle" / "reset": Return to idle
        
        Params:
        - speed_factor: float (0.0 to 1.0)
        """
        command = command.lower().strip()
        params = params or {}
        
        # Update speed factor if provided
        if 'speed_factor' in params:
            self.set_speed_factor(float(params['speed_factor']))
        
        # Map command to state
        if command in ('proceed', 'start', 'go', 'resume'):
            return self.transition_to(RobotState.MOVING)
        elif command in ('stop', 'yield', 'pause'):
            return self.transition_to(RobotState.PAUSED)
        elif command in ('slow', 'buffer', 'caution'):
            return self.transition_to(RobotState.SLOW)
        elif command in ('idle', 'reset'):
            return self.transition_to(RobotState.IDLE)
        else:
            self._log(f"⚠️ Unknown command: {command}")
            return False
    
    def start(self):
        """Convenience: Start moving"""
        return self.transition_to(RobotState.MOVING)
    
    def stop(self):
        """Convenience: Stop/pause"""
        return self.transition_to(RobotState.PAUSED)
    
    def slow_down(self, factor: float = 0.3):
        """Convenience: Slow down with optional factor"""
        self.set_speed_factor(factor)
        return self.transition_to(RobotState.SLOW)
    
    def reset(self):
        """Convenience: Reset to idle"""
        self._speed_factor = 1.0
        return self.transition_to(RobotState.IDLE)
