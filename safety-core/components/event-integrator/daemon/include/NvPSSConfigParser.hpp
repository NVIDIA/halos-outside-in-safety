/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <algorithm>
#include <vector>

namespace nvpss {

class PSSConfigParser {

public:

    //@brief Constructor for PSSConfigParser
    PSSConfigParser();

    //@brief Destructor for PSSConfigParser
    ~PSSConfigParser();

    // Load configuration from file
    bool loadFromFile(const std::string& filename);

    // Get string value with optional default
    std::string getString(const std::string& key, const std::string& defaultValue = "") const;

    // Get float value with validation
    float getFloat(const std::string& key, float defaultValue = 0.0f) const;

    // Get Unit value with validation
    uint8_t getUint(const std::string& key, uint8_t defaultValue = 0) const;

    // Get milliseconds duration
    std::chrono::milliseconds getMilliseconds(const std::string& key,
                                             std::chrono::milliseconds defaultValue = std::chrono::milliseconds(0)) const;

    // Get microseconds duration
    std::chrono::microseconds getMicroseconds(const std::string& key,
                                             std::chrono::microseconds defaultValue = std::chrono::microseconds(0)) const;

    // Validate that all required keys are present
    bool validateRequiredKeys(const std::vector<std::string>& requiredKeys) const;

    // Print all loaded configuration values for debugging
    void printLoadedConfig() const;

    // Get vector of event type names to bypass fusion
    std::vector<std::string> getBypassFusionEvents(const std::string& key);

private:
    std::unordered_map<std::string, std::string> configValues;

    // Trim whitespace from both ends of string
    std::string trim(const std::string& str);

    // Parse a single line and extract key-value pair
    bool parseLine(const std::string& line);

};
}
