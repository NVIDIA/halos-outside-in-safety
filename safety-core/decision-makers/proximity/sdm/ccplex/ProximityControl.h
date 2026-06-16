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

/* Launch the event-driven PSD decision loop.
 * gatewayIP/gatewayPort - NvPSD Gateway address (SDM sends REGR and receives DecisionRequests/HB)
 * plcIP/plcPort - PLC destination for commands/heartbeat
 * maxHbFailures - gateway HB miss threshold before tier-3 fail-safe (1..255; default 10)
 * decisionRepeatIntervalMs - repeat current PLC command at this period (ms); 0 = off.
 *   Valid: 0, or 100..36000 inclusive (ms). When fusion state changes (new event), an
 *   immediate command is still sent event-driven — this timer only re-asserts the
 *   most recent decision so a single lost UDP datagram does not leave the PLC
 *   holding a stale command. */
int  launchProximityControlAlgo(const std::string& gatewayIP,
                                unsigned int gatewayPort,
                                const std::string& plcIP,
                                unsigned int plcPort,
                                std::uint8_t maxHbFailures = 10U,
                                std::uint32_t decisionRepeatIntervalMs = 5000U);
void shutdownProximityControlAlgo();

void onEventNotificationReceive(const DecisionRequest* request);

/* Helper declarations */
std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket();
void sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request = nullptr,
                         int winningSlot = -1);
void ackHandlerLoop();

#endif // PROXIMITY_CTRL_ALGO_H
