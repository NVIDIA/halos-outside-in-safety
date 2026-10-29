/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "atl_sdm_config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/* Keys accepted in the configuration file. Index 0..ATL_EVENT_COUNT-1 are the
 * event keys and must stay in step with AtlSdmConfig::events and with the
 * situation order in ATLControl.cpp -- reordering this table silently rebinds
 * every situation. */
static const char* const kEventKeys[ATL_EVENT_COUNT] = {
    "forklift_entered",
    "forklift_exited",
    "person_entered",
    "person_exited",
    "roi_violation",
    "roi_cleared"
};

/* Retired. The safety-relevant pipeline set moved to
 * sensor_pipelines_config.conf, which every CCPLEX SDM and mdx-client read, so
 * one reviewed set serves the whole deployment. Named here rather than left to
 * fall through to "unknown key" so an existing file says what to do next --
 * silently ignoring it would run the SDM on a reviewed set the operator
 * believed they had stated here. */
static const char kRetiredPipelineKey[] = "safety_pipelines";

/* Whitespace this parser trims. Matches NvPSSConfigParser so the two config
 * formats on this system behave the same way on a hand-edited file. */
static const char kBlanks[] = " \t\r\n";

static std::string atlTrim(const std::string& str)
{
    const std::size_t first = str.find_first_not_of(kBlanks);
    if (first == std::string::npos)
        return std::string();
    const std::size_t last = str.find_last_not_of(kBlanks);
    return str.substr(first, last - first + 1U);
}

/* Drop a trailing comment so "EVENT_0  # zone 1" reads as EVENT_0. Whole-line
 * comments are handled by the caller; this covers the inline form an operator
 * will reach for after seeing it work in nvpss.conf. */
static std::string atlStripInlineComment(const std::string& str)
{
    const std::size_t hash = str.find('#');
    if (hash == std::string::npos)
        return str;
    return str.substr(0U, hash);
}

std::string atlEventTypeName(EventType type)
{
    if (type == SW_FAIL)
        return "SW_FAIL";

    const std::uint32_t raw = static_cast<std::uint32_t>(type);
    if (raw <= static_cast<std::uint32_t>(EVENT_23))
        return "EVENT_" + std::to_string(raw);
    return "EVENT(" + std::to_string(raw) + ")";
}

/* The generic block must stay contiguous and end immediately before SW_FAIL.
 * The EVENT_23 bound below is a literal on purpose -- it fails strict, so an
 * event code this build does not know refuses the launch with a named reason
 * rather than being accepted as something else. The trade is that the literal
 * can go stale, which is exactly what happened to the EVENT_22 it replaced, so
 * this assertion fires on the next enum edit and points at the bound. */
static_assert(static_cast<int>(SW_FAIL) == static_cast<int>(EVENT_23) + 1,
              "EVENT_23 must be the last generic event code; update the bound "
              "in atlEventTypeName and atlParseEventTypeName");

bool atlParseEventTypeName(const std::string& name, EventType& out)
{
    /* Only the generic block may carry a situation. SW_FAIL is reserved as a
     * fault and the trust-report types are owned by the gateway, so neither is
     * accepted here even though both are valid EventTypes. */
    if (name.compare(0U, 6U, "EVENT_") != 0)
        return false;

    const std::string digits = name.substr(6U);
    if (digits.empty() || digits.size() > 2U
        || digits.find_first_not_of("0123456789") != std::string::npos
        || (digits.size() > 1U && digits[0] == '0'))
        return false;

    /* Bounded to two digits with no leading zero above, so the value is at most
     * 99 and the conversion cannot overflow. */
    const int value = std::atoi(digits.c_str());
    if (value > static_cast<int>(EVENT_23))
        return false;

    out = static_cast<EventType>(value);
    return true;
}

/* Two situations sharing a code would make the dispatch order decide which one
 * an event means, so reject rather than silently prefer one. */
static bool atlEventsAreDistinct(const EventType events[ATL_EVENT_COUNT],
                                 std::string& err)
{
    for (std::size_t i = 0U; i < ATL_EVENT_COUNT; ++i)
    {
        for (std::size_t j = i + 1U; j < ATL_EVENT_COUNT; ++j)
        {
            if (events[i] == events[j])
            {
                err = "event types must be distinct; " + atlEventTypeName(events[i]) +
                      " is bound to both '" + kEventKeys[i] + "' and '" + kEventKeys[j] + "'";
                return false;
            }
        }
    }
    return true;
}

bool atlSdmConfigLoad(const std::string& path, AtlSdmConfig& out, std::string& err)
{
    std::ifstream file(path.c_str());
    if (!file.is_open())
    {
        err = "cannot open configuration file";
        return false;
    }

    /* Raw values, indexed like kEventKeys. Held as strings until the whole file
     * has been read so a duplicate or unknown key is reported before any of it
     * is interpreted. */
    std::string values[ATL_EVENT_COUNT];
    bool seen[ATL_EVENT_COUNT] = {};
    unsigned keyLines[ATL_EVENT_COUNT] = {};

    std::string line;
    unsigned lineNo = 0U;
    while (std::getline(file, line))
    {
        ++lineNo;
        const std::string trimmed = atlTrim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            err = "line " + std::to_string(lineNo) + ": expected 'key = value'";
            return false;
        }

        const std::string key = atlTrim(trimmed.substr(0U, eq));
        const std::string value = atlTrim(atlStripInlineComment(trimmed.substr(eq + 1U)));

        std::size_t slot = ATL_EVENT_COUNT;
        for (std::size_t i = 0U; i < ATL_EVENT_COUNT; ++i)
        {
            if (key == kEventKeys[i])
            {
                slot = i;
                break;
            }
        }

        /* An unrecognised key is an error, not something to ignore. Ignoring it
         * is how a typo leaves the intended situation unbound while the operator
         * reads the file and believes otherwise. */
        if (slot >= ATL_EVENT_COUNT)
        {
            if (key == kRetiredPipelineKey)
                err = "line " + std::to_string(lineNo) + ": '" +
                      std::string(kRetiredPipelineKey) + "' was removed from this file;"
                      " the safety-relevant pipelines now come from"
                      " sensor_pipelines_config.conf (--sensor_pipelines_config)."
                      " Delete this line.";
            else
                err = "line " + std::to_string(lineNo) + ": unknown key '" + key + "'";
            return false;
        }
        if (seen[slot])
        {
            err = "line " + std::to_string(lineNo) + ": key '" + key + "' is set more than once";
            return false;
        }
        if (value.empty())
        {
            err = "line " + std::to_string(lineNo) + ": key '" + key + "' has no value";
            return false;
        }

        values[slot] = value;
        seen[slot] = true;
        keyLines[slot] = lineNo;
    }

    if (file.bad() || (file.fail() && !file.eof()))
    {
        err = "error reading configuration file";
        return false;
    }

    /* Every key is mandatory. There is nothing to fall back to for the rest --
     * the SDM has no compiled-in binding -- so a partial file would leave part
     * of the binding unstated with no way to complete it. */
    for (std::size_t i = 0U; i < ATL_EVENT_COUNT; ++i)
    {
        if (!seen[i])
        {
            err = "missing required key '" + std::string(kEventKeys[i]) + "'";
            return false;
        }
    }
    AtlSdmConfig parsed = {};
    for (std::size_t i = 0U; i < ATL_EVENT_COUNT; ++i)
    {
        if (!atlParseEventTypeName(values[i], parsed.events[i]))
        {
            const std::string prefix = "line " + std::to_string(keyLines[i]) +
                                       ": '" + kEventKeys[i] + "': ";
            if (values[i] == "SW_FAIL")
                err = prefix + "SW_FAIL is reserved; it is always subscribed and handled as a"
                      " fault, so it cannot carry a situation";
            else
                err = prefix + "unrecognised event type '" +
                      values[i] + "' (expected EVENT_0..EVENT_" +
                      std::to_string(static_cast<unsigned>(EVENT_23)) + ")";
            return false;
        }
    }

    if (!atlEventsAreDistinct(parsed.events, err))
        return false;

    out = parsed;
    return true;
}
