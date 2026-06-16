/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "NvPSSConfigParser.hpp"

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

    configValues[key] = value;

    return true;
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

    try {
        return std::stof(it->second);
    } catch (const std::exception& e) {
        std::cerr << "Error: Invalid float value for key '" << key
                    << "': " << it->second << std::endl;
        return defaultValue;
    }
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
        std::stringstream ss(it->second);
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

