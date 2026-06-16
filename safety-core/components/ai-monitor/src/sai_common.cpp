/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_common.h"
#include "sai_config_parser.h"

#include <fstream>
#include <iostream>
#include <cstring>
#include <cctype>
#include <algorithm>

std::atomic<bool> g_stopFlag{false};
static_assert(ATOMIC_BOOL_LOCK_FREE == 2,
              "g_stopFlag must be lock-free for async-signal-safe use");

void signalHandler(int sig) {
    (void)sig;
    g_stopFlag.store(true, std::memory_order_relaxed);
}

/*
 * Decodes a base64-encoded string into raw bytes.
 * Used to extract SPS/PPS NAL units embedded in SDP sprop-parameter-sets.
 *
 * @param input  Base64 string (padding with '=' is handled).
 * @return       Decoded byte vector.
 */
std::vector<unsigned char> base64Decode(const std::string& input) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::vector<unsigned char> result;
    unsigned int val = 0;
    int bits = -8;

    for (char c : input) {
        if (c == '=') break;
        size_t pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | (unsigned int)pos;
        bits += 6;
        if (bits >= 0) {
            result.push_back((unsigned char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return result;
}

bool safe_stoi(const std::string &s, int &out) {
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    long val = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || errno == ERANGE ||
        val < INT_MIN || val > INT_MAX || *end != '\0')
        return false;
    out = (int)val;
    return true;
}

bool safe_stoul(const std::string &s, size_t &out) {
    if (s.empty() || s[0] == '-' || s[0] == '+') return false;
    char *end = nullptr;
    errno = 0;
    unsigned long val = strtoul(s.c_str(), &end, 10);
    if (end == s.c_str() || errno == ERANGE || *end != '\0')
        return false;
    out = (size_t)val;
    return true;
}

// Case-insensitive substring search. `needle` must be lowercase.
size_t ci_find(const std::string &haystack, const char *needle,
               size_t startPos) {
    size_t nlen = strlen(needle);
    if (startPos + nlen > haystack.size()) return std::string::npos;
    auto it = std::search(haystack.begin() + (ptrdiff_t)startPos, haystack.end(),
                          needle, needle + nlen,
                          [](char a, char b) {
                              return tolower((unsigned char)a) == (unsigned char)b;
                          });
    return it == haystack.end() ? std::string::npos : (size_t)(it - haystack.begin());
}

bool ThresholdConfig::loadFromFile(const std::string& path, ThresholdConfig& out) {
    SaiConfigParser parser;
    if (!parser.loadFromFile(path)) return false;

    static const std::vector<std::string> required = {
        "w_histogram", "w_contrast", "w_edge",
        "baseline_mean_margin", "baseline_var_margin",
        "baseline_contrast_margin", "baseline_edge_margin",
        "canny_low_thresh", "canny_high_thresh",
        "score_low_threshold", "score_high_threshold",
        "counter_max", "max_increment", "max_decrement"
    };
    if (!parser.validateRequiredKeys(required)) return false;

    out.w_histogram              = parser.getFloat("w_histogram",              out.w_histogram);
    out.w_contrast               = parser.getFloat("w_contrast",               out.w_contrast);
    out.w_edge                   = parser.getFloat("w_edge",                   out.w_edge);
    out.baseline_mean_margin     = parser.getFloat("baseline_mean_margin",     out.baseline_mean_margin);
    out.baseline_var_margin      = parser.getFloat("baseline_var_margin",      out.baseline_var_margin);
    out.baseline_contrast_margin = parser.getFloat("baseline_contrast_margin", out.baseline_contrast_margin);
    out.baseline_edge_margin     = parser.getFloat("baseline_edge_margin",     out.baseline_edge_margin);
    out.canny_low_thresh         = parser.getInt("canny_low_thresh",           out.canny_low_thresh);
    out.canny_high_thresh        = parser.getInt("canny_high_thresh",          out.canny_high_thresh);

    out.score_low_threshold      = parser.getInt("score_low_threshold",        out.score_low_threshold);
    out.score_high_threshold     = parser.getInt("score_high_threshold",       out.score_high_threshold);
    out.counter_max              = parser.getInt("counter_max",                out.counter_max);
    out.max_increment            = parser.getInt("max_increment",              out.max_increment);
    out.max_decrement            = parser.getInt("max_decrement",              out.max_decrement);

    return true;
}

bool ThresholdConfig::validate() const {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::cerr << "[Config] ThresholdConfig: " << msg << "\n";
        ok = false;
    };

    if (w_histogram < 0.f || w_histogram > 1.f)
        fail("w_histogram must be in [0, 1]");
    if (w_contrast < 0.f || w_contrast > 1.f)
        fail("w_contrast must be in [0, 1]");
    if (w_edge < 0.f || w_edge > 1.f)
        fail("w_edge must be in [0, 1]");

    float wsum = w_histogram + w_contrast + w_edge;
    if (std::fabs(wsum - 1.0f) > 0.01f)
        fail("score weights must sum to ~1.0 (w_histogram + w_contrast + w_edge)");

    if (baseline_mean_margin <= 0.f || baseline_mean_margin >= 1.f)
        fail("baseline_mean_margin must be in (0, 1)");
    if (baseline_var_margin <= 0.f || baseline_var_margin >= 1.f)
        fail("baseline_var_margin must be in (0, 1)");
    if (baseline_contrast_margin <= 0.f || baseline_contrast_margin >= 1.f)
        fail("baseline_contrast_margin must be in (0, 1)");
    if (baseline_edge_margin <= 0.f || baseline_edge_margin >= 1.f)
        fail("baseline_edge_margin must be in (0, 1)");

    if (canny_low_thresh <= 0)
        fail("canny_low_thresh must be > 0");
    if (canny_high_thresh <= canny_low_thresh)
        fail("canny_high_thresh must be > canny_low_thresh");

    if (score_low_threshold <= 0 || score_low_threshold >= 100)
        fail("score_low_threshold must be in (0, 100)");
    if (score_high_threshold <= 0 || score_high_threshold >= 100)
        fail("score_high_threshold must be in (0, 100)");
    if (score_high_threshold <= score_low_threshold)
        fail("score_high_threshold must be > score_low_threshold");
    if (counter_max <= 0 || counter_max > 100)
        fail("counter_max must be in [1, 100]");
    if (max_increment < 1 || max_increment > 10)
        fail("max_increment must be in [1, 10]");
    if (max_decrement < 1 || max_decrement > max_increment)
        fail("max_decrement must be in [1, max_increment]");

    return ok;
}

bool BaselineValues::saveToFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << "hist_mean=" << hist_mean << "\n"
      << "hist_var=" << hist_var << "\n"
      << "rms_contrast=" << rms_contrast << "\n"
      << "edge_density=" << edge_density << "\n"
      << "total_frames=" << total_frames << "\n";
    return f.good();
}

bool BaselineValues::loadFromFile(const std::string& path, BaselineValues& out) {
    SaiConfigParser parser;
    if (!parser.loadFromFile(path)) return false;

    static const std::vector<std::string> required = {
        "hist_mean", "hist_var", "rms_contrast", "edge_density", "total_frames"
    };
    if (!parser.validateRequiredKeys(required)) return false;

    out.hist_mean    = parser.getFloat("hist_mean",    out.hist_mean);
    out.hist_var     = parser.getFloat("hist_var",     out.hist_var);
    out.rms_contrast = parser.getFloat("rms_contrast", out.rms_contrast);
    out.edge_density = parser.getFloat("edge_density", out.edge_density);
    out.total_frames = parser.getInt("total_frames",   out.total_frames);

    return out.total_frames > 0;
}

bool BaselineValues::validate() const {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::cerr << "[Config] BaselineValues: " << msg << "\n";
        ok = false;
    };

    if (total_frames <= 0)
        fail("total_frames must be > 0");
    if (!std::isfinite(hist_mean) || hist_mean < 0.f)
        fail("hist_mean must be finite and >= 0");
    if (!std::isfinite(hist_var) || hist_var < 0.f)
        fail("hist_var must be finite and >= 0");
    if (!std::isfinite(rms_contrast) || rms_contrast < 0.f)
        fail("rms_contrast must be finite and >= 0");
    if (!std::isfinite(edge_density) || edge_density < 0.f)
        fail("edge_density must be finite and >= 0");

    return ok;
}
