/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/** Maximum number of decision-maker clients that can register over UDP. */
#define NVPSD_GATEWAY_MAX_CLIENTS 10

/**
 * Registration packet magic (4 bytes). Send to gateway to subscribe to EVENT_* types.
 */
#define NVPSD_GATEWAY_REG_MAGIC "REGR"

/** Heartbeat: gateway → client (4 bytes). */
#define NVPSD_GATEWAY_HB_MAGIC_GATEWAY "HBPG"
/** Heartbeat ACK: client → gateway (4 bytes). */
#define NVPSD_GATEWAY_HB_MAGIC_CLIENT  "HBPC"

/** Heartbeat message size: 4-byte magic + 4-byte seq (network byte order). */
#define NVPSD_GATEWAY_HB_MSG_SIZE 8
