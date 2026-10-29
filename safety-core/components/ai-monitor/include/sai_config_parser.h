/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAI_CONFIG_PARSER_H
#define SAI_CONFIG_PARSER_H

#include <string>
#include <unordered_map>
#include <vector>

/*
 * Generic key=value config file parser for the SAI module.
 * Modeled after PSS daemon's PSSConfigParser: parses simple text files with
 * one key=value pair per line, supports '#' comments and blank lines,
 * trims whitespace from keys and values, and provides typed getters with
 * errno-based safe parsing and sensible defaults.
 */
class SaiConfigParser {
public:
    SaiConfigParser() = default;

    // Parses a key=value config file. Returns false if the file cannot be
    // opened or contains malformed lines (missing '=' or empty key).
    bool loadFromFile(const std::string& path);

    // Typed getters -- return defaultValue when key is absent or unparseable.
    std::string getString(const std::string& key,
                          const std::string& defaultValue = "") const;
    float       getFloat(const std::string& key, float defaultValue = 0.f) const;
    int         getInt(const std::string& key, int defaultValue = 0) const;

    // Parses a comma-separated value into exactly `count` floats.
    //
    // Returns false without touching `out` when the key is absent, an element
    // is unparseable, or the element count differs from `count`. The count is
    // strict on purpose: a baseline written for a different tile grid must be
    // rejected outright, because zero-filling or truncating it would score the
    // stream against a silently wrong reference.
    bool getFloatList(const std::string& key, float* out, int count) const;

    bool hasKey(const std::string& key) const;

    // Returns false and logs if any required key is missing or unparseable by its getter.
    bool validateRequiredKeys(const std::vector<std::string>& floatKeys,
                              const std::vector<std::string>& intKeys) const;

    void printLoadedConfig() const;

private:
    std::unordered_map<std::string, std::string> values_;

    static std::string trim(const std::string& str);
    bool parseLine(const std::string& line, size_t lineNumber);
};

#endif
