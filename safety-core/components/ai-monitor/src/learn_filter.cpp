/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "learn_filter.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <new>
#include <string>

namespace sai {

namespace {

// Median; for even n this is the upper of the two middle values. Permutes v.
// Requires n > 0; both call sites run only after finalize() has established
// n >= MIN_LEARN_FRAMES_FOR_SIGMA.
float median_of(float* v, size_t n)
{
    const size_t k = n / 2;
    std::nth_element(v, v + k, v + n);
    return v[k];
}

// How many of one side's survivors to drop, nearest the reference mean first,
// counted against that side's own population `k`. The cast is safe because the
// static_assert on kLearnInnerTrimPct pins it positive. Returning 0 when the
// trim would consume the whole side keeps a side that had frames from being
// summarised as if it had none; at 40% that needs k == 0, but the constant is a
// tunable and the guard has to hold for every value the assert admits.
size_t inner_trim_drop_count(size_t k)
{
    const size_t cut =
        (size_t)std::llround((double)kLearnInnerTrimPct / 100.0 * (double)k);
    return (cut < k) ? cut : 0;
}

// The trim and sigmas of the frames of v that keep marks (all of them when
// keep is null); finalize() says why. hi, lo and kept are scratch of n floats
// each; a null sd_lo or sd_hi is a side not wanted, and not computed. Returns
// the frames the trim left; mu and the sigmas are set only when that is at
// least MIN_LEARN_FRAMES_FOR_SIGMA.
size_t summarize(const float* v, const uint8_t* keep, size_t n,
                 float* hi, float* lo, float* kept,
                 float& mu, float* sd_lo, float* sd_hi)
{
    // Side assignment only; never stored.
    double m = 0.0;
    uint64_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        if (keep && !keep[i]) continue;
        m += ((double)v[i] - m) / (double)(++k);
    }
    const float mu_full = (float)m;

    // A buffer per side, since each trims against its own count.
    size_t n_hi = 0, n_lo = 0;
    for (size_t i = 0; i < n; ++i) {
        if (keep && !keep[i]) continue;
        if (v[i] < mu_full) lo[n_lo++] = v[i];
        else                hi[n_hi++] = v[i];
    }

    // Nearest the mean is the smallest value on the hi side and the largest
    // on the lo side, so one ascending partition serves both: hi drops its
    // front, lo its tail.
    const size_t drop_hi = inner_trim_drop_count(n_hi);
    if (drop_hi > 0)
        std::nth_element(hi, hi + drop_hi, hi + n_hi);
    const size_t keep_lo = n_lo - inner_trim_drop_count(n_lo);
    if (keep_lo < n_lo)
        std::nth_element(lo, lo + keep_lo, lo + n_lo);

    // mu is recomputed, not kept at mu_full: with the near-centre bulk gone
    // the population is no longer centred where it was.
    size_t c = 0;
    for (size_t i = drop_hi; i < n_hi; ++i) kept[c++] = hi[i];
    for (size_t i = 0; i < keep_lo; ++i)    kept[c++] = lo[i];
    if (c < (size_t)MIN_LEARN_FRAMES_FOR_SIGMA) return c;

    double mt = 0.0;
    uint64_t kt = 0;
    for (size_t i = 0; i < c; ++i)
        mt += ((double)kept[i] - mt) / (double)(++kt);
    mu = (float)mt;

    // Re-split on the new mu so both sigmas share the centre ACTIVE draws its
    // band around. A frame exactly at mu counts high, matching zscore's
    // (dev < 0) ? lo : hi in tile_scorer.cpp.
    double s_lo = 0.0, s_hi = 0.0;
    uint64_t k_lo = 0, k_hi = 0;
    for (size_t i = 0; i < c; ++i) {
        const double d = (double)kept[i] - (double)mu;
        if (kept[i] < mu) { s_lo += d * d; ++k_lo; }
        else              { s_hi += d * d; ++k_hi; }
    }
    // pooled is a weighted mean of the two sides scaled by (c-2)/(c-1), so it
    // never exceeds the wider one and the floor only lifts the tighter side. A
    // side under two frames takes pooled; zero would collapse that half of the
    // band. pooled is not stored.
    const float pooled = (float)std::sqrt((s_lo + s_hi) / (double)(c - 1));
    auto floored = [pooled](double sum_sq, uint64_t frames) {
        const float side = (frames > 1) ? (float)std::sqrt(sum_sq / (double)(frames - 1)) : pooled;
        return std::max(side, pooled);
    };
    if (sd_lo) *sd_lo = floored(s_lo, k_lo);
    if (sd_hi) *sd_hi = floored(s_hi, k_hi);
    return c;
}

// QualityMetric order. Only used to name a column that produced no spread.
const char* const kMetricNames[QM_COUNT] = {
    "hist_mean", "hist_var", "rms_contrast", "edge_density",
    "entropy",   "lap_var",  "cb_mean",      "cb_var",
    "cr_mean",   "cr_var",   "sat_mean",     "sat_var"
};

}

bool learn_series(const float* v, size_t n, float* work, SigmaSide side, float& mu, float& sigma)
{
    if (n < (size_t)MIN_LEARN_FRAMES_FOR_SIGMA) return false;
    float* a = work;
    float* b = work + n;
    std::copy(v, v + n, b);
    const float med = median_of(b, n);
    for (size_t i = 0; i < n; ++i) b[i] = std::fabs(v[i] - med);
    const float mad = median_of(b, n);

    // As finalize(): a zero or denormal MAD places nothing, so every frame stays.
    size_t kept = 0;
    const bool spread = std::isnormal(mad);
    const float inv = spread ? 1.0f / mad : 0.0f;
    for (size_t i = 0; i < n; ++i)
        if (!spread || std::fabs(v[i] - med) * inv <= kLearnDeviationCeiling) a[kept++] = v[i];
    if ((double)(n - kept) > kLearnMaxRejectFraction * (double)n ||
        kept < (size_t)MIN_LEARN_FRAMES_FOR_SIGMA)
        return false;
    const bool up = side == SigmaSide::kAbove;
    return summarize(a, nullptr, kept, b, work + 2 * n, work + 3 * n, mu,
                     up ? nullptr : &sigma, up ? &sigma : nullptr)
           >= (size_t)MIN_LEARN_FRAMES_FOR_SIGMA;
}

bool summarize_series(const float* v, size_t n, float* work, SigmaSide side, float& mu,
                      float& sigma)
{
    const bool up = side == SigmaSide::kAbove;
    return summarize(v, nullptr, n, work, work + n, work + 2 * n, mu,
                     up ? nullptr : &sigma, up ? &sigma : nullptr)
           >= (size_t)MIN_LEARN_FRAMES_FOR_SIGMA;
}

bool LearnFilter::init(uint64_t maxFrames)
{
    if (buf_) return true;
    if (maxFrames == 0) return false;
    if (maxFrames > kLearnMaxSlots) {
        std::cerr << "LEARN: filter request of " << maxFrames
                  << " frames exceeds the " << kLearnMaxSlots << " slot ceiling\n";
        return false;
    }

    const size_t n = (size_t)maxFrames;
    buf_.reset(new (std::nothrow) float[n * (size_t)QM_COUNT]);
    scratch_.reset(new (std::nothrow) float[n]);
    dev1_.reset(new (std::nothrow) float[n]);
    dev2_.reset(new (std::nothrow) float[n]);
    ok_.reset(new (std::nothrow) uint8_t[n]);
    keep_.reset(new (std::nothrow) uint8_t[n]);

    if (!buf_ || !scratch_ || !dev1_ || !dev2_ || !ok_ || !keep_) {
        buf_.reset();
        scratch_.reset();
        dev1_.reset();
        dev2_.reset();
        ok_.reset();
        keep_.reset();
        return false;
    }
    cap_ = maxFrames;
    return true;
}

void LearnFilter::reset() noexcept
{
    // Only the counters: n_ gates every read of the buffers, so stale values
    // past it are unreachable and clearing them would be wasted work.
    n_        = 0;
    unusable_ = 0;
    overflow_ = 0;
    rejected_ = 0;
}

void LearnFilter::add(const FrameQualityResult& q)
{
    if (!buf_ || n_ >= cap_) {
        ++overflow_;
        return;
    }

    bool ok = q.valid;
    for (int j = 0; ok && j < QM_COUNT; ++j)
        ok = std::isfinite(q.raw[j]);

    // Slot kept so a frame's position in the run is preserved; value zeroed
    // because nth_element is undefined with a NaN in range. ok_ does the excluding.
    //
    // The log metrics are transformed here, at the buffer's edge, so that every
    // pass below - median, MAD, mu and both sigmas - reads one space. Doing it
    // later would leave the outlier filter measuring raw deviations that the
    // saved sigma is no longer in the units of. isfinite is checked on the raw
    // value above, and sai_safe_log of a finite input is finite.
    for (int j = 0; j < QM_COUNT; ++j) {
        const float v = q.raw[j];
        buf_[(size_t)j * cap_ + n_] = ok ? (sai_metric_is_var(j) ? sai_safe_log(v) : v) : 0.0f;
    }
    ok_[n_] = ok ? 1u : 0u;
    if (!ok) ++unusable_;
    ++n_;
}

bool LearnFilter::finalize(const std::string& label, BaselineValues& out)
{
    // n_ > 0 implies every buffer is held: add() checks buf_ before advancing
    // it, and init() releases all six if any one fails.
    const size_t n = (size_t)n_;
    size_t nOk = 0;
    for (size_t i = 0; i < n; ++i)
        nOk += ok_[i];

    if (nOk < (size_t)MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[" << label << "] LEARN: only " << nOk
                  << " usable frame(s) of " << n << " (need "
                  << MIN_LEARN_FRAMES_FOR_SIGMA << ")\n";
        return false;
    }
    if (overflow_ > 0)
        std::cerr << "[" << label << "] LEARN: buffer full, " << overflow_
                  << " frame(s) past the end excluded\n";

    for (size_t i = 0; i < n; ++i) {
        dev1_[i] = 0.0f;
        dev2_[i] = 0.0f;
    }

    // One verdict per frame, taken on its twelve metrics together.
    //
    // Per metric: median, MAD, then each frame's deviation in MAD units,
    // keeping the two largest across metrics.
    //
    // isnormal, not > 0: a zero MAD places nothing, and a denormal one makes
    // 1/mad infinite, so every deviation becomes inf or NaN. The metric
    // abstains either way.
    int flat[QM_COUNT];
    int nFlat = 0;
    for (int j = 0; j < QM_COUNT; ++j) {
        const float* col = &buf_[(size_t)j * cap_];

        size_t c = 0;
        for (size_t i = 0; i < n; ++i)
            if (ok_[i]) scratch_[c++] = col[i];
        const float med = median_of(scratch_.get(), c);

        for (size_t i = 0; i < c; ++i)
            scratch_[i] = std::fabs(scratch_[i] - med);
        const float mad = median_of(scratch_.get(), c);
        if (!std::isnormal(mad)) {
            flat[nFlat++] = j;
            continue;
        }

        const float inv = 1.0f / mad;
        for (size_t i = 0; i < n; ++i) {
            if (!ok_[i]) continue;
            const float d = std::fabs(col[i] - med) * inv;
            if (d > dev1_[i]) {
                dev2_[i] = dev1_[i];
                dev1_[i] = d;
            } else if (d > dev2_[i]) {
                dev2_[i] = d;
            }
        }
    }

    // MAD = zero: most frames identical, so no outlier filtering for these metrics.
    if (nFlat > 0) {
        std::cerr << "[" << label << "] LEARN: no spread in " << nFlat << " of "
                  << (int)QM_COUNT << " metrics (";
        for (int i = 0; i < nFlat; ++i)
            std::cerr << (i ? ", " : "") << kMetricNames[flat[i]];
        std::cerr << "); outlier filtering is inactive for them. Re-run LEARN on"
                     " a scene with motion if the saved sigmas look wrong.\n";
    }

    // Shape-independent by design: how far out a frame is does not depend on
    // how many others share its fate, so one damaged frame and a third of the
    // run meet the same limit.
    rejected_ = 0;
    size_t keep = 0;
    for (size_t i = 0; i < n; ++i) {
        keep_[i] = (ok_[i] && dev2_[i] <= kLearnDeviationCeiling) ? 1u : 0u;
        if (keep_[i]) {
            ++keep;
        } else if (ok_[i]) {
            ++rejected_;
        }
    }

    if ((double)rejected_ > kLearnMaxRejectFraction * (double)nOk) {
        std::cerr << "[" << label << "] LEARN: " << rejected_ << " of " << nOk
                  << " frames are outliers, above the "
                  << (int)(kLearnMaxRejectFraction * 100.0f)
                  << "% limit; the capture is too damaged to characterise the "
                     "scene - re-run LEARN\n";
        return false;
    }
    if (keep < (size_t)MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[" << label << "] LEARN: only " << keep
                  << " frame(s) survive filtering (need "
                  << MIN_LEARN_FRAMES_FOR_SIGMA << ")\n";
        return false;
    }

    // The only producer of the baseline mu/sigma; both backends leave them zero.
    // Filled into locals and copied out at the end, so a metric that fails
    // partway leaves the caller's baseline untouched rather than half-replaced.
    //
    // Bessel-corrected throughout, because ACTIVE's k*sigma band is a Shewhart
    // control limit and wants the unbiased sample sigma. The trim drops frames
    // near the mean, not on the far tail: near-static stretches of the capture
    // pull the spread in, and frames sitting on the mean say least about how far
    // the scene actually ranges.
    float mu[QM_COUNT] = {}, sd_lo[QM_COUNT] = {}, sd_hi[QM_COUNT] = {};
    for (int j = 0; j < QM_COUNT; ++j) {
        const size_t kept = summarize(&buf_[(size_t)j * cap_], keep_.get(), n, scratch_.get(),
                                      dev1_.get(), dev2_.get(), mu[j], &sd_lo[j], &sd_hi[j]);
        // total_frames stays the pre-trim count, so the sigma minimum is held here.
        if (kept < (size_t)MIN_LEARN_FRAMES_FOR_SIGMA) {
            std::cerr << "[" << label << "] LEARN: " << kMetricNames[j] << ": only " << kept
                      << " frame(s) remain after the inner trim (need "
                      << MIN_LEARN_FRAMES_FOR_SIGMA << "); run LEARN longer\n";
            return false;
        }
    }

    // Committed only now that every metric has passed. has_chroma and the canny
    // thresholds are left as the backend set them; this filter cannot tell
    // whether the stream carries a chroma plane.
    for (int j = 0; j < QM_COUNT; ++j) {
        out.mu[j]     = mu[j];
        out.std_lo[j] = sd_lo[j];
        out.std_hi[j] = sd_hi[j];
    }
    // Narrowing is safe by construction: keep <= cap_ <= kLearnMaxSlots, which
    // init() enforces and which is far below INT_MAX.
    out.total_frames = (int)keep;

    std::cout << "  [" << label << "][Learn] filter: kept " << keep << " of "
              << n << " frames (" << rejected_ << " outlier"
              << (rejected_ == 1 ? "" : "s") << ", " << unusable_
              << " unusable)\n";
    return true;
}

}
