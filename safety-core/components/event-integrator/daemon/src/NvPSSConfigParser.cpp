/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "NvPSSConfigParser.hpp"

#include <cmath>

namespace nvpss {

PSSConfigParser::PSSConfigParser() {}

PSSConfigParser::~PSSConfigParser() {}

std::string PSSConfigParser::trim(const std::string& str)
{
    auto start = str.find_first_not_of(" \t\r\n\f\v");
    if (start == std::string::npos)
    {
        return "";
    }
    auto end = str.find_last_not_of(" \t\r\n\f\v");

    return str.substr(start, end - start + 1);
}

bool PSSConfigParser::parseLine(const std::string& line)
{
    std::string trimmedLine = trim(line);

    // Skip empty lines and comments
    if (trimmedLine.empty() || trimmedLine[0] == '#')
    {
        return true;
    }

    // Find the '=' delimiter
    size_t delimiterPos = trimmedLine.find('=');
    if (delimiterPos == std::string::npos)
    {
        std::cerr << "Warning: Invalid config line (missing '='): " << line << std::endl;
        return false;
    }

    std::string key = trim(trimmedLine.substr(0, delimiterPos));
    std::string value = trim(trimmedLine.substr(delimiterPos + 1));

    if (key.empty())
    {
        std::cerr << "Warning: Empty key in config line: " << line << std::endl;
        return false;
    }

    /* Record re-definitions before overwriting. The map is last-wins, which is
     * tolerable for a tuning value but not for a key that selects between two
     * safety-relevant pipelines: an operator appending a line would get the
     * appended value while the original stays visible above it, and the startup
     * log would name a mode the file appears to contradict. Callers that care
     * ask duplicateKeys(); the rest keep the existing behaviour. */
    if (configValues.find(key) != configValues.end())
    {
        duplicatedKeys.insert(key);
    }

    configValues[key] = value;

    return true;
}

bool PSSConfigParser::isDuplicated(const std::string& key) const
{
    return duplicatedKeys.find(key) != duplicatedKeys.end();
}

bool PSSConfigParser::getBool(const std::string& key, BoolParse& outcome) const
{
    auto it = configValues.find(key);
    if (it == configValues.end())
    {
        outcome = BoolParse::Absent;
        return false;
    }

    /* Trim and strip an inline comment, matching every other scalar key. */
    std::string v = trimStatic(it->second);
    const size_t hash = v.find('#');
    if (hash != std::string::npos)
    {
        v = trimStatic(v.substr(0, hash));
    }

    std::string lowered = v;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lowered == "true")
    {
        outcome = BoolParse::Ok;
        return true;
    }
    if (lowered == "false")
    {
        outcome = BoolParse::Ok;
        return false;
    }

    outcome = BoolParse::Malformed;
    return false;
}

std::string PSSConfigParser::trimStatic(const std::string& str)
{
    const size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

bool PSSConfigParser::loadFromFile(const std::string& filename)
{
    std::ifstream configFile(filename);
    if (!configFile.is_open())
    {
        std::cerr << "Error: Cannot open config file: " << filename << std::endl;
        return false;
    }

    std::string line;
    size_t lineNumber = 0;
    bool success = true;

    while (std::getline(configFile, line))
    {
        ++lineNumber;
        if (!parseLine(line))
        {
            std::cerr << "Error parsing line " << lineNumber << ": " << line << std::endl;
            success = false;
        }
    }

    configFile.close();

    return success;
}

std::string PSSConfigParser::getString(const std::string& key,
                              const std::string& defaultValue) const
{
    auto it = configValues.find(key);
    return (it != configValues.end()) ? it->second : defaultValue;
}

float PSSConfigParser::getFloat(const std::string& key, float defaultValue) const
{
    auto it = configValues.find(key);
    if (it == configValues.end()) {
        return defaultValue;
    }

    /* std::stof accepts "nan", "inf" and "infinity" without throwing. None is a
     * usable value for any key read here -- they are all bounded -- and NaN gets
     * past every range check downstream, because each ordered comparison with
     * it is false. A NaN fusion threshold or weight left fusion silently off.
     * A non-finite result is therefore "not a number" and takes the same path
     * as text that does not parse: the documented error and the default. */
    try {
        const float value = std::stof(it->second);
        if (std::isfinite(value)) {
            return value;
        }
    } catch (const std::exception&) {
        /* Not a number at all; reported below. */
    }
    std::cerr << "Error: Invalid float value for key '" << key
                << "': " << it->second << std::endl;
    return defaultValue;
}

uint8_t PSSConfigParser::getUint(const std::string& key, uint8_t defaultValue) const
{
    auto it = configValues.find(key);
    if (it == configValues.end()) {
        return defaultValue;
    }

    try {
        return std::stoul(it->second);
    } catch (const std::exception& e) {
        std::cerr << "Error: Invalid float value for key '" << key
                    << "': " << it->second << std::endl;
        return defaultValue;
    }
}

std::chrono::milliseconds PSSConfigParser::getMilliseconds(const std::string& key,
                                           std::chrono::milliseconds defaultValue) const
{
    auto it = configValues.find(key);
    if (it == configValues.end())
    {
        return defaultValue;
    }

    try
    {
        long long value = std::stoll(it->second);
        return std::chrono::milliseconds(value);
    } catch (const std::exception& e)
    {
        std::cerr << "Error: Invalid milliseconds value for key '" << key
                  << "': " << it->second << std::endl;
        return defaultValue;
    }
}

std::chrono::microseconds PSSConfigParser::getMicroseconds(const std::string& key,
                                           std::chrono::microseconds defaultValue) const
{
    auto it = configValues.find(key);
    if (it == configValues.end())
    {
        return defaultValue;
    }

    try
    {
        long long value = std::stoll(it->second);
        return std::chrono::microseconds(value);
    } catch (const std::exception& e)
    {
        std::cerr << "Error: Invalid microseconds value for key '" << key
                  << "': " << it->second << std::endl;
        return defaultValue;
    }
}

bool PSSConfigParser::validateRequiredKeys(const std::vector<std::string>& requiredKeys) const
{
    bool allPresent = true;
    for (const auto& key : requiredKeys)
    {
        if (configValues.find(key) == configValues.end())
        {
            std::cerr << "Error: Required config key missing: " << key << std::endl;
            allPresent = false;
        }
    }

    return allPresent;
}

bool PSSConfigParser::hasKey(const std::string& key) const
{
    return configValues.find(key) != configValues.end();
}

void PSSConfigParser::printLoadedConfig() const
{
    std::cout << "Loaded configuration values:" << std::endl;
    for (const auto& pair : configValues)
    {
        std::cout << "  " << pair.first << " = " << pair.second << std::endl;
    }

}

std::vector<std::string> PSSConfigParser::getBypassFusionEvents(const std::string& key)
{
    auto it = configValues.find(key);
    std::vector<std::string> bypassEvents;

    if (it != configValues.end())
    {
        /* A trailing comment applies to the entire list, including commas in
         * the comment; strip it before splitting event names. */
        const std::string value = it->second.substr(0, it->second.find('#'));
        std::stringstream ss(value);
        std::string event;

        while (std::getline(ss, event, ','))
        {
            auto trimmed = trim(event);
            if (!trimmed.empty())
            {
                bypassEvents.push_back(trimmed);
            }
        }
    }
    return bypassEvents;
}

}
