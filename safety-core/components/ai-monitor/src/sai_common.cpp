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

    // Split to match the getter each key is read with below.
    static const std::vector<std::string> requiredFloats = {
        "w_histogram", "w_contrast", "w_edge", "w_entropy", "w_laplacian",
        "uv_weight_cb", "uv_weight_cr", "uv_weight_sat"
    };
    static const std::vector<std::string> requiredInts = {
        "score_low_threshold", "score_high_threshold",
        "counter_max", "max_increment", "max_decrement"
    };
    if (!parser.validateRequiredKeys(requiredFloats, requiredInts)) return false;

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
    out.score_at_k_sigma         = parser.getInt("score_at_k_sigma",           out.score_high_threshold);
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
    if (std::fabs(wsum - 1.0f) > kWeightSumTolerance)
        fail("luma score weights must sum to ~1.0 "
             "(w_histogram + w_contrast + w_edge + w_entropy + w_laplacian)");

    if (uv_weight_cb < 0.f || uv_weight_cb > 1.f)
        fail("uv_weight_cb must be in [0, 1]");
    if (uv_weight_cr < 0.f || uv_weight_cr > 1.f)
        fail("uv_weight_cr must be in [0, 1]");
    if (uv_weight_sat < 0.f || uv_weight_sat > 1.f)
        fail("uv_weight_sat must be in [0, 1]");

    float uv_wsum = uv_weight_cb + uv_weight_cr + uv_weight_sat;
    if (std::fabs(uv_wsum - 1.0f) > kWeightSumTolerance)
        fail("uv chroma weights must sum to ~1.0 (uv_weight_cb + uv_weight_cr + uv_weight_sat)");

    if (!std::isfinite(k) || k < 2.0f || k > 8.0f)
        fail("k must be finite and in [2.0, 8.0]");

    // A divide-by-zero guard, not a tolerance. The tightest learned sigmas are
    // ~7e-4 of their mean, so capping at 1e-3 blunts them by at most 1.5x.
    // 0 leaves the degenerate case to the scorer's zero-sigma branch (zscore
    // in tile_scorer.cpp).
    if (!std::isfinite(sigma_floor_fraction) ||
        sigma_floor_fraction < 0.f || sigma_floor_fraction > 1e-3f)
        fail("sigma_floor_fraction must be in [0, 0.001]");

    if (score_low_threshold <= 0 || score_low_threshold >= 100)
        fail("score_low_threshold must be in (0, 100)");
    if (score_high_threshold <= 0 || score_high_threshold >= 100)
        fail("score_high_threshold must be in (0, 100)");
    if (score_high_threshold <= score_low_threshold)
        fail("score_high_threshold must be > score_low_threshold");
    // Upper bound is 99, not 100: zero_score_sigmas_for divides by (100 - this),
    // and 100 would put the curve's zero crossing at infinity.
    if (score_at_k_sigma < 0 || score_at_k_sigma > 99)
        fail("score_at_k_sigma must be in [0, 99]");
    // counter_max is the patience in frames, so its ceiling is COUNTER_MAX_LIMIT
    // rather than a round number; that constant also carries the overflow
    // headroom the quadratic increment relies on.
    if (counter_max <= 0 || counter_max > COUNTER_MAX_LIMIT)
        fail("counter_max must be in [1, COUNTER_MAX_LIMIT]");
    // Bounded by counter_max, not by a fixed magnitude: max_increment is the
    // ratio between the slowest and fastest latch, and a step larger than the
    // cap would only mean "latch in one frame" while making the ratio a lie.
    if (max_increment < 1 || max_increment > counter_max)
        fail("max_increment must be in [1, counter_max]");
    // Release must never outpace attack. This bound compares endpoints only, so
    // it delivers the invariant solely because both counter steps share the same
    // quadratic shape (see nvdec_decoder.cpp); against a linear decrement a good
    // frame would still drain faster than an equally-distant bad frame fills.
    if (max_decrement < 1 || max_decrement > max_increment)
        fail("max_decrement must be in [1, max_increment]");
    // Same invariant, but in raw score points rather than as a fraction of each
    // side's span. Needed separately because the steps normalise by different
    // spans - low^2 for the fill, (100 - high)^2 for the drain.
    if (score_low_threshold > 0 && score_low_threshold < 100 &&
        score_high_threshold > score_low_threshold && score_high_threshold < 100 &&
        max_decrement >= 1 && max_decrement <= max_increment &&
        max_increment <= COUNTER_MAX_LIMIT) {
        const int drain_span = 100 - score_high_threshold;   // in [1, 98]
        if (max_decrement * score_low_threshold * score_low_threshold >
            max_increment * drain_span * drain_span)
            fail("max_decrement * score_low_threshold^2 must be <= max_increment"
                 " * (100 - score_high_threshold)^2, else a good frame drains"
                 " faster than an equally-distant bad frame fills");
    }

    if (canny_low_thresh <= 0 || canny_low_thresh >= 255)
        fail("canny_low_thresh must be in (0, 255)");
    if (canny_high_thresh <= 0 || canny_high_thresh >= 255)
        fail("canny_high_thresh must be in (0, 255)");
    if (canny_high_thresh <= canny_low_thresh)
        fail("canny_high_thresh must be > canny_low_thresh");

    return ok;
}

namespace {

// One row per key, a comma-separated list in tile order.
template <typename T, size_t N>
void write_list(std::ostream& f, const std::string& key, const T (&v)[N]) {
    f << key << "=";
    for (size_t i = 0; i < N; ++i)
        f << (i ? "," : "") << v[i];
    f << "\n";
}

const char* const kTileTwinNames[TT_COUNT] = {
    "level", "contrast", "fine", "cb", "cr", "cb_strength", "cr_strength", "sat",
    "cb_spread", "cr_spread", "sat_spread"
};

// Each tile band's two rows: its mean, and its sigma named for the side it keeps
// (_lo below the mean, _hi above). Save and load both read this list, so a file
// that still carries the other side's row never has it taken for this one.
struct BandRows {
    const char* mu;
    const char* sigma;
    TileBand TileBaseline::*band;
};
const BandRows kTileBands[] = {
    {"tile_grain_mu",  "tile_grain_hi",  &TileBaseline::grain},
    {"tile_detail_mu", "tile_detail_lo", &TileBaseline::detail},
    {"tile_motion_mu", "tile_motion_hi", &TileBaseline::motion},
};

}  // namespace

bool BaselineValues::saveToFile(const std::string& path) const {
    // The loader refuses a baseline below this, so writing one would only waste
    // the LEARN run that produced it and surface later as a confusing
    // rejected-on-startup failure.
    if (total_frames < MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[Config] BaselineValues: refusing to save total_frames="
                  << total_frames << " < MIN_LEARN_FRAMES_FOR_SIGMA="
                  << MIN_LEARN_FRAMES_FOR_SIGMA << "\n";
        return false;
    }
    // Same reason: the loader requires every metric, chroma included.
    if (!has_chroma) {
        std::cerr << "[Config] BaselineValues: refusing to save a baseline"
                     " without chroma rows\n";
        return false;
    }

    std::ofstream f(path);
    if (!f.is_open()) return false;

    // Grid dimensions lead the file so a baseline learned on another grid is
    // identifiable as such at load, rather than only failing on element count.
    f << "tile_rows="         << kTileRows         << "\n"
      << "tile_cols="         << kTileCols         << "\n"
      << "frame_width="       << frame_width       << "\n"
      << "frame_height="      << frame_height      << "\n"
      << "total_frames="      << total_frames      << "\n"
      << "canny_low_thresh="  << canny_low_thresh  << "\n"
      << "canny_high_thresh=" << canny_high_thresh << "\n";

    for (int m = 0; m < QM_COUNT; ++m) {
        const std::string name(kQualityMetricNames[m]);
        f << name << "="         << mu[m]     << "\n"
          << name << "_std_lo=" << std_lo[m] << "\n"
          << name << "_std_hi=" << std_hi[m] << "\n";
    }

    for (const BandRows& r : kTileBands) {
        write_list(f, r.mu,    (tile.*r.band).mu);
        write_list(f, r.sigma, (tile.*r.band).sigma);
    }
    for (int k = 0; k < TT_COUNT; ++k)
        write_list(f, std::string("tile_level_") + kTileTwinNames[k], tile.level[k]);
    write_list(f, "tile_black",  tile.black);
    write_list(f, "tile_white",  tile.white);
    return f.good();
}

bool BaselineValues::loadFromFile(const std::string& path, BaselineValues& out) {
    SaiConfigParser parser;
    if (!parser.loadFromFile(path)) return false;

    // Grid first: every list below is sized by it, and a mismatch is the one
    // failure worth naming precisely. Left to the element-count check it would
    // surface as a generic "wrong number of elements" and read as corruption.
    const int rows = parser.getInt("tile_rows", 0);
    const int cols = parser.getInt("tile_cols", 0);
    if (rows != kTileRows || cols != kTileCols) {
        std::cerr << "[Config] BaselineValues: " << path << " was learned on a "
                  << rows << "x" << cols << " tile grid but this build uses "
                  << kTileRows << "x" << kTileCols
                  << ". Re-run LEARN to regenerate this baseline.\n";
        return false;
    }

    // The metric rows are lists, which getFloatList checks element by element
    // below; only the scalar keys go through the typed check. The frame size is
    // required: a missing or garbled one would switch off ACTIVE's size check.
    static const std::vector<std::string> requiredInts = {
        "total_frames", "frame_width", "frame_height", "canny_low_thresh", "canny_high_thresh"
    };
    if (!parser.validateRequiredKeys({}, requiredInts)) {
        std::cerr << "[Config] BaselineValues: " << path
                  << " is incomplete. Re-run LEARN to regenerate it.\n";
        return false;
    }

    // Checked before `out` is touched so a stale baseline never leaks into
    // gates that read total_frames > 0 as "baseline exists".
    const int total_frames = parser.getInt("total_frames", 0);
    if (total_frames < MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[Config] BaselineValues: total_frames=" << total_frames
                  << " < MIN_LEARN_FRAMES_FOR_SIGMA=" << MIN_LEARN_FRAMES_FOR_SIGMA
                  << " (re-LEARN required for statistically meaningful sigma)\n";
        return false;
    }

    // Parsed into a temporary and committed only once every metric has loaded,
    // so a file that fails halfway leaves the caller's baseline untouched
    // rather than half-replaced.
    BaselineValues tmp;
    tmp.total_frames      = total_frames;
    tmp.frame_width       = parser.getInt("frame_width",  0);
    tmp.frame_height      = parser.getInt("frame_height", 0);
    tmp.canny_low_thresh  = parser.getInt("canny_low_thresh",  out.canny_low_thresh);
    tmp.canny_high_thresh = parser.getInt("canny_high_thresh", out.canny_high_thresh);

    // The pair is the only spread the file carries, so a metric missing either
    // side cannot be scored and has nothing to fall back on.
    auto load_metric = [&](int m) {
        const std::string base(kQualityMetricNames[m]);
        return parser.getFloatList(base,             &tmp.mu[m],     1) &&
               parser.getFloatList(base + "_std_lo", &tmp.std_lo[m], 1) &&
               parser.getFloatList(base + "_std_hi", &tmp.std_hi[m], 1);
    };

    // Every metric is required, chroma included: a partial baseline is never adopted.
    for (int m = 0; m < QM_COUNT; ++m) {
        if (!load_metric(m)) {
            std::cerr << "[Config] BaselineValues: " << path << " has no usable "
                      << kQualityMetricNames[m]
                      << " row. Re-run LEARN to regenerate this baseline.\n";
            return false;
        }
    }
    tmp.has_chroma = true;

    TileBaseline& tb = tmp.tile;
    bool tiles_ok = parser.getFloatList("tile_black",  tb.black,  kTileCount) &&
                    parser.getFloatList("tile_white",  tb.white,  kTileCount);
    for (const BandRows& r : kTileBands)
        tiles_ok = tiles_ok && parser.getFloatList(r.mu,    (tb.*r.band).mu,    kTileCount) &&
                               parser.getFloatList(r.sigma, (tb.*r.band).sigma, kTileCount);
    for (int k = 0; tiles_ok && k < TT_COUNT; ++k)
        tiles_ok = parser.getFloatList(std::string("tile_level_") + kTileTwinNames[k],
                                       tb.level[k], kTileCount);
    if (!tiles_ok) {
        std::cerr << "[Config] BaselineValues: " << path << " has no usable tile rows."
                     " Re-run LEARN to regenerate this baseline.\n";
        return false;
    }

    out = tmp;
    return true;
}

bool BaselineValues::validate() const {
    bool ok = true;
    auto fail = [&](const char* msg) {
        std::cerr << "[Config] BaselineValues: " << msg << "\n";
        ok = false;
    };

    // The sai_metric_is_var means are stored as sai_safe_log(raw), so they go
    // negative whenever the raw variance is under 1 - routine on a monochrome
    // source, whose chroma variances sit at 0. The surviving bound is the
    // clamp's own output: anything below it did not come from sai_safe_log.
    const float log_floor = logf(kMetricLogFloor);

    if (total_frames < MIN_LEARN_FRAMES_FOR_SIGMA)
        fail("total_frames must be >= MIN_LEARN_FRAMES_FOR_SIGMA");

    if (canny_low_thresh <= 0 || canny_low_thresh >= 255)
        fail("canny_low_thresh must be in (0, 255)");
    if (canny_high_thresh <= 0 || canny_high_thresh >= 255)
        fail("canny_high_thresh must be in (0, 255)");
    if (canny_high_thresh <= canny_low_thresh)
        fail("canny_high_thresh must be > canny_low_thresh");

    if (frame_width <= 0 || frame_width > MAX_DIM || frame_height <= 0 || frame_height > MAX_DIM)
        fail("frame_width and frame_height must be in (0, MAX_DIM]");

    // The lower bound on a mean is set by the metric's space: the log metrics
    // can legitimately sit below zero, the raw ones cannot. A negative or
    // non-finite half-sigma would invert the band on that side; zero is
    // permitted, because sigma_floor_fraction takes over there.
    auto fail_m = [&](int m, const char* why) {
        ok = false;
        std::cerr << "[Config] BaselineValues: " << kQualityMetricNames[m] << " " << why << "\n";
    };
    for (int m = 0; m < QM_COUNT; ++m) {
        if (sai_metric_is_chroma(m) && !has_chroma) continue;
        if (sai_metric_is_var(m)) {
            if (!std::isfinite(mu[m]) || mu[m] < log_floor)
                fail_m(m, "must be finite and >= log(kMetricLogFloor)");
        } else if (!std::isfinite(mu[m]) || mu[m] < 0.f) {
            fail_m(m, "must be finite and >= 0");
        }
        if (!std::isfinite(std_lo[m]) || std_lo[m] < 0.f ||
            !std::isfinite(std_hi[m]) || std_hi[m] < 0.f)
            fail_m(m, "sigmas must be finite and >= 0");
    }

    // A tile band has a finite mean and a finite non-negative sigma.
    int bad_tiles = 0;
    auto band_ok = [](const TileBand& b, int t) {
        return std::isfinite(b.mu[t]) && std::isfinite(b.sigma[t]) && b.sigma[t] >= 0.f;
    };
    for (int t = 0; t < kTileCount; ++t) {
        bool good = band_ok(tile.grain, t) && band_ok(tile.detail, t) && band_ok(tile.motion, t) &&
                    tile.black[t] >= 0.f && tile.black[t] <= 1.f &&
                    tile.white[t] >= 0.f && tile.white[t] <= 1.f;
        for (int k = 0; k < TT_COUNT; ++k) good = good && std::isfinite(tile.level[k][t]);
        if (!good) ++bad_tiles;
    }
    if (bad_tiles > 0) {
        std::cerr << "[Config] BaselineValues: " << bad_tiles << " tile(s) out of range\n";
        ok = false;
    }
    return ok;
}

// QualityMetric order; see the enum in sai_common.h.
const char* const kQualityMetricNames[QM_COUNT] = {
    "hist_mean", "hist_var", "rms_contrast", "edge_density",
    "entropy",   "lap_var",  "cb_mean",      "cb_var",
    "cr_mean",   "cr_var",   "sat_mean",     "sat_var"
};
