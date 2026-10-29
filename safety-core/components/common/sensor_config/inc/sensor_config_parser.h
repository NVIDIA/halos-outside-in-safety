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

/*
 * sensor_pipelines_config.conf: the identities that appear on decision-driving
 * events. Consumed by mdx-client (Kafka sensorId -> pipelineID) and by the
 * CCPLEX SDMs (the reviewed sensor set behind the
 * all-safety-relevant-sensors-failed latch).
 *
 * BOTH this file and sensor_config.conf carry pipelineIds, so the distinction
 * matters and is not guessable from the ids alone:
 *
 *   sensor_config.conf            pipelineId, sensorName, rtspUrl
 *                                 -> the RTSP streams SAIM must open and watch.
 *   sensor_pipelines_config.conf  pipelineId, sensorName
 *                                 -> the identities that arrive on events which
 *                                    drive decisions.
 *
 * In 2D the two list the same cameras, because in 2D the cameras ARE the event
 * sources. In 3D they diverge: SAIM still watches every camera, while this file
 * names ONLY the BEV group, because that is what Kafka reports and what the SDM
 * reviewed set must contain. A reader who expects the 3D cameras to appear here
 * too has it backwards.
 *
 * No rtspUrl, deliberately: a group has no stream. That requirement is exactly
 * what prevents the group being expressed in sensor_config.conf at all, and it
 * is why these are separate files rather than one with an extra column.
 *
 * Handing sensorPipelinesParse a sensor_config.conf is refused on the third
 * field rather than silently ignoring the URL.
 */
typedef struct SensorPipelineEntry_t {
    uint8_t  pipelineId;
    char     sensorName[SENSOR_CONFIG_MAX_NAME_LEN];
} SensorPipelineEntry;

typedef struct SensorPipelines_t {
    SensorPipelineEntry entries[SENSOR_CONFIG_MAX_ENTRIES];
    size_t            count;
} SensorPipelines;

/*
 * sensor_group_config: which cameras constitute a BEV group.
 *
 * 3D only, and read by the PSS daemon alone. The daemon derives the group's
 * health from its members' health; the group is never a stream and never a
 * member of the SDM reviewed set in 2D.
 *
 * Exactly one group is supported. A second is an error rather than being
 * silently ignored, because a deployment that declares two has a topology the
 * daemon cannot represent.
 */
typedef struct SensorGroupConfig_t {
    uint8_t  groupPipelineId;
    char     groupName[SENSOR_CONFIG_MAX_NAME_LEN];
    uint8_t  memberPipelineIds[SENSOR_CONFIG_MAX_ENTRIES];
    size_t   memberCount;
    bool     present;
} SensorGroupConfig;

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
    /* sensor_pipelines_config / sensor_group_config */
    SENSOR_CONFIG_ERR_EMPTY_FILE,
    SENSOR_CONFIG_ERR_EMPTY_MEMBER_LIST,
    SENSOR_CONFIG_ERR_MULTIPLE_GROUPS,
    SENSOR_CONFIG_ERR_DUPLICATE_MEMBER,
    SENSOR_CONFIG_ERR_GROUP_ID_IS_MEMBER,
    /* sensor_pipelines_config.conf has exactly two fields. Distinct from
     * SENSOR_CONFIG_ERR_PARSE because that code's message names the
     * three-column sensor_config.conf shape, which tells an operator who handed
     * in the wrong file to ADD the rtspUrl -- the opposite of the fix. */
    SENSOR_CONFIG_ERR_PIPELINES_FIELD_COUNT
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

/* Parse `pipelineId, sensorName` rows. Empty file is an error: a deployment that
 * names no event sources has nothing to gate, and silently accepting it would
 * leave the SDM reviewed set empty and the latch unreachable. */
SensorConfigErr sensorPipelinesParse(const char* path, SensorPipelines* out);

/* Parse one `groupPipelineId, groupName, memberId|memberId|...` row. */
SensorConfigErr sensorGroupConfigParse(const char* path, SensorGroupConfig* out);

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
