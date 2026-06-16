/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sensor_config_parser.h"

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <algorithm>

static std::string trimWhitespace(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1])))
        --j;
    return s.substr(i, j - i);
}

SensorConfigErr sensorConfigParse(const char* path, SensorConfig* out)
{
    if (path == nullptr || out == nullptr)
        return SENSOR_CONFIG_ERR_NULL_ARG;

    std::memset(out, 0, sizeof(SensorConfig));

    std::ifstream file(path);
    if (!file.is_open())
        return SENSOR_CONFIG_ERR_FILE_OPEN;

    std::unordered_set<uint8_t>     seenIds;
    std::unordered_set<std::string> seenNames;
    std::string line;

    while (std::getline(file, line))
    {
        std::string trimmed = trimWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        if (out->count >= SENSOR_CONFIG_MAX_ENTRIES)
            return SENSOR_CONFIG_ERR_TOO_MANY_ENTRIES;

        /* Split by comma: pipelineId, sensorName, rtspUrl */
        std::istringstream ss(trimmed);
        std::string idStr, nameStr, urlStr;

        if (!std::getline(ss, idStr, ','))
            return SENSOR_CONFIG_ERR_PARSE;
        if (!std::getline(ss, nameStr, ','))
            return SENSOR_CONFIG_ERR_PARSE;
        /* URL gets the rest of the line (may contain commas in query params) */
        if (!std::getline(ss, urlStr))
            return SENSOR_CONFIG_ERR_PARSE;

        idStr   = trimWhitespace(idStr);
        nameStr = trimWhitespace(nameStr);
        urlStr  = trimWhitespace(urlStr);

        if (nameStr.empty())
            return SENSOR_CONFIG_ERR_EMPTY_SENSOR_NAME;

        if (urlStr.empty())
            return SENSOR_CONFIG_ERR_EMPTY_RTSP_URL;

        char* endptr = nullptr;
        unsigned long idVal = std::strtoul(idStr.c_str(), &endptr, 10);
        if (endptr == idStr.c_str() || *endptr != '\0' || idVal < 1 || idVal > SENSOR_CONFIG_MAX_ENTRIES)
            return SENSOR_CONFIG_ERR_PIPELINE_ID_RANGE;

        uint8_t pid = static_cast<uint8_t>(idVal);

        if (!seenIds.insert(pid).second)
            return SENSOR_CONFIG_ERR_DUPLICATE_PIPELINE_ID;

        if (!seenNames.insert(nameStr).second)
            return SENSOR_CONFIG_ERR_DUPLICATE_SENSOR_NAME;

        SensorConfigEntry& e = out->entries[out->count];
        e.pipelineId = pid;
        std::strncpy(e.sensorName, nameStr.c_str(), SENSOR_CONFIG_MAX_NAME_LEN - 1);
        e.sensorName[SENSOR_CONFIG_MAX_NAME_LEN - 1] = '\0';
        std::strncpy(e.rtspUrl, urlStr.c_str(), SENSOR_CONFIG_MAX_URL_LEN - 1);
        e.rtspUrl[SENSOR_CONFIG_MAX_URL_LEN - 1] = '\0';

        out->count++;
    }

    if (out->count == 0)
        return SENSOR_CONFIG_ERR_EMPTY_FILE;

    return SENSOR_CONFIG_OK;
}

const char* sensorConfigErrStr(SensorConfigErr err)
{
    switch (err)
    {
        case SENSOR_CONFIG_OK:                       return "OK";
        case SENSOR_CONFIG_ERR_FILE_OPEN:            return "cannot open sensor config file";
        case SENSOR_CONFIG_ERR_PARSE:                return "malformed line (expected: pipelineId, sensorName, rtspUrl)";
        case SENSOR_CONFIG_ERR_DUPLICATE_PIPELINE_ID: return "duplicate pipelineId";
        case SENSOR_CONFIG_ERR_DUPLICATE_SENSOR_NAME: return "duplicate sensorName";
        case SENSOR_CONFIG_ERR_PIPELINE_ID_RANGE:    return "pipelineId out of range [1..8]";
        case SENSOR_CONFIG_ERR_EMPTY_SENSOR_NAME:    return "empty sensorName";
        case SENSOR_CONFIG_ERR_TOO_MANY_ENTRIES:     return "too many entries (max 8)";
        case SENSOR_CONFIG_ERR_NULL_ARG:             return "null argument";
        case SENSOR_CONFIG_ERR_EMPTY_RTSP_URL:       return "empty rtspUrl";
        case SENSOR_CONFIG_ERR_EMPTY_FILE:            return "sensor config file contains no entries";
        default:                                     return "unknown error";
    }
}

/* ---- C++ convenience wrappers ---- */

std::vector<SensorConfigEntry> sensorConfigLoad(const std::string& path, std::string* errMsg)
{
    SensorConfig cfg;
    SensorConfigErr err = sensorConfigParse(path.c_str(), &cfg);
    if (err != SENSOR_CONFIG_OK)
    {
        if (errMsg)
            *errMsg = std::string(sensorConfigErrStr(err)) + ": " + path;
        return {};
    }
    return std::vector<SensorConfigEntry>(cfg.entries, cfg.entries + cfg.count);
}

std::unordered_map<std::string, uint8_t> sensorConfigNameToIdMap(const std::vector<SensorConfigEntry>& entries)
{
    std::unordered_map<std::string, uint8_t> m;
    m.reserve(entries.size());
    for (const auto& e : entries)
        m[e.sensorName] = e.pipelineId;
    return m;
}

std::unordered_map<uint8_t, std::string> sensorConfigIdToNameMap(const std::vector<SensorConfigEntry>& entries)
{
    std::unordered_map<uint8_t, std::string> m;
    m.reserve(entries.size());
    for (const auto& e : entries)
        m[e.pipelineId] = e.sensorName;
    return m;
}
