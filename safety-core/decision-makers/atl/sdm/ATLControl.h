/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ATL_CTRL_ALGO_H
#define ATL_CTRL_ALGO_H

#include <string>
#include <cstdint>
#include <utility>

#include "pss_protocol.h"
#include "atl_cmd_pkt.h"

/* Launch the PSD decision loop.
 * gatewayIP/gatewayPort – NvPSD Gateway (UDP); client registers (REGR) and receives filtered requests + HB
 * plcIP/plcPort – PLC destination for commands/heartbeat (ports must be 1..65535; 0 rejected)
 * maxHbFailures – gateway HB miss threshold before tier-3 fail-safe (1..255; default 10)
 * decisionRepeatIntervalMs – PLC decision repeat period in ms (same command re-sent); 0 = off (event-driven only).
 *   Valid: 0, or 100..36000 inclusive (ms). State changes from events still trigger an immediate decision (out of band). */
int  launchATLControlAlgo(const std::string& gatewayIP,
                          std::uint16_t gatewayPort,
                          const std::string& plcIP,
                          std::uint16_t plcPort,
                          std::uint8_t maxHbFailures = 10U,
                          std::uint32_t decisionRepeatIntervalMs = 5000U);
void shutdownATLControlAlgo();

void onEventNotificationReceive(const DecisionRequest* request);
void evaluateATLDecision();

/* Helper declarations */
std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket();
void sendDecisionCommand(unsigned char command, bool trackAck,
                        const SensorData* sensorData = nullptr);
void ackHandlerLoop();

#endif // ATL_CTRL_ALGO_H