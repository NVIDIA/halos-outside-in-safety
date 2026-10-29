/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LEARN_FILTER_H
#define LEARN_FILTER_H

#include <cstdint>
#include <memory>
#include <string>

#include "sai_common.h"

// Outlier-resistant baseline estimation for LEARN: buffers the run, scores each
// frame as |x-median|/MAD, drops the frames past kLearnDeviationCeiling, drops
// the kLearnInnerTrimPct of each side nearest the survivors' mean, then
// summarises what is left as one mean plus a pair of one-sided sigmas taken
// about that mean, for each of the 12 whole-frame metrics.
//
// The metrics in sai_metric_is_var are buffered, filtered and summarised in log
// space; the rest stay raw. add() is the only place the transform happens, so
// nothing downstream mixes the two.
namespace sai {

// Limit on a frame's second largest deviation in MAD units - second, so no one
// metric can condemn a frame alone.
constexpr float kLearnDeviationCeiling = 500.0f;

// Discard fraction above which the baseline is refused: past this the capture
// is too damaged to characterise the scene, and re-running LEARN is correct.
constexpr float kLearnMaxRejectFraction = 0.05f;

// Fraction of each side of the split dropped before the sigmas are taken,
// counted against that side's own survivors. The frames dropped are the ones
// closest to the mean, so this widens the band; finalize() says why inward.
// total_frames keeps reporting the pre-trim survivors, so the stored sigmas
// rest on roughly (100 - pct)% of it.
constexpr float kLearnInnerTrimPct = 40.0f;

// The trim leaves (100 - pct)% of the survivors, less one for rounding, and
// never fewer than MIN_LEARN_FRAMES_FOR_SIGMA went in. Holding that at two or
// more is what lets finalize() take a Bessel-corrected sigma with no
// degenerate-population fallback, so a change to either constant fails here
// rather than silently reintroducing one. The positive bound is what keeps
// inner_trim_drop_count's round-then-cast away from a negative value.
static_assert(kLearnInnerTrimPct >= 0.0f &&
                  (100.0f - kLearnInnerTrimPct) / 100.0f
                      * (float)MIN_LEARN_FRAMES_FOR_SIGMA - 1.0f >= 2.0f,
              "kLearnInnerTrimPct must be positive and leave enough frames "
              "for a sample sigma");

// Ceiling on init()'s request, and the bound that keeps QM_COUNT * slots and
// BaselineValues::total_frames in range whatever the caller computes. Twice
// MAX_LEARN_DURATION_SEC * LEARN_FPS_MAX, the worst case the CLI admits (1.73 M
// slots, ~83 MiB); a request past it is refused rather than truncated.
constexpr uint64_t kLearnMaxSlots =
    2ULL * (uint64_t)MAX_LEARN_DURATION_SEC * (uint64_t)LEARN_FPS_MAX;

// The side of its mean a one-sided check alerts on, and so the one sigma it keeps.
enum class SigmaSide { kBelow, kAbove };

// LearnFilter's rule on one series v[0..n), as the tile checks learn it: the
// |x - median| / MAD outlier test, then the same trim and floored sigma as
// finalize(), on `side` only. work holds 4 * n floats. False where it refuses
// the series.
bool learn_series(const float* v, size_t n, float* work, SigmaSide side, float& mu, float& sigma);

// The same trim and sigma over every frame of v[0..n), with no outlier test.
// work holds 3 * n floats. False, outputs unset, when the trim leaves fewer than
// MIN_LEARN_FRAMES_FOR_SIGMA.
bool summarize_series(const float* v, size_t n, float* work, SigmaSide side, float& mu,
                      float& sigma);

class LearnFilter {
public:
    LearnFilter() = default;

    // Allocates for maxFrames, refusing 0 or more than kLearnMaxSlots.
    // Idempotent, so a mid-LEARN sequence header cannot discard what was
    // collected. Failure is fatal to the run: this is the sole producer of the
    // baseline mu/sigma, so there is nothing to fall back to.
    bool init(uint64_t maxFrames);

    // Drops every buffered frame and counter, keeping the allocation. Call when
    // the analyzer restarts LEARN: one median/MAD population must not span two
    // resolutions, or the spread widens and finalize() may refuse it.
    void reset() noexcept;

    // One LEARN frame, in decode order. Invalid or non-finite frames keep their
    // slot but enter no statistic; frames past the buffer end are counted lost.
    // Applies sai_safe_log to the log-space metrics as it buffers them, so this
    // is the single point where the two spaces are decided.
    void add(const FrameQualityResult& q);

    // Overwrites the 12 means and 12 sigma pairs, and total_frames, in `out`
    // from the survivors. False, logged and with `out` untouched, when
    // too few survive or the discarded fraction exceeds kLearnMaxRejectFraction.
    //
    // Survivors are split at their mean, the kLearnInnerTrimPct of each side
    // nearest it is dropped, and mu and both sigmas are then recomputed from
    // what the trim left. Each side's sigma is about that new mu, not its own.
    // Flooring both by the pooled sigma can only lift the tighter side, never
    // the wider one. A side holding under two frames gets pooled too.
    //
    // For log-space metrics mu and both sigmas are in log space, so scoring
    // must apply sai_safe_log first.
    bool finalize(const std::string& label, BaselineValues& out);

    uint64_t frames() const { return n_; }
    // Fixed for the run, so the caller can cap its LEARN target at what fits.
    uint64_t capacity() const { return cap_; }

private:
    // Column-major, stride cap_, one column per metric, so every pass streams
    // one column end to end.
    std::unique_ptr<float[]> buf_;
    // These three each serve two turns in finalize(), and stay one column wide
    // because the trim finishes one column before the next begins. First the
    // outlier pass: scratch_ holds one metric's column for the median and MAD,
    // and dev1_/dev2_ the two largest |x-median|/MAD per frame.
    // Once keep_ is written those are spent, so the trim reuses them as the
    // high side, the low side and the surviving population. Each holds cap_
    // floats, which bounds every use.
    std::unique_ptr<float[]> scratch_;
    std::unique_ptr<float[]> dev1_;
    std::unique_ptr<float[]> dev2_;
    std::unique_ptr<uint8_t[]> ok_;     // Frame usable at all (see add()).
    // Accepted into the baseline, for every metric at once. Written once so the
    // count and every later pass share one predicate instead of restating the
    // rule.
    std::unique_ptr<uint8_t[]> keep_;

    uint64_t cap_      = 0;
    uint64_t n_        = 0;  // Slots filled, usable or not.
    uint64_t unusable_ = 0;
    uint64_t overflow_ = 0;
    uint64_t rejected_ = 0;
};

}  // namespace sai

#endif  // LEARN_FILTER_H
