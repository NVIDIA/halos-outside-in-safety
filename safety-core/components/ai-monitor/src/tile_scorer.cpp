/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tile_scorer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <new>

#include "learn_filter.h"

namespace sai {

namespace {

constexpr double kWindowSec  = 5.0;         // s a tile broken outright takes to reach the alert line
constexpr double kLine       = 0.025;       // broken share of the frame at the alert line: half a 5% fault
constexpr double kLogFloor   = 1e-6;        // sigma floor of the motion check
// A twin this close to its LEARN level (relative) has not moved. The profile keeps
// 6 significant digits, off by up to 5e-6 of the value, so a tile that never changes
// must stay inside this.
constexpr double kFlat       = 1e-5;
constexpr double kZClip      = 1e4;
constexpr float  kHugeZ      = 1e6f;        // z of any deviation against a zero sigma
constexpr float  kNoBand     = 1e30f;       // sigma of a series too short for a band: no value leaves it

// The side of its LEARN mean each one-sided tile check alerts on, and so the one
// sigma LEARN keeps for it: colour texture added, fine detail lost, and a tile
// moving more than LEARN saw.
constexpr SigmaSide kGrainSide  = SigmaSide::kAbove;
constexpr SigmaSide kDetailSide = SigmaSide::kBelow;
constexpr SigmaSide kMotionSide = SigmaSide::kAbove;

// The floors are what 8-bit rounding alone puts into each number: an error even over
// +-1/2 level (variance 1/12), independent from pixel to pixel. Fine band energy per
// pixel; the errors' mean L4 is exactly 13/12.
const double kFineFloor = sai_band_noise_gain() / 12.0;
constexpr double kGrainFloor = 13.0 / 12.0;

// Tile counterparts of each whole-frame metric, in QualityMetric order. The spread
// metrics ask the contrast between tiles and the edge and sharpness ones the fine
// detail: two scales, so one coincidence cannot carry every luma metric. A spread
// is its part between tiles plus its part inside them. The luma spreads leave the
// inside part, which blur takes, to the fine-detail checks. Nothing else checks
// lost colour texture, so a colour spread asks both parts: how far each tile's
// colour is from grey and its spread inside the tile. A fade shrinks both, colour
// smoothing only the inside one, and a vote on their sum follows the larger part.
constexpr int kNoTwin = TT_COUNT;
constexpr int kTwinOf[QM_COUNT][2] = {
    {TT_LEVEL, kNoTwin},       {TT_CONTRAST, kNoTwin},        {TT_CONTRAST, kNoTwin},
    {TT_FINE, kNoTwin},        {TT_CONTRAST, kNoTwin},        {TT_FINE, kNoTwin},
    {TT_CB, kNoTwin},          {TT_CB_STRENGTH, TT_CB_SPREAD},
    {TT_CR, kNoTwin},          {TT_CR_STRENGTH, TT_CR_SPREAD},
    {TT_SAT, kNoTwin},         {TT_SAT, TT_SAT_SPREAD}
};

// Signed deviation in one-sided sigmas, floored as the backends floor them.
float zscore(int m, float x, float mu, float lo, float hi, float floorFrac)
{
    const bool logm = sai_metric_is_var(m);
    const float dev = (logm ? sai_safe_log(x) : x) - mu;
    const float side = (dev < 0.f) ? lo : hi;
    const float sigma = std::fmax(std::isfinite(side) ? side : 0.f,
                                  logm ? kSigmaFloorLog : std::fabs(mu) * floorFrac);
    if (sigma > 0.f) return dev / sigma;
    return (std::fabs(dev) < 1e-9f) ? 0.f : (dev < 0.f ? -kHugeZ : kHugeZ);
}

// The backends' ramp: 100 at no deviation, 0 from `bad` sigmas.
double ramp(double v, double bad)
{
    return std::min(std::max((v - bad) / (0.0 - bad) * 100.0, 0.0), 100.0);
}

// z of x against a tile's LEARN band on the side its check alerts on, the sigma
// floored; 0 on the other side.
double zs(double x, const TileBand& b, int t, double floor, SigmaSide side)
{
    const double q = (x - b.mu[t]) / std::max((double)b.sigma[t], floor);
    return std::min(std::max(side == SigmaSide::kAbove ? q : -q, 0.0), kZClip);
}

// Share of the tile newly at the limit, counted once its 5 s average from 0
// has reached `band` of it, in a tile at the limit for under 1 - band of LEARN.
double held(double p, double avg, double rate, double band)
{
    const double now = p - rate;
    return (now > 0.0 && avg - rate > band * now && rate < 1.0 - band) ? now : 0.0;
}

// Share of an energy that a rise of d in its log takes away (detail) or adds
// (grain): 1 - e^-d, so half at a doubling, the sigma floor's alert line.
double share(double d)
{
    return (d > 0.0) ? 1.0 - std::exp(-d) : 0.0;
}

// Log spread of n samples from their sum and sum of squares, as the backends form
// the whole-frame spreads: Bessel-corrected, clamped at 0, then sai_safe_log.
double logSpread(double sum, double sq, double n)
{
    const double var = (n > 1.0) ? (sq - sum * sum / n) / (n - 1.0) : 0.0;
    // sai_safe_log would turn a corrupt sum into its floor; non-finite, tile_numbers() refuses it.
    if (!std::isfinite(var)) return var;
    return sai_safe_log((float)std::max(var, 0.0));
}

double lumaScore(const double* s, const ThresholdConfig& c)
{
    return c.w_histogram * std::min(s[QM_HIST_MEAN], s[QM_HIST_VAR]) +
           c.w_contrast  * s[QM_RMS_CONTRAST] + c.w_edge * s[QM_EDGE_DENSITY] +
           c.w_entropy   * s[QM_ENTROPY]      + c.w_laplacian * s[QM_LAP_VAR];
}

double chromaScore(const double* s, const ThresholdConfig& c)
{
    return c.uv_weight_cb  * std::min(s[QM_CB_MEAN],  s[QM_CB_VAR]) +
           c.uv_weight_cr  * std::min(s[QM_CR_MEAN],  s[QM_CR_VAR]) +
           c.uv_weight_sat * std::min(s[QM_SAT_MEAN], s[QM_SAT_VAR]);
}

}  // namespace

bool tile_numbers(const TileSums& s, TileNumbers (&out)[kTileCount])
{
    if (!(s.px > 0.0) || !(s.cpx > 0.0)) return false;
    for (int t = 0; t < kTileCount; ++t) {
        TileNumbers& n = out[t];
        const double m = std::max(s.ysum[t] / s.px, 1.0);
        n.level  = std::log(m);
        n.detail = std::log(s.detail[t] + s.px * kFineFloor) - 2.0 * n.level;
        n.grain  = std::log(s.clap[t] / (2.0 * s.cpx) + kGrainFloor) - std::log(s.ylap[t] / s.px + kGrainFloor);
        n.motion = s.motion[t] / s.px;
        n.black  = s.dark[t] / s.px;
        n.white  = s.bright[t] / s.px;
        n.twin[TT_LEVEL]        = n.level;
        n.twin[TT_FINE]         = n.detail + 2.0 * n.level;
        n.twin[TT_CB]           = s.cb[t] / s.cpx;
        n.twin[TT_CR]           = s.cr[t] / s.cpx;
        n.twin[TT_CB_STRENGTH]  = s.cb_abs[t] / s.cpx;
        n.twin[TT_CR_STRENGTH]  = s.cr_abs[t] / s.cpx;
        n.twin[TT_SAT]          = (s.cb_abs[t] + s.cr_abs[t]) / (2.0 * s.cpx);
        n.twin[TT_CB_SPREAD]    = logSpread(s.cb[t],  s.cb2[t],  s.cpx);
        n.twin[TT_CR_SPREAD]    = logSpread(s.cr[t],  s.cr2[t],  s.cpx);
        n.twin[TT_SAT_SPREAD]   = logSpread(s.sat[t], s.sat2[t], s.cpx);
        bool ok = std::isfinite(n.detail) && std::isfinite(n.grain) && std::isfinite(n.motion) &&
                  std::isfinite(n.black) && std::isfinite(n.white);
        // The contrast is filled below from the levels checked here.
        for (int k = 0; k < TT_COUNT; ++k) ok = ok && (k == TT_CONTRAST || std::isfinite(n.twin[k]));
        if (!ok) return false;
    }
    // Contrast between tiles: the mean log brightness step to the 4 neighbours.
    // A gain on the whole frame cancels in it; a veil over the lens flattens it.
    static_assert(kTileRows > 1 && kTileCols > 1, "every tile needs a neighbour");
    for (int t = 0; t < kTileCount; ++t) {
        const int r = t / kTileCols, c = t % kTileCols;
        const double v = out[t].level;
        double sum = 0.0;
        int k = 0;
        if (r > 0)             { sum += std::fabs(v - out[t - kTileCols].level); ++k; }
        if (r < kTileRows - 1) { sum += std::fabs(v - out[t + kTileCols].level); ++k; }
        if (c > 0)             { sum += std::fabs(v - out[t - 1].level); ++k; }
        if (c < kTileCols - 1) { sum += std::fabs(v - out[t + 1].level); ++k; }
        out[t].twin[TT_CONTRAST] = sum / k;
    }
    return true;
}

bool TileScorer::configure(const BaselineValues& b, const ThresholdConfig& cfg)
{
    configured_ = false;
    if (!b.has_chroma || !b.validate() || !cfg.validate()) return false;
    base_ = b;
    cfg_ = cfg;
    bad_     = cfg.k * zero_score_sigmas_for((float)cfg.score_at_k_sigma);
    band_    = 1.0 - cfg.score_low_threshold / 100.0;
    zAlert_  = bad_ * band_;
    factor2_ = std::log(2.0) / zAlert_;   // a log number's sigma floor: doubling reaches the alert line
    zero_    = kLine / band_;
    started_ = false;
    for (int t = 0; t < kTileCount; ++t) {
        black_[t] = white_[t] = 0.0;
        grain_[t] = detail_[t] = 100.0;
    }
    configured_ = true;
    resizeAverages();
    return true;
}

void TileScorer::setFps(double fps)
{
    if (!std::isfinite(fps) || !(fps > 0.0)) return;
    fps_ = fps;
    resizeAverages();
}

void TileScorer::resizeAverages()
{
    if (!configured_ || !(fps_ > 0.0)) return;
    // A step reaches 1 - score_low_threshold of its size in kWindowSec at the
    // frame rate the stream header declares (setFps), not the measured rate.
    a5_  = 1.0 - std::pow(cfg_.score_low_threshold / 100.0, 1.0 / (kWindowSec * fps_));
}

int TileScorer::update(const FrameQualityResult& q)
{
    if (!configured_ || !(fps_ > 0.0)) return 0;
    for (int m = 0; m < QM_COUNT; ++m)
        if (!std::isfinite(q.raw[m]) || q.raw[m] < 0.f) return 0;
    if (!tile_numbers(q.tiles, num_)) return 0;

    float z[QM_COUNT];
    double s[QM_COUNT];
    for (int m = 0; m < QM_COUNT; ++m) {
        z[m] = zscore(m, q.raw[m], base_.mu[m], base_.std_lo[m], base_.std_hi[m],
                      cfg_.sigma_floor_fraction);
        s[m] = ramp(std::fabs(z[m]), bad_);
    }

    const TileBaseline& L = base_.tile;
    const double low = cfg_.score_low_threshold;

    // Each metric keeps its drop in the share of tiles, beyond chance, whose
    // counterpart moved its way on this frame; a tile still at its LEARN level
    // counts half. A metric with two counterparts keeps the larger share. The
    // decoder's counter supplies the persistence, so a vote on averages would
    // only delay the onset.
    auto agreement = [&](int k, int way) {
        double agree = 0.0;
        for (int t = 0; t < kTileCount; ++t) {
            const double mid = L.level[k][t];
            const double d = num_[t].twin[k] - mid;
            if (std::fabs(d) <= kFlat * (1.0 + std::fabs(mid))) agree += 0.5;
            else if (d * way > 0.0)                              agree += 1.0;
        }
        return std::min(std::max(2.0 * agree / kTileCount - 1.0, 0.0), 1.0);
    };
    double kept[QM_COUNT];
    for (int m = 0; m < QM_COUNT; ++m) {
        double o = 1.0;
        if (std::isfinite(z[m])) {
            const int way = (z[m] > 0.f) - (z[m] < 0.f);
            o = agreement(kTwinOf[m][0], way);
            if (kTwinOf[m][1] != kNoTwin) o = std::max(o, agreement(kTwinOf[m][1], way));
        }
        kept[m] = 100.0 - (100.0 - s[m]) * o;
    }
    // Edge density or sharpness down and kept down by the tiles' fine detail, the
    // counterpart of both: the whole image lost fine detail, which blur or fog does
    // and a moving object cannot.
    const bool frameBlurred = (z[QM_EDGE_DENSITY] < 0.f && kept[QM_EDGE_DENSITY] < low) ||
                              (z[QM_LAP_VAR] < 0.f && kept[QM_LAP_VAR] < low);

    double broken = 0.0;
    for (int t = 0; t < kTileCount; ++t) {
        const TileNumbers& x = num_[t];
        if (!started_) motion_[t] = x.motion;
        else           motion_[t] += a5_ * (x.motion - motion_[t]);
        black_[t]  += a5_ * (x.black - black_[t]);
        white_[t]  += a5_ * (x.white - white_[t]);
        grain_[t]  += a5_ * (ramp(zs(x.grain,  L.grain,  t, factor2_, kGrainSide), bad_) - grain_[t]);
        detail_[t] += a5_ * (ramp(zs(x.detail, L.detail, t, factor2_, kDetailSide), bad_) - detail_[t]);

        // A tile moving more than LEARN saw is a scene change, not a fault: it counts
        // only lost detail, and only while the whole image lost detail too. Its own
        // loss, however deep, is what a smooth object passing close to the lens gives.
        const bool moving = zs(motion_[t], L.motion, t, kLogFloor, kMotionSide) > zAlert_;
        // Black and white are separate pixels, so their shares add. A grain or detail
        // check out of its band for 5 s counts the share this frame has changed
        // against the LEARN mean: colour texture added, fine detail lost.
        double hit = 0.0;
        if (!moving) {
            hit = held(x.black, black_[t], L.black[t], band_) +
                  held(x.white, white_[t], L.white[t], band_);
            if (grain_[t] < low)
                hit = std::max(hit, share(x.grain - L.grain.mu[t]));
        }
        if (detail_[t] < low && (!moving || frameBlurred))
            hit = std::max(hit, share(L.detail.mu[t] - x.detail));
        broken += hit;
    }
    const double area =
        std::min(std::max(100.0 * (1.0 - broken / kTileCount / zero_), 0.0), 100.0);

    const double y  = lumaScore(kept, cfg_);
    const double uv = chromaScore(kept, cfg_);
    started_ = true;

    const double score = std::min(std::min(y, uv), area);
    return (int)std::min(std::max(score, 0.0), 100.0);
}

bool TileLearn::init(uint64_t maxFrames, const ThresholdConfig& cfg)
{
    low_ = cfg.score_low_threshold;
    if (buf_) return true;
    if (maxFrames == 0) return false;
    cap_ = maxFrames;
    const size_t n = (size_t)cap_;
    buf_.reset(new (std::nothrow) float[(size_t)S_COUNT * kTileCount * n]);
    work_.reset(new (std::nothrow) float[4 * n]);
    if (!buf_ || !work_) {
        buf_.reset();
        work_.reset();
        cap_ = 0;
        return false;
    }
    setFps(fps_);
    return true;
}

void TileLearn::setFps(double fps)
{
    if (!std::isfinite(fps) || !(fps > 0.0) || !(low_ > 0.0)) return;
    fps_ = fps;
    a5_  = 1.0 - std::pow(low_ / 100.0, 1.0 / (kWindowSec * fps));
}

void TileLearn::reset() noexcept
{
    seen_ = n_ = nm_ = 0;
    for (int t = 0; t < kTileCount; ++t) {
        sumBlack_[t] = sumWhite_[t] = 0.0;
        for (int k = 0; k < TT_COUNT; ++k) sumTwin_[k][t] = 0.0;
    }
}

void TileLearn::add(const FrameQualityResult& q)
{
    if (!buf_ || !q.valid || !(fps_ > 0.0)) return;
    if (!tile_numbers(q.tiles, num_)) return;

    const uint64_t i = seen_++;
    const bool keep = n_ < cap_;
    // The motion series starts once its 5 s average has settled.
    const bool settled = i >= (uint64_t)(kWindowSec * fps_);
    for (int t = 0; t < kTileCount; ++t) {
        const TileNumbers& x = num_[t];
        motion_[t] = (i == 0) ? x.motion : motion_[t] + a5_ * (x.motion - motion_[t]);
        sumBlack_[t] += x.black;
        sumWhite_[t] += x.white;
        for (int k = 0; k < TT_COUNT; ++k) sumTwin_[k][t] += x.twin[k];
        if (!keep) continue;
        column(S_GRAIN, t)[n_]  = (float)x.grain;
        column(S_DETAIL, t)[n_] = (float)x.detail;
        if (settled) column(S_MOTION, t)[nm_] = (float)motion_[t];
    }
    if (keep) {
        ++n_;
        if (settled) ++nm_;
    }
}

bool TileLearn::finalize(const std::string& label, TileBaseline& out)
{
    if (n_ < (uint64_t)MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[" << label << "] LEARN: tiles: only " << n_ << " frame(s) kept (need "
                  << MIN_LEARN_FRAMES_FOR_SIGMA << ")\n";
        return false;
    }
    float* work = work_.get();
    TileBaseline tb{};
    int noBand = 0;
    // Outliers too many to be glitches are the tile's own behaviour, so its band
    // comes from every frame; only a series too short for any band gets one no
    // value leaves.
    auto learn = [&](const float* v, uint64_t n, SigmaSide side, TileBand& b, int t) {
        if (learn_series(v, (size_t)n, work, side, b.mu[t], b.sigma[t]) ||
            summarize_series(v, (size_t)n, work, side, b.mu[t], b.sigma[t]))
            return;
        double sum = 0.0;
        for (uint64_t j = 0; j < n; ++j) sum += v[j];
        b.mu[t] = (n > 0) ? (float)(sum / (double)n) : 0.f;
        b.sigma[t] = kNoBand;
        ++noBand;
    };
    for (int t = 0; t < kTileCount; ++t) {
        learn(column(S_GRAIN, t), n_, kGrainSide, tb.grain, t);
        learn(column(S_DETAIL, t), n_, kDetailSide, tb.detail, t);
        learn(column(S_MOTION, t), nm_, kMotionSide, tb.motion, t);
        for (int k = 0; k < TT_COUNT; ++k)
            tb.level[k][t] = (float)(sumTwin_[k][t] / (double)seen_);
        tb.black[t]  = (float)(sumBlack_[t] / (double)seen_);
        tb.white[t]  = (float)(sumWhite_[t] / (double)seen_);
    }
    out = tb;

    if (noBand > 0)
        std::cerr << "[" << label << "] LEARN: tiles: " << noBand
                  << " series too short for a band; run LEARN longer\n";
    return true;
}

}  // namespace sai
