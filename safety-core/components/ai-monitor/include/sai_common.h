/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAI_COMMON_H
#define SAI_COMMON_H

// The SAIM safety-event cause vocabulary used to live here as string constants.
// It is now the EventCause enum in event-integrator/daemon/include/pss_protocol.h,
// surfaced for SAIM as kSaim* constants in include/safety_event_reporter.h.
//
// It is not re-exported here on purpose: this header is included by the
// gpu_backend CUDA kernels, which have no PSS include path and no business
// knowing the wire format.

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
#include <initializer_list>

#include "tile_geometry.h"

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

// The 12 quality metrics, in the one order every metric-indexed array and
// extract helper uses. Adding a metric means editing this enum, the name table
// in sai_common.cpp, FrameQualityResult::raw and BaselineValues in the same
// change, or the layouts fall out of step silently.
enum QualityMetric {
    QM_HIST_MEAN = 0,
    QM_HIST_VAR,
    QM_RMS_CONTRAST,
    QM_EDGE_DENSITY,
    QM_ENTROPY,
    QM_LAP_VAR,
    QM_CB_MEAN,
    QM_CB_VAR,
    QM_CR_MEAN,
    QM_CR_VAR,
    QM_SAT_MEAN,
    QM_SAT_VAR,
    QM_COUNT
};

// True for the six chroma metrics, which are absent on a monochrome source and
// are the ones BaselineValues::has_chroma gates.
constexpr bool sai_metric_is_chroma(int j) noexcept { return j >= QM_CB_MEAN; }

// Metric key names in QualityMetric order. Used for baseline.cfg keys and logs,
// so the file layout and the enum cannot drift apart.
extern const char* const kQualityMetricNames[QM_COUNT];


constexpr int   kDarkLevel     = 16;     // Y at or below: black, on full range (0-255)
constexpr int   kBrightLevel   = 239;    // Y at or above: white, on full range (0-255)

// The black and white lines one stream's luma is counted at. A limited-range
// stream (16-235, which H.264 means when a stream signals no range) carries a
// full-range level v at 16 + v * 219 / 255: Y <= 29.7 for black and Y >= 221.3
// for white, 29 and 222 in whole values.
struct LumaLines {
    int dark   = kDarkLevel;     // Y at or below: black
    int bright = kBrightLevel;   // Y at or above: white
};

constexpr LumaLines lumaLines(bool full_range) noexcept {
    return full_range ? LumaLines{kDarkLevel, kBrightLevel}
                      : LumaLines{16 + kDarkLevel * 219 / 255,
                                  16 + (kBrightLevel * 219 + 254) / 255};
}

constexpr bool lumaLinesValid(const LumaLines& l) noexcept {
    return l.dark >= 0 && l.dark < l.bright && l.bright <= 255;
}
static_assert(lumaLinesValid(lumaLines(true)) && lumaLinesValid(lumaLines(false)),
              "luma lines must lie in 0..255 with black below white");

// Radii of the band's sigma 1 and sigma 4 Gaussians, ceil(4 sigma): the cut
// tails hold under 0.01% of the weight.
constexpr int kBandR1 = 4;
constexpr int kBandR4 = 16;

// The 2 r + 1 sampled Gaussian weights, scaled to sum to 1 so a flat area gives a
// band of exactly 0. Sampling is accurate from sigma 1 up. False when r is not
// ceil(4 sigma) or not in 1..kBandR4.
inline bool sai_gauss_taps(double sigma, int r, float* w) noexcept {
    if (r < 1 || r > kBandR4 || (int)std::ceil(4.0 * sigma) != r) return false;
    double t[kBandR4];
    double sum = 0.0;
    for (int i = 0; i < r; ++i) {
        const double d = (double)(i - r);
        t[i] = std::exp(-d * d / (2.0 * sigma * sigma));
        sum += t[i];
    }
    const double mul = 1.0 / (2.0 * sum + 1.0);
    for (int i = 0; i < r; ++i) w[i] = w[2 * r - i] = (float)(t[i] * mul);
    w[r] = (float)mul;
    return true;
}

// Energy the band G(1) - G(4) keeps per unit variance of white noise in its input:
// the sum of its squared 2-D weights, (a.a)^2 + (b.b)^2 - 2 (a.b)^2 for the 1-D
// taps a, b. 0 when the taps cannot be built.
inline double sai_band_noise_gain() noexcept {
    float g1[2 * kBandR1 + 1], g4[2 * kBandR4 + 1];
    if (!sai_gauss_taps(1.0, kBandR1, g1) || !sai_gauss_taps(4.0, kBandR4, g4)) return 0.0;
    double aa = 0.0, bb = 0.0, ab = 0.0;
    for (int i = 0; i <= 2 * kBandR4; ++i) {
        const int k = i - kBandR4 + kBandR1;
        const double a = (k >= 0 && k <= 2 * kBandR1) ? (double)g1[k] : 0.0;
        aa += a * a;
        bb += (double)g4[i] * g4[i];
        ab += a * g4[i];
    }
    return aa * aa + bb * bb - 2.0 * ab * ab;
}

// Per-tile sums of one frame, the input of the tile checks (tile_scorer.h).
// L4 is |4p - its 4 neighbours|, 0 on the plane's outer ring. The fine band is
// G(1) - G(4) of Y, G(s) a Gaussian blur of sigma s pixels (mirror border).
struct TileSums {
    double px, cpx;               // luma and chroma pixels per tile
    double ysum[kTileCount];      // sum Y
    double dark[kTileCount];      // pixels at or below the stream's LumaLines::dark
    double bright[kTileCount];    // pixels at or above its LumaLines::bright
    double ylap[kTileCount];      // sum L4(Y)
    double motion[kTileCount];    // sum |Y - previous Y|; 0 on the first frame
    double detail[kTileCount];    // sum of the fine band squared
    double cb[kTileCount];        // sum Cb
    double cr[kTileCount];        // sum Cr
    double cb2[kTileCount];       // sum Cb^2
    double cr2[kTileCount];       // sum Cr^2
    double sat[kTileCount];       // sum s, the colorfulness above
    double sat2[kTileCount];      // sum s^2
    double cb_abs[kTileCount];    // sum |Cb - 128|
    double cr_abs[kTileCount];    // sum |Cr - 128|
    double clap[kTileCount];      // sum L4(Cb) + sum L4(Cr)
};

// One frame's measurements, produced by the linked backend and passed by value
// across the plugin boundary. TileScorer scores them.
struct FrameQualityResult {
    // Whole-frame metric values in the units of BaselineValues::mu, composed
    // from the per-tile integer accumulators.
    float raw[QM_COUNT];

    TileSums   tiles;

    bool  valid;                 // True only when backend analysis completed without errors.
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

// Max counter_max value (alert patience, frames).
// Also caps both quadratic counter steps: max_increment (>= max_decrement) * dist^2
// ≤ COUNTER_MAX_LIMIT * 99^2 (~9.8e6), fits in int32.
constexpr int COUNTER_MAX_LIMIT = 1000;

// FPS assumed when the H.264 sequence header carries none (zero/invalid
// frame_rate fields). Sizes the LEARN target and the ACTIVE tile waits.
constexpr int LEARN_FPS_FALLBACK = 30;

// Upper bound on stream-reported FPS used to compute the LEARN target and the
// ACTIVE tile waits. Guards against malformed sequence headers (e.g. frame_rate
// of 1000+ fps) that would otherwise inflate learn_target_frames_ to an
// unreachable value.
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

// Max time a sensor can hold its CAUSE_SAIM_INIT seed, from first NAL.
// Prevents dead cameras from looking like they're starting up.
constexpr int kIdrWaitTimeoutSec = 30;  // no keyframe: source fault
constexpr int kInitSeedGraceSec  = 15;  // keyframe seen, never reached VALID

// LEARN duration default and CLI cap (seconds). Shared so unit tests target
// the same frame count as the production binary.
constexpr int DEFAULT_LEARN_DURATION_SEC = 300;
// Caps learn_target_frames_ at ~LEARN_FPS_MAX * MAX_LEARN_DURATION_SEC.
constexpr int MAX_LEARN_DURATION_SEC = 3600;

// Bins of the frame luma histogram both backends build, one per 8-bit level.
constexpr int kSwHistogramBinCount = 256;

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

// Deviation at which the ACTIVE k-sigma score reaches 0, in units of k: the
// curve runs 100 at zero deviation, score_at_limit at the k-sigma limit, 0 at
// k * this. Pass score_at_k_sigma.
inline float zero_score_sigmas_for(float score_at_limit) noexcept {
    // validate() pins score_at_k_sigma to [0, 99], so this catches only a
    // malformed in-memory config. Falling back to 1 puts the zero crossing at
    // the limit itself: stricter, so the failure direction is fail-safe.
    if (!std::isfinite(score_at_limit) || score_at_limit < 0.0f ||
        score_at_limit > 99.0f)
        return 1.0f;
    return 100.0f / (100.0f - score_at_limit);
}

// True for the five variance metrics, which are the ones learned and scored on
// log(x) rather than raw. Their distributions are strongly right-skewed, so a
// k*sigma band in raw space either clips normal content variation or widens
// until it stops discriminating. The other seven metrics stay raw.
constexpr bool sai_metric_is_var(int j) noexcept {
    return j == QM_HIST_VAR || j == QM_LAP_VAR || j == QM_CB_VAR ||
           j == QM_CR_VAR   || j == QM_SAT_VAR;
}

// Clamp on the log's argument. A flat, black or frozen frame gives a variance
// of exactly 0, so 0 does reach the transform.
constexpr float kMetricLogFloor = 1e-6f;

// Negative tolerance on the raw (sum_sq - N*mean^2) subtraction
constexpr float kMetricCancelEpsilon = 1e-3f;

// Floor on a log-space sigma, replacing |mu| * sigma_floor_fraction: log
// compresses mu until it says nothing about the spread.
constexpr float kSigmaFloorLog = 1e-6f;

// Weights may sum to 1 +/- this, so a healthy blended score can reach 101.
constexpr float kWeightSumTolerance = 0.01f;

inline float sai_safe_log(float x) noexcept {
    return logf(x > kMetricLogFloor ? x : kMetricLogFloor);
}

// Means, variances, ratios and counts cannot be non-finite or negative, so either one
// means corrupt memory. Callers route a false to handleFrameFailure. Zero stays valid:
// a flat frame reads 0 on all of them, so a corrupt value must not be clamped into one.
inline bool sai_metrics_valid(std::initializer_list<float> values) noexcept {
    for (float v : values) {
        if (!std::isfinite(v) || v < 0.f) return false;
    }
    return true;
}

// Floors only fp rounding error from the subtraction. A larger negative is corruption
// and passes through unchanged for sai_metrics_valid to reject.
inline float sai_clamp_nonneg(double v) noexcept {
    const bool is_rounding = (v < 0.0) &&
                             (v >= -static_cast<double>(kMetricCancelEpsilon));
    return static_cast<float>(is_rounding ? 0.0 : v);
}

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
    //   score     = ramp(z, k * zero_score_sigmas_for(score_at_k_sigma), 0)
    // So z = 0 -> 100, z = k -> score_at_k_sigma, reaching 0 only further out.
    //
    // k is the Shewhart control limit: the deviation at which a metric is
    // declared out of control, not the deviation at which its score bottoms out.
    // The headroom past the limit is what keeps a mild excursion and gross
    // corruption distinguishable instead of collapsing both to 0.
    float k                       = 4.0f;   // Shewhart control limit, in sigmas.
    // Fractional-of-|mu| floor on the effective sigma. A divide-by-zero guard
    // for a degenerate (static-scene) sigma, not a tolerance knob: it must stay
    // far below the smallest sigma/|mu| any metric actually learns, or it
    // silently replaces the measurement. 0 = use the measured sigma as-is.
    //
    // Applies to the seven raw-space metrics only. The five log-space metrics
    // (sai_metric_is_var) use the absolute kSigmaFloorLog instead, because |mu|
    // stops tracking the spread once the metric is logged.
    float sigma_floor_fraction    = 0.0001f;

    // Score a metric gets at exactly k sigma, which fixes where the curve
    // reaches 0. Set it equal to score_high_threshold to make k mean the same
    // thing to the scorer and to the hysteresis counter below; set it lower to
    // put the control limit inside the fault region, higher to widen the
    // tolerance. Optional in thresholds.cfg - absent, it takes
    // score_high_threshold, the value it was fixed to before the key existed.
    int score_at_k_sigma          = 55;

    // Dual-threshold hysteresis for alert transitions. Strictly below
    // score_low_threshold increments toward INVALID, strictly above
    // score_high_threshold decrements toward VALID, and the closed band
    // [low, high] freezes the counter. Both comparisons are on the truncated
    // integer score, so the band includes every score in [low, high + 1).
    //
    // The frozen band is deliberate anti-chatter, but note what it costs when
    // score_at_k_sigma sits inside it: a metric just past its k-sigma limit
    // lands in the band and never accumulates. Widening the band trades
    // sensitivity to mild excursions for fewer false positives; narrowing it
    // does the reverse.
    //
    // Both counter steps are quadratic in the distance from the dead band, so
    // counter_max and max_increment are coupled: the worst score latches in
    // counter_max / max_increment frames and a score at low - 1 in counter_max.
    // Their ratio is max_increment, so that is the counter's resolution - the
    // number of distinguishable urgencies, which reshaping the curve cannot exceed.
    // max_decrement sets how sparse corruption can be and still alert: the
    // counter drifts upward above max_decrement / (max_increment + max_decrement)
    // of frames scoring below the dead band.
    int score_low_threshold       = 45;     // Below this: frame classified INVALID.
    int score_high_threshold      = 55;     // Above this: VALID.
    int counter_max               = 225;    // Counter cap and INVALID alert threshold.
    int max_increment             = 45;     // Max counter increment for worst scores.
    int max_decrement             = 9;      // Max counter decrement for best scores.

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

// Tile counterparts of the 12 whole-frame metrics (tile_scorer.cpp kTwinOf):
// log brightness, its mean step to the neighbouring tiles, log fine detail,
// mean Cb and Cr, mean |Cb - 128| and |Cr - 128|, their mean, and the log
// spread of Cb, Cr and s inside the tile, formed as the whole-frame spreads are.
enum TileTwin { TT_LEVEL, TT_CONTRAST, TT_FINE, TT_CB, TT_CR, TT_CB_STRENGTH,
                TT_CR_STRENGTH, TT_SAT, TT_CB_SPREAD, TT_CR_SPREAD, TT_SAT_SPREAD,
                TT_COUNT };

// LEARN of one tile series whose check alerts on one side of its mean: the mean,
// and the sigma on that side only.
struct TileBand {
    float mu[kTileCount];
    float sigma[kTileCount];
};

// LEARN of the tile checks. Each band is learn_series' summary of one tile's
// series, or summarize_series' over every frame where it refused it; a series
// too short for either keeps its mean with a sigma so wide that no value falls
// outside.
struct TileBaseline {
    TileBand grain;                   // log chroma texture over luma texture; sigma above the mean
    TileBand detail;                  // log fine band energy over brightness squared; sigma below
    TileBand motion;                  // 5 s mean |luma change| per pixel; sigma above
    float level[TT_COUNT][kTileCount];  // mean of each twin over every LEARN frame
    float black[kTileCount];          // mean share of the tile at black
    float white[kTileCount];          // mean share at white
};

// Averaged quality metrics from the LEARN phase, used as reference in ACTIVE mode.
// Stores both the mean (mu) and standard deviation (sigma) of each metric so
// ACTIVE-mode scoring can use a learned k*sigma tolerance band per camera
// instead of a static fractional margin.
struct BaselineValues {
    // Whole-frame mean of each metric over the LEARN survivors.
    float mu[QM_COUNT] = {};

    // Spread is carried only by the one-sided pair below; there is no symmetric
    // per-metric sigma.
    int   total_frames     = 0;

    // Picture size LEARN ran at. The texture metrics are per pixel, so ACTIVE
    // refuses a stream of any other size.
    int   frame_width      = 0;
    int   frame_height     = 0;

    // Canny thresholds pinned at LEARN time so ACTIVE reproduces the same
    // edge map regardless of thresholds.cfg drift. Optional in older
    // baseline.cfg files; when absent, loadFromFile leaves the caller's
    // pre-seeded values intact (thresholds.cfg fallback or sai_common.h
    // defaults, in that order).
    int canny_low_thresh  = CANNY_LOW_THRESH;
    int canny_high_thresh = CANNY_HIGH_THRESH;

    // Chroma (UV-plane) metrics live in the same mu / std arrays under the
    // QM_CB_* .. QM_SAT_* indices. This only records whether they were learned:
    // a monochrome source has none, and scoring must skip them rather than
    // compare against zeros.
    bool  has_chroma = false;

    /* One-sided sigmas: _lo over the LEARN frames below mu, _hi over those
     * above, both about that same mu, giving the ACTIVE band
     * [mu - k*sigma_lo, mu + k*sigma_hi].
     *
     * Needed because the variance metrics sit against a floor of 0 and only
     * content pushes them up: a single sigma averages their tight side with
     * their loose one, loosening the side a fault travels on.
     *
     * LearnFilter floors both by the pooled sigma, which cannot exceed the wider
     * side, so only the tighter one is ever lifted. That sigma is the floor only
     * and is not stored.
     *
     * Both keys are required: loadFromFile refuses a file missing either and
     * asks for a re-LEARN, there being no symmetric sigma to fall back on.
     */
    float std_lo[QM_COUNT] = {};
    float std_hi[QM_COUNT] = {};

    TileBaseline tile{};

    bool saveToFile(const std::string& path) const;
    // Loads key=value pairs via SaiConfigParser. Returns false on file/parse
    // error, and for a file from before the tile keys (asks for a re-LEARN).
    static bool loadFromFile(const std::string& path, BaselineValues& out);

    // Checks that all values (mu and sigma) are finite and non-negative, and
    // that total_frames >= MIN_LEARN_FRAMES_FOR_SIGMA.
    bool validate() const;
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
