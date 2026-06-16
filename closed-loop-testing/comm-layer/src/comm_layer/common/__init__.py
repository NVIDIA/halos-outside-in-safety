# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""Common modules for Communication Layer"""

from .safety_commands import (
    # Core 64B types
    CommandCode,
    SafetyStatus,
    SafetyCommand,
    CmdPacket,
    ObjectRecord,
    # Constants
    ATL_PACKET_IDENTIFIER,
    COMMAND_PACKET_SIZE,
    COMMAND_HEADER_SIZE,
    OBJECT_SIZE,
    COMMAND_NUM_OBJECTS,
    ACK_TIMEOUT_SECONDS,
    # Backward-compat constants
    CMD_NOP,
    CMD_MUTE,
    CMD_UNMUTE_ALARM,
    ACK_CODE,
)

from .config import (
    UdpReceiverConfig,
    OpcUaConfig,
    CommLayerConfig,
    get_config,
    set_config,
)

__all__ = [
    'CommandCode',
    'SafetyStatus',
    'SafetyCommand',
    'CmdPacket',
    'ObjectRecord',
    'ATL_PACKET_IDENTIFIER',
    'COMMAND_PACKET_SIZE',
    'COMMAND_HEADER_SIZE',
    'OBJECT_SIZE',
    'COMMAND_NUM_OBJECTS',
    'ACK_TIMEOUT_SECONDS',
    'CMD_NOP',
    'CMD_MUTE',
    'CMD_UNMUTE_ALARM',
    'ACK_CODE',
    'UdpReceiverConfig',
    'OpcUaConfig',
    'CommLayerConfig',
    'get_config',
    'set_config',
]

