/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FRAME_QUALITY_ANALYZER_H
#define FRAME_QUALITY_ANALYZER_H

#include <cuda.h>
#include <cuda_runtime.h>
#include "sai_common.h"

constexpr int MAX_DIM = 16384; // Max dimensions (16Kx16K) for the analyzer.

/*
 * Orchestrates per-frame GPU quality analysis: histogram computation,
 * Canny edge detection pipeline, and final scoring via compute_quality_kernel.
 * Manages all GPU buffer allocations for the analysis pipeline.
 */
class FrameQualityAnalyzer {
public:
    FrameQualityAnalyzer() = default;
    ~FrameQualityAnalyzer() { cleanup(); }

    // Allocates GPU buffers for all intermediate analysis stages.
    // @param nvdec_hist  True if NVDEC hardware histogram is available (avoids fallback kernel).
    // @param hw_hist_bins Number of HW histogram bins from decodeCaps (0 if unavailable).
    bool init(int width, int height, bool nvdec_hist, int hw_hist_bins = 0);
    void cleanup();

    /*
     * Runs the full analysis pipeline on one decoded frame.
     *
     * @param d_y           Device pointer to the NV12 Y plane.
     * @param y_pitch       Row pitch of the Y plane in bytes.
     * @param dpHistogram   NVDEC hardware histogram (0 if unavailable).
     * @param hist_bins     Number of bins in the hardware histogram.
     * @param counter_bytes Size of each histogram counter (4 or 8 bytes).
     * @return              Per-frame quality scores.
     */
    FrameQualityResult analyze(
        const unsigned char* d_y, int y_pitch,
        CUdeviceptr dpHistogram, int hist_bins, int counter_bytes);

    void setConfig(const ThresholdConfig& cfg) { cfg_ = cfg; syncParams(); }
    const ThresholdConfig& config() const { return cfg_; }

    void setMode(RunMode m) { mode_ = m; syncParams(); }
    void setBaseline(const BaselineValues& b) { baseline_ = b; syncParams(); }

    // Copies the GPU learn accumulator to host and computes averaged baseline values.
    BaselineValues getLearnedBaseline();

private:
    // Returns a device pointer to the histogram to use for scoring.
    // When the HW histogram is 32-bit, returns it directly (zero-copy);
    // otherwise converts into d_hist_ or runs the fallback kernel.
    // out_bins is set to the actual number of bins produced (256 for fallback).
    const unsigned int* prepareHistogram(const unsigned char* d_y, int y_pitch,
                                        CUdeviceptr dpHistogram, int hist_bins,
                                        int counter_bytes, int& out_bins);
    // Runs the full Canny-style edge pipeline: Gaussian blur -> Scharr gradients ->
    // NMS + double threshold -> hysteresis linking -> edge pixel count.
    void runEdgeDetection(const unsigned char* d_y, int y_pitch);
    // Copies current config, baseline, and mode into the flat GPU params struct.
    void syncParams();

    bool initialized_ = false;
    int  W_ = 0, H_ = 0;
    bool use_nvdec_hist_ = false;  // True when NVDEC provides a hardware histogram.

    ThresholdConfig    cfg_;
    RunMode           mode_ = RunMode::ACTIVE;
    BaselineValues    baseline_{};
    GpuScoringParams  gpu_params_{};  // Host-side mirror, passed by value to kernel.

    int   hist_bins_     = 256;       // Actual histogram bin count (from HW or fallback).

    // GPU buffers for the analysis pipeline.
    void* d_hist_        = nullptr;  // N-bin histogram (uint32), sized to hist_bins_.
    void* d_result_      = nullptr;  // Single FrameQualityResult.
    void* d_learn_accum_ = nullptr;  // Single GpuLearnAccum.
    void* d_blurred_     = nullptr;  // Gaussian-blurred Y plane (also reused as hysteresis output).
    void* d_grad_mag_    = nullptr;  // Scharr gradient magnitude (int16).
    void* d_grad_dir_    = nullptr;  // Quantised gradient direction (uint8, 0-3).
    void* d_edges_       = nullptr;  // NMS + threshold output (uint8).
    void* d_edgeCount_   = nullptr;  // Scalar edge pixel count (uint32).
    // Pinned host buffer for the per-frame D2H result copy. cudaMallocHost lets
    // cudaMemcpyAsync do a direct DMA transfer instead of routing through a
    // pageable staging buffer, which removes a per-frame host-side copy.
    FrameQualityResult* h_result_ = nullptr;
    cudaStream_t stream_ = nullptr;  // Per-pipeline CUDA stream for kernel isolation.
};

#endif
