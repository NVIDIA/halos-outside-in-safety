/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSD_H
#define NVPSD_H

#include "pss_protocol.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enumeration of PSD error codes.
 *
 * This enumeration defines various error codes that can occur during PSD communication.
 */
typedef enum NvPSDErr
{
    NVPSD_SUCCESS, /**< Operation completed successfully. */
    NVPSD_FAIL,    /**< Generic failure. */
    NVPSD_NO_RSP   /**< No response received. */
} NvPSDErr;

typedef enum NvPSDEndpoint_t
{
    NVPSD_PSS,
    NVPSD_CLIENT
}NvPSDEndpoint;

/**
 * @brief Structure for PSD callbacks.
 *
 * Structure to define the callback functions.
 */
typedef struct NvPSDCallbacks
{
    /**< Callback for decision request. */
    NvPSDErr (*processDecisionRequest)(const DecisionRequest* request, DecisionResponse* response);
    /**< Callback for decision reporting. */
    NvPSDErr (*publishDecisionResponse)(const DecisionResponse* response);
    /**< Callback for termination request. */
    NvPSDErr (*notifyShutdownRequest)(void);
} NvPSDCallbacks;


/**
 * NvPSDCtx
 */
typedef struct NvPSDCtx* NvPSDCtx_t ;

/**
 * @brief Creates a new NvPSD context.
 *
 * @return NvPSDCtx* Pointer to the newly created NvPSD context.
 */
NvPSDCtx* NvPSDCreateContext();


/**
 * @brief Initializes NvPSD with message queue communication.
 *
 * This function initializes the PSD-PSS communication using the specified write and read channels.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 * @param[in] writeChannel Name of the channel used for writing data.
 * @param[in] readChannel Name of the channel used for reading data.
 * @param[in] criticalWriteChannel Name of the channel used for writing CRITICAL severtity data.
 * @param[in] criticalReadChannel Name of the channel used for reading CRITICAL severity data.
 * @param[in] endpoint Param indicating whether endpoint is PSS or Client
 * @return NvPSDErr Returns an error code indicating the success or failure of the initialization.
 */
NvPSDErr NvPSDInitialize(NvPSDCtx* ctx, const char* writeChannel, const char* readChannel,
                        const char* criticalWriteChannel, const char* criticalReadChannel,
                        NvPSDEndpoint endpoint);

/**
 * @brief Initializes NvPSD with socket communication.
 *
 * This function initializes the PSD-PSS Daemon communication using UNIX domain sockets.
 * This is the preferred method for multi PSD Client connections to a single PSS Daemon.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 * @param[in] endpoint Param indicating whether endpoint is PSS or Client
 * @return NvPSDErr Returns an error code indicating the success or failure of the initialization.
 */
NvPSDErr NvPSDSocketInitialize(NvPSDCtx* ctx, NvPSDEndpoint endpoint);

/**
 * @brief Registers callbacks PSD.
 *
 * Function to register the specified callbacks for PSD.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 * @param[in] callbacks Pointer to the structure containing the callbacks to register.
 *
 * @return NvPSDErr Returns an error code indicating the success or failure of the registration.
 */
NvPSDErr NvPSDRegisterCallbacks(NvPSDCtx* ctx, NvPSDCallbacks* callbacks);

/**
 * @brief Control whether NvPSD registers a PSS client and runs the internal heartbeat thread.
 *
 * When @p externallyManaged is non-zero, NvPSD will not call NvPSSRegisterPSSClient or start psdHeartbeatLoop;
 * the embedding process must register and send heartbeats (e.g. PSD Gateway does so once for the process).
 * Call after NvPSDInitialize / NvPSDSocketInitialize and before NvPSDRegisterCallbacks. Fails if the
 * listener has already been started.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 * @param[in] externallyManaged Non-zero to disable internal PSS registration and heartbeat.
 * @return NvPSDErr NVPSD_SUCCESS or NVPSD_FAIL.
 */
NvPSDErr NvPSDSetPssHeartbeatExternallyManaged(NvPSDCtx* ctx, int externallyManaged);

/**
 * @brief Starts the PSD communication.
 *
 * Function to initiate the PSD communication process.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 *
 * @return NvPSDErr Returns an error code indicating the success or failure of the operation.
 */
NvPSDErr NvPSDStart(NvPSDCtx* ctx);

/*
 *  @brief Receive a DecisionRequest and send a DecisionResponse.
 *  @description  This function is used by PSD to receive a DecisionRequest, process it, and send back a DecisionResponse.
 *
 *  @param[in] ctx Pointer to the NvPSD context.
 *  @param[in] request A const reference to a DecisionRequest as input.
 *  @param[in] response A non-const reference to a DecisionResponse to store the result.
 *
 *  @return NvPSDErr Return an error code indicating whether the processing and communication were successful.
 */
NvPSDErr NvPSDProcessDecisionRequest(NvPSDCtx* ctx, const DecisionRequest* request, DecisionResponse* response);

/**
 * @brief Stops the PSD communication.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 *
 * @return NvPSDErr Returns an error code indicating the success or failure of the operation.
 */
NvPSDErr NvPSDStop(NvPSDCtx* ctx);

/**
 * @brief Exits NvPSD when created with Message Queue communication type with PSS Daemon.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 *
 * @return NvPSDErr Returns an error code indicating the success or failure of the operation.
 */
NvPSDErr NvPSDExit(NvPSDCtx* ctx);

/**
 * @brief Exits NvPSD when created with Socket communication type with PSS Daemon.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 *
 * @return NvPSDErr Returns an error code indicating the success or failure of the operation.
 */
NvPSDErr NvPSDSocketExit(NvPSDCtx* ctx);

/**
 * @brief Destroys the NvPSD context.
 *
 * @param[in] ctx Pointer to the NvPSD context to be destroyed.
 */
void NvPSDDestroyContext(NvPSDCtx* ctx);

/**
 * @brief Registers event types for socket-based PSD communication.
 *
 * @param[in] ctx Pointer to the NvPSD context.
 * @param[in] eventTypes Array of event types to register for.
 * @param[in] count Number of event types in the array.
 *
 * @return NvPSDErr Returns an error code indicating the success or failure of the registration.
 */
NvPSDErr NvPSDRegisterEventTypes(NvPSDCtx* ctx, const EventType* eventTypes, uint32_t count);

// Functions to update Configuration Data

#ifdef __cplusplus
}
#endif

#endif
