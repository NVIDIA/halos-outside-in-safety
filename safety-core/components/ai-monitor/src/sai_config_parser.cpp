/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_config_parser.h"

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
    try {
        size_t pos = 0;
        float val = std::stof(it->second, &pos);
        std::string tail = trim(it->second.substr(pos));
        if (!tail.empty()) {
            std::cerr << "[Config] Trailing junk after float for key '"
                      << key << "': '" << it->second << "'\n";
            return defaultValue;
        }
        return val;
    } catch (const std::exception&) {
        std::cerr << "[Config] Invalid float for key '" << key
                  << "': " << it->second << "\n";
        return defaultValue;
    }
}

int SaiConfigParser::getInt(const std::string& key, int defaultValue) const {
    auto it = values_.find(key);
    if (it == values_.end()) return defaultValue;
    try {
        size_t pos = 0;
        int val = std::stoi(it->second, &pos);
        std::string tail = trim(it->second.substr(pos));
        if (!tail.empty()) {
            std::cerr << "[Config] Trailing junk after int for key '"
                      << key << "': '" << it->second << "'\n";
            return defaultValue;
        }
        return val;
    } catch (const std::exception&) {
        std::cerr << "[Config] Invalid int for key '" << key
                  << "': " << it->second << "\n";
        return defaultValue;
    }
}

bool SaiConfigParser::hasKey(const std::string& key) const {
    return values_.find(key) != values_.end();
}

bool SaiConfigParser::validateRequiredKeys(
        const std::vector<std::string>& requiredKeys) const {
    bool allPresent = true;
    for (const auto& key : requiredKeys) {
        if (values_.find(key) == values_.end()) {
            std::cerr << "[Config] Required key missing: " << key << "\n";
            allPresent = false;
        }
    }
    return allPresent;
}

void SaiConfigParser::printLoadedConfig() const {
    std::cout << "[Config] Loaded values:\n";
    for (const auto& kv : values_) {
        std::cout << "  " << kv.first << " = " << kv.second << "\n";
    }
}
