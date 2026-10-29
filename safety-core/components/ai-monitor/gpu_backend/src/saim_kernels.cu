/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "saim_kernels.h"

#include <cuda_runtime.h>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Floors only fp rounding error from the subtraction. A larger negative is corruption
// and passes through for the host's sai_metrics_valid gate to reject.
__device__ __forceinline__ float clamp_nonneg(float v) {
    return (v < 0.f && v >= -kMetricCancelEpsilon) ? 0.f : v;
}

// Threads per block for the two plane shapes, from the block geometry that
// computeTileGeometry() validates against the tile size. The block reductions
// below sum each warp by shuffle, so a block must be whole warps.
constexpr int kWarpSize           = 32;
constexpr int kLumaBlockThreads   = kGpuBlockW * kGpuBlockH;
constexpr int kChromaBlockThreads = kGpuBlockW * kGpuChromaBlockH;
constexpr int kLumaWarps          = kLumaBlockThreads   / kWarpSize;
constexpr int kChromaWarps        = kChromaBlockThreads / kWarpSize;

static_assert((kLumaBlockThreads % kWarpSize) == 0,
              "luma block must be whole warps");
static_assert((kChromaBlockThreads % kWarpSize) == 0,
              "chroma block must be whole warps");
// The histogram is zeroed and flushed one bin per thread, so a block must be at
// least as wide as the bin count.
static_assert(kLumaBlockThreads >= kSwHistogramBinCount,
              "luma block must have one thread per histogram bin");

// Sum of v over the calling warp, valid in lane 0. All 32 lanes must call it,
// which holds because no reduction kernel returns before its reduce.
template <typename T>
__device__ __forceinline__ T warp_sum(T v)
{
    #pragma unroll
    for (int off = kWarpSize / 2; off > 0; off >>= 1)
        v += __shfl_down_sync(0xFFFFFFFFu, v, off);
    return v;
}

// How many thread blocks span one analysis tile, and how many rows each thread
// walks (tile_geometry.h). Passed by value to every reduction kernel; fixed for
// the run.
//
// The launchers size their grid to cover the plane exactly, with no grid-stride
// loop, so a block occupies one fixed rectangle for the whole kernel and its
// tile follows from blockIdx alone. That is what lets each block reduce on chip
// and flush once to its own tile's accumulator, instead of every thread
// carrying a tile index and every tile staying resident in shared memory.
using TileMap = GpuTileMap;

// Tile index in [0, kTileCount) for the calling block, row-major.
//
// Sound only because computeTileGeometry() refuses any resolution whose tiles
// are not whole multiples of the block shape, so the division below can never
// place one block in two tiles.
__device__ __forceinline__ int tile_of_block(TileMap tm)
{
    return (int)(blockIdx.y / (unsigned)tm.blocks_y) * kTileCols +
           (int)(blockIdx.x / (unsigned)tm.blocks_x);
}

// Flushes one block's per-thread sums to its tile: warp shuffles, then one
// atomic per slot. Every thread of the block must call it.
template <int N, int Warps>
__device__ __forceinline__ void flush_block(unsigned long long (&l)[N],
                                            unsigned long long (&sh)[N][Warps],
                                            unsigned long long* out)
{
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    #pragma unroll
    for (int k = 0; k < N; ++k) {
        const unsigned long long v = warp_sum(l[k]);
        if ((tid % kWarpSize) == 0) sh[k][tid / kWarpSize] = v;
    }
    __syncthreads();
    if (tid < N) {
        unsigned long long v = 0;
        for (int w = 0; w < Warps; ++w) v += sh[tid][w];
        if (v != 0ULL) atomicAdd(&out[tid], v);
    }
}

/*
 * Chroma sums per tile of the interleaved NV12 UV plane (Cb low byte, Cr high
 * byte) into acc->chroma: Cb, Cb^2, Cr, Cr^2, s, s^2 (s the colourfulness of
 * sai_common.h), |Cb - 128|, |Cr - 128|, and L4(Cb) + L4(Cr).
 * Each thread slides down rows_per_thread rows of one column; the grid covers
 * the plane exactly, so a block sits in one tile.
 */
__global__ void chroma_stats_kernel(const unsigned char* __restrict__ uv, int uv_pitch,
                                    int cw, int ch, TileMap tm,
                                    GpuTileAccum* __restrict__ acc)
{
    __shared__ unsigned long long sh_part[CH_COUNT][kChromaWarps];

    unsigned long long l[CH_COUNT] = {};
    const int c       = blockIdx.x * blockDim.x + threadIdx.x;
    const int r_begin = (blockIdx.y * blockDim.y + threadIdx.y) * tm.rows_per_thread;

    // Guard, not early return: every lane has to reach the reduce below.
    if (c < cw && r_begin < ch) {
        auto at = [uv, uv_pitch](int r, int x) {
            return ((const uchar2*)(uv + (size_t)r * uv_pitch))[x];
        };
        const int  r_end   = min(r_begin + tm.rows_per_thread, ch);
        const bool inner_c = c > 0 && c < cw - 1;
        uchar2 up  = at(max(r_begin - 1, 0), c);
        uchar2 mid = at(r_begin, c);
        for (int r = r_begin; r < r_end; ++r) {
            const uchar2 dn = at(min(r + 1, ch - 1), c);
            const int cb = mid.x, cr = mid.y;
            const int dcb = abs(cb - 128), dcr = abs(cr - 128);
            const int mx = max(dcb, dcr), mn = min(dcb, dcr);
            const int approx = (kSatAlphaQ7 * mx + kSatBetaQ7 * mn + kSatRoundQ7) >> kSatShiftQ7;
            const unsigned int s = (unsigned int)max(mx, approx);   // near-axis correction
            l[CH_CB]  += cb;  l[CH_CB2] += (unsigned int)(cb * cb);
            l[CH_CR]  += cr;  l[CH_CR2] += (unsigned int)(cr * cr);
            l[CH_S]   += s;   l[CH_S2]  += s * s;
            l[CH_CB_ABS] += dcb;
            l[CH_CR_ABS] += dcr;
            if (inner_c && r > 0 && r < ch - 1) {
                const uchar2 lf = at(r, c - 1), rt = at(r, c + 1);
                l[CH_L4] += abs(4 * cb - up.x - dn.x - lf.x - rt.x) +
                            abs(4 * cr - up.y - dn.y - lf.y - rt.y);
            }
            up  = mid;
            mid = dn;
        }
    }
    flush_block(l, sh_part, acc->chroma[tile_of_block(tm)]);
}

/*
 * One pass over Y: the frame's 256 luma bins, and per tile the L8 sum and sum
 * of squares (frame lap_var), sum Y, pixels at or below lines.dark and at or
 * above lines.bright, sum L4, and sum |Y - previous Y| (0 on the first frame).
 * Each thread slides a 3x3 window down rows_per_thread rows of one column;
 * L8 neighbours clamp at the frame edge, L4 is 0 on the outer ring. The L8 sum
 * is signed, carried as two's complement bits. Each pixel is also copied to
 * d_cur. d_prev and d_cur must be separate planes: written in place, every
 * d_prev load waits behind the store before it, which costs several times this
 * pass once the plane has left the cache between frames.
 */
__global__ void k_lumaStats(const unsigned char* __restrict__ d_y, int pitch, int W, int H,
                            const unsigned char* __restrict__ d_prev,
                            unsigned char* __restrict__ d_cur, int first,
                            TileMap tm, LumaLines lines, GpuTileAccum* __restrict__ acc)
{
    __shared__ unsigned long long sh_part[LU_COUNT][kLumaWarps];
    __shared__ unsigned int       sh_hist[kSwHistogramBinCount];

    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    if (tid < kSwHistogramBinCount) sh_hist[tid] = 0u;
    __syncthreads();

    unsigned long long l[LU_COUNT] = {};
    const int c       = blockIdx.x * blockDim.x + threadIdx.x;
    const int r_begin = (blockIdx.y * blockDim.y + threadIdx.y) * tm.rows_per_thread;

    // Guard, not early return: every lane has to reach the reduce below.
    if (c < W && r_begin < H) {
        const int  r_end   = min(r_begin + tm.rows_per_thread, H);
        const int  cL      = max(c - 1, 0);
        const int  cR      = min(c + 1, W - 1);
        const bool inner_c = c > 0 && c < W - 1;

        const unsigned char* rowU = d_y + (size_t)max(r_begin - 1, 0) * pitch;
        const unsigned char* rowM = d_y + (size_t)r_begin * pitch;
        int uL = rowU[cL], uC = rowU[c], uR = rowU[cR];
        int mL = rowM[cL], mC = rowM[c], mR = rowM[cR];

        for (int r = r_begin; r < r_end; ++r) {
            const unsigned char* rowD = d_y + (size_t)min(r + 1, H - 1) * pitch;
            const int dL = rowD[cL], dC = rowD[c], dR = rowD[cR];

            const int lap = uL + uC + uR + mL + mR + dL + dC + dR - 8 * mC;
            l[LU_L8]     += (unsigned long long)(long long)lap;
            l[LU_L8SQ]   += (unsigned long long)(lap * lap);
            l[LU_Y]      += mC;
            l[LU_DARK]   += (mC <= lines.dark);
            l[LU_BRIGHT] += (mC >= lines.bright);
            if (inner_c && r > 0 && r < H - 1)
                l[LU_L4] += abs(4 * mC - uC - dC - mL - mR);
            const size_t i = (size_t)r * W + c;
            if (!first) l[LU_MOTION] += abs(mC - (int)d_prev[i]);
            d_cur[i] = (unsigned char)mC;
            atomicAdd(&sh_hist[mC], 1u);

            uL = mL;  uC = mC;  uR = mR;
            mL = dL;  mC = dC;  mR = dR;
        }
    }

    const int tile = tile_of_block(tm);
    // Its barrier also orders every thread's histogram atomics before the flush below.
    flush_block(l, sh_part, acc->luma[tile]);
    if (tid < kSwHistogramBinCount) {
        const unsigned int v = sh_hist[tid];
        if (v != 0u) atomicAdd(&acc->hist[tid], v);
    }
}

// The band of Y is e = G(1) - G(4), G(s) a Gaussian blur of sigma s pixels, run
// along the rows and then down the columns.
constexpr int kR1 = kBandR1;
constexpr int kR4 = kBandR4;
static_assert(kR4 == kBandPad, "the scratch planes' mirrored margin is the sigma 4 radius");
__constant__ float c_g1[2 * kR1 + 1];
__constant__ float c_g4[2 * kR4 + 1];

// Mirror (reflect-101) index into [0, n): -1 -> 1, n -> n - 2. Valid for -n < i < 2n - 1.
__device__ __forceinline__ int mirror(int i, int n)
{
    return (i < 0) ? -i : ((i >= n) ? 2 * (n - 1) - i : i);
}

// Band step 1 block: kBandRowWarps warps, each filtering kBandRowPx pixels of one
// plane row, kBandRowStep adjacent pixels per lane.
constexpr int kBandRowStep  = 4;
constexpr int kBandRowPx    = kWarpSize * kBandRowStep;
constexpr int kBandRowWarps = 4;
constexpr int kBandRowSpan  = kBandRowPx + 2 * kR4;   // a warp's pixels plus both margins
static_assert(kTileCols * kCannyBlockPxW > kBandRowPx + kR4 && kTileRows * kCannyBlockPxH > kR4,
              "step 1's mirrored reads must stay within one reflection of the smallest frame");
static_assert(kBandRowStep == 4 && kR4 % 4 == 0 && kCannyBlockPxW % 4 == 0,
              "a lane's taps are whole float4s and its outputs one float4 per plane");

// Band step 1, along the rows of p: a = g1 * p and b = g4 * p. Plane row r is picture
// row r - kR4, mirrored above and below the picture, so step 2 reads straight down
// with no border test. A warp copies its pixels and kR4 more on each side into shared
// memory once, mirrored at the picture's sides; each lane then filters its pixels
// from registers.
__global__ void k_bandRows(const unsigned char* __restrict__ p, int pitch, int W, int H,
                           float* __restrict__ a, float* __restrict__ b)
{
    __shared__ __align__(16) float s[kBandRowWarps][kBandRowSpan];
    const int r = blockIdx.y * kBandRowWarps + threadIdx.y;
    // Warps share no data, so a whole warp may leave before its __syncwarp.
    if (r >= H + 2 * kR4) return;
    const int x0 = blockIdx.x * kBandRowPx;
    const unsigned char* row = p + (size_t)mirror(r - kR4, H) * pitch;
    float* sr = s[threadIdx.y];
    for (int i = threadIdx.x; i < kBandRowSpan; i += kWarpSize)
        sr[i] = (float)row[mirror(x0 + i - kR4, W)];
    __syncwarp();
    const int x = x0 + threadIdx.x * kBandRowStep;
    if (x >= W) return;

    // c[i] is pixel x - kR4 + i.
    float c[kBandRowStep + 2 * kR4];
    const float4* src = reinterpret_cast<const float4*>(sr + threadIdx.x * kBandRowStep);
    #pragma unroll
    for (int i = 0; i < (kBandRowStep + 2 * kR4) / 4; ++i) {
        const float4 v = src[i];
        c[4 * i] = v.x; c[4 * i + 1] = v.y; c[4 * i + 2] = v.z; c[4 * i + 3] = v.w;
    }
    float s1[kBandRowStep], s4[kBandRowStep];
    #pragma unroll
    for (int j = 0; j < kBandRowStep; ++j) {
        s1[j] = 0.f;
        s4[j] = 0.f;
        #pragma unroll
        for (int k = -kR1; k <= kR1; ++k) s1[j] += c_g1[k + kR1] * c[j + kR4 + k];
        #pragma unroll
        for (int k = -kR4; k <= kR4; ++k) s4[j] += c_g4[k + kR4] * c[j + kR4 + k];
    }
    const size_t o = (size_t)r * W + x;
    *reinterpret_cast<float4*>(a + o) = make_float4(s1[0], s1[1], s1[2], s1[3]);
    *reinterpret_cast<float4*>(b + o) = make_float4(s4[0], s4[1], s4[2], s4[3]);
}

// Band step 2 work item: kBandColRows consecutive rows of one column, so a thread
// loads the rows around them once for all of them.
constexpr int kBandColThreads = 256;
constexpr int kBandColWarps   = kBandColThreads / kWarpSize;

// Band step 2, down the columns: e = g1 * a - g4 * b is the band; per tile it is
// squared and summed. One block per tile, so each tile's sum is written once, with
// no atomics, and is the same on every run. A thread adds its few dozen values in
// float, the block adds the threads' totals in double.
__global__ void k_bandCols(const float* __restrict__ a, const float* __restrict__ b,
                           int W, int H, double* __restrict__ sq)
{
    __shared__ double sh[kBandColWarps];
    const int tw = W / kTileCols, th = H / kTileRows;
    const int x0 = (blockIdx.x % kTileCols) * tw, y0 = (blockIdx.x / kTileCols) * th;
    float q = 0.f;
    for (int w = threadIdx.x; w < tw * (th / kBandColRows); w += kBandColThreads) {
        const int x = x0 + w % tw;
        const int y = y0 + (w / tw) * kBandColRows;
        // Plane row y + kR4 is picture row y.
        const float* pa = a + (size_t)(y + kR4 - kR1) * W + x;
        const float* pb = b + (size_t)y * W + x;
        float va[kBandColRows + 2 * kR1], vb[kBandColRows + 2 * kR4];
        #pragma unroll
        for (int i = 0; i < kBandColRows + 2 * kR1; ++i) va[i] = pa[(size_t)i * W];
        #pragma unroll
        for (int i = 0; i < kBandColRows + 2 * kR4; ++i) vb[i] = pb[(size_t)i * W];
        #pragma unroll
        for (int j = 0; j < kBandColRows; ++j) {
            float u = 0.f, v = 0.f;
            #pragma unroll
            for (int k = 0; k <= 2 * kR1; ++k) u += c_g1[k] * va[j + k];
            #pragma unroll
            for (int k = 0; k <= 2 * kR4; ++k) v += c_g4[k] * vb[j + k];
            const float e = u - v;
            q += e * e;
        }
    }
    const double wq = warp_sum((double)q);
    if ((threadIdx.x % kWarpSize) == 0) sh[threadIdx.x / kWarpSize] = wq;
    __syncthreads();
    if (threadIdx.x == 0) {
        double Q = 0.0;
        for (int w = 0; w < kBandColWarps; ++w) Q += sh[w];
        sq[blockIdx.x] = Q;
    }
}

// Binomial 5-tap [1 4 6 4 1]. The 5x5 Gaussian (sum 256) is its outer product
// with itself, so a row pass then a column pass gives the 2D sum exactly.
__device__ __forceinline__ int binomial5(int a, int b, int c, int d, int e)
{
    return (a + e) + 4 * (b + d) + 6 * c;
}

// Every Canny step produces this many adjacent pixels of one row per thread, and
// every output is one vector store; after the blur, every input row is one
// 4-pixel vector load plus a scalar at each end. That needs W % 4 == 0, which
// holds because computeTileGeometry() admits only tile widths that are multiples
// of kGpuBlockW. A block, one warp per row, covers kCannyBlockPxW x kCannyBlockPxH.
constexpr int kCannyPx     = 4;
constexpr int kCannyBlockW = kCannyBlockPxW / kCannyPx;
constexpr int kCannyWarps  = kCannyBlockPxH;
static_assert((kGpuBlockW % kCannyPx) == 0,
              "frame widths must stay whole multiples of the Canny pixel group");
static_assert(kCannyBlockW == kWarpSize, "each thread row of a Canny block is one warp");

// Output rows one blur thread produces, sliding its five input rows down.
constexpr int kBlurRows = 8;

// The blur's row pass for pixels c0 .. c0 + kCannyPx - 1 of row r, with the row
// and the columns clamped to the frame (replicate border). Byte loads: the
// source surface's alignment is the decoder's, not ours.
__device__ __forceinline__ void blur_row(const uint8_t* y, int pitch, int W, int H,
                                         int r, int c0, int (&h)[kCannyPx])
{
    const uint8_t* p = y + (size_t)min(max(r, 0), H - 1) * pitch;
    int x[kCannyPx + 4];
    #pragma unroll
    for (int i = 0; i < kCannyPx + 4; ++i) x[i] = p[min(max(c0 - 2 + i, 0), W - 1)];
    #pragma unroll
    for (int j = 0; j < kCannyPx; ++j) h[j] = binomial5(x[j], x[j + 1], x[j + 2], x[j + 3], x[j + 4]);
}

/*
 * 5x5 Gaussian blur of the Y plane, replicate border. Each thread makes
 * kCannyPx adjacent pixels on kBlurRows rows: one row pass per input row, the
 * last five kept in registers for the column pass, then / 256 rounded to
 * nearest, as OpenCV rounds.
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
    const int c0   = (blockIdx.x * blockDim.x + threadIdx.x) * kCannyPx;
    const int row0 = (blockIdx.y * blockDim.y + threadIdx.y) * kBlurRows;
    if (c0 >= W || row0 >= H) return;

    // h[k] is the row pass of input row (output row - 2 + k).
    int h[5][kCannyPx];
    #pragma unroll
    for (int k = 0; k < 4; ++k) blur_row(d_yplane, pitch, W, H, row0 - 2 + k, c0, h[k]);
    #pragma unroll
    for (int i = 0; i < kBlurRows; ++i) {
        const int row = row0 + i;
        if (row >= H) return;
        blur_row(d_yplane, pitch, W, H, row + 2, c0, h[4]);
        uint8_t o[kCannyPx];
        #pragma unroll
        for (int j = 0; j < kCannyPx; ++j)
            o[j] = (uint8_t)((binomial5(h[0][j], h[1][j], h[2][j], h[3][j], h[4][j]) + 128) >> 8);
        *reinterpret_cast<uchar4*>(blurred + (size_t)row * W + c0) =
            make_uchar4(o[0], o[1], o[2], o[3]);
        #pragma unroll
        for (int k = 0; k < 4; ++k)
            #pragma unroll
            for (int j = 0; j < kCannyPx; ++j) h[k][j] = h[k + 1][j];
    }
}

// Columns c0 - 1 .. c0 + 4 of one row into p[0..5], reading 0 outside the frame.
// V is T's 4-lane vector type. Its load is aligned and inside the row because
// c0 and W are multiples of 4 and every row starts a multiple of W elements
// into a cudaMalloc'd buffer.
template <typename T, typename V>
__device__ __forceinline__ void load_row_window(const T* row, int c0, int W,
                                                int (&p)[kCannyPx + 2])
{
    const V v = *reinterpret_cast<const V*>(row + c0);
    p[0] = (c0 > 0) ? (int)row[c0 - 1] : 0;
    p[1] = v.x; p[2] = v.y; p[3] = v.z; p[4] = v.w;
    p[5] = (c0 + kCannyPx < W) ? (int)row[c0 + kCannyPx] : 0;
}

// tan(22.5 deg) in Q15, OpenCV's TG22. tan(67.5 deg) is tan(22.5 deg) + 2.
constexpr int kTan22Q15     = 13573;
constexpr int kScharrMaxAbs = 16 * 255;
static_assert(kScharrMaxAbs * (kTan22Q15 + (1 << 16)) <= INT_MAX &&
              (kScharrMaxAbs << 15) <= INT_MAX,
              "the Q15 direction test must not overflow int");
static_assert(2 * kScharrMaxAbs * kScharrMaxAbs <= 32767 * 32767,
              "gx^2 + gy^2 must fit int and its square root int16");

// Direction bucket for NMS: 0 = horiz, 1 = diag45, 2 = vert, 3 = diag135, split
// at 22.5 and 67.5 degrees with OpenCV's integer test.
__device__ __forceinline__ uint8_t quantise_dir(int gx, int gy)
{
    const int ax   = abs(gx);
    const int ay   = abs(gy) << 15;
    const int tg22 = ax * kTan22Q15;
    if (ay < tg22) return 0;
    if (ay > tg22 + (ax << 16)) return 2;
    // gy is below minus above, so a same-sign gx and gy points from top-left
    // to bottom-right: diag135.
    return (gx * gy > 0) ? 3 : 1;
}

/*
 * Computes gradient magnitude and quantised direction using Scharr operators.
 * Magnitude = sqrt(Gx^2 + Gy^2) (L2) truncated to an integer.
 * Direction is quantised to 4 orientations (0=horiz, 1=diag45, 2=vert, 3=diag135)
 * for use by non-maximum suppression. Each thread writes kCannyPx adjacent
 * pixels of one row.
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
    const int c0  = (blockIdx.x * blockDim.x + threadIdx.x) * kCannyPx;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (c0 >= W || row >= H) return;

    const int idx = row * W + c0;
    if (row < 1 || row >= H - 1) {
        *reinterpret_cast<short4*>(mag_buf + idx) = make_short4(0, 0, 0, 0);
        *reinterpret_cast<uchar4*>(dir_buf + idx) = make_uchar4(0, 0, 0, 0);
        return;
    }

    // p[r][j] is column c0 - 1 + j of row row - 1 + r, so pixel c0 + j has its
    // 3x3 neighbourhood in columns j .. j + 2.
    int p[3][kCannyPx + 2];
    #pragma unroll
    for (int r = 0; r < 3; ++r)
        load_row_window<uint8_t, uchar4>(blurred + (row - 1 + r) * W, c0, W, p[r]);

    int16_t m[kCannyPx];
    uint8_t d[kCannyPx];
    #pragma unroll
    for (int j = 0; j < kCannyPx; ++j) {
        const int col = c0 + j;
        if (col < 1 || col >= W - 1) { m[j] = 0; d[j] = 0; continue; }

        // Scharr X = [[-3,0,3],[-10,0,10],[-3,0,3]] and Y = its transpose, written
        // out so the zero taps cost nothing. Higher centre weight than Sobel gives
        // better rotational symmetry.
        const int gx = 3 * (p[0][j + 2] - p[0][j] + p[2][j + 2] - p[2][j])
                     + 10 * (p[1][j + 2] - p[1][j]);
        const int gy = 3 * (p[2][j] - p[0][j] + p[2][j + 2] - p[0][j + 2])
                     + 10 * (p[2][j + 1] - p[0][j + 1]);
        // Correctly rounded under any math flags, so an exact square such as
        // sqrt(2601) = 51 never truncates to 50.
        m[j] = (int16_t)__fsqrt_rn((float)(gx * gx + gy * gy));
        d[j] = quantise_dir(gx, gy);
    }
    *reinterpret_cast<short4*>(mag_buf + idx) = make_short4(m[0], m[1], m[2], m[3]);
    *reinterpret_cast<uchar4*>(dir_buf + idx) = make_uchar4(d[0], d[1], d[2], d[3]);
}

/*
 * Hysteresis (Canny step 4): a weak pixel survives when a chain of 8-connected
 * NMS survivors links it to a strong one, which is what OpenCV's flood fill from
 * the strong pixels keeps. Computed as connected components by union-find
 * (Playne & Hawick 2018): NMS starts the trees, then a merge and a resolve
 * launch, no host round trip, and a result independent of thread scheduling.
 *
 * A survivor's key is its pixel index, plus W*H when weak, so every strong key
 * is below every weak one. labels[i] holds the parent key of pixel i and a tree
 * is rooted at its smallest key, so a component's root is strong exactly when
 * the component holds a strong pixel.
 */
static_assert(2ull * MAX_DIM * MAX_DIM <= 0xFFFFFFFFull,
              "weak hysteresis keys reach 2*W*H - 1 and must fit uint32");

__device__ __forceinline__ uint32_t hyst_key(int i, int nms, uint32_t n)
{
    return (nms == 255) ? (uint32_t)i : (uint32_t)i + n;
}

__device__ __forceinline__ uint32_t hyst_slot(uint32_t key, uint32_t n)
{
    return (key < n) ? key : key - n;
}

/*
 * Non-maximum suppression + double thresholding (Canny steps 2-3), with
 * OpenCV's comparisons. A pixel above low_thresh survives when it beats the
 * neighbour behind it along the gradient and is not beaten by the one ahead,
 * so a two-pixel plateau keeps one pixel; on a diagonal it must beat both.
 * Outputs 255 (strong: above high_thresh), 128 (weak) or 0 (suppressed), and
 * makes every survivor a one-pixel hysteresis tree. Suppressed pixels' label
 * entries are never linked or read. Each thread writes kCannyPx adjacent
 * pixels of one row.
 */
__global__ void k_nmsThreshold(const int16_t* __restrict__ mag_buf,
                                const uint8_t* __restrict__ dir_buf,
                                uint8_t*       __restrict__ edges,
                                uint32_t*      __restrict__ labels,
                                int W, int H,
                                int low_thresh, int high_thresh)
{
    const int c0  = (blockIdx.x * blockDim.x + threadIdx.x) * kCannyPx;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (c0 >= W || row >= H) return;

    const int idx = row * W + c0;
    if (row < 1 || row >= H - 1) {
        *reinterpret_cast<uchar4*>(edges + idx) = make_uchar4(0, 0, 0, 0);
        return;
    }

    // g[r][j] is the magnitude at column c0 - 1 + j of row row - 1 + r.
    int g[3][kCannyPx + 2];
    #pragma unroll
    for (int r = 0; r < 3; ++r)
        load_row_window<int16_t, short4>(mag_buf + (row - 1 + r) * W, c0, W, g[r]);
    const uchar4 dv = *reinterpret_cast<const uchar4*>(dir_buf + idx);
    const int dir[kCannyPx] = {dv.x, dv.y, dv.z, dv.w};

    uint8_t e[kCannyPx];
    #pragma unroll
    for (int j = 0; j < kCannyPx; ++j) {
        const int col = c0 + j;
        const int m   = g[1][j + 1];
        if (col < 1 || col >= W - 1 || m <= low_thresh) { e[j] = 0; continue; }

        bool peak;
        switch (dir[j]) {
            case 0:  peak = m > g[1][j]     && m >= g[1][j + 2]; break;
            case 2:  peak = m > g[0][j + 1] && m >= g[2][j + 1]; break;
            case 3:  peak = m > g[0][j]     && m >  g[2][j + 2]; break;
            default: peak = m > g[0][j + 2] && m >  g[2][j];     break;
        }
        e[j] = peak ? ((m > high_thresh) ? 255 : 128) : 0;
    }
    *reinterpret_cast<uchar4*>(edges + idx) = make_uchar4(e[0], e[1], e[2], e[3]);
    if (e[0] | e[1] | e[2] | e[3]) {
        const uint32_t n = (uint32_t)W * (uint32_t)H;
        *reinterpret_cast<uint4*>(labels + idx) =
            make_uint4(hyst_key(idx, e[0], n),     hyst_key(idx + 1, e[1], n),
                       hyst_key(idx + 2, e[2], n), hyst_key(idx + 3, e[3], n));
    }
}

// Root key of key's tree. Parent keys only ever decrease, so the walk ends.
// During the merge Labels is volatile: those loads are then relaxed atomics in
// the PTX memory model, so racing another thread's atomicMin is not a data race.
template <typename Labels>
__device__ __forceinline__ uint32_t hyst_root(Labels labels, uint32_t key, uint32_t n)
{
    for (;;) {
        const uint32_t parent = labels[hyst_slot(key, n)];
        if (parent == key) return key;
        key = parent;
    }
}

// Joins the trees holding keys a and b under the smaller root, unless both
// already hold a strong pixel: a weak pixel survives through the first strong
// pixel on its chain, so joining two such trees changes no result, and strong
// pixels never get a parent. When another thread relinked b first, atomicMin
// returns b's new parent and the loop joins a with that tree instead, so no
// link is lost.
__device__ void hyst_union(volatile uint32_t* labels, uint32_t a, uint32_t b, uint32_t n)
{
    while (a >= n || b >= n) {
        a = hyst_root(labels, a, n);
        b = hyst_root(labels, b, n);
        if (a == b) return;
        if (a > b) { const uint32_t t = a; a = b; b = t; }
        if (b < n) return;
        const uint32_t old = atomicMin(const_cast<uint32_t*>(&labels[hyst_slot(b, n)]), a);
        if (old == b) return;
        b = old;
    }
}

// Joins every pair of 8-connected survivors into one tree. Each pair is joined
// by its later pixel in raster order, so a pixel looks at its left, up-left, up
// and up-right neighbours.
__global__ void k_hystMerge(const uint8_t* __restrict__ nms_edges,
                            volatile uint32_t* labels,
                            int W, int H)
{
    const int c0  = (blockIdx.x * blockDim.x + threadIdx.x) * kCannyPx;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    // NMS zeroes rows 0 and H - 1, so they hold no survivors.
    if (c0 >= W || row < 1 || row >= H - 1) return;

    // cur[j] and up[j] are the NMS values at column c0 - 1 + j of this row and
    // the row above; 0 outside the frame.
    int cur[kCannyPx + 2], up[kCannyPx + 2];
    load_row_window<uint8_t, uchar4>(nms_edges + row * W, c0, W, cur);
    load_row_window<uint8_t, uchar4>(nms_edges + (row - 1) * W, c0, W, up);

    const uint32_t n = (uint32_t)W * (uint32_t)H;
    #pragma unroll
    for (int j = 0; j < kCannyPx; ++j) {
        if (cur[j + 1] == 0) continue;
        const int      i = row * W + c0 + j;
        const uint32_t k = hyst_key(i, cur[j + 1], n);
        // Neighbours that touch each other are joined by their own links, so
        // two unions cover all four: the left survivor (else up, else up-left),
        // and the up-right one unless the up one already reaches it.
        int nb = cur[j], at = i - 1;
        if (!nb) { nb = up[j + 1]; at = i - W; }
        if (!nb) { nb = up[j];     at = i - W - 1; }
        if (nb) hyst_union(labels, k, hyst_key(at, nb, n), n);
        if (up[j + 2] && !up[j + 1])
            hyst_union(labels, k, hyst_key(i - W + 1, up[j + 2], n), n);
    }
}

// Adds each tile's edge pixels to acc->edges: strong pixels, and weak ones whose
// tree has a strong root. Runs after the merge launch, so the trees are final.
// A block's pixels lie in one tile (init() checks), so it adds one total.
__global__ void k_hystResolve(const uint8_t*  __restrict__ nms_edges,
                              const uint32_t* __restrict__ labels,
                              int W, int H, GpuTileAccum* __restrict__ acc)
{
    __shared__ unsigned int sh[kCannyWarps];
    const int c0  = (blockIdx.x * blockDim.x + threadIdx.x) * kCannyPx;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;

    unsigned int kept = 0;
    // Guard, not early return: every lane has to reach the reduce below.
    if (c0 < W && row < H) {
        const int      idx = row * W + c0;
        const uint32_t n   = (uint32_t)W * (uint32_t)H;
        const uchar4   v   = *reinterpret_cast<const uchar4*>(nms_edges + idx);
        const int      s[kCannyPx] = {v.x, v.y, v.z, v.w};
        #pragma unroll
        for (int j = 0; j < kCannyPx; ++j)
            kept += (s[j] == 255) ||
                    (s[j] == 128 && hyst_root(labels, hyst_key(idx + j, s[j], n), n) < n);
    }

    const unsigned int w = warp_sum(kept);
    if (threadIdx.x == 0) sh[threadIdx.y] = w;
    __syncthreads();
    if (threadIdx.x == 0 && threadIdx.y == 0) {
        unsigned int total = 0;
        for (int i = 0; i < kCannyWarps; ++i) total += sh[i];
        const int tile = (int)(blockIdx.y * kCannyBlockPxH) / (H / kTileRows) * kTileCols
                       + (int)(blockIdx.x * kCannyBlockPxW) / (W / kTileCols);
        if (total != 0u) atomicAdd(&acc->edges[tile], total);
    }
}

constexpr int kQualityThreads = kSwHistogramBinCount;   // one histogram bin, and at most one tile, per thread
constexpr int kQualityWarps   = kQualityThreads / kWarpSize;
static_assert(kTileCount <= kQualityThreads, "compute_quality_kernel reads one tile per thread");

// Integer frame sums compute_quality_kernel reduces, in uint64 because FP64 is
// slow on these GPUs; each stays below 2^53, so it converts to double exactly.
// The L8 sum carries its signed value's bits.
enum FrameSum { FS_COUNT, FS_BIN, FS_BIN2, FS_L8, FS_L8SQ, FS_EDGES,
                FS_CB, FS_CB2, FS_CR, FS_CR2, FS_S, FS_S2, FS_N };

/*
 * One block of kQualityThreads. Thread b reads histogram bin b; thread
 * t < kTileCount copies tile t's sums into result->tiles. The
 * block then reduces the frame sums, and thread 0 derives the 12 frame
 * metrics. total_pixels is W * H, chroma_pixels (W/2) * (H/2).
 */
__global__ void compute_quality_kernel(const GpuTileAccum* __restrict__ acc,
                                       int total_pixels, int chroma_pixels,
                                       FrameQualityResult* __restrict__ result)
{
    __shared__ unsigned long long sh[FS_N][kQualityWarps];
    __shared__ double sh_clogc[kQualityWarps];

    const int tid = threadIdx.x;
    unsigned long long u[FS_N] = {};

    const unsigned long long count = acc->hist[tid];
    u[FS_COUNT] = count;
    u[FS_BIN]   = (unsigned long long)tid * count;
    u[FS_BIN2]  = (unsigned long long)tid * (unsigned long long)tid * count;
    // count * log2(count), the entropy's one fractional sum.
    const double clogc = (count > 0) ? (double)count * log2((double)count) : 0.0;

    if (tid < kTileCount) {
        const unsigned long long* lu = acc->luma[tid];
        const unsigned long long* cs = acc->chroma[tid];
        u[FS_L8]    = lu[LU_L8];
        u[FS_L8SQ]  = lu[LU_L8SQ];
        u[FS_EDGES] = acc->edges[tid];
        u[FS_CB] = cs[CH_CB];  u[FS_CB2] = cs[CH_CB2];
        u[FS_CR] = cs[CH_CR];  u[FS_CR2] = cs[CH_CR2];
        u[FS_S]  = cs[CH_S];   u[FS_S2]  = cs[CH_S2];

        TileSums& ts = result->tiles;
        ts.ysum[tid]    = (double)lu[LU_Y];
        ts.dark[tid]    = (double)lu[LU_DARK];
        ts.bright[tid]  = (double)lu[LU_BRIGHT];
        ts.ylap[tid]    = (double)lu[LU_L4];
        ts.motion[tid]  = (double)lu[LU_MOTION];
        ts.detail[tid]  = acc->detail[tid];
        ts.cb[tid]      = (double)cs[CH_CB];
        ts.cr[tid]      = (double)cs[CH_CR];
        ts.cb2[tid]     = (double)cs[CH_CB2];
        ts.cr2[tid]     = (double)cs[CH_CR2];
        ts.sat[tid]     = (double)cs[CH_S];
        ts.sat2[tid]    = (double)cs[CH_S2];
        ts.cb_abs[tid]  = (double)cs[CH_CB_ABS];
        ts.cr_abs[tid]  = (double)cs[CH_CR_ABS];
        ts.clap[tid]    = (double)cs[CH_L4];
    }

    #pragma unroll
    for (int k = 0; k < FS_N; ++k) {
        const unsigned long long w = warp_sum(u[k]);
        if ((tid % kWarpSize) == 0) sh[k][tid / kWarpSize] = w;
    }
    const double wc = warp_sum(clogc);
    if ((tid % kWarpSize) == 0) sh_clogc[tid / kWarpSize] = wc;
    __syncthreads();

    if (tid == 0) {
        double v[FS_N];
        for (int k = 0; k < FS_N; ++k) {
            unsigned long long s = 0;
            for (int w = 0; w < kQualityWarps; ++w) s += sh[k][w];
            if (k == FS_L8) {
                long long l8;
                memcpy(&l8, &s, sizeof(l8));
                v[k] = (double)l8;
            } else {
                v[k] = (double)s;
            }
        }
        double v_clogc = 0.0;
        for (int w = 0; w < kQualityWarps; ++w) v_clogc += sh_clogc[w];
        result->tiles.px  = (double)(total_pixels / kTileCount);
        result->tiles.cpx = (double)(chroma_pixels / kTileCount);

        const uint64_t total = (uint64_t)v[FS_COUNT];
        float mean, var;
        if (total == 0) { mean = (float)(kSwHistogramBinCount / 2); var = 0.f; }
        else {
            double mean_d = v[FS_BIN] / (double)total;
            mean = (float)mean_d;
            var  = (total > 1)
                 ? clamp_nonneg((float)((v[FS_BIN2] - (double)total * mean_d * mean_d)
                                              / (double)(total - 1)))
                 : 0.f;
        }

        float std_dev = sqrtf(var);
        float rms_contrast = (mean > 1e-6f) ? (std_dev / mean) : 0.f;

        // Histogram Shannon entropy (bits) via H = log2(N) - (1/N)*sum(c*log2 c).
        float entropy = 0.f;
        if (total > 0) {
            entropy = clamp_nonneg((float)(log2((double)total)
                                           - v_clogc / (double)total));
        }

        const int luma_px   = total_pixels;
        const int chroma_px = chroma_pixels;

        // Laplacian variance (sharpness) on raw Y; Bessel-corrected to match chroma var.
        float lap_var = 0.f;
        if (luma_px > 1) {
            const double Nd    = (double)luma_px;
            const double meanL = v[FS_L8] / Nd;
            lap_var = clamp_nonneg((float)((v[FS_L8SQ] - Nd * meanL * meanL) / (Nd - 1.0)));
        }

        float edge_density = (luma_px > 0) ? (float)(v[FS_EDGES] / (double)luma_px) : 0.f;

        float cb_mean = 0.f, cb_var = 0.f;
        float cr_mean = 0.f, cr_var = 0.f;
        float sat_mean = 0.f, sat_var = 0.f;
        if (chroma_px > 1) {
            const double Nc      = (double)chroma_px;
            const double inv_div = 1.0 / (Nc - 1.0);
            const double cb_mean_d  = v[FS_CB] / Nc;
            const double cr_mean_d  = v[FS_CR] / Nc;
            const double sat_mean_d = v[FS_S]  / Nc;
            cb_mean  = (float)cb_mean_d;
            cr_mean  = (float)cr_mean_d;
            sat_mean = (float)sat_mean_d;
            cb_var  = clamp_nonneg((float)((v[FS_CB2] - Nc * cb_mean_d  * cb_mean_d)  * inv_div));
            cr_var  = clamp_nonneg((float)((v[FS_CR2] - Nc * cr_mean_d  * cr_mean_d)  * inv_div));
            sat_var = clamp_nonneg((float)((v[FS_S2]  - Nc * sat_mean_d * sat_mean_d) * inv_div));
        }

        result->raw[QM_HIST_MEAN]    = mean;
        result->raw[QM_HIST_VAR]     = var;
        result->raw[QM_RMS_CONTRAST] = rms_contrast;
        result->raw[QM_EDGE_DENSITY] = edge_density;
        result->raw[QM_ENTROPY]      = entropy;
        result->raw[QM_LAP_VAR]      = lap_var;
        result->raw[QM_CB_MEAN]      = cb_mean;
        result->raw[QM_CB_VAR]       = cb_var;
        result->raw[QM_CR_MEAN]      = cr_mean;
        result->raw[QM_CR_VAR]       = cr_var;
        result->raw[QM_SAT_MEAN]     = sat_mean;
        result->raw[QM_SAT_VAR]      = sat_var;
    }
}

// ---- Wrapper functions (called from .cpp files) ----

namespace {

// CUDA launch configuration for one plane. No grid-stride loop and no grid cap:
// a block must keep the same pixels for the whole kernel or it cannot name a
// single tile. gpuPlaneGrid() carries the arithmetic and its guarantees.
struct LaunchShape {
    dim3    grid;
    dim3    block;
    TileMap map;
};

LaunchShape planeLaunch(int plane_w, int plane_h, int block_h)
{
    const GpuPlaneGrid g = gpuPlaneGrid(plane_w, plane_h, block_h);
    LaunchShape s;
    s.block = dim3((unsigned)kGpuBlockW, (unsigned)block_h);
    s.grid  = dim3((unsigned)g.grid_x, (unsigned)g.grid_y);
    s.map   = g.map;
    return s;
}

void reportLaunch(const char* what)
{
    const cudaError_t err = cudaPeekAtLastError();
    if (err != cudaSuccess)
        fprintf(stderr, "[CUDA] %s launch failed: %s\n", what, cudaGetErrorString(err));
}

} // namespace

bool upload_band_weights()
{
    float g1[2 * kR1 + 1], g4[2 * kR4 + 1];
    if (!sai_gauss_taps(1.0, kR1, g1) || !sai_gauss_taps(4.0, kR4, g4)) {
        fprintf(stderr, "[CUDA] band Gaussian radii disagree with their sigmas\n");
        return false;
    }
    const cudaError_t errs[] = {
        cudaMemcpyToSymbol(c_g1, g1, sizeof(g1)), cudaMemcpyToSymbol(c_g4, g4, sizeof(g4)),
    };
    for (const cudaError_t err : errs) {
        if (err != cudaSuccess) {
            fprintf(stderr, "[CUDA] band weight upload failed: %s\n", cudaGetErrorString(err));
            return false;
        }
    }
    return true;
}

void launch_edge_detection(const unsigned char* d_y, int y_pitch, int W, int H,
                           void* d_blurred, void* d_grad_mag, void* d_grad_dir,
                           void* d_edges, void* d_labels,
                           int canny_low_thresh, int canny_high_thresh,
                           GpuTileAccum* acc, cudaStream_t stream)
{
    const dim3 canny_block(kCannyBlockW, kCannyBlockPxH);
    const dim3 canny_grid((W + kCannyBlockPxW - 1) / kCannyBlockPxW,
                          (H + kCannyBlockPxH - 1) / kCannyBlockPxH);
    const dim3 blur_grid(canny_grid.x,
                         (H + kCannyBlockPxH * kBlurRows - 1) / (kCannyBlockPxH * kBlurRows));

    cudaError_t err;
    {
        NVTX_RANGE("GaussianBlur", 0xFFFFAA00);
        k_gaussBlurY<<<blur_grid, canny_block, 0, stream>>>(
            d_y, (uint8_t*)d_blurred, W, H, y_pitch);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] gaussBlur launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("ScharrGradient", 0xFFFFCC00);
        k_scharrGradient<<<canny_grid, canny_block, 0, stream>>>(
            (const uint8_t*)d_blurred,
            (int16_t*)d_grad_mag, (uint8_t*)d_grad_dir, W, H);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] scharrGradient launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("NMSThreshold", 0xFFFFEE00);
        k_nmsThreshold<<<canny_grid, canny_block, 0, stream>>>(
            (const int16_t*)d_grad_mag, (const uint8_t*)d_grad_dir,
            (uint8_t*)d_edges, (uint32_t*)d_labels, W, H,
            canny_low_thresh, canny_high_thresh);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] nmsThreshold launch failed: %s\n", cudaGetErrorString(err));
    }
    {
        NVTX_RANGE("Hysteresis", 0xFFEEFF00);
        k_hystMerge<<<canny_grid, canny_block, 0, stream>>>(
            (const uint8_t*)d_edges, (uint32_t*)d_labels, W, H);
        k_hystResolve<<<canny_grid, canny_block, 0, stream>>>(
            (const uint8_t*)d_edges, (const uint32_t*)d_labels, W, H, acc);
        err = cudaPeekAtLastError();
        if (err != cudaSuccess)
            fprintf(stderr, "[CUDA] hysteresis launch failed: %s\n", cudaGetErrorString(err));
    }
}

void launch_chroma_stats(const unsigned char* d_uv, int uv_pitch, int cw, int ch,
                         GpuTileAccum* acc, cudaStream_t stream)
{
    // 32x4 = 128 threads/block: a chroma tile is half as tall as a luma tile.
    const LaunchShape s = planeLaunch(cw, ch, kGpuChromaBlockH);
    chroma_stats_kernel<<<s.grid, s.block, 0, stream>>>(d_uv, uv_pitch, cw, ch, s.map, acc);
    reportLaunch("chroma_stats");
}

void launch_luma_stats(const unsigned char* d_y, int y_pitch, int W, int H,
                       const unsigned char* d_prev, unsigned char* d_cur, int first,
                       LumaLines lines, GpuTileAccum* acc, cudaStream_t stream)
{
    const LaunchShape s = planeLaunch(W, H, kGpuBlockH);
    k_lumaStats<<<s.grid, s.block, 0, stream>>>(d_y, y_pitch, W, H, d_prev, d_cur, first, s.map,
                                                 lines, acc);
    reportLaunch("luma_stats");
}

void launch_fine_band(const unsigned char* d_y, int y_pitch, int W, int H,
                      float* d_a, float* d_b, GpuTileAccum* acc, cudaStream_t stream)
{
    const dim3 rows_grid((W + kBandRowPx - 1) / kBandRowPx,
                         (H + 2 * kR4 + kBandRowWarps - 1) / kBandRowWarps);
    k_bandRows<<<rows_grid, dim3(kWarpSize, kBandRowWarps), 0, stream>>>(d_y, y_pitch, W, H, d_a, d_b);
    k_bandCols<<<kTileCount, kBandColThreads, 0, stream>>>(d_a, d_b, W, H, acc->detail);
    reportLaunch("fine_band");
}

void launch_quality_metrics(const GpuTileAccum* acc, int total_pixels, int chroma_pixels,
                            FrameQualityResult* result, cudaStream_t stream)
{
    compute_quality_kernel<<<1, kQualityThreads, 0, stream>>>(acc, total_pixels, chroma_pixels,
                                                              result);
    reportLaunch("quality_metrics");
}
