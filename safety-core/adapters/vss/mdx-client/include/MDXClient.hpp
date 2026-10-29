/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_MDXCLIENT_HPP
#define MDX_CLIENT_MDXCLIENT_HPP

#include <cstdint>
#include <string>

#define MSGBUS_MSG_BUFFER_SIZE 8192
#define SEC_TO_NANO_SEC 1000000000UL

// Launch the config-driven MDX client. config_path is path to EventMappingConfig (binary or text).
// identity_config_path is the ONE file the Kafka sensorName -> pipelineId map is built from, and
// identity_is_pipelines selects which file it is:
//   false -> sensor_config.conf           (2D; pipelineId, sensorName, rtspUrl)
//   true  -> sensor_pipelines_config.conf (3D/BEV; pipelineId, sensorName -- the group, not the cameras)
// Exactly one is supplied; the caller has already refused both and neither. Loading both would resolve
// two topologies at once, letting a per-camera event in a 3D deployment be gated against one camera's
// health instead of the group's.
// When debug_mode is true: no PSS registration/heartbeat; matched events are printed to stdout instead of reported.
// broker_override: when non-empty, use this instead of localhost:9092 (e.g. "192.168.1.10:9092" for remote broker).
// pss_instance_id: PSS instance id, 0 (anonymous) or 1..PSS_MDX_INSTANCE_MAX; see --instance-id.
// Returns 0 on success.
int launchMDXClient(const std::string& config_path, const std::string& identity_config_path,
                    bool identity_is_pipelines = false,
                    bool debug_mode = false, const std::string& broker_override = "",
                    uint8_t pss_instance_id = 0U);

#endif
