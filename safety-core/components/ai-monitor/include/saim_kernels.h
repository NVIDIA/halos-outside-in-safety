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

// Runs the full Canny-style edge detection pipeline:
//   Gaussian blur -> Scharr gradient -> NMS + threshold -> hysteresis -> count.
// d_blurred is reused as the hysteresis output buffer.
void launch_edge_detection(const unsigned char* d_y, int y_pitch, int W, int H,
                           void* d_blurred, void* d_grad_mag, void* d_grad_dir,
                           void* d_edges, void* d_edgeCount,
                           int canny_low_thresh, int canny_high_thresh,
                           cudaStream_t stream = 0);

// Derives per-frame quality scores from the histogram and edge count.
// In LEARN mode, accumulates running sums in learn_accum instead of scoring.
void launch_quality_scoring(const unsigned int* hist,
                            const uint32_t* d_edgeCount,
                            GpuScoringParams params,
                            FrameQualityResult* result,
                            GpuLearnAccum* learn_accum,
                            int numBins,
                            cudaStream_t stream = 0);

#endif
