/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "pss_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/** DecisionRequest packet: gateway -> SDM (4 bytes). */
#define NVPSD_GATEWAY_DECISION_MAGIC "DRPG"

/** DecisionRequest packet version for the Gateway-to-SDM UDP envelope. */
#define NVPSD_GATEWAY_DECISION_PACKET_VERSION 1U

/** Valid Gateway-to-SDM sequence range. 0 and UINT32_MAX are reserved sentinels. */
#define NVPSD_GATEWAY_DECISION_TX_SEQ_MIN 1U
#define NVPSD_GATEWAY_DECISION_TX_SEQ_MAX (UINT32_MAX - 1U)
#define NVPSD_GATEWAY_DECISION_EPOCH_MAX UINT64_MAX

#pragma pack(push, 1)
typedef struct NvPSDGatewayDecisionRequestPacket {
    char magic[4];
    uint16_t version;
    uint16_t headerSize;
    uint64_t gatewayEpoch;
    uint32_t gatewayTxSeq;
    DecisionRequest request;
} NvPSDGatewayDecisionRequestPacket;
#pragma pack(pop)

#define NVPSD_GATEWAY_DECISION_HEADER_SIZE \
    ((uint16_t)offsetof(NvPSDGatewayDecisionRequestPacket, request))

static inline bool NvPSDGatewayDecisionPacketEpochIsValid(uint64_t epoch)
{
    return epoch != 0U && epoch != NVPSD_GATEWAY_DECISION_EPOCH_MAX;
}

static inline bool NvPSDGatewayDecisionPacketSequenceIsValid(uint32_t txSeq)
{
    return txSeq >= NVPSD_GATEWAY_DECISION_TX_SEQ_MIN &&
           txSeq <= NVPSD_GATEWAY_DECISION_TX_SEQ_MAX;
}

static inline bool NvPSDGatewayDecisionPacketHeaderIsValid(
    const NvPSDGatewayDecisionRequestPacket *packet)
{
    if (packet == NULL)
        return false;

    return memcmp(packet->magic, NVPSD_GATEWAY_DECISION_MAGIC, 4U) == 0 &&
           packet->version == NVPSD_GATEWAY_DECISION_PACKET_VERSION &&
           packet->headerSize == NVPSD_GATEWAY_DECISION_HEADER_SIZE &&
           NvPSDGatewayDecisionPacketEpochIsValid(packet->gatewayEpoch) &&
           NvPSDGatewayDecisionPacketSequenceIsValid(packet->gatewayTxSeq);
}
