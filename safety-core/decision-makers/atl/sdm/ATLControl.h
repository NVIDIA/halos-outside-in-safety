/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ATL_CTRL_ALGO_H
#define ATL_CTRL_ALGO_H

#include <string>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pss_protocol.h"
#include "atl_cmd_pkt.h"
#include "atl_sdm_config.h"
#include "nvpsf_transport.h"
#include "sdm_reviewed_sensor_set.hpp"

/* Startup bindings, so the same binary can follow a deployment whose event map
 * emits different codes or whose safety-relevant camera set is not the one this
 * build was compiled with. Each must be called before launchATLControlAlgo and
 * returns false without changing anything on bad input; all are startup-only
 * and immutable once the control threads are running.
 *
 * THERE ARE NO DEFAULTS. Both bindings start unset and launchATLControlAlgo
 * refuses to run until each has been set, so the binary cannot fall back to a
 * binding the configuration did not state. A defaulted event set is not a
 * cosmetic problem here: a second ATL SDM started without a configuration would
 * silently register for the first one's codes, track the first one's trailer,
 * and emit a mute permission for a trailer it has no evidence about. Same rule
 * and same reason as `enableFusion` and `sensorConfig` in nvpss.conf. */

/* Bind the six situations to event codes, in the order they are declared in
 * ATLControl.cpp and supplied by the configuration file: forklift-entered,
 * forklift-exited, person-entered, person-exited, roi-violation, roi-cleared.
 * Accepts only EVENT_0..EVENT_23, rejects a count other than ATL_EVENT_COUNT,
 * and rejects duplicates: two situations sharing a code would leave the
 * dispatch order deciding which one an event means. Also rejects changes once
 * launchATLControlAlgo freezes the binding, even if later initialization fails.
 *
 * @param source Where this binding came from, for the startup log. Must be
 *        non-null and non-empty; atl_sdm passes "--config", which is its only
 *        source. Retained as a parameter rather than hard-coded because an
 *        empty recorded source is also how launchATLControlAlgo distinguishes
 *        "never bound" from "bound", and because a second entry point in the
 *        dev package would otherwise report a provenance it did not have. */
bool atlSetSubscribedEvents(const EventType* events, std::size_t count,
                            const char* source);


/* Choose this instance's log file. Two ATL SDMs started from one working
 * directory would otherwise both append to atl_sdm.log, interleaving their
 * lines with nothing in the file to tell the instances apart. Returns false
 * for an empty path or once logging has started, even if opening failed. */
bool atlSetLogFile(const std::string& path);

/* Launch the PSD decision loop.
 * gatewayIP/gatewayPort – NvPSD Gateway endpoint when gatewayTransport is UDP_SOCKET;
 *   client registers (REGR) and receives filtered requests + HB. Ignored for UNIX_SOCKET.
 * plcIP/plcPort – PLC destination for commands/heartbeat (ports must be 1..65535; 0 rejected)
 * maxHbFailures – gateway HB miss threshold before tier-3 fail-safe (1..255; default 10)
 * decisionRepeatIntervalMs – PLC decision repeat period in ms (same command re-sent); 0 = off (event-driven only).
 *   Valid: 0, or 100..36000 inclusive (ms). State changes from events still trigger an immediate decision (out of band).
 * hbStaleMs/hbPeriodMs – gateway HB miss timing model; bounded positive milliseconds.
 * decisionFreshnessTimeoutMs – max age of a valid DecisionRequest before local safe-state fallback.
 * gatewayTransport – upstream transport for the gateway link. Startup-only and
 *   immutable; the caller has already rejected anything unusable, so this
 *   is never a fallback. The selection is carried into the control layer and
 *   logged but never reached this layer.
 * gatewaySocketPath – AF_UNIX path when gatewayTransport is UNIX_SOCKET; ignored otherwise.
 * sensorPipelinesConfigPath – sensor_pipelines_config.conf, the reviewed sensor set behind the
 *   all-safety-relevant-sensors-failed latch. Read once at startup. A missing or malformed file
 *   is fatal and returns non-zero: the alternative is a latch whose condition no event can ever
 *   satisfy, which fails silently. */
int  launchATLControlAlgo(const std::string& gatewayIP,
                          std::uint16_t gatewayPort,
                          const std::string& plcIP,
                          std::uint16_t plcPort,
                          std::uint8_t maxHbFailures = 10U,
                          std::uint32_t decisionRepeatIntervalMs = 5000U,
                          std::uint32_t hbStaleMs = 5000U,
                          std::uint32_t hbPeriodMs = 5500U,
                          std::uint32_t decisionFreshnessTimeoutMs = 7000U,
                          NvPSFTransportKind gatewayTransport = NVPSF_TRANSPORT_UNIX_SOCKET,
                          const std::string& gatewaySocketPath = NVPSF_SOCKET_PATH_PSD_GATEWAY,
                          const std::string& sensorPipelinesConfigPath =
                              SDM_SENSOR_PIPELINES_CONFIG_DEFAULT);
void shutdownATLControlAlgo();

void onEventNotificationReceive(const DecisionRequest* request);
void evaluateATLDecision();

/* Helper declarations */
std::pair<uint64_t, uint64_t> getCurrentUTCTimeForPacket();
bool sendDecisionCommand(unsigned char command, bool trackAck,
                        const SensorData* sensorData = nullptr);
void ackHandlerLoop();

#endif // ATL_CTRL_ALGO_H
