/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAIM_KERNELS_H
#define SAIM_KERNELS_H

#include <cuda_runtime.h>
#include "sai_common.h"
// Block shape and the block-to-tile mapping the reduction kernels index with.
#include "tile_geometry.h"

// Pixels one Canny block covers: 32 x 8 threads, 4 adjacent pixels of a row
// each. The hysteresis counts edges per block, so init() admits only tiles that
// are whole multiples of this rectangle.
constexpr int kCannyBlockPxW = 128;
constexpr int kCannyBlockPxH = 8;

// Slots of GpuTileAccum::luma and ::chroma. L8 is the 8-neighbour Laplacian
// (frame lap_var), L4 = |4p - its 4 neighbours| with 0 on the plane's outer ring.
enum LumaSlot   { LU_L8, LU_L8SQ, LU_Y, LU_DARK, LU_BRIGHT, LU_L4, LU_MOTION, LU_COUNT };
enum ChromaSlot { CH_CB, CH_CB2, CH_CR, CH_CR2, CH_S, CH_S2, CH_CB_ABS, CH_CR_ABS, CH_L4, CH_COUNT };

// Every per-frame accumulator, cleared with one memset and reduced by
// compute_quality_kernel into FrameQualityResult.
struct GpuTileAccum {
    unsigned int       hist[kSwHistogramBinCount];    // frame luma bins
    unsigned int       edges[kTileCount];             // Canny edge pixels
    unsigned long long luma[kTileCount][LU_COUNT];    // LU_L8 holds a signed sum's bits
    unsigned long long chroma[kTileCount][CH_COUNT];
    double             detail[kTileCount];            // sum of the fine band squared
};

// Uploads the band's two Gaussian kernels (sampled, radius ceil(4 sigma)).
// Once per process is enough; returns false on a CUDA error.
bool upload_band_weights();

// Chroma sums per tile into acc->chroma; cw = W/2 pairs per row, ch = H/2 rows.
void launch_chroma_stats(const unsigned char* d_uv, int uv_pitch, int cw, int ch,
                         GpuTileAccum* acc, cudaStream_t stream = 0);

// One pass over Y: the frame luma histogram and acc->luma per tile. d_prev
// (pitch W) holds the previous frame, unread when first != 0 (motion is then
// 0); this frame is written to d_cur (pitch W), a separate plane. L8
// neighbours clamp at the frame edge. Black and white are counted at lines.
void launch_luma_stats(const unsigned char* d_y, int y_pitch, int W, int H,
                       const unsigned char* d_prev, unsigned char* d_cur, int first,
                       LumaLines lines, GpuTileAccum* acc, cudaStream_t stream = 0);

// Mirrored rows above and below the band scratch planes: the sigma 4 blur's radius.
constexpr int kBandPad = 16;
// Rows of one column the band's column step filters per work item, so init()
// admits only tile heights that are whole multiples of it.
constexpr int kBandColRows = 9;

// Fine band G(1) - G(4) of Y, mirror border, squared and summed per tile into
// acc->detail. d_a and d_b are W x (H + 2 kBandPad) float scratch planes.
void launch_fine_band(const unsigned char* d_y, int y_pitch, int W, int H,
                      float* d_a, float* d_b, GpuTileAccum* acc, cudaStream_t stream = 0);

// Runs the full Canny edge detection pipeline:
//   Gaussian blur -> Scharr gradient -> NMS + threshold -> hysteresis,
// adding each tile's edge pixels to acc->edges (cleared with the rest of acc).
// The magnitude is the L2 norm truncated to an integer. From the NMS
// on, the rules are OpenCV's Canny(dx, dy, low, high, L2gradient = true),
// applied to that truncated magnitude. d_labels (W*H uint32) is hysteresis
// scratch. W must be a multiple of 4, the W*H buffers must come from cudaMalloc
// (4 pixels per vector load and store), and the tiles whole multiples of
// kCannyBlockPxW x kCannyBlockPxH.
void launch_edge_detection(const unsigned char* d_y, int y_pitch, int W, int H,
                           void* d_blurred, void* d_grad_mag, void* d_grad_dir,
                           void* d_edges, void* d_labels,
                           int canny_low_thresh, int canny_high_thresh,
                           GpuTileAccum* acc, cudaStream_t stream = 0);

// The 12 whole-frame metrics, plus result->tiles, from the accumulators.
// total_pixels is W * H, chroma_pixels (W/2) * (H/2).
void launch_quality_metrics(const GpuTileAccum* acc, int total_pixels, int chroma_pixels,
                            FrameQualityResult* result, cudaStream_t stream = 0);

#endif
