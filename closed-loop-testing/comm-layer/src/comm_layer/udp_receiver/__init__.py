# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
"""UDP Receiver module (simple, non-safe)"""

from .safety_receiver import SafetyReceiver, ReceiverStats

__all__ = ['SafetyReceiver', 'ReceiverStats']
