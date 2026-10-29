/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CROSSING_JUNCTION_SAFETY_CTRL_ALGO_H
#define CROSSING_JUNCTION_SAFETY_CTRL_ALGO_H

#include <string>
#include <cstdint>
#include <utility>

#include "pss_protocol.h"
#include "crossing_junction_safety_cmd_pkt.h"
#include "nvpsf_transport.h"

/* Bounds for the junction timing parameters. Declared here rather than in each
 * translation unit so the CLI parser and the library validate against one set of
 * limits and cannot drift apart. */
static constexpr std::int64_t  kJunctionHoldMsMin       = 100;
static constexpr std::int64_t  kJunctionHoldMsMax       = 60000;
static constexpr std::uint32_t kJunctionEvalPeriodMsMin = 20U;
static constexpr std::uint32_t kJunctionEvalPeriodMsMax = 5000U;

/* Launch the event-driven PSD decision loop for the Crossing-Junction-Safety-Monitor
 * SDM.
 *
 * The alarm rule is siren = A1 AND A2, where A1 is "a Forklift crossed a tripwire
 * inbound and has not left the junction ROI" and A2 is "at least one Person is
 * inside that ROI". See CrossingJunctionSafetyControl.cpp for the state machine and
 * for which mdx-client events carry each condition.
 *
 * gatewayIP/gatewayPort - NvPSD Gateway address (SDM sends REGR and receives DecisionRequests/HB)
 * plcIP/plcPort - PLC destination for commands/heartbeat
 * maxHbFailures - gateway HB miss threshold before tier-3 fail-safe (1..255; default 10)
 * decisionRepeatIntervalMs - repeat current PLC command at this period (ms); 0 = off.
 *   Valid: 0, or 100..36000 inclusive (ms). When the verdict changes, an immediate
 *   command is still sent — this timer only re-asserts the most recent decision so a
 *   single lost UDP datagram does not leave the PLC holding a stale command.
 * hbStaleMs/hbPeriodMs - gateway HB miss timing model; bounded positive milliseconds.
 * decisionFreshnessTimeoutMs - max age of a valid DecisionRequest before local safe-state fallback.
 * haloIP/haloPort - optional second fan-out destination for the same 64-byte CmdPacket,
 *   for a VST/VIOS overlay listener. Fire-and-forget (no ACK tracking); disabled when
 *   haloIP is empty or haloPort is 0 (default).
 * tripwireEngageWindowMs - how long after an inbound crossing a Forklift counts as
 *   engaged before the ROI has to confirm it (100..60000 ms).
 * roiEntryPulseMs - A1's backstop for a crossing the wire never reported: how long a
 *   Forklift counts as engaged from the moment it is first seen inside the junction
 *   ROI, independent of the tripwire (0 disables; otherwise 100..60000 ms).
 *   Measured on this deployment, 18 of 20 forklift ROI entries had no inbound
 *   tripwire event within the preceding 10 s, because only one camera sees the wire
 *   and single-shot tripwire events are also the ones most easily lost upstream.
 *   Without this, A1 is blind to most vehicles that enter the junction. It is a
 *   bounded pulse rather than a second latch on purpose: sustaining it from ROI
 *   occupancy would make A1 mean "a forklift is present" and, on this footage,
 *   would nearly triple siren time (201 s against 69 s over 13 minutes).
 * personRoiHoldMs / forkliftRoiHoldMs - how long a *sensor* stays believed without
 *   a report of either polarity before its occupancy is discounted (100..60000 ms).
 *   A backstop for a stream that dies; normal release comes from the vacated
 *   reports and clearDebounceMs, not from these.
 * clearDebounceMs - how long every sensor must read vacated before a level is
 *   released (100..60000 ms). This is what sets release latency. Release only;
 *   the siren is raised on the crossing frame with no added delay.
 * junctionEvalPeriodMs - cadence of the state-machine re-evaluation tick
 *   (20..5000 ms). Must not exceed clearDebounceMs, or the release would be
 *   rounded up to this period.
 * perSensorJunctions - false (default) pools both cameras as one physical crossing,
 *   which this deployment requires because only one camera sees the tripwire. true
 *   keys state by pipelineID, for genuinely separate crossings per camera.
 *
 * gatewayTransport/gatewaySocketPath - which link to reach the gateway on. The gateway
 *   offers this as AF_UNIX SOCK_SEQPACKET, so gatewayIP/gatewayPort are only consulted
 *   for an inet-kind transport and are otherwise carried for diagnostics. */
int  launchCrossingJunctionSafetyControlAlgo(const std::string& gatewayIP,
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
                                std::uint32_t tripwireEngageWindowMs = 5000U,
                                std::uint32_t personRoiHoldMs = 1500U,
                                std::uint32_t forkliftRoiHoldMs = 2000U,
                                std::uint32_t junctionEvalPeriodMs = 200U,
                                bool perSensorJunctions = false,
                                std::uint32_t clearDebounceMs = 500U,
                                std::uint32_t roiEntryPulseMs = 5000U,
                                NvPSFTransportKind gatewayTransport = NVPSF_TRANSPORT_UNIX_SOCKET,
                                const std::string& gatewaySocketPath = NVPSF_SOCKET_PATH_PSD_GATEWAY);
void shutdownCrossingJunctionSafetyControlAlgo();

void onEventNotificationReceive(const DecisionRequest* request);

/* Helper declarations */
std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket();
bool sendDecisionCommand(unsigned char command, bool trackAck,
                         const DecisionRequest* request = nullptr,
                         int winningSlot = -1);
void ackHandlerLoop();

#endif // CROSSING_JUNCTION_SAFETY_CTRL_ALGO_H
