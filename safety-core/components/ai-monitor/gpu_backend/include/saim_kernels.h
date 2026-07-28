/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAIM_KERNELS_H
#define SAIM_KERNELS_H

#include <cstdint>
#include <cuda_runtime.h>
#include "sai_common.h"

// Computes a 256-bin luminance histogram using shared-memory atomics.
// Clears d_hist before accumulating. Used when NVDEC HW histogram is unavailable.
void launch_histogram_fallback(const unsigned char* src, int pitch,
                               int W, int H, unsigned int* d_hist,
                               cudaStream_t stream = 0);

// Converts NVDEC's 64-bit histogram counters to 32-bit.
void launch_convert_hist64(const uint64_t* src, unsigned int* dst, int n,
                           cudaStream_t stream = 0);

// Reduces the interleaved NV12 UV plane to six uint64 sums:
//   [Sum_cb, Sum_cb2, Sum_cr, Sum_cr2, Sum_s, Sum_s2]; s = alpha-max-plus-beta-min
//   approximation of sqrt(dCb^2 + dCr^2).
// Zeroes d_chroma_sums before accumulating. cw = W/2 pairs/row, ch = H/2 rows.
void launch_chroma_stats(const unsigned char* d_uv, int uv_pitch,
                         int cw, int ch, unsigned long long* d_chroma_sums,
                         cudaStream_t stream = 0);

// Reduces the raw Y plane to two Laplacian sums: [sum(L), sum(L^2)] where L is
// the 8-connected 3x3 Laplacian (8 neighbors - 8*center) over the full frame
// (rows [0,H), cols [0,W)). Slot 0 holds sum(L) as signed bits in an unsigned slot.
// Zeroes d_lap_sums before accumulating. Consumer divides by N = W*H.
void launch_laplacian_stats(const unsigned char* d_y, int y_pitch, int W, int H,
                            unsigned long long* d_lap_sums,
                            cudaStream_t stream = 0);

// Runs the full Canny-style edge detection pipeline:
//   Gaussian blur -> Scharr gradient -> NMS + threshold -> hysteresis -> count.
// d_blurred is reused as the hysteresis output buffer.
void launch_edge_detection(const unsigned char* d_y, int y_pitch, int W, int H,
                           void* d_blurred, void* d_grad_mag, void* d_grad_dir,
                           void* d_edges, void* d_edgeCount,
                           int canny_low_thresh, int canny_high_thresh,
                           cudaStream_t stream = 0);

// Derives per-frame quality scores from the histogram, edge count, and the six
// chroma sums (d_chroma_sums from launch_chroma_stats). In LEARN mode, updates
// Welford's online (mu, M2) state per luma and chroma metric in learn_accum
// instead of scoring; the host derives the unbiased sample sigma =
// sqrt(M2 / (N - 1)). In ACTIVE mode, overall = min(y_score, uv_score) when a
// chroma baseline is present (uv_score = weighted avg of the Cb/Cr/saturation
// sub-scores, each itself the worst of its mean/variance k-sigma score), else
// y_score only.
void launch_quality_scoring(const unsigned int* hist,
                            const uint32_t* d_edgeCount,
                            const unsigned long long* d_chroma_sums,
                            const unsigned long long* d_lap_sums,
                            GpuScoringParams params,
                            FrameQualityResult* result,
                            GpuLearnAccum* learn_accum,
                            int numBins,
                            cudaStream_t stream = 0);

#endif
