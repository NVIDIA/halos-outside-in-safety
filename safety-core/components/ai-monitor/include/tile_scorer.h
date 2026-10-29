/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAI_TILE_SCORER_H
#define SAI_TILE_SCORER_H

#include <cstdint>
#include <memory>
#include <string>

#include "sai_common.h"

namespace sai {

// One tile's numbers for one frame (log brightness, log fine detail over
// brightness squared, log chroma texture over luma texture, mean |luma change|,
// black and white shares, twins).
struct TileNumbers {
    double level, detail, grain, motion, black, white;
    double twin[TT_COUNT];
};
// Every tile's numbers for one frame; false when a sum is not finite or a pixel
// count is zero.
bool tile_numbers(const TileSums& s, TileNumbers (&out)[kTileCount]);

/*
 * ACTIVE score of one camera, 0..100: the lowest of
 *  - the whole-frame luma and chroma scores, each metric keeping its drop only
 *    in the share of tiles whose counterpart moved its way on this frame beyond
 *    chance (a colour spread: the larger share of its two counterparts);
 *  - the area score: 45 at 2.5% of the frame broken, 0 at 2.5% / 0.55. A still
 *    tile (5 s motion within its LEARN band) counts its new black plus white
 *    share or, while its grain or fine detail has been out of its LEARN band
 *    for 5 s, the share of colour texture added or fine detail lost if larger.
 *    A moving tile counts only fine detail lost, and only while the frame's edge
 *    density or sharpness has kept a loss.
 * Decode thread only.
 */
class TileScorer {
public:
    // Adopts the baseline and config, validating both; the averages restart.
    bool configure(const BaselineValues& b, const ThresholdConfig& cfg);
    // Stream rate the averages are counted in; the averages are kept.
    void setFps(double fps);
    // One analysed frame. 0, with no state changed, before configure() and
    // setFps() or on a non-finite or negative input.
    int update(const FrameQualityResult& q);

private:
    void resizeAverages();

    BaselineValues  base_;
    ThresholdConfig cfg_;
    bool   configured_ = false;
    bool   started_    = false;   // the from-first-frame averages hold a value
    double fps_ = 0.0;
    double a5_  = 0.0;            // 5 s average weight per frame
    double bad_ = 0.0, zAlert_ = 0.0, factor2_ = 0.0, band_ = 0.0, zero_ = 0.0;

    double motion_[kTileCount] = {};
    double black_[kTileCount]  = {};
    double white_[kTileCount]  = {};
    double grain_[kTileCount]  = {};
    double detail_[kTileCount] = {};
    TileNumbers num_[kTileCount];
};

/*
 * LEARN of the tile checks: buffers each tile's banded series over every LEARN
 * frame and summarises them with learn_series(), which needs them all
 * (medians). That is 2.7 KB a frame (3 series x 225 tiles x 4 B): 24 MB for
 * 5 min at 30 fps. Shares and twins need only running sums.
 * Decode thread only.
 */
class TileLearn {
public:
    // Allocates for maxFrames; idempotent, so a mid-LEARN header keeps the data.
    bool init(uint64_t maxFrames, const ThresholdConfig& cfg);
    // Stream rate the 5 s motion average is counted in.
    void setFps(double fps);
    void reset() noexcept;
    // One LEARN frame, in decode order; invalid frames are skipped.
    void add(const FrameQualityResult& q);
    // Writes the tile baseline; false, logged and with `out` untouched, when
    // fewer than MIN_LEARN_FRAMES_FOR_SIGMA frames were kept.
    bool finalize(const std::string& label, TileBaseline& out);

private:
    enum Series { S_GRAIN, S_DETAIL, S_MOTION, S_COUNT };
    float* column(int s, int t) { return &buf_[((size_t)s * kTileCount + (size_t)t) * cap_]; }

    std::unique_ptr<float[]> buf_;    // [series][tile][cap_]
    std::unique_ptr<float[]> work_;   // learn_series scratch
    uint64_t cap_ = 0;                // frames the buffers hold
    uint64_t seen_ = 0;               // valid frames added
    uint64_t n_ = 0, nm_ = 0;         // kept samples, kept motion samples
    double   fps_ = 0.0, a5_ = 0.0;
    double   low_ = 0.0;              // score_low_threshold
    double   sumBlack_[kTileCount] = {}, sumWhite_[kTileCount] = {};
    double   sumTwin_[TT_COUNT][kTileCount] = {};
    double   motion_[kTileCount] = {};
    TileNumbers num_[kTileCount];
};

}  // namespace sai

#endif
