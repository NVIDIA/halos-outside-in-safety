/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _NVPSS_COM_H_
#define _NVPSS_COM_H_

#include <stdint.h>

#include "NvPSSErr.h"

#define MAX_DATA_SIZE 2048

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NvPSSComCmd_t
{
    START_PSS,        /**< Command to start the data transfer withing PSS data souece and sink. */
    PSS_RDY,          /**< Command indicating PSS is ready. */
    PSS_NOT_RDY,      /**< Command indicating PSS is not ready. */
    FLOW_RATE,        /**< Command to set the flow rate. */
    FLOW_RATE_ACK,    /**< Acknowledgment for flow rate command. */
    DATA,             /**< Command to send data. */
    DATA_ACK,         /**< Acknowledgment for data command. */
    BAD_DATA,         /**< Command indicating bad data received. */
    BAD_DATA_ACK,     /**< Acknowledgment for bad data command. */
    CRC_MISMATCH,     /**< Command indicating a CRC mismatch. */
    PAUSE,            /**< Command to pause the operation. */
    PAUSE_ACK,        /**< Acknowledgment for pause command. */
    RESUME,           /**< Command to resume the operation. */
    RESUME_ACK,       /**< Acknowledgment for resume command. */
    BYE,              /**< Command to terminate the session. */
    BYE_ACK,          /**< Acknowledgment for bye command. */
    BAD_RESPONSE,     /**< Command indicating a bad response. */
    PING,             /**< Command to ping the receiver. */
    PING_ACK          /**< Acknowledgment for ping command. */
} NvPSSComCmd;

/**
 * @brief Structure representing a PSS communication packet.
 *
 * This structure defines the format of a packet used in PSS data source and sink
 * communication
 */
typedef struct NvPSSComPacket_t
{
    uint32_t pktSrNo;
    NvPSSComCmd cmd;     /**< Command associated with the packet. */
    uint32_t size;       /**< Size of the data in the packet. */
    uint8_t data[MAX_DATA_SIZE]; /**< Data buffer for the packet. */
    uint32_t ackSrNo;
    uint64_t checksum;     /**< Checksum for data integrity verification. */
} NvPSSComPacket;

/**
 * @brief Structure for data source callbacks.
 *
 * Structure to define the callback functions for the data source.
 */
typedef struct NvPSSComDataSrcCallbacks
{
    NvPSSComErr (*onDataRequest)(NvPSSComPacket* pkt); /**< Callback for data request. */
    NvPSSComErr (*onPause)(void);         /**< Callback for pause request. */
    NvPSSComErr (*onResume)(void);        /**< Callback for resume request. */
    NvPSSComErr (*onStop)(void);           /**< Callback for termination request. */
} NvPSSComDataSrcCallbacks;

/**
 * @brief Structure for data sink callbacks.
 *
 * Structure to define the callback functions for the data sink.
 */
typedef struct NvPSSComDataSinkCallbacks
{
    NvPSSComErr (*onDataAvailable)(NvPSSComPacket* pkt); /**< Callback for data availability. */
    NvPSSComErr (*onFlowRateChange)(uint8_t flowRate); /**< Callback for flow rate change. */
    NvPSSComErr (*onStop)(void);             /**< Callback for termination request. */
} NvPSSComDataSinkCallbacks;


/**
 * NvPSSComCtx
 */
typedef struct NvPSSComCtx* NvPSSComCtx_t ;

/**
 * @brief Creates a new NvPSSCom context.
 *
 * @return NvPSSComCtx* Pointer to the newly created NvPSSCom context.
 */
NvPSSComCtx* NvPSSComCreateContext();

/**
 * @brief Initializes the NvPSSCom data source.
 *
 * This function initializes the data source for PSS communication using the specified write and read channels.
 *
 * @param[in] ctx Pointer to the NvPSSCom context.
 * @param[in] writeChannel Name of the channel used for writing data.
 * @param[in] readChannel Name of the channel used for reading data.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the initialization.
 */
NvPSSComErr NvPSSComDataSrcInit(NvPSSComCtx* ctx, const char* writeChannel, const char* readChannel);


/**
 * @brief Initializes the NvPSSCom data sink.
 *
 * This function initializes the data sink for PSS communication using the specified write and read channels.
 *
 * @param[in] ctx Pointer to the NvPSSCom context.
 * @param[in] writeChannel Name of the channel used for writing data.
 * @param[in] readChannel Name of the channel used for reading data.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the initialization.
 */

NvPSSComErr NvPSSComDataSinkInit(NvPSSComCtx* ctx, const char* writeChannel, const char* readChannel );


/**
 * @brief Registers callbacks for the data source.
 *
 * Function to register the specified callbacks for the data source.
 *
 * @param[in] srcCallbacks Pointer to the structure containing the callbacks to register.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the registration.
 */
NvPSSComErr NvPSSDataSrcRegisterCallbacks(NvPSSComCtx* ctx, NvPSSComDataSrcCallbacks* srcCallbacks);

/**
 * @brief Registers callbacks for the data sink.
 *
 * Function to register the specified callbacks for the data sink.
 *
 * @param[in] sinkCallbacks Pointer to the structure containing the callbacks to register.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the callabck registration.
 */
NvPSSComErr NvPSSDataSinkRegisterCallbacks(NvPSSComCtx* ctx, NvPSSComDataSinkCallbacks* sinkCallbacks);

/**
 * @brief Starts the PSS communication.
 *
 * Function to initiate the PSS communication process.
 *
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComStart(NvPSSComCtx* ctx);

/**
 * @brief Sets the flow rate for PSS communication.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @param flowRate The flow rate to be set.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComSetFlowRate(NvPSSComCtx* ctx, uint8_t flowRate);

/**
 * @brief Pushes data to the PSS communication channel.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @param pkt Pointer to the NvPSSComPacket containing the data to be pushed.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComPushData(NvPSSComCtx* ctx, NvPSSComPacket* pkt);

/**
 * @brief Pauses the PSS communication.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComPause(NvPSSComCtx* ctx);

/**
 * @brief Resumes the PSS communication.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComResume(NvPSSComCtx* ctx);

/**
 * @brief Stops the PSS communication.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComStop(NvPSSComCtx* ctx);

/**
 * @brief Exits the PSS communication for the data source.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComDataSrcExit(NvPSSComCtx* ctx);

/**
 * @brief Exits the PSS communication for the data sink.
 *
 * @param ctx Pointer to the NvPSSCom context.
 * @return NvPSSComErr Returns an error code indicating the success or failure of the operation.
 */
NvPSSComErr NvPSSComDataSinkExit(NvPSSComCtx* ctx);

/**
 * @brief Destroys the NvPSSCom context.
 *
 * @param ctx Pointer to the NvPSSCom context to be destroyed.
 */
void NvPSSComDestroyContext(NvPSSComCtx* ctx);


#ifdef __cplusplus
}
#endif

#endif //_NVPSS_COM_H_
