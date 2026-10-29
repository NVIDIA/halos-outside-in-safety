/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROXIMITY_CTRL_ALGO_H
#define PROXIMITY_CTRL_ALGO_H

#include <string>
#include <cstdint>
#include <utility>

#include "pss_protocol.h"
#include "proximity_cmd_pkt.h"
#include "nvpsf_transport.h"
#include "sdm_reviewed_sensor_set.hpp"
#include "proximity_sdm_config.h"

/* Startup-only setters. Binding and log path are frozen before control threads
 * start; the same binding drives both gateway registration and tier lookup. */
bool proximitySetSubscribedEvents(const ProximitySdmConfig& config);
bool proximitySetLogFile(const std::string& path);

/* Launch the event-driven PSD decision loop for the Proximity SDM
 * (startup-selected machine<->Person pairs).
 *
 * gatewayIP/gatewayPort - NvPSD Gateway address (SDM sends REGR and receives DecisionRequests/HB)
 * plcIP/plcPort - PLC destination for commands/heartbeat
 * maxHbFailures - gateway HB miss threshold before tier-3 fail-safe (1..255; default 10)
 * decisionRepeatIntervalMs - repeat current PLC command at this period (ms); 0 = off.
 *   Valid: 0, or 100..36000 inclusive (ms). When fusion state changes (new event), an
 *   immediate command is still sent event-driven — this timer only re-asserts the
 *   most recent decision so a single lost UDP datagram does not leave the PLC
 *   holding a stale command.
 * hbStaleMs/hbPeriodMs - gateway HB miss timing model; bounded positive milliseconds.
 * decisionFreshnessTimeoutMs - max age of a valid DecisionRequest before local safe-state fallback.
 * haloIP/haloPort - optional second fan-out destination for the same 64-byte CmdPacket,
 *   consumed by VST/VIOS's PfpHaloCommandListener to drive the Normal/Reduced/Stop halo
 *   overlay. Fire-and-forget (no ACK tracking); disabled when haloIP is empty or
 *   haloPort is 0 (default).
 *
 * gatewayTransport/gatewaySocketPath - which link to reach the gateway on. The gateway
 *   offers this as AF_UNIX SOCK_SEQPACKET, so gatewayIP/gatewayPort are only consulted
 *   for an inet-kind transport and are otherwise carried for diagnostics.
 *
 * sensorPipelinesConfigPath - sensor_pipelines_config.conf, the reviewed sensor set behind the
 *   all-safety-relevant-sensors-failed latch. Read once at startup. A missing or malformed file
 *   is fatal and returns non-zero: the alternative is a latch whose condition no event can ever
 *   satisfy, which fails silently.
 *
 * The forklift-operator gate takes no parameter here. Whether a Person is riding a
 * Forklift is decided in mdx-client, which sees every frame, and reaches this SDM as
 * its own event naming that pair; its criteria are configured in the mdx-client event
 * mapping rather than on this call. */
int  launchProximityControlAlgo(const std::string& gatewayIP,
                                unsigned int gatewayPort,
                                const std::string& plcIP,
                                unsigned int plcPort,
                                std::uint8_t maxHbFailures = 10U,
                                std::uint32_t decisionRepeatIntervalMs = 5000U,
                                std::uint32_t hbStaleMs = 5000U,
                                std::uint32_t hbPeriodMs = 5500U,
                                std::uint32_t decisionFreshnessTimeoutMs = 7000U,
                                const std::string& haloIP = std::string(),
                                unsigned int haloPort = 0U,
                                NvPSFTransportKind gatewayTransport = NVPSF_TRANSPORT_UNIX_SOCKET,
                                const std::string& gatewaySocketPath = NVPSF_SOCKET_PATH_PSD_GATEWAY,
                                const std::string& sensorPipelinesConfigPath =
                                    SDM_SENSOR_PIPELINES_CONFIG_DEFAULT);
void shutdownProximityControlAlgo();

void onEventNotificationReceive(const DecisionRequest* request);

/* Helper declarations */
std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket();
bool sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request = nullptr,
                         int winningSlot = -1);
void ackHandlerLoop();

#endif // PROXIMITY_CTRL_ALGO_H
