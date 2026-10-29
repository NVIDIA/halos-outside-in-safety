/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_config_parser.h"

#include "sai_common.h"

#include <fstream>
#include <iostream>

std::string SaiConfigParser::trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n\f\v");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n\f\v");
    return str.substr(start, end - start + 1);
}

bool SaiConfigParser::parseLine(const std::string& line, size_t lineNumber) {
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') return true;

    size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
        std::cerr << "[Config] Line " << lineNumber
                  << ": missing '=' delimiter: " << line << "\n";
        return false;
    }

    std::string key = trim(trimmed.substr(0, eq));
    std::string val = trim(trimmed.substr(eq + 1));

    size_t hash = val.find('#');
    if (hash != std::string::npos)
        val = trim(val.substr(0, hash));

    if (key.empty()) {
        std::cerr << "[Config] Line " << lineNumber
                  << ": empty key: " << line << "\n";
        return false;
    }

    values_[key] = val;
    return true;
}

bool SaiConfigParser::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[Config] Cannot open config file: " << path << "\n";
        return false;
    }

    std::string line;
    size_t lineNumber = 0;
    bool success = true;
    values_.clear();

    while (std::getline(f, line)) {
        ++lineNumber;
        if (!parseLine(line, lineNumber)) {
            success = false;
        }
    }
    if (f.bad()) {
        std::cerr << "[Config] Read error on config file: " << path << "\n";
        return false;
    }
    return success;
}

std::string SaiConfigParser::getString(const std::string& key,
                                       const std::string& defaultValue) const {
    auto it = values_.find(key);
    return (it != values_.end()) ? it->second : defaultValue;
}

float SaiConfigParser::getFloat(const std::string& key, float defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultValue;
    float val;
    if (!safe_stof(it->second, val)) {
        std::cerr << "[Config] Invalid float for key '" << key
                  << "': " << it->second << "\n";
        return defaultValue;
    }
    return val;
}

bool SaiConfigParser::getFloatList(const std::string& key, float* out, int count) const {
    if (out == nullptr || count <= 0) return false;

    auto it = values_.find(key);
    if (it == values_.end()) return false;

    // Parse into a temporary so a malformed list leaves the caller's values
    // intact; a half-written baseline row would otherwise satisfy downstream
    // "has data" checks with garbage in the tail.
    std::vector<float> parsed;
    parsed.reserve(static_cast<size_t>(count));

    const std::string& csv = it->second;
    size_t             pos = 0;
    while (pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        const size_t end   = (comma == std::string::npos) ? csv.size() : comma;

        float val = 0.f;
        if (!safe_stof(trim(csv.substr(pos, end - pos)), val)) {
            std::cerr << "[Config] Invalid float in list for key '" << key
                      << "' at element " << parsed.size() << "\n";
            return false;
        }
        // Stop growing past the expected count so a pathological line cannot
        // drive an unbounded allocation before the size check below.
        if (parsed.size() >= static_cast<size_t>(count)) {
            std::cerr << "[Config] Key '" << key << "' has more than " << count
                      << " elements. This baseline was written by a different"
                         " build or tile grid; re-run LEARN to regenerate it.\n";
            return false;
        }
        parsed.push_back(val);

        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    if (parsed.size() != static_cast<size_t>(count)) {
        std::cerr << "[Config] Key '" << key << "' has " << parsed.size()
                  << " elements; expected exactly " << count
                  << ". This baseline was written by a different build or"
                     " tile grid; re-run LEARN to regenerate it.\n";
        return false;
    }

    for (int i = 0; i < count; ++i) out[i] = parsed[static_cast<size_t>(i)];
    return true;
}

int SaiConfigParser::getInt(const std::string& key, int defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultValue;
    int val;
    if (!safe_stoi(it->second, val)) {
        std::cerr << "[Config] Invalid int for key '" << key
                  << "': " << it->second << "\n";
        return defaultValue;
    }
    return val;
}

bool SaiConfigParser::hasKey(const std::string& key) const {
    return values_.find(key) != values_.end();
}

bool SaiConfigParser::validateRequiredKeys(
        const std::vector<std::string>& floatKeys,
        const std::vector<std::string>& intKeys) const {
    bool allValid = true;

    // safe_stof also rejects non-finite text, so "nan"/"inf" cannot reach a
    // caller that only range-checks the parsed value.
    for (const auto& key : floatKeys) {
        auto it = values_.find(key);
        if (it == values_.end()) {
            std::cerr << "[Config] Required key missing: " << key << "\n";
            allValid = false;
            continue;
        }
        float val;
        if (!safe_stof(it->second, val)) {
            std::cerr << "[Config] Required key '" << key
                      << "' is not a finite number: " << it->second << "\n";
            allValid = false;
        }
    }

    // safe_stoi, not safe_stof: a fractional value like "15.5" in an integer
    // field is a config error, not something to round silently.
    for (const auto& key : intKeys) {
        auto it = values_.find(key);
        if (it == values_.end()) {
            std::cerr << "[Config] Required key missing: " << key << "\n";
            allValid = false;
            continue;
        }
        int val;
        if (!safe_stoi(it->second, val)) {
            std::cerr << "[Config] Required key '" << key
                      << "' is not an integer: " << it->second << "\n";
            allValid = false;
        }
    }

    return allValid;
}

void SaiConfigParser::printLoadedConfig() const {
    std::cout << "[Config] Loaded values:\n";
    for (const auto& kv : values_) {
        std::cout << "  " << kv.first << " = " << kv.second << "\n";
    }
}
