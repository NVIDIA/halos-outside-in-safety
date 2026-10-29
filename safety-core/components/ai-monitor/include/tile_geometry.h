/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAI_TILE_GEOMETRY_H
#define SAI_TILE_GEOMETRY_H

// The analysis grid.
#define SAI_TILE_ROWS 15
#define SAI_TILE_COLS 15

constexpr int kTileRows  = SAI_TILE_ROWS;
constexpr int kTileCols  = SAI_TILE_COLS;
constexpr int kTileCount = kTileRows * kTileCols;

// Resolves how a W x H frame is cut into analysis tiles, and how the backend's
// work unit is sized so that it never straddles a tile boundary.
//
// That non-straddling property is the one thing the whole per-tile design rests
// on: it is what keeps accumulation local (no cross-tile atomics on GPU) and
// what makes the composed global bit-exactly equal to a whole-frame pass. A
// geometry that cannot satisfy it is reported as invalid rather than
// approximated.

// CUDA thread-block shape. Width is 32 so a row of threads is one warp reading
// consecutive bytes; that is the only dimension chosen for speed.
//
// The heights are chosen by nesting, not by tuning. A block must lie inside one
// analysis tile or it cannot name a single tile to flush its reduction to, so
// the height has to divide the tile height. Luma runs 8 (256 threads). Chroma
// runs 4 because its plane is half resolution: at 1080p a chroma tile is 36
// rows, not divisible by 8 but by 4. The block reduce works per warp, so 128
// threads behave the same as 256.
constexpr int kGpuBlockW        = 32;
constexpr int kGpuBlockH        = 8;
constexpr int kGpuChromaBlockH  = 4;

// Most plane rows one reduction thread walks. Every block pays a fixed reduce
// and flush (all 256 histogram bins on luma), so fewer, taller blocks pay it
// less often. 9 divides the tile height in blocks at 1080p and 4K.
constexpr int kGpuMaxRowsPerThread = 9;

// Resolved geometry for one (resolution, grid).
struct TileGeometry {
    bool valid  = false;
    int  rows   = 0;
    int  cols   = 0;
    int  tile_w = 0;
    int  tile_h = 0;
};

namespace tile_geometry_detail {

// Largest h in [1, cap] that divides n and satisfies pred; 0 when none exists.
template <typename Pred>
inline int largest_divisor(int n, int cap, Pred pred) noexcept
{
    const int start = (n < cap) ? n : cap;
    for (int h = start; h >= 1; --h) {
        if ((n % h) != 0) continue;
        if (!pred(h)) continue;
        return h;
    }
    return 0;
}

}  // namespace tile_geometry_detail

// Resolves the geometry for a W x H frame, or returns valid == false when it
// cannot be honored exactly. Callers must refuse to start on an invalid result:
// there is no meaningful approximation, because a straddling block silently
// attributes pixels to the wrong tile.
inline TileGeometry computeTileGeometry(int W, int H) noexcept
{
    TileGeometry g;

    if (W <= 0 || H <= 0)
        return g;
    // Equal tiles only. An uneven split would give tiles different pixel counts,
    // which the per-tile normalization and the composed global both assume away.
    if ((W % kTileCols) != 0 || (H % kTileRows) != 0)
        return g;

    const int tile_w = W / kTileCols;
    const int tile_h = H / kTileRows;

    // A CUDA block must sit inside one tile so it can reduce in shared memory
    // and flush once to its own tile's accumulator. If it straddled, every
    // tile would have to be resident in shared memory and each pixel would
    // need its own tile index.
    if ((tile_w % kGpuBlockW) != 0 || (tile_h % kGpuBlockH) != 0)
        return g;
    // The chroma plane is half resolution in both axes, so its tiles are half
    // the size and are checked separately against the chroma block shape.
    if (((tile_w / 2) % kGpuBlockW) != 0 || ((tile_h / 2) % kGpuChromaBlockH) != 0)
        return g;

    g.valid  = true;
    g.rows   = kTileRows;
    g.cols   = kTileCols;
    g.tile_w = tile_w;
    g.tile_h = tile_h;
    return g;
}

// How many CUDA blocks span one tile on a plane, and how many rows each thread
// walks. The GPU reduction kernels take this by value and derive their tile from
// blockIdx alone.
//
// Lives here, not in the .cu, so the mapping the kernels use is the same code a
// host test can exercise; the property it has to hold (one block, one tile,
// every tile reachable) is not something a GPU-only definition could be checked
// against without a device.
struct GpuTileMap {
    int blocks_x        = 0;
    int blocks_y        = 0;
    int rows_per_thread = 1;  // consecutive plane rows per thread; a block spans block_h * this
};

// Grid dimensions and tile map for a plane_w x plane_h plane with a block of
// kGpuBlockW x block_h threads.
//
// Exact for any geometry computeTileGeometry() accepted. The ceiling form is
// the fail-safe for a caller that skipped that check: it over-covers rather
// than dropping pixels, which the kernels' bounds guards then absorb.
struct GpuPlaneGrid {
    int grid_x = 0;
    int grid_y = 0;
    GpuTileMap map;
};

inline GpuPlaneGrid gpuPlaneGrid(int plane_w, int plane_h, int block_h) noexcept
{
    const int w  = (plane_w > 0) ? plane_w : 1;
    const int h  = (plane_h > 0) ? plane_h : 1;
    const int bh = (block_h > 0) ? block_h : 1;
    const int tile_w = (w + kTileCols - 1) / kTileCols;
    const int tile_h = (h + kTileRows - 1) / kTileRows;

    // The rows must divide the tile's height in blocks, or a block would
    // straddle a tile row. A tile that is not whole blocks tall (unvalidated
    // geometry) keeps one row per thread.
    int rows = 1;
    if ((tile_h % bh) == 0)
        rows = tile_geometry_detail::largest_divisor(
            tile_h / bh, kGpuMaxRowsPerThread, [](int) { return true; });
    const int span_h = bh * rows;

    GpuPlaneGrid g;
    g.grid_x = (w + kGpuBlockW - 1) / kGpuBlockW;
    g.grid_y = (h + span_h      - 1) / span_h;
    // At least one block per tile even on a degenerate frame, so the kernels'
    // block-to-tile division (tile_of_block in saim_kernels.cu) cannot be by zero.
    g.map.blocks_x = (tile_w + kGpuBlockW - 1) / kGpuBlockW;
    g.map.blocks_y = (tile_h + span_h      - 1) / span_h;
    if (g.map.blocks_x < 1) g.map.blocks_x = 1;
    if (g.map.blocks_y < 1) g.map.blocks_y = 1;
    g.map.rows_per_thread = rows;
    return g;
}

#endif  // SAI_TILE_GEOMETRY_H
