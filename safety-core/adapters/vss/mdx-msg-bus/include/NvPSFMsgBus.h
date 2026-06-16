/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file NvPSFMsgBus.h
 * @brief Interface for message bus operations.
 *
 * This header file provides an interface for creating, sending, receiving,
 * and managing message bus producers and consumers.
 *
 * The functions defined in this header can be used in both C and C++
 * programs.
 */

#ifndef NVPSF_MSGBUS_H
#define NVPSF_MSGBUS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NvPSFMSGBUS_SUCCESS = 0,
    NvPSFMSGBUS_FAIL = 1
} NvPSFMsgBusErr;

typedef struct {
    NvPSFMsgBusErr err;
    int retCode; // 0 for success, or error code
    size_t recvd_bytes; // For receive, number of bytes received
} NvPSFMsgBusStatus;

typedef enum {
    MSGBUS_PRODUCER = 0,
    MSGBUS_CONSUMER = 1
} NvPSFMsgBusEndpointType;

typedef struct NvPSFMsgBusHandle_t NvPSFMsgBusHandle;

/**
 * @brief Create a message bus producer or consumer handle.
 *
 * @param brokers Comma-separated list of broker addresses.
 * @param topic Topic name.
 * @param endpointType Producer or Consumer.
 * @param group_id (Consumer only) Consumer group ID, NULL for producer.
 * @param out_handle Pointer to handle pointer to be set on success.
 * @return NvPSFMsgBusStatus indicating success or failure.
 */
NvPSFMsgBusStatus NvPSFMsgBusCreate(const char* brokers, const char* topic, NvPSFMsgBusEndpointType endpointType, const char* group_id, NvPSFMsgBusHandle** out_handle);

/**
 * @brief Destroy a message bus handle and free resources.
 *
 * @param handle Pointer to NvPSFMsgBusHandle.
 * @return NvPSFMsgBusStatus indicating success or failure.
 */
NvPSFMsgBusStatus NvPSFMsgBusDestroy(NvPSFMsgBusHandle* handle);

/**
 * @brief Send a message (producer only).
 *
 * @param handle NvPSFMsgBusHandle for producer.
 * @param msg Pointer to message data.
 * @param msgLen Length of message.
 * @return NvPSFMsgBusStatus indicating success or failure.
 */
NvPSFMsgBusStatus NvPSFMsgBusSend(NvPSFMsgBusHandle* handle, const void* msg, size_t msgLen);

/**
 * @brief Receive a message (consumer only).
 *
 * @param handle NvPSFMsgBusHandle for consumer.
 * @param buffer Buffer to store received message.
 * @param bufferLen Size of buffer.
 * @param outLen Pointer to size_t to store actual message length.
 * @return NvPSFMsgBusStatus indicating success or failure and number of bytes received.
 */
NvPSFMsgBusStatus NvPSFMsgBusReceive(NvPSFMsgBusHandle* handle, void* buffer, size_t bufferLen, size_t* outLen);

/**
 * @brief Seek to the end of partitions after subscription (consumer only).
 *
 * @param handle NvPSFMsgBusHandle for consumer.
 * @return NvPSFMsgBusStatus indicating success or failure.
 */
NvPSFMsgBusStatus NvPSFMsgBusSeekToEnd(NvPSFMsgBusHandle* handle);

#ifdef __cplusplus
}
#endif

#endif // NVPSF_MSGBUS_H
