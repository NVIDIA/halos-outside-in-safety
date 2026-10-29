/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#include <set>

namespace nvpss {

/** Outcome of getBool(), which must distinguish three cases, not two. */
enum class BoolParse {
    Ok,        /**< Key present and read as true/false. */
    Absent,    /**< Key not in the file. */
    Malformed  /**< Key present but not true/false. */
};

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

    // Read a boolean key. Accepts "true"/"false", case-insensitive, after
    // trimming and stripping an inline comment. The return value is only
    // meaningful when @p outcome is BoolParse::Ok -- a bool return alone cannot
    // carry absent, malformed and value as three distinct answers, and a
    // mandatory safety-relevant key must be able to tell them apart.
    bool getBool(const std::string& key, BoolParse& outcome) const;

    // Whether the key was defined more than once in the file. parseLine() is
    // last-wins, so a caller that must not silently accept a re-definition has
    // to ask.
    bool isDuplicated(const std::string& key) const;

    // Whether the key appeared in the file at all, irrespective of its value.
    //
    // Needed to reject a retired key. Presence must be the test, not the value:
    // a line such as "RetiredKey = X # comment" carries its comment in the
    // value, so any value-based check can be evaded by editing the value, which
    // is exactly the operator error the rejection exists to catch. parseLine()
    // trims the key, so leading and trailing whitespace cannot evade this
    // either. Distinct from validateRequiredKeys(), which reports absence as an
    // error on std::cerr and would emit a spurious message on the normal path
    // if it were inverted for this purpose.
    bool hasKey(const std::string& key) const;

    // Print all loaded configuration values for debugging
    void printLoadedConfig() const;

    // Get vector of event type names to bypass fusion
    std::vector<std::string> getBypassFusionEvents(const std::string& key);

private:
    std::unordered_map<std::string, std::string> configValues;
    std::set<std::string> duplicatedKeys;

    // Whitespace trim usable from const members.
    static std::string trimStatic(const std::string& str);

    // Trim whitespace from both ends of string
    std::string trim(const std::string& str);

    // Parse a single line and extract key-value pair
    bool parseLine(const std::string& line);

};
}
