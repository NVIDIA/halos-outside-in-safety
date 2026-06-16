/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SENSOR_CONFIG_PARSER_H
#define SENSOR_CONFIG_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SENSOR_CONFIG_MAX_NAME_LEN   64
#define SENSOR_CONFIG_MAX_URL_LEN    256
#define SENSOR_CONFIG_MAX_ENTRIES     8

typedef struct SensorConfigEntry_t {
    uint8_t  pipelineId;
    char     sensorName[SENSOR_CONFIG_MAX_NAME_LEN];
    char     rtspUrl[SENSOR_CONFIG_MAX_URL_LEN];
} SensorConfigEntry;

typedef struct SensorConfig_t {
    SensorConfigEntry entries[SENSOR_CONFIG_MAX_ENTRIES];
    size_t            count;
} SensorConfig;

typedef enum SensorConfigErr_t {
    SENSOR_CONFIG_OK = 0,
    SENSOR_CONFIG_ERR_FILE_OPEN,
    SENSOR_CONFIG_ERR_PARSE,
    SENSOR_CONFIG_ERR_DUPLICATE_PIPELINE_ID,
    SENSOR_CONFIG_ERR_DUPLICATE_SENSOR_NAME,
    SENSOR_CONFIG_ERR_PIPELINE_ID_RANGE,
    SENSOR_CONFIG_ERR_EMPTY_SENSOR_NAME,
    SENSOR_CONFIG_ERR_TOO_MANY_ENTRIES,
    SENSOR_CONFIG_ERR_NULL_ARG,
    SENSOR_CONFIG_ERR_EMPTY_RTSP_URL,
    SENSOR_CONFIG_ERR_EMPTY_FILE
} SensorConfigErr;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a sensor configuration file.
 *
 * File format: one sensor per line, fields separated by commas.
 *   pipelineId, sensorName, rtspUrl
 * Lines starting with '#' are comments. Blank lines are ignored.
 *
 * @param path    Path to the configuration file.
 * @param out     Output structure populated on success.
 * @return SENSOR_CONFIG_OK on success, an error code otherwise.
 */
SensorConfigErr sensorConfigParse(const char* path, SensorConfig* out);

/**
 * @brief Return a human-readable string for a SensorConfigErr code.
 */
const char* sensorConfigErrStr(SensorConfigErr err);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <vector>
#include <unordered_map>

/**
 * @brief C++ convenience: parse sensor config and return entries as a vector.
 * @param path    Path to the configuration file.
 * @param errMsg  If non-null and an error occurs, populated with a description.
 * @return Vector of SensorConfigEntry (empty on failure).
 */
std::vector<SensorConfigEntry> sensorConfigLoad(const std::string& path, std::string* errMsg = nullptr);

/**
 * @brief Build a sensorName -> pipelineId lookup map from parsed entries.
 */
std::unordered_map<std::string, uint8_t> sensorConfigNameToIdMap(const std::vector<SensorConfigEntry>& entries);

/**
 * @brief Build a pipelineId -> sensorName lookup map from parsed entries.
 */
std::unordered_map<uint8_t, std::string> sensorConfigIdToNameMap(const std::vector<SensorConfigEntry>& entries);

#endif /* __cplusplus */

#endif /* SENSOR_CONFIG_PARSER_H */
