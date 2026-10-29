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

    return SENSOR_CONFIG_OK;
}

SensorConfigErr sensorPipelinesParse(const char* path, SensorPipelines* out)
{
    if (path == nullptr || out == nullptr)
        return SENSOR_CONFIG_ERR_NULL_ARG;

    std::memset(out, 0, sizeof(SensorPipelines));

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

        /* pipelineId, sensorName -- no URL.
         *
         * Split on the comma explicitly rather than with getline: a line ending
         * in a comma ("1,") leaves getline with nothing to extract and would
         * report a generic parse error, hiding which field is actually empty.
         * The diagnostic is the point of these checks. */
        const std::string::size_type c1 = trimmed.find(',');
        if (c1 == std::string::npos)
            return SENSOR_CONFIG_ERR_PIPELINES_FIELD_COUNT;

        std::string idStr   = trimWhitespace(trimmed.substr(0, c1));
        std::string rest    = trimmed.substr(c1 + 1U);

        /* A second comma means the caller handed us a sensor_config by mistake.
         * Refuse rather than ignoring the URL and behaving as though the file
         * were the right one -- and refuse with a code whose message names the
         * TWO-column shape, so the operator is told to drop the URL rather than
         * to supply one. */
        if (rest.find(',') != std::string::npos)
            return SENSOR_CONFIG_ERR_PIPELINES_FIELD_COUNT;

        std::string nameStr = trimWhitespace(rest);

        if (nameStr.empty())
            return SENSOR_CONFIG_ERR_EMPTY_SENSOR_NAME;

        char* endptr = nullptr;
        unsigned long idVal = std::strtoul(idStr.c_str(), &endptr, 10);
        if (endptr == idStr.c_str() || *endptr != '\0' || idVal < 1 || idVal > SENSOR_CONFIG_MAX_ENTRIES)
            return SENSOR_CONFIG_ERR_PIPELINE_ID_RANGE;

        uint8_t pid = static_cast<uint8_t>(idVal);

        if (!seenIds.insert(pid).second)
            return SENSOR_CONFIG_ERR_DUPLICATE_PIPELINE_ID;
        if (!seenNames.insert(nameStr).second)
            return SENSOR_CONFIG_ERR_DUPLICATE_SENSOR_NAME;

        SensorPipelineEntry& e = out->entries[out->count];
        e.pipelineId = pid;
        std::strncpy(e.sensorName, nameStr.c_str(), SENSOR_CONFIG_MAX_NAME_LEN - 1);
        e.sensorName[SENSOR_CONFIG_MAX_NAME_LEN - 1] = '\0';

        out->count++;
    }

    /* An empty file leaves the SDM reviewed set empty, which makes the
     * all-safety-relevant-sensors-failed latch unreachable. Fail loudly. */
    if (out->count == 0U)
        return SENSOR_CONFIG_ERR_EMPTY_FILE;

    return SENSOR_CONFIG_OK;
}

SensorConfigErr sensorGroupConfigParse(const char* path, SensorGroupConfig* out)
{
    if (path == nullptr || out == nullptr)
        return SENSOR_CONFIG_ERR_NULL_ARG;

    std::memset(out, 0, sizeof(SensorGroupConfig));

    std::ifstream file(path);
    if (!file.is_open())
        return SENSOR_CONFIG_ERR_FILE_OPEN;

    std::string line;

    while (std::getline(file, line))
    {
        std::string trimmed = trimWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        /* Exactly one group. A second is refused, not ignored: a deployment
         * declaring two has a topology the daemon cannot represent, and picking
         * the first would silently run a different system than the file says. */
        if (out->present)
            return SENSOR_CONFIG_ERR_MULTIPLE_GROUPS;

        /* groupPipelineId, groupName, memberId|memberId|...
         *
         * Explicit splits, for the same reason as sensorPipelinesParse: a line
         * ending in a comma must reach EMPTY_MEMBER_LIST, not a generic parse
         * error. */
        const std::string::size_type c1 = trimmed.find(',');
        if (c1 == std::string::npos)
            return SENSOR_CONFIG_ERR_PARSE;
        const std::string::size_type c2 = trimmed.find(',', c1 + 1U);
        if (c2 == std::string::npos)
            return SENSOR_CONFIG_ERR_PARSE;

        std::string idStr      = trimWhitespace(trimmed.substr(0, c1));
        std::string nameStr    = trimWhitespace(trimmed.substr(c1 + 1U, c2 - c1 - 1U));
        std::string membersStr = trimWhitespace(trimmed.substr(c2 + 1U));

        if (nameStr.empty())
            return SENSOR_CONFIG_ERR_EMPTY_SENSOR_NAME;

        char* endptr = nullptr;
        unsigned long idVal = std::strtoul(idStr.c_str(), &endptr, 10);
        if (endptr == idStr.c_str() || *endptr != '\0' || idVal < 1 || idVal > SENSOR_CONFIG_MAX_ENTRIES)
            return SENSOR_CONFIG_ERR_PIPELINE_ID_RANGE;

        out->groupPipelineId = static_cast<uint8_t>(idVal);
        std::strncpy(out->groupName, nameStr.c_str(), SENSOR_CONFIG_MAX_NAME_LEN - 1);
        out->groupName[SENSOR_CONFIG_MAX_NAME_LEN - 1] = '\0';

        std::istringstream ms(membersStr);
        std::string memberStr;
        std::unordered_set<uint8_t> seenMembers;
        while (std::getline(ms, memberStr, '|'))
        {
            memberStr = trimWhitespace(memberStr);
            if (memberStr.empty())
                return SENSOR_CONFIG_ERR_PARSE;

            if (out->memberCount >= SENSOR_CONFIG_MAX_ENTRIES)
                return SENSOR_CONFIG_ERR_TOO_MANY_ENTRIES;

            char* mend = nullptr;
            unsigned long mVal = std::strtoul(memberStr.c_str(), &mend, 10);
            if (mend == memberStr.c_str() || *mend != '\0' ||
                mVal < 1 || mVal > SENSOR_CONFIG_MAX_ENTRIES)
                return SENSOR_CONFIG_ERR_PIPELINE_ID_RANGE;

            const uint8_t mid = static_cast<uint8_t>(mVal);

            /* A group that contains itself would make its own health depend on
             * its own health. Refuse rather than recursing. */
            if (mid == out->groupPipelineId)
                return SENSOR_CONFIG_ERR_GROUP_ID_IS_MEMBER;

            if (!seenMembers.insert(mid).second)
                return SENSOR_CONFIG_ERR_DUPLICATE_MEMBER;

            out->memberPipelineIds[out->memberCount] = mid;
            out->memberCount++;
        }

        /* A group with no members has no derivable health; every verdict would
         * be vacuous. */
        if (out->memberCount == 0U)
            return SENSOR_CONFIG_ERR_EMPTY_MEMBER_LIST;

        out->present = true;
    }

    if (!out->present)
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
        case SENSOR_CONFIG_ERR_EMPTY_FILE:           return "file contains no entries";
        case SENSOR_CONFIG_ERR_EMPTY_MEMBER_LIST:    return "group has no member pipelineIds";
        case SENSOR_CONFIG_ERR_MULTIPLE_GROUPS:      return "more than one group declared (only one is supported)";
        case SENSOR_CONFIG_ERR_DUPLICATE_MEMBER:     return "duplicate member pipelineId in group";
        case SENSOR_CONFIG_ERR_GROUP_ID_IS_MEMBER:   return "group pipelineId listed as its own member";
        case SENSOR_CONFIG_ERR_PIPELINES_FIELD_COUNT:
            return "malformed line (expected exactly two fields: pipelineId, sensorName; "
                   "a line with three fields is a sensor_config.conf, which is a different file)";
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
