/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PSS_DAEMON_H
#define PSS_DAEMON_H

#include "pss_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NvPSSDStatus_t
{
    NVPSSD_SUCCESS=0,
    NVPSSD_FAIL
}NvPSSDErr;

/* PSD Socket Communication System Definitions */
/* AF_UNIX path for the PSS<->PSD control channel. Lives under /run/nvpsf so
 * the launcher can bind-mount a single narrow directory into the container
 * rather than sharing /tmp with the host. */
#define PSS_DAEMON_SOCKET_PATH "/run/nvpsf/nvpssd_to_psd"
#define MAX_PSD_CLIENTS 32

/* Message types for PSD socket communication */
#define REGISTER_PSD_CLIENT 100
#define REGISTER_EVENT_TYPES 101
#define UNREGISTER_PSD_CLIENT 102

/**
 * @brief Registration message structure for PSD socket communication
 *
 * This structure is used for event type registration between PSS Daemon and PSD clients
 * DecisionRequest/DecisionResponse are sent directly without wrapping
 */
typedef struct PSDRegistrationMsg_t
{
    uint32_t msgType;
    uint32_t clientId;
    uint32_t eventTypesCount;
    EventType eventTypes[10];
} PSDRegistrationMsg;

/**
 * @brief Response structure for PSD socket communication
 *
 * This structure is used by PSS Daemon to respond to PSD client requests.
 */
typedef struct NvPSSDToPSDResp_t
{
    uint32_t clientId;
    uint32_t status;
} NvPSSDToPSDResp;


/*
 *  @brief Register a PSS client.
 *  @description This function allows a client to register with the PSS daemon,
 *  identifying itself by type. Multiple CLIENT_MDX clients may coexist;
 *  CLIENT_SAFETY_MONITOR and CLIENT_PSD_GATEWAY are singletons (only one active
 *  client of each is allowed at a time).
 *
 *  @param clientId  Output: unique identifier assigned by the daemon.
 *  @param clientType One of CLIENT_MDX, CLIENT_SAFETY_MONITOR, or CLIENT_PSD_GATEWAY.
 *
 *  @return NVPSSD_SUCCESS if the daemon accepted the registration, NVPSSD_FAIL otherwise.
 */
NvPSSDErr NvPSSRegisterPSSClient(uint32_t* clientId, uint8_t clientType);

/* REGISTER_CLIENT response status codes.
 * Returned in respPayload[4] of the RPC response (after the 4-byte client id). */
#define REGISTER_ACCEPTED               1  /* Registration successful */
#define REGISTER_REJECTED_DUPLICATE_TYPE 2  /* Another active client of the same type exists */
#define REGISTER_REJECTED_INVALID_TYPE   3  /* clientType not one of the valid CLIENT_* constants */

/*
 *  @brief Terminate a PSS client.
 *  @description This function allows a client to unregister from the PSS, typically when it's shutting down or no longer needs to monitor sensor.
 *
 *  @param clientId The ID of the client to be terminated.
 *
 *  @return A boolean indicating whether the client was successfully terminated.
 */
NvPSSDErr NvPSSTerminatePSSClient(const uint32_t clientId);

/* TERMINATE_CLIENT response status codes.
 * Returned in respPayload[0] of the RPC response. */
#define TERMINATE_ACCEPTED  1  /* Client termination completed successfully */

/* REPORT_SAFETY_EVENT response status codes.
 * Returned in respPayload[0] of the RPC response. */
#define REPORT_ACCEPTED                    1  /* Event accepted and forwarded to fusion / routing */
#define REPORT_REJECTED_CLIENTID_MISMATCH  2  /* fusionMetadata.clientID does not match the RPC-assigned client id */
#define REPORT_REJECTED_UNAUTHORIZED       3  /* Heartbeat fault, payload too small, or unauthorized client type */
#define REPORT_REJECTED_LOW_CONFIDENCE     4  /* confidenceLevel below configured acceptance threshold */
#define REPORT_REJECTED_VALIDATION_FAILED  5  /* CRC, schema, or field-range validation failed */

/**
 *  @brief Report a safety event.
 *
 *  Clients use this function to report safety events to the PSS daemon
 *  when they detect potential hazards.
 *
 *  @param clientId The ID of the client reporting the event.
 *  @param event    A SafetyEvent structure containing details about the
 *                  detected safety event.
 *
 *  @return NVPSSD_SUCCESS if the daemon accepted the event
 *          (REPORT_ACCEPTED), NVPSSD_FAIL otherwise.  The specific
 *          rejection reason (one of the REPORT_REJECTED_* codes) is
 *          logged server-side.
 */
NvPSSDErr NvPSSReportSafetyEvent(const uint32_t clientId, const SafetyEvent* event);

/* SEND_HEARTBEAT response status codes.
 * Returned in respPayload[0] of the RPC response. */
#define HEARTBEAT_ACK 1  /* PSS daemon acknowledges SEND_HEARTBEAT */

/* Heartbeat definitions */
#define HB_MSG           0x7E
#define HB_INTERVAL_MS   5000    /* Nominal period between SEND_HEARTBEAT (clients + gateway PSS-HB thread). */
#define HB_STALE_GRACE_MS 1500   /* Slack above HB_INTERVAL_MS so jitter/scheduling does not false-stale before next HB */
#define HB_TIMEOUT_MS    (HB_INTERVAL_MS + HB_STALE_GRACE_MS) /* Stale if no SEND_HEARTBEAT within this window */

/* Client type identifiers (used at registration and heartbeat) */
#define CLIENT_MDX              1
#define CLIENT_SAFETY_MONITOR   2
#define CLIENT_PSD_GATEWAY      3

/*
 *  @brief Send heartbeat to PSS daemon and wait for HEARTBEAT_ACK.
 *  @description All client types send the same 2-byte payload (HB_MSG, clientType). No token.
 *  Returns NVPSSD_SUCCESS only if the daemon acknowledges with HEARTBEAT_ACK.
 *
 *  @param clientId The ID of the client sending heartbeat.
 *  @param clientType The type of client (CLIENT_MDX, CLIENT_SAFETY_MONITOR, or CLIENT_PSD_GATEWAY).
 *  @return NVPSSD_SUCCESS on success, NVPSSD_FAIL on failure.
 */
NvPSSDErr NvPSSSendHeartbeat(const uint32_t clientId, const uint8_t clientType);

#ifdef __cplusplus
}
#endif

#endif

