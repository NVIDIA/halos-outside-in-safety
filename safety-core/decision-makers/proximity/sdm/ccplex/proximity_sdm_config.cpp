/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "proximity_sdm_config.h"

#include <fstream>
#include <string>

namespace {

/* Keep this order aligned with PROXIMITY_EVENT_TABLE in ProximityControl.cpp. */
const char* const kEventKeys[PROXIMITY_EVENT_COUNT] = {
    "humanoid_person_normal", "humanoid_person_reduce", "humanoid_person_stop",
    "forklift_person_normal", "forklift_person_reduce", "forklift_person_stop",
    "transporter_person_normal", "transporter_person_reduce", "transporter_person_stop",
    "fourier_humanoid_person_normal", "fourier_humanoid_person_reduce", "fourier_humanoid_person_stop"
};
const char* const kPairGroups[PROXIMITY_PAIR_GROUP_COUNT] = {
    "humanoid_person", "forklift_person", "transporter_person", "fourier_humanoid_person"
};

std::string trim(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1U);
}

bool parsePairGroups(const std::string& value, bool (&enabled)[PROXIMITY_PAIR_GROUP_COUNT])
{
    for (std::size_t i = 0U; i < PROXIMITY_PAIR_GROUP_COUNT; ++i)
        enabled[i] = false;
    std::size_t begin = 0U;
    for (;;)
    {
        const std::size_t comma = value.find(',', begin);
        const std::string name = trim(value.substr(begin, comma == std::string::npos
                                                  ? std::string::npos : comma - begin));
        std::size_t group = 0U;
        while (group < PROXIMITY_PAIR_GROUP_COUNT && name != kPairGroups[group])
            ++group;
        // Unknown, empty and duplicate selections are never silently ignored.
        if (group == PROXIMITY_PAIR_GROUP_COUNT || enabled[group])
            return false;
        enabled[group] = true;
        if (comma == std::string::npos)
            return true;
        begin = comma + 1U;
    }
}

static_assert(static_cast<int>(EVENT_0) == 0 &&
              static_cast<int>(SW_FAIL) == static_cast<int>(EVENT_23) + 1,
              "Update proximity config bounds when the generic event block changes");

bool parseEvent(const std::string& value, EventType& event)
{
    if (value.compare(0U, 6U, "EVENT_") != 0)
        return false;
    const std::string digits = value.substr(6U);
    if (digits.empty() || digits.size() > 2U ||
        (digits.size() > 1U && digits[0] == '0'))
        return false;
    unsigned raw = 0U;
    for (char ch : digits)
    {
        if (ch < '0' || ch > '9')
            return false;
        raw = raw * 10U + static_cast<unsigned>(ch - '0');
    }
    if (raw > static_cast<unsigned>(EVENT_23))
        return false;
    event = static_cast<EventType>(raw);
    return true;
}

} // namespace

const char* proximityEventKey(std::size_t index)
{
    return index < PROXIMITY_EVENT_COUNT ? kEventKeys[index] : "";
}

bool proximitySdmConfigLoad(const std::string& path, ProximitySdmConfig& out,
                           std::string& err)
{
    std::ifstream file(path.c_str());
    if (!file.is_open())
    {
        err = "cannot open configuration file";
        return false;
    }

    ProximitySdmConfig parsed = {};
    for (std::size_t i = 0U; i < PROXIMITY_EVENT_COUNT; ++i)
        parsed.events[i] = EVENT_UNKNOWN;
    bool seen[PROXIMITY_EVENT_COUNT] = {};
    // Preserve the existing nine-key configuration unless selection is explicit.
    bool enabled[PROXIMITY_PAIR_GROUP_COUNT] = { true, true, true, false };
    bool groupsSeen = false;
    std::string line;
    std::size_t lineNo = 0U;
    while (std::getline(file, line))
    {
        ++lineNo;
        const std::string text = trim(line.substr(0U, line.find('#')));
        if (text.empty())
            continue;
        const std::string prefix = "line " + std::to_string(lineNo) + ": ";
        const std::size_t eq = text.find('=');
        if (eq == std::string::npos)
        {
            err = prefix + "expected 'key = value'";
            return false;
        }
        const std::string key = trim(text.substr(0U, eq));
        const std::string value = trim(text.substr(eq + 1U));
        if (key == "pair_groups")
        {
            if (groupsSeen)
            {
                err = prefix + "key 'pair_groups' is set more than once";
                return false;
            }
            if (!parsePairGroups(value, enabled))
            {
                err = prefix + "pair_groups must select a non-empty, duplicate-free list of "
                      "humanoid_person, forklift_person, transporter_person, fourier_humanoid_person";
                return false;
            }
            groupsSeen = true;
            continue;
        }
        std::size_t slot = 0U;
        while (slot < PROXIMITY_EVENT_COUNT && key != kEventKeys[slot])
            ++slot;
        if (slot == PROXIMITY_EVENT_COUNT)
        {
            err = prefix + "unknown key '" + key + "'";
            return false;
        }
        if (seen[slot])
        {
            err = prefix + "key '" + key + "' is set more than once";
            return false;
        }
        if (!parseEvent(value, parsed.events[slot]))
        {
            err = prefix + "'" + key + "' must name EVENT_0..EVENT_23; fault/health codes are reserved";
            return false;
        }
        for (std::size_t i = 0U; i < PROXIMITY_EVENT_COUNT; ++i)
        {
            if (seen[i] && parsed.events[i] == parsed.events[slot])
            {
                err = prefix + "event types must be distinct: '" + key + "' and '" + kEventKeys[i] + "'";
                return false;
            }
        }
        seen[slot] = true;
    }
    if (file.bad() || (file.fail() && !file.eof()))
    {
        err = "error reading configuration file";
        return false;
    }
    for (std::size_t i = 0U; i < PROXIMITY_EVENT_COUNT; ++i)
    {
        const bool selected = enabled[i / PROXIMITY_TIERS_PER_GROUP];
        if (selected && !seen[i])
        {
            err = "missing required key '" + std::string(kEventKeys[i]) + "'";
            return false;
        }
        if (!selected && seen[i])
        {
            err = "key '" + std::string(kEventKeys[i]) + "' belongs to an unselected pair group";
            return false;
        }
    }
    out = parsed;
    return true;
}
