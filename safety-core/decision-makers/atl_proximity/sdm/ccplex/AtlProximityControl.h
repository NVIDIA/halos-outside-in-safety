/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Forklift-to-person proximity SDM for the combined ATL + proximity deployment.
 * Independent of decision-makers/proximity; see AtlProximityControl.cpp for the
 * relationship between the two and for what differs.
 */

#ifndef ATL_PROXIMITY_CTRL_ALGO_H
#define ATL_PROXIMITY_CTRL_ALGO_H

#include <string>
#include <cstdint>
#include <utility>

#include "pss_protocol.h"
#include "proximity_cmd_pkt.h"
#include "atl_proximity_pair_fusion.h"

/* Launch the event-driven PSD decision loop.
 * gatewayIP/gatewayPort - NvPSD Gateway address (SDM sends REGR and receives DecisionRequests/HB)
 * plcIP/plcPort - PLC destination for commands/heartbeat
 * maxHbFailures - gateway HB miss threshold before tier-3 fail-safe (1..255; default 10)
 * decisionRepeatIntervalMs - repeat current PLC command at this period (ms); 0 = off.
 *   Valid: 0, or 100..36000 inclusive (ms). When fusion state changes (new event), an
 *   immediate command is still sent event-driven. This timer re-derives the current
 *   worst case from retained pair state and re-asserts it, so a single lost UDP
 *   datagram does not leave the PLC holding a stale command, and so a hazard that
 *   ended by pair expiry is released without waiting for the next event.
 * hbStaleMs/hbPeriodMs - gateway HB miss timing model; bounded positive milliseconds.
 * decisionFreshnessTimeoutMs - max age of a valid DecisionRequest before local safe-state fallback.
 * pairTtlMs - how long a forklift/person pair keeps contributing to the worst case
 *   after its last observation. Must be in
 *   ATLPXC_PAIR_TTL_MS_MIN..ATLPXC_PAIR_TTL_MS_MAX; out-of-range is rejected. */
int  launchAtlProximityControlAlgo(const std::string& gatewayIP,
                                unsigned int gatewayPort,
                                const std::string& plcIP,
                                unsigned int plcPort,
                                std::uint8_t maxHbFailures = 10U,
                                std::uint32_t decisionRepeatIntervalMs = 5000U,
                                std::uint32_t hbStaleMs = 5000U,
                                std::uint32_t hbPeriodMs = 5500U,
                                std::uint32_t decisionFreshnessTimeoutMs = 7000U,
                                std::uint32_t pairTtlMs = ATLPXC_PAIR_TTL_MS_DEFAULT);
void shutdownAtlProximityControlAlgo();

void onAtlProximityEventNotificationReceive(const DecisionRequest* request);

/* Helper declarations */
std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket();
/* pairObjects, when non-null, supplies the object records instead of the
 * request/winningSlot pair. The command reflects retained state that can
 * predate the current request, so the responsible pair may be absent from it. */
bool sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request = nullptr,
                         int winningSlot = -1,
                         const AtlPxcPairObjects* pairObjects = nullptr);
void ackHandlerLoop();

#endif // ATL_PROXIMITY_CTRL_ALGO_H
