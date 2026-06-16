/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "NvPSD.h"
#include "NvPSDGatewayProtocol.h"
#include "pss_protocol.h"
#include <string>

/**
 * Launch the PSD gateway control loop.
 * Gateway binds to sdmIP:sdmPort for UDP: receives SDM client registrations (event subscriptions),
 * sends DecisionRequests to registered clients filtered by their EVENT_* subscription,
 * and runs heartbeat (HB) exchange with each registered client.
 *
 * @param sdmIP   IP address to bind (e.g. "0.0.0.0" for all interfaces).
 * @param sdmPort UDP port to bind (SDM clients send registrations and HB ACKs here).
 * @param numClients Maximum number of SDM clients (1..NVPSD_GATEWAY_MAX_CLIENTS).
 * @return 0 on success, negative value on error.
 */
int launchPSDControl(const std::string& sdmIP, unsigned int sdmPort, unsigned int numClients);

/**
 * Signal the PSD gateway control to shut down and release resources.
 */
void shutdownPSDControl();

/**
 * Close the gateway UDP socket. Call after NvPSDExit(ctx) so no callback uses the socket.
 */
void closePSDControlSocket();

/**
 * Callback invoked by NvPSD when a decision request is received from PSS.
 */
NvPSDErr onEventNotificationReceive(const DecisionRequest* request,
                                    DecisionResponse* response);

/**
 * Callback invoked by NvPSD when a shutdown is requested.
 */
NvPSDErr onPSDControlStop();
