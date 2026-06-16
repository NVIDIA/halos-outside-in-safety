# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""
Communication Layer Configuration

Configuration for UDP receiver and OPC UA server components.

NOTE: "ESLConfig" naming is legacy. This is NOT Edge Safety Link (ESL).
ESL is an abstraction layer for safety protocols that is not yet deployed.
"""

from dataclasses import dataclass
from typing import Optional


@dataclass
class UdpReceiverConfig:
    """UDP Receiver configuration"""
    host: str = "0.0.0.0"
    port: int = 12345
    buffer_size: int = 1024
    queue_size: int = 100
    send_ack: bool = True


@dataclass
class OpcUaConfig:
    """OPC UA Server configuration"""
    endpoint: str = "opc.tcp://0.0.0.0:4840/safety/"
    server_name: str = "Safety OPC UA Server"
    namespace: str = "http://nvidia.com/safety"


@dataclass
class CommLayerConfig:
    """Combined Communication Layer configuration"""
    udp_receiver: UdpReceiverConfig = None
    opc_ua: OpcUaConfig = None
    
    def __post_init__(self):
        if self.udp_receiver is None:
            self.udp_receiver = UdpReceiverConfig()
        if self.opc_ua is None:
            self.opc_ua = OpcUaConfig()


# Default configuration
_default_config: Optional[CommLayerConfig] = None


def get_config() -> CommLayerConfig:
    """Get default configuration"""
    global _default_config
    if _default_config is None:
        _default_config = CommLayerConfig()
    return _default_config


def set_config(config: CommLayerConfig):
    """Set default configuration"""
    global _default_config
    _default_config = config

