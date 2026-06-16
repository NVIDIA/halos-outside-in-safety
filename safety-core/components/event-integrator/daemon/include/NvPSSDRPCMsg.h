/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSSD_RPC_MSGS_H
#define NVPSSD_RPC_MSGS_H

#include <cstring>
#include <utility>

#define MAX_RPC_MSG_SIZE 512

typedef enum NvPSSDRPCBackend_t
{
    SOCKET=0,
    NVSCI
}NvPSSDRPCBackend;

typedef enum NvPSSDRPCMsg_t
{
    REGISTER_CLIENT=0,
    TERMINATE_CLIENT,
    REPORT_SAFETY_EVENT,
    SEND_HEARTBEAT
}NvPSSDRPCMsg;

typedef struct NvPSSDRPCMsgReq_t
{
    NvPSSDRPCMsg msg;
    uint64_t reqSeqNo;
    uint16_t size;
    uint8_t reqPayload[MAX_RPC_MSG_SIZE];
} NvPSSDRPCMsgReq;

typedef struct NvPSSDRPCMsgResp_t
{
    uint64_t respSeqNo;
    uint8_t size;
    uint8_t respPayload[MAX_RPC_MSG_SIZE];
}NvPSSDRPCMsgResp;

#endif
