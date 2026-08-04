/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "saim_kernels.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>

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

// Maps |x - mu| / sigma_eff to [0, 100] via gpu_ramp(z, k, 0).
// sigma_eff = max(sigma, |mu| * sigma_floor_fraction) avoids div-by-zero on
// static LEARN scenes and reins in under-estimated sigma. If both mu and sigma
// are 0, any non-zero x scores 0.
__device__ float ksigma_score(float x, float mu, float sigma,
                              float k, float sigma_floor_fraction) {
    float floor_val = fabsf(mu) * sigma_floor_fraction;
    float sigma_eff = fmaxf(sigma, floor_val);
    if (sigma_eff <= 0.f) {
        return (fabsf(x - mu) < 1e-9f) ? 100.f : 0.f;
    }
    float z = fabsf(x - mu) / sigma_eff;
    return gpu_ramp(z, k, 0.0f);
}

// Welford's online update: incorporates one new sample x into the running
// (mu, M2) state for a given metric. inv_n_new = 1 / (count after this sample).
__device__ static inline void welford_update(double& mu, double& M2,
                                             double x, double inv_n_new) {
    const double delta = x - mu;
    mu += delta * inv_n_new;
    const double delta2 = x - mu;
    M2 += delta * delta2;
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

    // Flatten the 2D block (launcher uses 32x8 = 256 threads).
    const int tid  = threadIdx.y * blockDim.x + threadIdx.x;
    const int nthr = blockDim.x * blockDim.y;
    for (int i = tid; i < 256; i += nthr) sh[i] = 0;
    __syncthreads();

    // Map each thread directly to a pixel with a 2D grid-stride. This removes
    // the per-pixel integer divide/modulo and keeps a warp's row reads
    // contiguous (the shared-bin atomics dominate, but the reads still coalesce).
    const int xstride = blockDim.x * gridDim.x;
    const int ystride = blockDim.y * gridDim.y;
    const int x0 = blockIdx.x * blockDim.x + threadIdx.x;
    const int y0 = blockIdx.y * blockDim.y + threadIdx.y;
    for (int y = y0; y < H; y += ystride) {
        const unsigned char* row = src + (size_t)y * pitch;
        for (int x = x0; x < W; x += xstride)
            atomicAdd(&sh[row[x]], 1);
    }
    __syncthreads();

    for (int i = tid; i < 256; i += nthr)
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
 * Reduces the interleaved NV12 chroma (UV) plane to six 64-bit sums used to
 * derive Cb/Cr/saturation mean and variance:
 *   d_sums = [Sum_cb, Sum_cb2, Sum_cr, Sum_cr2, Sum_s, Sum_s2]
 * with per-pixel saturation s = alpha-max-plus-beta-min approximation of
 * sqrt(dCb^2 + dCr^2). Cb is the low byte and Cr the high byte of each
 * interleaved pair, read via a coalesced uchar2 load.
 * d_sums must be zeroed before launch (the wrapper does this).
 *
 * @param uv        Device pointer to the NV12 UV plane base.
 * @param uv_pitch  Row pitch in bytes of the UV plane (same as the Y pitch).
 * @param cw, ch    Chroma-plane dimensions: cw = W/2 pairs per row, ch = H/2 rows.
 * @param d_sums    Output: 6 uint64 accumulators (atomically summed across blocks).
 */
__global__ void chroma_stats_kernel(
    const unsigned char* __restrict__ uv, int uv_pitch,
    int cw, int ch,
    unsigned long long* __restrict__ d_sums)
{
    __shared__ unsigned long long sh_cb [256];
    __shared__ unsigned long long sh_cb2[256];
    __shared__ unsigned long long sh_cr [256];
    __shared__ unsigned long long sh_cr2[256];
    __shared__ unsigned long long sh_s  [256];
    __shared__ unsigned long long sh_s2 [256];

    // Flatten the 2D block for the shared-memory reduction. The launcher uses a
    // 32x8 = 256-thread block (power of two).
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;

    unsigned long long l_cb = 0, l_cb2 = 0, l_cr = 0, l_cr2 = 0, l_s = 0, l_s2 = 0;

    // Map each thread directly to a chroma pixel with a 2D grid-stride. This
    // removes the per-pixel integer divide/modulo the old linear index needed
    // and keeps a warp's uchar2 reads contiguous (coalesced).
    const int xstride = blockDim.x * gridDim.x;
    const int ystride = blockDim.y * gridDim.y;
    const int x0 = blockIdx.x * blockDim.x + threadIdx.x;
    const int y0 = blockIdx.y * blockDim.y + threadIdx.y;

    for (int r = y0; r < ch; r += ystride) {
        const uchar2* row = (const uchar2*)(uv + (size_t)r * uv_pitch);
        for (int c = x0; c < cw; c += xstride) {
            uchar2 px = row[c];
            unsigned int cb = px.x;
            unsigned int cr = px.y;
            int dcb = abs((int)cb - 128);
            int dcr = abs((int)cr - 128);
            int mx  = max(dcb, dcr);
            int mn  = min(dcb, dcr);
            int approx = (kSatAlphaQ7 * mx + kSatBetaQ7 * mn + kSatRoundQ7) >> kSatShiftQ7;
            unsigned int s = (unsigned int)max(mx, approx);   // near-axis correction
            l_cb  += cb;
            l_cb2 += (unsigned long long)cb * cb;
            l_cr  += cr;
            l_cr2 += (unsigned long long)cr * cr;
            l_s   += s;
            l_s2  += (unsigned long long)s * s;
        }
    }

    sh_cb[tid]  = l_cb;  sh_cb2[tid] = l_cb2;
    sh_cr[tid]  = l_cr;  sh_cr2[tid] = l_cr2;
    sh_s[tid]   = l_s;   sh_s2[tid]  = l_s2;
    __syncthreads();

    for (int off = (blockDim.x * blockDim.y) >> 1; off > 0; off >>= 1) {
        if (tid < off) {
            sh_cb[tid]  += sh_cb[tid + off];  sh_cb2[tid] += sh_cb2[tid + off];
            sh_cr[tid]  += sh_cr[tid + off];  sh_cr2[tid] += sh_cr2[tid + off];
            sh_s[tid]   += sh_s[tid + off];   sh_s2[tid]  += sh_s2[tid + off];
        }
        __syncthreads();
    }

    if (tid == 0) {
        atomicAdd(&d_sums[0], sh_cb[0]);
        atomicAdd(&d_sums[1], sh_cb2[0]);
        atomicAdd(&d_sums[2], sh_cr[0]);
        atomicAdd(&d_sums[3], sh_cr2[0]);
        atomicAdd(&d_sums[4], sh_s[0]);
        atomicAdd(&d_sums[5], sh_s2[0]);
    }
}

/*
 * Reduces the raw Y plane to the two sums needed for Laplacian variance:
 *   L(x,y) = (sum of the 8 neighbors) - 8*center   (3x3 8-connected Laplacian,
 *            signed; kernel [[1,1,1],[1,-8,1],[1,1,1]] == box3x3 - 9*center)
 *   d_lap_sums[0] = sum(L), d_lap_sums[1] = sum(L^2)
 * over the full frame (rows [0,H), cols [0,W)). The consumer divides by N = W*H.
 *
 * sum(L) is signed but accumulated bit-wise into an unsigned 64-bit slot
 * the consumer reinterprets slot 0 as signed. sum(L^2) is non-negative.
 *
 * @param d_y         Y plane (pitch-linear).
 * @param pitch       Source row pitch in bytes.
 * @param W, H        Frame dimensions.
 * @param d_lap_sums  Output [2]: {sum(L) bits, sum(L^2)}, accumulated across blocks.
 */
__global__ void k_laplacianStats(const unsigned char* __restrict__ d_y, int pitch,
                                 int W, int H,
                                 unsigned long long* __restrict__ d_lap_sums)
{
    __shared__ long long          sh_l [256];
    __shared__ unsigned long long sh_l2[256];

    // Flatten the 2D block for the shared-memory reduction. The launcher uses a
    // 32x8 = 256-thread block (power of two) so the tree reduction stays valid.
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;

    long long          l_sum  = 0;
    unsigned long long l_sum2 = 0;

    // Map each thread to a full-frame pixel (cols [0,W), rows [0,H)) with a 2D
    // grid-stride for frames larger than the grid.
    const int xstride = blockDim.x * gridDim.x;
    const int ystride = blockDim.y * gridDim.y;
    const int x0 = blockIdx.x * blockDim.x + threadIdx.x;
    const int y0 = blockIdx.y * blockDim.y + threadIdx.y;

    for (int r = y0; r < H; r += ystride) {
        const int rU = max(r - 1, 0);
        const int rD = min(r + 1, H - 1);
        const unsigned char* row  = d_y + (size_t)r  * pitch;
        const unsigned char* rowU = d_y + (size_t)rU * pitch;
        const unsigned char* rowD = d_y + (size_t)rD * pitch;
        for (int c = x0; c < W; c += xstride) {
            const int cL = max(c - 1, 0);
            const int cR = min(c + 1, W - 1);
            const int lap = (int)rowU[cL] + (int)rowU[c] + (int)rowU[cR]
                          + (int)row [cL]                + (int)row [cR]
                          + (int)rowD[cL] + (int)rowD[c] + (int)rowD[cR]
                          - 8 * (int)row[c];
            l_sum  += (long long)lap;
            l_sum2 += (unsigned long long)((long long)lap * (long long)lap);
        }
    }

    sh_l[tid]  = l_sum;
    sh_l2[tid] = l_sum2;
    __syncthreads();

    for (int off = (blockDim.x * blockDim.y) >> 1; off > 0; off >>= 1) {
        if (tid < off) {
            sh_l[tid]  += sh_l[tid + off];
            sh_l2[tid] += sh_l2[tid + off];
        }
        __syncthreads();
    }

    if (tid == 0) {
        atomicAdd(&d_lap_sums[0], (unsigned long long)sh_l[0]);
        atomicAdd(&d_lap_sums[1], sh_l2[0]);
    }
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
 * In LEARN mode: updates Welford's online (mu, M2) state per metric in
 *   learn_accum instead of scoring; the host derives the unbiased sample
 *   sigma = sqrt(M2 / (N - 1)).
 * In ACTIVE mode: compares current metrics against baseline (if available)
 *   or uses hardcoded fallback thresholds, producing per-metric and overall scores.
 *
 * @param hist          N-bin luminance histogram (numBins elements).
 * @param d_edgeCount   Total strong-edge pixel count from k_countEdges.
 * @param d_chroma_sums 6 uint64 chroma sums from chroma_stats_kernel
 *                      [Sum_cb, Sum_cb2, Sum_cr, Sum_cr2, Sum_s, Sum_s2].
 * @param d_lap_sums    2 uint64 Laplacian sums [sum(L) bits, sum(L^2)] from
 *                      k_laplacianStats; slot 0 is reinterpreted as signed.
 * @param params        Scoring weights, baseline values, mode flags.
 * @param result        Output: per-frame quality scores.
 * @param learn_accum   In/Out: Welford (mu, M2) state per metric and frame_count
 *                      (LEARN mode only, may be nullptr).
 */
__global__ void compute_quality_kernel(
    const unsigned int* __restrict__ hist,
    const uint32_t*     __restrict__ d_edgeCount,
    const unsigned long long* __restrict__ d_chroma_sums,
    const unsigned long long* __restrict__ d_lap_sums,
    GpuScoringParams params,
    FrameQualityResult* __restrict__ result,
    GpuLearnAccum*      __restrict__ learn_accum,
    int numBins)
{
    __shared__ double sh_sum[256];
    __shared__ double sh_sq[256];
    __shared__ uint64_t sh_total[256];
    __shared__ double sh_clogc[256];

    int tid = threadIdx.x;

    double local_sum = 0.0;
    double local_sq  = 0.0;
    uint64_t local_total = 0;
    double local_clogc = 0.0;

    for (int i = tid; i < numBins; i += blockDim.x) {
        uint64_t count = hist[i];
        local_sum   += (double)i * count;
        local_sq    += (double)i * (double)i * count;
        local_total += count;
        local_clogc += (count > 0) ? (double)count * log2((double)count) : 0.0;
    }

    sh_sum[tid]   = local_sum;
    sh_sq[tid]    = local_sq;
    sh_total[tid] = local_total;
    sh_clogc[tid] = local_clogc;
    __syncthreads();

    for (int s = 128; s > 0; s >>= 1) {
        if (tid < s) {
            sh_sum[tid]   += sh_sum[tid + s];
            sh_sq[tid]    += sh_sq[tid + s];
            sh_total[tid] += sh_total[tid + s];
            sh_clogc[tid] += sh_clogc[tid + s];
        }
        __syncthreads();
    }

    if (tid == 0) {
        uint64_t total = sh_total[0];
        float mean, var;
        if (total == 0) { mean = (float)(numBins / 2); var = 0.f; }
        else {
            double mean_d = sh_sum[0] / (double)total;
            mean = (float)mean_d;
            var  = (total > 1)
                 ? fmaxf((float)((sh_sq[0] - (double)total * mean_d * mean_d)
                                 / (double)(total - 1)), 0.f)
                 : 0.f;
        }

        float std_dev = sqrtf(var);
        float rms_contrast = (mean > 1e-6f) ? (std_dev / mean) : 0.f;

        // Histogram Shannon entropy (bits) via H = log2(N) - (1/N)*sum(c*log2 c).
        // Clamp a tiny negative produced by floating-point cancellation to 0.
        float entropy = 0.f;
        if (total > 0) {
            entropy = (float)(log2((double)total) - sh_clogc[0] / (double)total);
            if (entropy < 0.f) entropy = 0.f;
        }

        // Laplacian variance (sharpness) on raw Y; Bessel-corrected to match chroma var.
        float lap_var = 0.f;
        {
            long long sumL;
            memcpy(&sumL, &d_lap_sums[0], sizeof(sumL));
            const unsigned long long sumL2 = d_lap_sums[1];
            const uint64_t Nl = (params.lap_pixels > 0) ? (uint64_t)params.lap_pixels : 0ULL;
            if (Nl > 1ULL) {
                const double Nd    = (double)Nl;
                const double meanL = (double)sumL / Nd;
                lap_var = fmaxf((float)(((double)sumL2 - Nd * meanL * meanL)
                                        / (Nd - 1.0)), 0.f);
            }
        }

        float edge_density = (params.total_pixels > 0)
            ? (float)(*d_edgeCount) / (float)params.total_pixels
            : 0.f;

        const uint32_t chroma_count = (uint32_t)params.chroma_pixels;
        float cb_mean = 0.f, cb_var = 0.f;
        float cr_mean = 0.f, cr_var = 0.f;
        float sat_mean = 0.f, sat_var = 0.f;
        if (chroma_count > 1u) {
            const double Nc      = (double)chroma_count;
            const double inv_div = 1.0 / (Nc - 1.0);
            const double s_cb  = (double)d_chroma_sums[0];
            const double s_cb2 = (double)d_chroma_sums[1];
            const double s_cr  = (double)d_chroma_sums[2];
            const double s_cr2 = (double)d_chroma_sums[3];
            const double s_s   = (double)d_chroma_sums[4];
            const double s_s2  = (double)d_chroma_sums[5];
            const double cb_mean_d  = s_cb / Nc;
            const double cr_mean_d  = s_cr / Nc;
            const double sat_mean_d = s_s  / Nc;
            cb_mean  = (float)cb_mean_d;
            cr_mean  = (float)cr_mean_d;
            sat_mean = (float)sat_mean_d;
            cb_var  = fmaxf((float)((s_cb2 - Nc * cb_mean_d  * cb_mean_d)  * inv_div), 0.f);
            cr_var  = fmaxf((float)((s_cr2 - Nc * cr_mean_d  * cr_mean_d)  * inv_div), 0.f);
            sat_var = fmaxf((float)((s_s2  - Nc * sat_mean_d * sat_mean_d) * inv_div), 0.f);
        }

        if (params.is_learn_mode && learn_accum) {
            // Welford's algorithm: maintain (mu, M2) per metric so that
            // the host can read mu directly and derive the unbiased sample
            // sigma = sqrt(M2 / (N - 1)).
            const double d_mean = (double)mean;
            const double d_var  = (double)var;
            const double d_ctr  = (double)rms_contrast;
            const double d_edge = (double)edge_density;
            const double d_ent  = (double)entropy;
            const double d_lap  = (double)lap_var;

            // Increment frame_count first; Welford uses the post-increment N.
            const uint64_t n_new = ++learn_accum->frame_count;
            const double inv_n_new = 1.0 / (double)n_new;

            welford_update(learn_accum->hist_mean_mu,    learn_accum->hist_mean_M2,
                           d_mean, inv_n_new);
            welford_update(learn_accum->hist_var_mu,     learn_accum->hist_var_M2,
                           d_var,  inv_n_new);
            welford_update(learn_accum->rms_contrast_mu, learn_accum->rms_contrast_M2,
                           d_ctr,  inv_n_new);
            welford_update(learn_accum->edge_density_mu, learn_accum->edge_density_M2,
                           d_edge, inv_n_new);
            welford_update(learn_accum->entropy_mu,      learn_accum->entropy_M2,
                           d_ent,  inv_n_new);
            welford_update(learn_accum->lap_var_mu,      learn_accum->lap_var_M2,
                           d_lap,  inv_n_new);

            welford_update(learn_accum->cb_mean_mu,  learn_accum->cb_mean_M2,  (double)cb_mean,  inv_n_new);
            welford_update(learn_accum->cb_var_mu,   learn_accum->cb_var_M2,   (double)cb_var,   inv_n_new);
            welford_update(learn_accum->cr_mean_mu,  learn_accum->cr_mean_M2,  (double)cr_mean,  inv_n_new);
            welford_update(learn_accum->cr_var_mu,   learn_accum->cr_var_M2,   (double)cr_var,   inv_n_new);
            welford_update(learn_accum->sat_mean_mu, learn_accum->sat_mean_M2, (double)sat_mean, inv_n_new);
            welford_update(learn_accum->sat_var_mu,  learn_accum->sat_var_M2,  (double)sat_var,  inv_n_new);

            result->histogram_score    = 0.f;
            result->contrast_score     = 0.f;
            result->edge_density_score = 0.f;
            result->entropy_score      = 0.f;
            result->laplacian_score    = 0.f;
            result->cb_score           = 0.f;
            result->cr_score           = 0.f;
            result->sat_score          = 0.f;
            result->uv_score           = 0.f;
            result->overall_confidence = 0.f;
        } else {
            float hist_score, ctr_score, edge_score, ent_score, lap_score;

            if (params.has_baseline) {
                float bright = ksigma_score(mean, params.baseline_hist_mean,
                                            params.baseline_hist_mean_std,
                                            params.k, params.sigma_floor_fraction);
                float vsc    = ksigma_score(var,  params.baseline_hist_var,
                                            params.baseline_hist_var_std,
                                            params.k, params.sigma_floor_fraction);
                hist_score = fminf(bright, vsc);

                ctr_score  = ksigma_score(rms_contrast,
                                          params.baseline_rms_contrast,
                                          params.baseline_rms_contrast_std,
                                          params.k, params.sigma_floor_fraction);
                edge_score = ksigma_score(edge_density,
                                          params.baseline_edge_density,
                                          params.baseline_edge_density_std,
                                          params.k, params.sigma_floor_fraction);
                ent_score  = ksigma_score(entropy,
                                          params.baseline_entropy,
                                          params.baseline_entropy_std,
                                          params.k, params.sigma_floor_fraction);
                lap_score  = ksigma_score(lap_var,
                                          params.baseline_lap_var,
                                          params.baseline_lap_var_std,
                                          params.k, params.sigma_floor_fraction);
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

                ent_score = gpu_ramp(entropy, 2.0f, 6.0f);
                lap_score = gpu_ramp(lap_var, 30.0f, 300.0f);
            }

            if (!isfinite(hist_score)) hist_score = 0.f;
            if (!isfinite(ctr_score))  ctr_score  = 0.f;
            if (!isfinite(edge_score)) edge_score = 0.f;
            if (!isfinite(ent_score))  ent_score  = 0.f;
            if (!isfinite(lap_score))  lap_score  = 0.f;

            result->histogram_score    = hist_score;
            result->contrast_score     = ctr_score;
            result->edge_density_score = edge_score;
            result->entropy_score      = ent_score;
            result->laplacian_score    = lap_score;

            const float y_score =
                params.w_histogram * hist_score +
                params.w_contrast  * ctr_score  +
                params.w_edge      * edge_score  +
                params.w_entropy   * ent_score   +
                params.w_laplacian * lap_score;

            if (params.has_chroma_baseline) {
                float cb_m  = ksigma_score(cb_mean,  params.baseline_cb_mean,
                                           params.baseline_cb_mean_std,
                                           params.k, params.sigma_floor_fraction);
                float cb_v  = ksigma_score(cb_var,   params.baseline_cb_var,
                                           params.baseline_cb_var_std,
                                           params.k, params.sigma_floor_fraction);
                float cr_m  = ksigma_score(cr_mean,  params.baseline_cr_mean,
                                           params.baseline_cr_mean_std,
                                           params.k, params.sigma_floor_fraction);
                float cr_v  = ksigma_score(cr_var,   params.baseline_cr_var,
                                           params.baseline_cr_var_std,
                                           params.k, params.sigma_floor_fraction);
                float sat_m = ksigma_score(sat_mean, params.baseline_sat_mean,
                                           params.baseline_sat_mean_std,
                                           params.k, params.sigma_floor_fraction);
                float sat_v = ksigma_score(sat_var,  params.baseline_sat_var,
                                           params.baseline_sat_var_std,
                                           params.k, params.sigma_floor_fraction);

                float cb_s  = fminf(cb_m,  cb_v);
                float cr_s  = fminf(cr_m,  cr_v);
                float sat_s = fminf(sat_m, sat_v);
                if (!isfinite(cb_s))  cb_s  = 0.f;
                if (!isfinite(cr_s))  cr_s  = 0.f;
                if (!isfinite(sat_s)) sat_s = 0.f;

                float uv_score = params.uv_weight_cb  * cb_s  +
                                 params.uv_weight_cr  * cr_s  +
                                 params.uv_weight_sat * sat_s;

                result->cb_score           = cb_s;
                result->cr_score           = cr_s;
                result->sat_score          = sat_s;
                result->uv_score           = uv_score;
                result->overall_confidence = fminf(y_score, uv_score);
            } else {
                result->cb_score           = 0.f;
                result->cr_score           = 0.f;
                result->sat_score          = 0.f;
                result->uv_score           = 0.f;
                result->overall_confidence = y_score;
            }
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

    // 32x8 = 256 threads/block. The grid maps the Y plane directly; each dim is
    // capped so a very large frame falls back to the kernel's 2D grid-stride loop.
    const dim3 block(32, 8);
    unsigned gx = (unsigned)((W + (int)block.x - 1) / (int)block.x);
    unsigned gy = (unsigned)((H + (int)block.y - 1) / (int)block.y);
    if (gx < 1u)   gx = 1u;
    if (gx > 256u) gx = 256u;
    if (gy < 1u)   gy = 1u;
    if (gy > 256u) gy = 256u;
    const dim3 grid(gx, gy);
    histogram_fallback_kernel<<<grid, block, 0, stream>>>(src, pitch, W, H, d_hist);
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

void launch_chroma_stats(const unsigned char* d_uv, int uv_pitch,
                         int cw, int ch, unsigned long long* d_chroma_sums,
                         cudaStream_t stream)
{
    cudaError_t err = cudaMemsetAsync(d_chroma_sums, 0,
                                      6 * sizeof(unsigned long long), stream);
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] cudaMemsetAsync chroma_sums failed: %s\n",
                cudaGetErrorString(err));

    // 32x8 = 256 threads/block (matches the kernel's shared-memory reduction).
    // The grid maps the chroma plane directly; each dim is capped so a very
    // large frame falls back to the kernel's 2D grid-stride loop.
    const dim3 block(32, 8);
    unsigned gx = (unsigned)((cw + (int)block.x - 1) / (int)block.x);
    unsigned gy = (unsigned)((ch + (int)block.y - 1) / (int)block.y);
    if (gx < 1u)   gx = 1u;
    if (gx > 256u) gx = 256u;
    if (gy < 1u)   gy = 1u;
    if (gy > 256u) gy = 256u;
    const dim3 grid(gx, gy);
    chroma_stats_kernel<<<grid, block, 0, stream>>>(d_uv, uv_pitch, cw, ch, d_chroma_sums);
    err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] chroma_stats launch failed: %s\n", cudaGetErrorString(err));
}

void launch_laplacian_stats(const unsigned char* d_y, int y_pitch, int W, int H,
                            unsigned long long* d_lap_sums, cudaStream_t stream)
{
    cudaError_t err = cudaMemsetAsync(d_lap_sums, 0,
                                      2 * sizeof(unsigned long long), stream);
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] cudaMemsetAsync lap_sums failed: %s\n",
                cudaGetErrorString(err));

    // 32x8 = 256 threads/block (matches the kernel's shared-memory reduction).
    // The grid covers the full frame directly; each dimension is capped so a
    // pathologically large frame falls back to the kernel's 2D grid-stride loop.
    const dim3 block(32, 8);
    const int iw = (W > 0) ? W : 1;
    const int ih = (H > 0) ? H : 1;
    unsigned gx = (unsigned)((iw + (int)block.x - 1) / (int)block.x);
    unsigned gy = (unsigned)((ih + (int)block.y - 1) / (int)block.y);
    if (gx < 1u)   gx = 1u;
    if (gx > 256u) gx = 256u;
    if (gy < 1u)   gy = 1u;
    if (gy > 256u) gy = 256u;
    const dim3 grid(gx, gy);
    k_laplacianStats<<<grid, block, 0, stream>>>(d_y, y_pitch, W, H, d_lap_sums);
    err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] laplacian_stats launch failed: %s\n",
                cudaGetErrorString(err));
}

void launch_quality_scoring(const unsigned int* hist,
                            const uint32_t* d_edgeCount,
                            const unsigned long long* d_chroma_sums,
                            const unsigned long long* d_lap_sums,
                            GpuScoringParams params,
                            FrameQualityResult* result,
                            GpuLearnAccum* learn_accum,
                            int numBins,
                            cudaStream_t stream)
{
    compute_quality_kernel<<<1, 256, 0, stream>>>(
        hist, d_edgeCount, d_chroma_sums, d_lap_sums, params, result, learn_accum, numBins);
    cudaError_t err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] quality_scoring launch failed: %s\n", cudaGetErrorString(err));
}
