/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "saim_kernels.h"

#include <cuda_runtime.h>
#include <cstdio>

// Edge detection kernel tile dimensions (threads per block in x and y).
#define EDGE_TILE_W   16
#define EDGE_TILE_H   16
// Radius of the 5x5 Gaussian blur pre-filter.
#define BLUR_RADIUS    2
// Extra border pixels loaded into shared memory so the blur window doesn't
// read out-of-tile. SMEM dimensions = tile + 2 * halo on each side.
#define BLUR_HALO     (BLUR_RADIUS)
#define BLUR_SMEM_W   (EDGE_TILE_W + 2 * BLUR_HALO)
#define BLUR_SMEM_H   (EDGE_TILE_H + 2 * BLUR_HALO)

// Scharr 3x3 gradient kernels (horizontal and vertical) in GPU constant memory.
// Higher weight on centre row/col compared to Sobel, giving better rotational symmetry.
__constant__ int c_scharrX[3][3] = {{ -3,  0,  3},
                                    {-10,  0, 10},
                                    { -3,  0,  3}};

__constant__ int c_scharrY[3][3] = {{ -3, -10, -3},
                                    {  0,   0,  0},
                                    {  3,  10,  3}};

// 5x5 Gaussian kernel (un-normalized, sum = 256) stored in constant memory.
// Division by 256 is done via right-shift (>> 8) in the blur kernel.
__constant__ int c_gauss5[5][5] = {
    { 1,  4,  6,  4,  1},
    { 4, 16, 24, 16,  4},
    { 6, 24, 36, 24,  6},
    { 4, 16, 24, 16,  4},
    { 1,  4,  6,  4,  1}
};

// Linearly maps `v` from the [bad, good] range to [0, 100], clamped.
// Used to convert a raw metric into a 0-100 quality score.
__device__ float gpu_ramp(float v, float bad, float good) {
    float range = good - bad;
    if (fabsf(range) < 1e-9f)
        return (fabsf(v - good) < 1e-9f) ? 100.0f : 0.0f;
    float t = (v - bad) / range;
    return fminf(fmaxf(t * 100.f, 0.f), 100.f);
}

/*
 * Computes a 256-bin luminance histogram using shared-memory atomics.
 * Used as a fallback when the NVDEC hardware histogram is unavailable.
 *
 * @param src     Device pointer to the Y plane (NV12).
 * @param pitch   Row pitch in bytes of the source surface.
 * @param W, H    Frame dimensions in pixels.
 * @param d_hist  Output: 256-element histogram (atomically accumulated across blocks).
 */
__global__ void histogram_fallback_kernel(
    const unsigned char* __restrict__ src, int pitch,
    int W, int H,
    unsigned int* __restrict__ d_hist)
{
    __shared__ unsigned int sh[256];
    int tid = threadIdx.x;
    for (int i = tid; i < 256; i += blockDim.x) sh[i] = 0;
    __syncthreads();

    int total = W * H;
    int gid = blockIdx.x * blockDim.x + tid;
    int stride = blockDim.x * gridDim.x;
    for (int i = gid; i < total; i += stride) {
        int x = i % W;
        int y = i / W;
        atomicAdd(&sh[src[y * pitch + x]], 1);
    }
    __syncthreads();

    for (int i = tid; i < 256; i += blockDim.x)
        atomicAdd(&d_hist[i], sh[i]);
}

// Converts NVDEC's 64-bit histogram counters to 32-bit, truncating bins beyond `n` to 0.
__global__ void convert_hist64_kernel(
    const uint64_t* __restrict__ src,
    unsigned int*   __restrict__ dst,
    int n)
{
    int gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid < n)
        dst[gid] = (unsigned int)src[gid];
}

/*
 * Applies a 5x5 Gaussian blur to the Y plane using shared-memory tiling.
 * Each thread block loads a tile + halo border into shared memory, then
 * convolves with the c_gauss5 kernel (sum=256, normalized via >>8).
 *
 * @param d_yplane  Source Y plane (pitch-linear, may differ from W).
 * @param blurred   Output: blurred image (W-stride, no padding).
 * @param W, H      Frame dimensions.
 * @param pitch     Source row pitch in bytes.
 */
__global__ void k_gaussBlurY(const uint8_t* __restrict__ d_yplane,
                              uint8_t*       __restrict__ blurred,
                              int W, int H, int pitch)
{
    __shared__ uint8_t smem[BLUR_SMEM_H][BLUR_SMEM_W];

    int tx  = threadIdx.x;
    int ty  = threadIdx.y;
    int col = blockIdx.x * EDGE_TILE_W + tx;
    int row = blockIdx.y * EDGE_TILE_H + ty;

    auto loadY = [&](int r, int c, int sr, int sc) {
        int cr = min(max(r, 0), H - 1);
        int cc = min(max(c, 0), W - 1);
        smem[sr][sc] = d_yplane[cr * pitch + cc];
    };

    loadY(row, col, ty + BLUR_HALO, tx + BLUR_HALO);

    if (tx < BLUR_HALO) {
        loadY(row, col - BLUR_HALO,   ty + BLUR_HALO, tx);
        loadY(row, col + EDGE_TILE_W, ty + BLUR_HALO, tx + EDGE_TILE_W + BLUR_HALO);
    }
    if (ty < BLUR_HALO) {
        loadY(row - BLUR_HALO,   col, ty,                            tx + BLUR_HALO);
        loadY(row + EDGE_TILE_H, col, ty + EDGE_TILE_H + BLUR_HALO, tx + BLUR_HALO);
    }
    if (tx < BLUR_HALO && ty < BLUR_HALO) {
        loadY(row - BLUR_HALO,   col - BLUR_HALO,   ty,                            tx);
        loadY(row - BLUR_HALO,   col + EDGE_TILE_W, ty,                            tx + EDGE_TILE_W + BLUR_HALO);
        loadY(row + EDGE_TILE_H, col - BLUR_HALO,   ty + EDGE_TILE_H + BLUR_HALO, tx);
        loadY(row + EDGE_TILE_H, col + EDGE_TILE_W, ty + EDGE_TILE_H + BLUR_HALO, tx + EDGE_TILE_W + BLUR_HALO);
    }
    __syncthreads();

    if (row >= H || col >= W) return;

    int sum = 0;
    #pragma unroll
    for (int ky = 0; ky < 5; ++ky)
        #pragma unroll
        for (int kx = 0; kx < 5; ++kx)
            sum += c_gauss5[ky][kx] * (int)smem[ty + ky][tx + kx];

    blurred[row * W + col] = (uint8_t)(sum >> 8);
}

/*
 * Computes gradient magnitude and quantised direction using Scharr operators.
 * Magnitude = |Gx| + |Gy| (L1 approximation).
 * Direction is quantised to 4 orientations (0=horiz, 1=diag45, 2=vert, 3=diag135)
 * for use by non-maximum suppression.
 *
 * @param blurred  Gaussian-blurred Y plane (W-stride).
 * @param mag_buf  Output: gradient magnitude per pixel (int16).
 * @param dir_buf  Output: quantised direction per pixel (0-3).
 */
__global__ void k_scharrGradient(const uint8_t* __restrict__ blurred,
                                  int16_t*       __restrict__ mag_buf,
                                  uint8_t*       __restrict__ dir_buf,
                                  int W, int H)
{
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (col >= W || row >= H) return;

    int idx = row * W + col;

    if (col < 1 || col >= W - 1 || row < 1 || row >= H - 1) {
        mag_buf[idx] = 0;
        dir_buf[idx] = 0;
        return;
    }

    int gx = 0, gy = 0;
    #pragma unroll
    for (int ky = 0; ky < 3; ++ky)
        #pragma unroll
        for (int kx = 0; kx < 3; ++kx) {
            int v = (int)blurred[(row - 1 + ky) * W + (col - 1 + kx)];
            gx += c_scharrX[ky][kx] * v;
            gy += c_scharrY[ky][kx] * v;
        }

    int m = abs(gx) + abs(gy);
    mag_buf[idx] = (int16_t)min(m, 32767);

    int ax = abs(gx), ay = abs(gy);
    if      (ay <= (ax >> 2))  dir_buf[idx] = 0;
    else if (ax <= (ay >> 2))  dir_buf[idx] = 2;
    else                       dir_buf[idx] = (gx * gy > 0) ? 1 : 3;
}

/*
 * Non-maximum suppression + double thresholding (Canny step 2-3).
 * Suppresses pixels that aren't local maxima along the gradient direction.
 * Outputs 255 (strong edge), 128 (weak edge), or 0 (suppressed).
 */
__global__ void k_nmsThreshold(const int16_t* __restrict__ mag_buf,
                                const uint8_t* __restrict__ dir_buf,
                                uint8_t*       __restrict__ edges,
                                int W, int H,
                                int low_thresh, int high_thresh)
{
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (col >= W || row >= H) return;

    int idx = row * W + col;

    if (col < 1 || col >= W - 1 || row < 1 || row >= H - 1) {
        edges[idx] = 0;
        return;
    }

    int m = (int)mag_buf[idx];
    if (m < low_thresh) { edges[idx] = 0; return; }

    int m1, m2;
    switch (dir_buf[idx]) {
        case 0:  m1 = mag_buf[idx + 1];              m2 = mag_buf[idx - 1];              break;
        case 1:  m1 = mag_buf[(row-1)*W + col + 1];  m2 = mag_buf[(row+1)*W + col - 1];  break;
        case 2:  m1 = mag_buf[(row-1)*W + col];      m2 = mag_buf[(row+1)*W + col];      break;
        default: m1 = mag_buf[(row-1)*W + col - 1];  m2 = mag_buf[(row+1)*W + col + 1];  break;
    }

    if (m >= m1 && m >= m2)
        edges[idx] = (m >= high_thresh) ? 255 : 128;
    else
        edges[idx] = 0;
}

// Hysteresis edge linking (Canny step 4): promotes weak edges (128) to strong (255)
// if any 8-connected neighbour is a strong edge, otherwise suppresses them.
__global__ void k_hysteresis(const uint8_t* __restrict__ nms_edges,
                              uint8_t*       __restrict__ final_edges,
                              int W, int H)
{
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (col >= W || row >= H) return;

    int idx = row * W + col;
    uint8_t v = nms_edges[idx];

    if (v == 255) { final_edges[idx] = 255; return; }
    if (v != 128) { final_edges[idx] = 0;   return; }

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int ny = row + dy, nx = col + dx;
            if (ny >= 0 && ny < H && nx >= 0 && nx < W) {
                if (nms_edges[ny * W + nx] == 255) {
                    final_edges[idx] = 255;
                    return;
                }
            }
        }
    }
    final_edges[idx] = 0;
}

// Parallel reduction that counts pixels with value 255 (strong edges).
// Uses shared-memory tree reduction within each block, then global atomicAdd.
__global__ void k_countEdges(const uint8_t* __restrict__ edges,
                              uint32_t*      __restrict__ d_edgeCount,
                              int total_pixels)
{
    __shared__ uint32_t sdata[256];

    int tid    = threadIdx.x;
    int gid    = blockIdx.x * blockDim.x + tid;
    int stride = gridDim.x  * blockDim.x;

    uint32_t count = 0;
    for (int i = gid; i < total_pixels; i += stride)
        count += (edges[i] == 255) ? 1u : 0u;

    sdata[tid] = count;
    __syncthreads();

    if (blockDim.x >= 256) { if (tid < 128) sdata[tid] += sdata[tid + 128]; __syncthreads(); }
    if (blockDim.x >= 128) { if (tid <  64) sdata[tid] += sdata[tid +  64]; __syncthreads(); }
    if (tid < 32) {
        uint32_t val = sdata[tid];
        if (blockDim.x >= 64) val += sdata[tid + 32];
        val += __shfl_down_sync(0xFFFFFFFF, val, 16);
        val += __shfl_down_sync(0xFFFFFFFF, val, 8);
        val += __shfl_down_sync(0xFFFFFFFF, val, 4);
        val += __shfl_down_sync(0xFFFFFFFF, val, 2);
        val += __shfl_down_sync(0xFFFFFFFF, val, 1);
        if (tid == 0) atomicAdd(d_edgeCount, val);
    }
}

/*
 * Single-block kernel (<<<1, 256>>>) that derives quality scores from the histogram
 * and edge count. Each thread handles one histogram bin; a parallel reduction
 * computes mean and variance, then thread 0 calculates all scores.
 *
 * In LEARN mode: accumulates running sums in learn_accum instead of scoring.
 * In ACTIVE mode: compares current metrics against baseline (if available)
 *   or uses hardcoded fallback thresholds, producing per-metric and overall scores.
 *
 * @param hist         N-bin luminance histogram (numBins elements).
 * @param d_edgeCount  Total strong-edge pixel count from k_countEdges.
 * @param params       Scoring weights, baseline values, mode flags.
 * @param result       Output: per-frame quality scores.
 * @param learn_accum  In/Out: running sums (LEARN mode only, may be nullptr).
 */
__global__ void compute_quality_kernel(
    const unsigned int* __restrict__ hist,
    const uint32_t*     __restrict__ d_edgeCount,
    GpuScoringParams params,
    FrameQualityResult* __restrict__ result,
    GpuLearnAccum*      __restrict__ learn_accum,
    int numBins)
{
    __shared__ double sh_sum[256];
    __shared__ double sh_sq[256];
    __shared__ uint64_t sh_total[256];

    int tid = threadIdx.x;

    double local_sum = 0.0;
    double local_sq  = 0.0;
    uint64_t local_total = 0;

    for (int i = tid; i < numBins; i += blockDim.x) {
        uint64_t count = hist[i];
        local_sum   += (double)i * count;
        local_sq    += (double)i * (double)i * count;
        local_total += count;
    }

    sh_sum[tid]   = local_sum;
    sh_sq[tid]    = local_sq;
    sh_total[tid] = local_total;
    __syncthreads();

    for (int s = 128; s > 0; s >>= 1) {
        if (tid < s) {
            sh_sum[tid]   += sh_sum[tid + s];
            sh_sq[tid]    += sh_sq[tid + s];
            sh_total[tid] += sh_total[tid + s];
        }
        __syncthreads();
    }

    if (tid == 0) {
        uint64_t total = sh_total[0];
        float mean, var;
        if (total == 0) { mean = (float)(numBins / 2); var = 0.f; }
        else {
            mean = (float)(sh_sum[0] / total);
            var  = (float)(sh_sq[0] / total - (double)mean * mean);
        }

        float std_dev = sqrtf(fmaxf(var, 0.f));
        float rms_contrast = (mean > 1e-6f) ? (std_dev / mean) : 0.f;

        float edge_density = (params.total_pixels > 0)
            ? (float)(*d_edgeCount) / (float)params.total_pixels
            : 0.f;

        if (params.is_learn_mode && learn_accum) {
            learn_accum->hist_mean_sum    += mean;
            learn_accum->hist_var_sum     += var;
            learn_accum->rms_contrast_sum += rms_contrast;
            learn_accum->edge_density_sum += edge_density;
            learn_accum->frame_count++;

            result->histogram_score    = 0.f;
            result->contrast_score     = 0.f;
            result->edge_density_score = 0.f;
            result->overall_confidence = 0.f;
        } else {
            float hist_score, ctr_score, edge_score;

            if (params.has_baseline) {
                float mean_dev = fabsf(mean - params.baseline_hist_mean);
                float mean_tol = params.baseline_hist_mean * params.baseline_mean_margin;
                float bright = gpu_ramp(mean_dev, 2.0f * mean_tol, 0.0f);

                float var_ratio = var / fmaxf(params.baseline_hist_var, 1.0f);
                float var_dev = fabsf(var_ratio - 1.0f);
                float vsc = gpu_ramp(var_dev, params.baseline_var_margin, 0.0f);

                hist_score = fminf(bright, vsc);

                float ctr_ratio = rms_contrast / fmaxf(params.baseline_rms_contrast, 0.001f);
                float ctr_dev = fabsf(ctr_ratio - 1.0f);
                ctr_score = gpu_ramp(ctr_dev, params.baseline_contrast_margin, 0.0f);

                float edge_ratio = edge_density / fmaxf(params.baseline_edge_density, 0.001f);
                float edge_dev = fabsf(edge_ratio - 1.0f);
                edge_score = gpu_ramp(edge_dev, params.baseline_edge_margin, 0.0f);
            } else {
                float bright = (mean < 128.f)
                    ? gpu_ramp(mean, 15.f, 60.f)
                    : gpu_ramp(mean, 240.f, 200.f);
                float vsc = gpu_ramp(var, 200.f, 1500.f);
                hist_score = fminf(bright, vsc);

                ctr_score = gpu_ramp(rms_contrast, 0.05f, 0.3f);

                float low  = gpu_ramp(edge_density, 0.005f, 0.03f);
                float high = gpu_ramp(edge_density, 0.30f,  0.15f);
                edge_score = fminf(low, high);
            }

            result->histogram_score    = hist_score;
            result->contrast_score     = ctr_score;
            result->edge_density_score = edge_score;
            result->overall_confidence =
                params.w_histogram * hist_score +
                params.w_contrast  * ctr_score  +
                params.w_edge      * edge_score;
        }
    }
}

// ---- Wrapper functions (called from .cpp files) ----

void launch_histogram_fallback(const unsigned char* src, int pitch,
                               int W, int H, unsigned int* d_hist,
                               cudaStream_t stream)
{
    cudaError_t err = cudaMemsetAsync(d_hist, 0, 256 * sizeof(unsigned int), stream);
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] cudaMemsetAsync hist failed: %s\n", cudaGetErrorString(err));
    histogram_fallback_kernel<<<128, 256, 0, stream>>>(src, pitch, W, H, d_hist);
    err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] histogram_fallback launch failed: %s\n", cudaGetErrorString(err));
}

void launch_convert_hist64(const uint64_t* src, unsigned int* dst, int n,
                           cudaStream_t stream)
{
    int blocks = (n + 255) / 256;
    convert_hist64_kernel<<<blocks, 256, 0, stream>>>(src, dst, n);
    cudaError_t err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] convert_hist64 launch failed: %s\n", cudaGetErrorString(err));
}

void launch_edge_detection(const unsigned char* d_y, int y_pitch, int W, int H,
                           void* d_blurred, void* d_grad_mag, void* d_grad_dir,
                           void* d_edges, void* d_edgeCount,
                           int canny_low_thresh, int canny_high_thresh,
                           cudaStream_t stream)
{
    int total = W * H;

    dim3 block(EDGE_TILE_W, EDGE_TILE_H);
    dim3 grid((W + EDGE_TILE_W - 1) / EDGE_TILE_W,
              (H + EDGE_TILE_H - 1) / EDGE_TILE_H);

    cudaError_t err;
    {
        NVTX_RANGE("GaussianBlur", 0xFFFFAA00);
        k_gaussBlurY<<<grid, block, 0, stream>>>(
            d_y, (uint8_t*)d_blurred, W, H, y_pitch);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] gaussBlur launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("ScharrGradient", 0xFFFFCC00);
        k_scharrGradient<<<grid, block, 0, stream>>>(
            (const uint8_t*)d_blurred,
            (int16_t*)d_grad_mag, (uint8_t*)d_grad_dir, W, H);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] scharrGradient launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("NMSThreshold", 0xFFFFEE00);
        k_nmsThreshold<<<grid, block, 0, stream>>>(
            (const int16_t*)d_grad_mag, (const uint8_t*)d_grad_dir,
            (uint8_t*)d_edges, W, H,
            canny_low_thresh, canny_high_thresh);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] nmsThreshold launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("Hysteresis", 0xFFEEFF00);
        k_hysteresis<<<grid, block, 0, stream>>>(
            (const uint8_t*)d_edges, (uint8_t*)d_blurred, W, H);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] hysteresis launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("CountEdges", 0xFFCCFF00);
        err = cudaMemsetAsync(d_edgeCount, 0, sizeof(uint32_t), stream);
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] cudaMemsetAsync edgeCount failed: %s\n", cudaGetErrorString(err));
        int count_blocks = (total + 255) / 256;
        if (count_blocks > 256) count_blocks = 256;
        k_countEdges<<<count_blocks, 256, 0, stream>>>(
            (const uint8_t*)d_blurred, (uint32_t*)d_edgeCount, total);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] countEdges launch failed: %s\n", cudaGetErrorString(err));
    }
}

void launch_quality_scoring(const unsigned int* hist,
                            const uint32_t* d_edgeCount,
                            GpuScoringParams params,
                            FrameQualityResult* result,
                            GpuLearnAccum* learn_accum,
                            int numBins,
                            cudaStream_t stream)
{
    compute_quality_kernel<<<1, 256, 0, stream>>>(
        hist, d_edgeCount, params, result, learn_accum, numBins);
    cudaError_t err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] quality_scoring launch failed: %s\n", cudaGetErrorString(err));
}
