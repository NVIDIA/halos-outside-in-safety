/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAI_COMMON_H
#define SAI_COMMON_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cmath>
#include <atomic>
#include <ctime>
#include <cstdio>

#ifdef PROFILE
#include <nvtx3/nvToolsExt.h>

// RAII wrapper for NVTX push/pop ranges; automatically pops on scope exit.
struct NvtxRange {
    NvtxRange(const char* name, uint32_t color = 0xFF00FF00) {
        nvtxEventAttributes_t attr = {};
        attr.version       = NVTX_VERSION;
        attr.size          = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        attr.colorType     = NVTX_COLOR_ARGB;
        attr.color         = color;
        attr.messageType   = NVTX_MESSAGE_TYPE_ASCII;
        attr.message.ascii = name;
        nvtxRangePushEx(&attr);
    }
    ~NvtxRange() { nvtxRangePop(); }
};
#define NVTX_CONCAT_IMPL(a, b) a##b
#define NVTX_CONCAT(a, b)      NVTX_CONCAT_IMPL(a, b)
#define NVTX_RANGE(name, color) NvtxRange NVTX_CONCAT(_nvtx_, __LINE__)(name, color)
#define NVTX_MARK(name)        nvtxMark(name)
#else
#define NVTX_RANGE(name, color)
#define NVTX_MARK(name)
#endif

enum class RunMode { LEARN, ACTIVE };

// Per-frame quality scores (0-100 each). Computed entirely on the GPU.
struct FrameQualityResult {
    float histogram_score;       // Brightness/variance health.
    float contrast_score;        // RMS contrast health.
    float edge_density_score;    // Edge density health.
    float overall_confidence;    // Weighted combination of the three scores.
    bool  valid;                 // True only when GPU analysis completed without errors.
};

// Tunable parameters for the quality analyzer, loaded from a config file.
// Defaults are safe fallbacks; production values should always come from a config file.
struct ThresholdConfig {
    float w_histogram             = 0.4f;   // Weight for histogram score in overall confidence.
    float w_contrast              = 0.3f;   // Weight for contrast score.
    float w_edge                  = 0.3f;   // Weight for edge density score.

    // Margins are the allowed fractional deviation from the baseline (0,1).
    // A margin of 0.20 means score drops to 0 at 20% deviation from baseline.
    // All metrics use the same interpretation: gpu_ramp(deviation, margin, 0).
    float baseline_mean_margin    = 0.15f;  // Mean brightness: tolerance = baseline_mean * margin.
    float baseline_var_margin     = 0.20f;  // Variance: score=0 when |var_ratio-1| >= margin.
    float baseline_contrast_margin= 0.20f;  // RMS contrast: score=0 when |ctr_ratio-1| >= margin.
    float baseline_edge_margin    = 0.25f;  // Edge density: score=0 when |edge_ratio-1| >= margin.

    int canny_low_thresh          = 50;     // Canny low (weak edge) threshold on gradient magnitude.
    int canny_high_thresh         = 100;    // Canny high (strong edge) threshold on gradient magnitude.

    // Dual-threshold hysteresis for alert transitions.
    // Frames below score_low_threshold are INVALID; above score_high_threshold are VALID;
    // between them: no counter change
    int score_low_threshold       = 40;     // Below this: frame classified INVALID.
    int score_high_threshold      = 60;     // Above this: frame classified VALID.
    int counter_max               = 15;     // Counter cap and INVALID alert threshold.
    int max_increment             = 3;      // Max counter increment for worst scores.
    int max_decrement             = 2;      // Max counter decrement for best scores.

    // Loads key=value pairs via SaiConfigParser. Returns false on file or parse error.
    static bool loadFromFile(const std::string& path, ThresholdConfig& out);

    // Range and consistency checks for safety-critical operation.
    // Logs all violations to stderr and returns false if any fail.
    bool validate() const;
};

// Averaged quality metrics from the LEARN phase, used as reference in ACTIVE mode.
struct BaselineValues {
    float hist_mean    = 0.f;
    float hist_var     = 0.f;
    float rms_contrast = 0.f;
    float edge_density = 0.f;
    int   total_frames = 0;

    bool saveToFile(const std::string& path) const;
    // Loads key=value pairs via SaiConfigParser. Returns false on file/parse error
    // or if total_frames <= 0.
    static bool loadFromFile(const std::string& path, BaselineValues& out);

    // Checks that all values are finite, non-negative, and total_frames > 0.
    bool validate() const;
};

// Flat struct passed by value to the GPU scoring kernel (no pointers).
// Combines analyzer config weights, baseline reference values, and mode flags.
struct GpuScoringParams {
    float w_histogram, w_contrast, w_edge;
    float baseline_mean_margin, baseline_var_margin, baseline_contrast_margin, baseline_edge_margin;
    float baseline_hist_mean, baseline_hist_var, baseline_rms_contrast, baseline_edge_density;
    int   has_baseline;   // 1 if a learned baseline is available.
    int   is_learn_mode;  // 1 during LEARN mode (accumulates stats instead of scoring).
    int   total_pixels;   // W * H, used to normalize edge count to density.
};

// Running sums accumulated on the GPU during LEARN mode, copied to host
// at the end to compute average baseline values.
struct GpuLearnAccum {
    double hist_mean_sum;
    double hist_var_sum;
    double rms_contrast_sum;
    double edge_density_sum;
    int    frame_count;
};

constexpr int MAX_PSS_REGISTER_RETRIES = 5;
constexpr int MAX_PSS_REPORT_RETRIES   = 5;

// Global flag set by SIGINT/SIGTERM to cleanly shut down receiver + decoder threads.
extern std::atomic<bool> g_stopFlag;

void signalHandler(int sig);

/*
 * Decodes a base64-encoded string into raw bytes.
 * Used to extract SPS/PPS NAL units embedded in SDP sprop-parameter-sets.
 *
 * @param input  Base64 string (padding with '=' is handled).
 * @return       Decoded byte vector.
 */
std::vector<unsigned char> base64Decode(const std::string& input);
bool   safe_stoi(const std::string &s, int &out);
bool   safe_stoul(const std::string &s, size_t &out);
// Case-insensitive substring search. `needle` must be lowercase.
size_t ci_find(const std::string &haystack, const char *needle,
               size_t startPos = 0);

#endif
