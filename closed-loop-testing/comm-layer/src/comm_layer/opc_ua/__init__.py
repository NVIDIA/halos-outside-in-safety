# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""OPC UA module for Communication Layer"""

from .safety_opc_server import SafetyOpcUaServer, HAS_OPCUA

__all__ = ['SafetyOpcUaServer', 'HAS_OPCUA']

