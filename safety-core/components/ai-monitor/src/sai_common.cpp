/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_common.h"
#include "sai_config_parser.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

uint64_t monotonic_now_ns() {
    constexpr uint64_t kNsPerSec = 1000000000ULL;
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        std::cerr << "[SAI] clock_gettime failed: "
                  << std::strerror(errno) << "\n";
        return 0;
    }
    if (ts.tv_sec < 0 ||
        static_cast<uint64_t>(ts.tv_sec) >
            std::numeric_limits<uint64_t>::max() / kNsPerSec) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec
         + static_cast<uint64_t>(ts.tv_nsec);
}

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

bool safe_stof(const std::string &s, float &out) {
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    float val = strtof(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE || *end != '\0' || !std::isfinite(val))
        return false;
    out = val;
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
        "w_histogram", "w_contrast", "w_edge", "w_entropy", "w_laplacian",
        "uv_weight_cb", "uv_weight_cr", "uv_weight_sat",
        "score_low_threshold", "score_high_threshold",
        "counter_max", "max_increment", "max_decrement"
    };
    if (!parser.validateRequiredKeys(required)) return false;

    out.w_histogram              = parser.getFloat("w_histogram",          out.w_histogram);
    out.w_contrast               = parser.getFloat("w_contrast",           out.w_contrast);
    out.w_edge                   = parser.getFloat("w_edge",               out.w_edge);
    out.w_entropy                = parser.getFloat("w_entropy",            out.w_entropy);
    out.w_laplacian              = parser.getFloat("w_laplacian",          out.w_laplacian);

    out.uv_weight_cb             = parser.getFloat("uv_weight_cb",         out.uv_weight_cb);
    out.uv_weight_cr             = parser.getFloat("uv_weight_cr",         out.uv_weight_cr);
    out.uv_weight_sat            = parser.getFloat("uv_weight_sat",        out.uv_weight_sat);

    // k: no legacy equivalent. Use the config value if present, else default.
    // Must be finalized before sigma_floor_fraction derivation below uses it.
    if (parser.hasKey("k")) {
        out.k = parser.getFloat("k", out.k);
    } else {
        std::cerr << "[Config] WARNING: 'k' not in " << path
                  << "; using default " << out.k
                  << " Add 'k=" << out.k
                  << "' to silence.\n";
    }

    // sigma_floor_fraction has three sources, in priority order:
    //   1. New key 'sigma_floor_fraction' present -> use as-is.
    //   2. Legacy 'baseline_*_margin' keys present -> derive a single global
    //      floor by averaging the per-metric margins and scaling by 1/k.
    //   3. Neither -> struct default + warning.
    static const char* kLegacyMarginKeys[] = {
        "baseline_mean_margin",     "baseline_var_margin",
        "baseline_contrast_margin", "baseline_edge_margin",
    };
    if (parser.hasKey("sigma_floor_fraction")) {
        out.sigma_floor_fraction = parser.getFloat("sigma_floor_fraction",
                                                   out.sigma_floor_fraction);
    } else {
        float margin_sum = 0.f;
        int   margin_n   = 0;
        for (const char* m : kLegacyMarginKeys) {
            if (parser.hasKey(m)) {
                margin_sum += parser.getFloat(m, 0.f);
                ++margin_n;
            }
        }
        if (margin_n > 0 && std::isfinite(out.k) && out.k > 0.f) {
            const float avg_margin = margin_sum / (float)margin_n;
            float derived = avg_margin / out.k;

            if (derived <= 0.f)  derived = 0.001f;
            if (derived >= 0.5f) derived = 0.499f;
            std::cerr << "[Config] WARNING: 'sigma_floor_fraction' not in "
                      << path << "; derived " << derived
                      << " from " << margin_n
                      << " legacy baseline_*_margin key(s) (avg=" << avg_margin
                      << ", k=" << out.k << "). This is a heuristic translation"
                         " and does NOT exactly reproduce legacy scoring when"
                         " learned sigma is non-trivial. Set "
                         "'sigma_floor_fraction=" << derived
                      << "' explicitly (and remove the legacy keys) to lock in"
                         " the new behavior.\n";
            out.sigma_floor_fraction = derived;
        } else {
            std::cerr << "[Config] WARNING: 'sigma_floor_fraction' not in "
                      << path << "; using default "
                      << out.sigma_floor_fraction
                      << ". Add 'sigma_floor_fraction="
                      << out.sigma_floor_fraction << "' to silence.\n";
        }
    }

    out.score_low_threshold      = parser.getInt("score_low_threshold",        out.score_low_threshold);
    out.score_high_threshold     = parser.getInt("score_high_threshold",       out.score_high_threshold);
    out.counter_max              = parser.getInt("counter_max",                out.counter_max);
    out.max_increment            = parser.getInt("max_increment",              out.max_increment);
    out.max_decrement            = parser.getInt("max_decrement",              out.max_decrement);

    // Canny thresholds are optional - cfg overrides the CANNY_*_THRESH
    // compile-time defaults when present, but missing keys keep the defaults
    // (so legacy thresholds.cfg files without canny_* entries still work).
    out.canny_low_thresh         = parser.getInt("canny_low_thresh",           out.canny_low_thresh);
    out.canny_high_thresh        = parser.getInt("canny_high_thresh",          out.canny_high_thresh);

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
    if (w_entropy < 0.f || w_entropy > 1.f)
        fail("w_entropy must be in [0, 1]");
    if (w_laplacian < 0.f || w_laplacian > 1.f)
        fail("w_laplacian must be in [0, 1]");

    float wsum = w_histogram + w_contrast + w_edge + w_entropy + w_laplacian;
    if (std::fabs(wsum - 1.0f) > 0.01f)
        fail("luma score weights must sum to ~1.0 "
             "(w_histogram + w_contrast + w_edge + w_entropy + w_laplacian)");

    if (uv_weight_cb < 0.f || uv_weight_cb > 1.f)
        fail("uv_weight_cb must be in [0, 1]");
    if (uv_weight_cr < 0.f || uv_weight_cr > 1.f)
        fail("uv_weight_cr must be in [0, 1]");
    if (uv_weight_sat < 0.f || uv_weight_sat > 1.f)
        fail("uv_weight_sat must be in [0, 1]");

    float uv_wsum = uv_weight_cb + uv_weight_cr + uv_weight_sat;
    if (std::fabs(uv_wsum - 1.0f) > 0.01f)
        fail("uv chroma weights must sum to ~1.0 (uv_weight_cb + uv_weight_cr + uv_weight_sat)");

    // Intentionally tighter than the original [0.5, 10.0]: k <= 2 produces
    // hair-trigger alarms (~95% Gaussian coverage at k=2 still flags ~5% of
    // healthy frames as bad), and k >= 5 makes the system practically blind
    // (~1 in 1.7M Gaussian tail). [2, 5] brackets the Shewhart control-chart
    // band around the 3-sigma convention. Legacy configs that loaded a value
    // outside this range will fail loudly here, which is the desired
    // behavior - that value was almost certainly mis-tuned.
    if (!std::isfinite(k) || k < 2.0f || k > 5.0f)
        fail("k must be finite and in [2.0, 5.0]");

    // Tighter than the original (0, 1): a floor >= 50% of |mu| means
    // sigma_eff dominates any realistic learned sigma, collapsing the
    // k*sigma scoring to a fixed-fraction band and defeating the LEARN step.
    // Lower bound stays open so 0 is rejected (would re-introduce
    // divide-by-zero on static-scene baselines). The struct default of 0.05
    // and the legacy-translation output range (~0.1 - 0.17) both sit
    // comfortably inside.
    if (!std::isfinite(sigma_floor_fraction) ||
        sigma_floor_fraction <= 0.f || sigma_floor_fraction >= 0.5f)
        fail("sigma_floor_fraction must be in (0, 0.5)");

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

    if (canny_low_thresh <= 0 || canny_low_thresh >= 255)
        fail("canny_low_thresh must be in (0, 255)");
    if (canny_high_thresh <= 0 || canny_high_thresh >= 255)
        fail("canny_high_thresh must be in (0, 255)");
    if (canny_high_thresh <= canny_low_thresh)
        fail("canny_high_thresh must be > canny_low_thresh");

    return ok;
}

bool BaselineValues::saveToFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << "hist_mean="        << hist_mean        << "\n"
      << "hist_var="         << hist_var         << "\n"
      << "rms_contrast="     << rms_contrast     << "\n"
      << "edge_density="     << edge_density     << "\n"
      << "entropy="          << entropy          << "\n"
      << "lap_var="          << lap_var          << "\n"
      << "hist_mean_std="    << hist_mean_std    << "\n"
      << "hist_var_std="     << hist_var_std     << "\n"
      << "rms_contrast_std=" << rms_contrast_std << "\n"
      << "edge_density_std=" << edge_density_std << "\n"
      << "entropy_std="      << entropy_std      << "\n"
      << "lap_var_std="      << lap_var_std      << "\n"
      << "total_frames="     << total_frames     << "\n"
      << "canny_low_thresh=" << canny_low_thresh << "\n"
      << "canny_high_thresh=" << canny_high_thresh << "\n";

    if (has_chroma) {
        f << "cb_mean="       << cb_mean       << "\n"
          << "cb_mean_std="   << cb_mean_std   << "\n"
          << "cb_var="        << cb_var        << "\n"
          << "cb_var_std="    << cb_var_std    << "\n"
          << "cr_mean="       << cr_mean       << "\n"
          << "cr_mean_std="   << cr_mean_std   << "\n"
          << "cr_var="        << cr_var        << "\n"
          << "cr_var_std="    << cr_var_std    << "\n"
          << "sat_mean="      << sat_mean      << "\n"
          << "sat_mean_std="  << sat_mean_std  << "\n"
          << "sat_var="       << sat_var       << "\n"
          << "sat_var_std="   << sat_var_std   << "\n";
    }
    return f.good();
}

bool BaselineValues::loadFromFile(const std::string& path, BaselineValues& out) {
    SaiConfigParser parser;
    if (!parser.loadFromFile(path)) return false;

    static const std::vector<std::string> required = {
        "hist_mean", "hist_var", "rms_contrast", "edge_density",
        "hist_mean_std", "hist_var_std", "rms_contrast_std", "edge_density_std",
        "total_frames"
    };
    if (!parser.validateRequiredKeys(required)) return false;

    // Validate total_frames before touching `out` so a stale baseline never
    // leaks into gates that read total_frames > 0 as "baseline exists".
    const int total_frames = parser.getInt("total_frames", 0);
    if (total_frames < MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[Config] BaselineValues: total_frames=" << total_frames
                  << " < MIN_LEARN_FRAMES_FOR_SIGMA=" << MIN_LEARN_FRAMES_FOR_SIGMA
                  << " (re-LEARN required for statistically meaningful sigma)\n";
        return false;
    }

    out.hist_mean        = parser.getFloat("hist_mean",        out.hist_mean);
    out.hist_var         = parser.getFloat("hist_var",         out.hist_var);
    out.rms_contrast     = parser.getFloat("rms_contrast",     out.rms_contrast);
    out.edge_density     = parser.getFloat("edge_density",     out.edge_density);
    out.hist_mean_std    = parser.getFloat("hist_mean_std",    out.hist_mean_std);
    out.hist_var_std     = parser.getFloat("hist_var_std",     out.hist_var_std);
    out.rms_contrast_std = parser.getFloat("rms_contrast_std", out.rms_contrast_std);
    out.edge_density_std = parser.getFloat("edge_density_std", out.edge_density_std);
    out.entropy          = parser.getFloat("entropy",          out.entropy);
    out.lap_var          = parser.getFloat("lap_var",          out.lap_var);
    out.entropy_std      = parser.getFloat("entropy_std",      out.entropy_std);
    out.lap_var_std      = parser.getFloat("lap_var_std",      out.lap_var_std);
    out.total_frames     = total_frames;
    out.canny_low_thresh  = parser.getInt("canny_low_thresh",  out.canny_low_thresh);
    out.canny_high_thresh = parser.getInt("canny_high_thresh", out.canny_high_thresh);

    // Chroma (UV-plane) baseline is optional: only adopt it when all 12 keys are present.
    out.has_chroma =
        parser.hasKey("cb_mean")  && parser.hasKey("cb_mean_std")  &&
        parser.hasKey("cb_var")   && parser.hasKey("cb_var_std")   &&
        parser.hasKey("cr_mean")  && parser.hasKey("cr_mean_std")  &&
        parser.hasKey("cr_var")   && parser.hasKey("cr_var_std")   &&
        parser.hasKey("sat_mean") && parser.hasKey("sat_mean_std") &&
        parser.hasKey("sat_var")  && parser.hasKey("sat_var_std");
    if (out.has_chroma) {
        out.cb_mean      = parser.getFloat("cb_mean",      out.cb_mean);
        out.cb_mean_std  = parser.getFloat("cb_mean_std",  out.cb_mean_std);
        out.cb_var       = parser.getFloat("cb_var",       out.cb_var);
        out.cb_var_std   = parser.getFloat("cb_var_std",   out.cb_var_std);
        out.cr_mean      = parser.getFloat("cr_mean",      out.cr_mean);
        out.cr_mean_std  = parser.getFloat("cr_mean_std",  out.cr_mean_std);
        out.cr_var       = parser.getFloat("cr_var",       out.cr_var);
        out.cr_var_std   = parser.getFloat("cr_var_std",   out.cr_var_std);
        out.sat_mean     = parser.getFloat("sat_mean",     out.sat_mean);
        out.sat_mean_std = parser.getFloat("sat_mean_std", out.sat_mean_std);
        out.sat_var      = parser.getFloat("sat_var",      out.sat_var);
        out.sat_var_std  = parser.getFloat("sat_var_std",  out.sat_var_std);
    } else {
        std::cerr << "[Config] no chroma baseline in " << path
                  << "; chroma validation disabled, scoring on luma (Y) only.\n";
    }
    return true;
}

bool BaselineValues::validate() const {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::cerr << "[Config] BaselineValues: " << msg << "\n";
        ok = false;
    };

    if (total_frames < MIN_LEARN_FRAMES_FOR_SIGMA)
        fail("total_frames must be >= MIN_LEARN_FRAMES_FOR_SIGMA");
    if (!std::isfinite(hist_mean) || hist_mean < 0.f)
        fail("hist_mean must be finite and >= 0");
    if (!std::isfinite(hist_var) || hist_var < 0.f)
        fail("hist_var must be finite and >= 0");
    if (!std::isfinite(rms_contrast) || rms_contrast < 0.f)
        fail("rms_contrast must be finite and >= 0");
    if (!std::isfinite(edge_density) || edge_density < 0.f)
        fail("edge_density must be finite and >= 0");

    if (canny_low_thresh <= 0 || canny_low_thresh >= 255)
        fail("canny_low_thresh must be in (0, 255)");
    if (canny_high_thresh <= 0 || canny_high_thresh >= 255)
        fail("canny_high_thresh must be in (0, 255)");
    if (canny_high_thresh <= canny_low_thresh)
        fail("canny_high_thresh must be > canny_low_thresh");

    if (!std::isfinite(hist_mean_std) || hist_mean_std < 0.f)
        fail("hist_mean_std must be finite and >= 0");
    if (!std::isfinite(hist_var_std) || hist_var_std < 0.f)
        fail("hist_var_std must be finite and >= 0");
    if (!std::isfinite(rms_contrast_std) || rms_contrast_std < 0.f)
        fail("rms_contrast_std must be finite and >= 0");
    if (!std::isfinite(edge_density_std) || edge_density_std < 0.f)
        fail("edge_density_std must be finite and >= 0");
    if (!std::isfinite(entropy) || entropy < 0.f)
        fail("entropy must be finite and >= 0");
    if (!std::isfinite(lap_var) || lap_var < 0.f)
        fail("lap_var must be finite and >= 0");
    if (!std::isfinite(entropy_std) || entropy_std < 0.f)
        fail("entropy_std must be finite and >= 0");
    if (!std::isfinite(lap_var_std) || lap_var_std < 0.f)
        fail("lap_var_std must be finite and >= 0");

    if (has_chroma) {
        auto chk = [&](float v, const char* msg) {
            if (!std::isfinite(v) || v < 0.f) fail(msg);
        };
        chk(cb_mean,      "cb_mean must be finite and >= 0");
        chk(cb_mean_std,  "cb_mean_std must be finite and >= 0");
        chk(cb_var,       "cb_var must be finite and >= 0");
        chk(cb_var_std,   "cb_var_std must be finite and >= 0");
        chk(cr_mean,      "cr_mean must be finite and >= 0");
        chk(cr_mean_std,  "cr_mean_std must be finite and >= 0");
        chk(cr_var,       "cr_var must be finite and >= 0");
        chk(cr_var_std,   "cr_var_std must be finite and >= 0");
        chk(sat_mean,     "sat_mean must be finite and >= 0");
        chk(sat_mean_std, "sat_mean_std must be finite and >= 0");
        chk(sat_var,      "sat_var must be finite and >= 0");
        chk(sat_var_std,  "sat_var_std must be finite and >= 0");
    }

    return ok;
}
