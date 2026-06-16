/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_MDXCLIENT_HPP
#define MDX_CLIENT_MDXCLIENT_HPP

#include <string>

#define MSGBUS_MSG_BUFFER_SIZE 8192
#define SEC_TO_NANO_SEC 1000000000UL

// Launch the config-driven MDX client. config_path is path to EventMappingConfig (binary or text).
// sensor_config_path is path to sensor_config.conf for sensorName -> pipelineId lookup.
// When debug_mode is true: no PSS registration/heartbeat; matched events are printed to stdout instead of reported.
// broker_override: when non-empty, use this instead of localhost:9092 (e.g. "192.168.1.10:9092" for remote broker).
// Returns 0 on success.
int launchMDXClient(const std::string& config_path, const std::string& sensor_config_path,
                    bool debug_mode = false, const std::string& broker_override = "");

#endif
