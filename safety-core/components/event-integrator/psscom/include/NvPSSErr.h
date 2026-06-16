/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _NVPSS_ERR_H_
#define _NVPSS_ERR_H_

/**
 * @brief Enumeration of PSS communication error codes.
 *
 * This enumeration defines various error codes that can occur during PSS
 *     data source - sink communication.
 */
typedef enum NvPSSComErr
{
    NVPSSCOM_SUCCESS, /**< Operation completed successfully. */
    NVPSSCOM_FAIL,    /**< Generic failure. */
    NVPSSCOM_Q_FULL,  /**< Queue is full. */
    NVPSSCOM_NO_RSP   /**< No response received. */
} NvPSSComErr;

#endif //_NVPSS_ERR_H_
