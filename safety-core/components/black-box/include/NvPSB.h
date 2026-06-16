/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSB_H
#define NVPSB_H


#include <string>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NvPSBEndpoint_t
{
    NVPSB_PSS_SOURCE,
    NVPSB_PSS_SINK,
    NVPSB_PSS_DAEMON,
    NVPSB_PSD_CLIENT,
    NVPSB_SDM_CLIENT
}NvPSBEndpoint;

typedef enum NvPSBLogLevel_t
{
    NVPSB_LOG_EMERG, //system is unusable
    NVPSB_LOG_ALERT, //action must be taken immediately
    NVPSB_LOG_CRIT, //critical conditions
    NVPSB_LOG_ERR, //error conditions
    NVPSB_LOG_WARNING, //warning conditions
    NVPSB_LOG_NOTICE, //normal, but significant, condition
    NVPSB_LOG_INFO, //informational message
    NVPSB_LOG_DEBUG //debug-level message
}NvPSBLogLevel;

/**
 * @brief Enumeration of PSB error codes.
 *
 * This enumeration defines various error codes that can occur during PSB communication.
 */
typedef enum NvPSBErr
{
    NVPSB_SUCCESS, /**< Operation completed successfully. */
    NVPSB_FAIL,    /**< Generic failure. */
    NVPSB_NO_RSP,   /**< No response received. */
    NVPSB_UNINITIALIZED /**< Module not initialised before performing action. */
} NvPSBErr;

/**
 * @brief Initializes NvPSB.
 *
 * This function initializes PSB communication channel to log data.
 *
 * @param[in] ident Name of the channel used for writing data.
 * @param[in] endpoint Param indicating whether endpoint is
 *                     PSS SOURCE / SINK / DAEMON or PSD Client
 * @return NvPSBErr Returns an error code indicating the success or failure of the initialization.
 */
NvPSBErr NvPSBInitialize(const char* ident, NvPSBEndpoint endpoint);

/**
  * @brief Writes data via PSB to the secure storage
  *
  * @param level Logging Level
  * @param data string containing all the data to be written
  * @param additionalInfo string containing additonal data to be written
  *
  * @return NvPSBErr Returns an error code indicating the success or failure of the operation.
  */
NvPSBErr NvPSBWriteData(NvPSBLogLevel level, const std::string data, const std::string additionalInfo);

/**
 * @brief Exits NvPSB.
 *
 * @return NvPSBErr Returns an error code indicating the success or failure of the operation.
 */
NvPSBErr NvPSBExit();


// Functions for exchanging heartbeat messages

#ifdef __cplusplus
}
#endif

#endif
