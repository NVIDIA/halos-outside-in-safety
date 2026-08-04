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

// SAIM event-cause identifiers written into SafetyEvent.ruleIdentifier so PSS
// logs and the SDMs can attribute why a sensor changed state. The verdict stays
// in EventType (SENSOR_INVALID/VALID).
// SENSOR_INVALID causes:
constexpr const char* SAIM_INPUT_DEGRADED    = "SAIM_INPUT_DEGRADED";    // frame-quality score below threshold
constexpr const char* SAIM_INTERNAL_ERROR    = "SAIM_INTERNAL_ERROR";    // analyzer init / per-frame compute failure
constexpr const char* SAIM_STREAM_DISCONNECT = "SAIM_STREAM_DISCONNECT"; // connect-retry exhaustion / reconnect
constexpr const char* SAIM_FRAME_DROP        = "SAIM_FRAME_DROP";        // FU-A NAL drops
constexpr const char* SAIM_INIT              = "SAIM_INIT";              // first-registration fail-safe seed (pre-frames)
constexpr const char* SAIM_UNKNOWN           = "SAIM_UNKNOWN";           // defensive fallback: INVALID with missing/contradictory cause
// SENSOR_VALID cause:
constexpr const char* SAIM_SENSOR_HEALTHY    = "SAIM_SENSOR_HEALTHY";    // recovery edge (sustained good streak)

// Per-frame quality scores (0-100 each). Computed entirely on the GPU.
struct FrameQualityResult {
    float histogram_score;       // Brightness/variance health.
    float contrast_score;        // RMS contrast health.
    float edge_density_score;    // Edge density health.
    float entropy_score;         // Histogram (Shannon) entropy health.
    float laplacian_score;       // Laplacian-variance (sharpness) health.
    float cb_score;              // Cb (U) chroma health.
    float cr_score;              // Cr (V) chroma health.
    float sat_score;             // Saturation (alpha-max-plus-beta-min colorfulness) health.
    float uv_score;              // Weighted avg: w_cb*cb_score + w_cr*cr_score + w_sat*sat_score.
    float overall_confidence;    // Worst-of luma and chroma: min(y_score, uv_score).
    bool  valid;                 // True only when GPU analysis completed without errors.
};

// Largest frame dimension any analyzer/pipeline accepts (16K x 16K). Single source
// of truth for the decoder and the analysis backend.
constexpr int MAX_DIM = 16384;

// Canny edge-detection thresholds (gradient magnitude on 8-bit input).
// LEARN mode uses these compile-time defaults directly (thresholds.cfg is
// not loaded in LEARN) and pins the values into baseline.cfg.
// ACTIVE-mode precedence: baseline.cfg > thresholds.cfg > these defaults.
constexpr int CANNY_LOW_THRESH  = 50;
constexpr int CANNY_HIGH_THRESH = 150;

// LEARN-mode FPS fallback when the parser cannot determine stream FPS
// from the H.264 sequence header (zero/invalid frame_rate fields).
constexpr int LEARN_FPS_FALLBACK = 30;

// Upper bound on stream-reported FPS used to compute the LEARN target. Guards
// against malformed sequence headers (e.g. frame_rate of 1000+ fps) that would
// otherwise inflate learn_target_frames_ to an unreachable value.
constexpr int LEARN_FPS_MAX = 240;

// Analyzer fault-isolation budget. After this many *consecutive* per-frame
// analysis failures the analyzer emits one SENSOR_INVALID and tries a bounded
// pipeline reinit; up to kMaxAnalyzerReinitAttempts reinit cycles are allowed
// over the lifetime of the analyzer before the affected camera's thread is
// permanently shut down (other cameras keep running).
constexpr unsigned int kMaxConsecutiveAnalyzerErrors = 30U;
constexpr unsigned int kMaxAnalyzerReinitAttempts    = 5U;

// Consecutive cuvidParseVideoData() failures before decodeStream aborts and
// hands off to the RTSP reconnect path. Distinct domain from
// kMaxConsecutiveAnalyzerErrors (pre-decode parse vs. post-decode scoring).
constexpr unsigned int kMaxConsecutiveParseErrors = 30U;

// NVDEC output surfaces per SPS. Weave deinterlace needs 2 (display + fill).
constexpr unsigned int kNumDecodeOutputSurfaces = 2U;

// LEARN-mode progress log cadence (frames).
constexpr uint64_t kLearnProgressLogIntervalFrames = 100ULL;

// Backoff between reportSafetyEvent retries.
// Total window = MAX_PSS_REPORT_RETRIES * kPssReportRetryDelayMs.
constexpr unsigned int kPssReportRetryDelayMs = 25U;

// CLOCK_MONOTONIC nanoseconds since boot. Returns 0 on clock_gettime
// failure or tv_sec range/overflow. Consumers must treat 0 as a sentinel
// (e.g., clamp to last-known timestamp before a monotonic-staleness check);
// downstream SafetyEventReporter::update_slot_impl already does this clamp.
uint64_t monotonic_now_ns();

// LEARN duration default and CLI cap (seconds). Shared so unit tests target
// the same frame count as the production binary.
constexpr int DEFAULT_LEARN_DURATION_SEC = 300;
// Caps learn_target_frames_ at ~LEARN_FPS_MAX * MAX_LEARN_DURATION_SEC,
// within GpuLearnAccum's uint64_t frame_count.
constexpr int MAX_LEARN_DURATION_SEC = 3600;

// SW-fallback luminance histogram bin count; also the analyzer's allocation
// floor when NVDEC reports fewer (or no) bins.
constexpr int kSwHistogramBinCount = 256;

// NVDEC reports the luma-histogram bin count <= 4096 for up to 12-bit luma.
constexpr int kMaxHistBins = 4096;

// Minimum frame count required for a baseline's per-metric standard deviation
// to be statistically meaningful. Baselines saved/loaded with fewer frames are rejected.
constexpr int MIN_LEARN_FRAMES_FOR_SIGMA = 30;

// Per-pixel chroma "colorfulness" s = distance of (Cb,Cr) from neutral gray (128):
// the Euclidean sqrt(dCb^2 + dCr^2), dCb=|Cb-128|, dCr=|Cr-128|, approximated without
// a sqrt via alpha-max-plus-beta-min, with Max=max(dCb,dCr), Min=min(dCb,dCr) and
// lowest error is at alpha=0.960433870 and beta=0.397824735:
//   s = max(Max, (kSatAlphaQ7*Max + kSatBetaQ7*Min + kSatRoundQ7) >> kSatShiftQ7)
// max(Max, .) clamps the near-axis under-estimate. Q7 fixed point (~3.96% error)
// keeps every step <= 22336, so the whole computation stays in 16-bit lanes.
// dCb,dCr,Max,Min in [0,128]; s in [0,174].
constexpr int kSatShiftQ7 = 7;
constexpr int kSatAlphaQ7 = 123;                     // 123 = round(0.960433870 * 128)
constexpr int kSatBetaQ7  = 51;                      // 51 = round(0.397824735 * 128)
constexpr int kSatRoundQ7 = 1 << (kSatShiftQ7 - 1);  // 64: round-to-nearest before >>

// Tunable parameters for the quality analyzer, loaded from a config file.
// Defaults are safe fallbacks; production values should always come from a config file.
struct ThresholdConfig {
    float w_histogram             = 0.25f;  // Weight for histogram (brightness/variance) score.
    float w_contrast              = 0.2f;   // Weight for contrast score.
    float w_edge                  = 0.2f;   // Weight for edge density score.
    float w_entropy               = 0.2f;   // Weight for histogram-entropy score.
    float w_laplacian             = 0.15f;  // Weight for Laplacian-variance (sharpness) score.

    float uv_weight_cb            = 0.4f;   // Weight for Cb (U) chroma health.
    float uv_weight_cr            = 0.4f;   // Weight for Cr (V) chroma health.
    float uv_weight_sat           = 0.2f;   // Weight for saturation (colorfulness) health.

    // Statistical (k*sigma) thresholding parameters. ACTIVE-mode per-metric scoring:
    //   floor     = |mu| * sigma_floor_fraction
    //   sigma_eff = max(sigma, floor)
    //   z         = |x - mu| / sigma_eff
    //   score     = clamp(100 * (1 - z/k), 0, 100)
    // So z = 0 -> 100, z = k -> 0, z > k stays at 0 (clamped). Larger k = wider
    // tolerance band before a metric is considered bad; k = 3 lines up with
    // the 3-sigma rule (~99.7% Gaussian coverage), the Shewhart control-chart convention.
    float k                       = 3.0f;   // Number of sigmas at which score drops to 0 (~99.7% Gaussian coverage at k=3).
    float sigma_floor_fraction    = 0.05f;  // Fractional-of-|mu| floor on effective sigma; guards against sigma~0.

    // Dual-threshold hysteresis for alert transitions.
    // Frames below score_low_threshold are INVALID; above score_high_threshold are VALID;
    // between them: no counter change
    int score_low_threshold       = 40;     // Below this: frame classified INVALID.
    int score_high_threshold      = 60;     // Above this: frame classified VALID.
    int counter_max               = 15;     // Counter cap and INVALID alert threshold.
    int max_increment             = 3;      // Max counter increment for worst scores.
    int max_decrement             = 2;      // Max counter decrement for best scores.

    // Canny edge-detection thresholds. Optional in thresholds.cfg; when absent
    // the compile-time CANNY_*_THRESH defaults are used. In ACTIVE mode these
    // are overridden by baseline.cfg's pinned values in loadBaseline() so the
    // edge map matches the one LEARN saw.
    int canny_low_thresh          = CANNY_LOW_THRESH;
    int canny_high_thresh         = CANNY_HIGH_THRESH;

    // Loads key=value pairs via SaiConfigParser. Returns false on file or parse error.
    static bool loadFromFile(const std::string& path, ThresholdConfig& out);

    // Range and consistency checks for safety-critical operation.
    // Logs all violations to stderr and returns false if any fail.
    bool validate() const;
};

// Averaged quality metrics from the LEARN phase, used as reference in ACTIVE mode.
// Stores both the mean (mu) and standard deviation (sigma) of each metric so
// ACTIVE-mode scoring can use a learned k*sigma tolerance band per camera
// instead of a static fractional margin.
struct BaselineValues {
    // Per-metric means computed across the LEARN window.
    float hist_mean        = 0.f;
    float hist_var         = 0.f;
    float rms_contrast     = 0.f;
    float edge_density     = 0.f;
    float entropy          = 0.f;
    float lap_var          = 0.f;

    // Per-metric standard deviations computed across the LEARN window.
    // Reflect each camera's natural frame-to-frame variability and drive the
    // k*sigma tolerance band in ACTIVE mode. May be zero on extremely static
    // scenes; the sigma_floor_fraction in ThresholdConfig handles that case.
    float hist_mean_std    = 0.f;
    float hist_var_std     = 0.f;
    float rms_contrast_std = 0.f;
    float edge_density_std = 0.f;
    float entropy_std      = 0.f;
    float lap_var_std      = 0.f;

    int   total_frames     = 0;

    // Canny thresholds pinned at LEARN time so ACTIVE reproduces the same
    // edge map regardless of thresholds.cfg drift. Optional in older
    // baseline.cfg files; when absent, loadFromFile leaves the caller's
    // pre-seeded values intact (thresholds.cfg fallback or sai_common.h
    // defaults, in that order).
    int canny_low_thresh  = CANNY_LOW_THRESH;
    int canny_high_thresh = CANNY_HIGH_THRESH;

    // Chroma (UV-plane) baseline: per-frame mean and Bessel-corrected variance
    // of Cb, Cr, and saturation (alpha-max-plus-beta-min colorfulness).
    float cb_mean = 0.f,  cb_mean_std = 0.f;
    float cb_var  = 0.f,  cb_var_std  = 0.f;
    float cr_mean = 0.f,  cr_mean_std = 0.f;
    float cr_var  = 0.f,  cr_var_std  = 0.f;
    float sat_mean = 0.f, sat_mean_std = 0.f;
    float sat_var  = 0.f, sat_var_std  = 0.f;
    bool  has_chroma = false;

    bool saveToFile(const std::string& path) const;
    // Loads key=value pairs via SaiConfigParser. Returns false on file/parse error.
    static bool loadFromFile(const std::string& path, BaselineValues& out);

    // Checks that all values (mu and sigma) are finite and non-negative, and
    // that total_frames >= MIN_LEARN_FRAMES_FOR_SIGMA.
    bool validate() const;
};

// Flat struct passed by value to the GPU scoring kernel (no pointers).
// Combines analyzer config weights, baseline reference values (mu and sigma
// per metric), the global k*sigma tuning knobs, and mode flags.
struct GpuScoringParams {
    float w_histogram, w_contrast, w_edge, w_entropy, w_laplacian;

    float uv_weight_cb, uv_weight_cr, uv_weight_sat;

    // Statistical thresholding knobs (see ThresholdConfig::k / sigma_floor_fraction).
    float k;
    float sigma_floor_fraction;

    // Per-metric baseline means (mu).
    float baseline_hist_mean, baseline_hist_var;
    float baseline_rms_contrast, baseline_edge_density;
    float baseline_entropy, baseline_lap_var;

    // Per-metric baseline standard deviations (sigma); used by ACTIVE-mode
    // scoring as the divisor in the z-score |x - mu| / sigma_eff.
    float baseline_hist_mean_std, baseline_hist_var_std;
    float baseline_rms_contrast_std, baseline_edge_density_std;
    float baseline_entropy_std, baseline_lap_var_std;

    // Chroma (UV-plane) baseline means (mu) and sigmas.
    float baseline_cb_mean, baseline_cb_mean_std;
    float baseline_cb_var,  baseline_cb_var_std;
    float baseline_cr_mean, baseline_cr_mean_std;
    float baseline_cr_var,  baseline_cr_var_std;
    float baseline_sat_mean, baseline_sat_mean_std;
    float baseline_sat_var,  baseline_sat_var_std;

    int   has_baseline;          // 1 if a learned luma baseline is available.
    int   has_chroma_baseline;   // 1 if a learned chroma baseline is available.
    int   is_learn_mode;  // 1 during LEARN mode (accumulates stats instead of scoring).
    int   total_pixels;   // W * H, used to normalize edge count to density.
    int   chroma_pixels;  // Nc = (W/2) * (H/2), used to normalize chroma sums.
    int   lap_pixels;     // W*H, divisor for Laplacian variance.
};

// Per-metric Welford's online statistics accumulated on the GPU during LEARN.
struct GpuLearnAccum {
    double hist_mean_mu,    hist_mean_M2;
    double hist_var_mu,     hist_var_M2;
    double rms_contrast_mu, rms_contrast_M2;
    double edge_density_mu, edge_density_M2;
    double entropy_mu,      entropy_M2;
    double lap_var_mu,      lap_var_M2;
    double cb_mean_mu,  cb_mean_M2;
    double cb_var_mu,   cb_var_M2;
    double cr_mean_mu,  cr_mean_M2;
    double cr_var_mu,   cr_var_M2;
    double sat_mean_mu, sat_mean_M2;
    double sat_var_mu,  sat_var_M2;
    uint64_t frame_count;
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
bool   safe_stof(const std::string &s, float &out);
// Case-insensitive substring search. `needle` must be lowercase.
size_t ci_find(const std::string &haystack, const char *needle,
               size_t startPos = 0);

#endif
