/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FRAME_QUALITY_ANALYZER_GPU_H
#define FRAME_QUALITY_ANALYZER_GPU_H

#include <cuda.h>
#include <cuda_runtime.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

#include "i_frame_quality_analyzer.h"
#include "sai_common.h"

namespace sai { namespace gpu {

/*
 * Orchestrates per-frame GPU quality analysis: histogram computation,
 * Canny edge detection pipeline, and final scoring via compute_quality_kernel.
 * Manages all GPU buffer allocations for the analysis pipeline.
 */
class FrameQualityAnalyzer : public IFrameQualityAnalyzer {
public:
    FrameQualityAnalyzer() = default;
    ~FrameQualityAnalyzer() override { cleanup(); }

    // Allocates GPU buffers for all intermediate analysis stages.
    // @param nvdec_hist  True if NVDEC hardware histogram is available (avoids fallback kernel).
    // @param hw_hist_bins Number of HW histogram bins from decodeCaps (0 if unavailable).
    bool init(int width, int height, bool nvdec_hist, int hw_hist_bins = 0) override;
    void cleanup() override;

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
        CUdeviceptr dpHistogram, int hist_bins, int counter_bytes) override;

    void setConfig(const ThresholdConfig& cfg) override { cfg_ = cfg; syncParams(); }
    const ThresholdConfig& config() const override { return cfg_; }

    void setMode(RunMode m) override { mode_ = m; syncParams(); }
    void setBaseline(const BaselineValues& b) override { baseline_ = b; syncParams(); }

    // Copies the GPU learn accumulator to host and computes averaged baseline values.
    BaselineValues getLearnedBaseline() override;

    // Host mirror of GpuLearnAccum::frame_count; avoids a per-frame GPU sync
    // in the LEARN-stop path.
    uint64_t getLearnFramesProcessed() const override { return learn_frames_processed_; }

    // Current LEARN-input dimensions. Callers use these to detect whether a
    // pending init() will keep or wipe the accumulator (init() preserves it
    // only when both dimensions match the current values).
    int getWidth()  const noexcept override { return W_; }
    int getHeight() const noexcept override { return H_; }

    // Per-camera fault-isolation wiring. The report callback emits SENSOR_INVALID
    void setSensorInvalidFlag(std::atomic<bool>* p)  noexcept override { sensor_invalid_flag_  = p; }
    void setPermanentFaultFlag(std::atomic<bool>* p) noexcept override { permanent_fault_flag_ = p; }

    void setReportInvalidCallback(PssReportInvalidFn fn) noexcept override {
        report_invalid_ = std::move(fn);
    }

private:
    // Returns a device pointer to the histogram for scoring: HW 32-bit
    // histogram passed through, HW 64-bit converted into d_hist_, or the
    // fallback kernel run into d_hist_. out_bins is set to actual bin count.
    const unsigned int* prepareHistogram(const unsigned char* d_y, int y_pitch,
                                        CUdeviceptr dpHistogram, int hist_bins,
                                        int counter_bytes, int& out_bins);
    // Gaussian blur -> Scharr gradients -> NMS + double threshold ->
    // hysteresis linking -> edge pixel count.
    void runEdgeDetection(const unsigned char* d_y, int y_pitch);
    void syncParams();

    // Shared allocation path used by both init() and tryReinitPipeline().
    // Neither path resets reinit_attempts_; only the constructor's in-class
    // initializer sets it, preserving the lifetime cap across reconnects.
    bool allocateGpuResources() noexcept;

    // Frame-failure recovery contract:
    //  - Transient host-side and CUDA errors are absorbed by the 30-frame
    //    consecutive-error budget; a clean frame resets it.
    //  - On threshold trip, tryReinitPipeline() rebuilds GPU buffers and
    //    succeeds for recoverable failures (resume) or fails for
    //    unrecoverable CUDA errors that poisoned the CUcontext (escalate
    //    to permanent fault for this stream).
    bool tryReinitPipeline() noexcept;
    void handleFrameFailure(const char* where) noexcept;

    bool initialized_ = false;
    int  W_ = 0, H_ = 0;
    bool use_nvdec_hist_ = false;  // True when NVDEC provides a hardware histogram.

    ThresholdConfig    cfg_;
    RunMode           mode_ = RunMode::ACTIVE;
    BaselineValues    baseline_{};
    GpuScoringParams  gpu_params_{};  // Host-side mirror, passed by value to kernel.

    int   hist_bins_     = kSwHistogramBinCount; // Actual histogram bin count (from HW or fallback).
    uint64_t learn_frames_processed_ = 0;  // Host mirror of GpuLearnAccum::frame_count.

    // GPU buffers for the analysis pipeline.
    void* d_hist_        = nullptr;  // N-bin histogram (uint32), sized to hist_bins_.
    void* d_result_      = nullptr;  // Single FrameQualityResult.
    void* d_learn_accum_ = nullptr;  // Single GpuLearnAccum.
    void* d_blurred_     = nullptr;  // Gaussian-blurred Y plane (also reused as hysteresis output).
    void* d_grad_mag_    = nullptr;  // Scharr gradient magnitude (int16).
    void* d_grad_dir_    = nullptr;  // Quantised gradient direction (uint8, 0-3).
    void* d_edges_       = nullptr;  // NMS + threshold output (uint8).
    void* d_edgeCount_   = nullptr;  // Scalar edge pixel count (uint32).
    void* d_chroma_sums_ = nullptr;  // 6 uint64 chroma sums (Cb/Cr/sat sum+sumsq).
    void* d_lap_sums_    = nullptr;  // 2 uint64 Laplacian sums [sum(L) bits, sum(L^2)].
    // Pinned host buffer for the per-frame D2H result copy. cudaMallocHost lets
    // cudaMemcpyAsync do a direct DMA transfer instead of routing through a
    // pageable staging buffer, which removes a per-frame host-side copy.
    FrameQualityResult* h_result_ = nullptr;
    cudaStream_t stream_ = nullptr;  // Per-pipeline CUDA stream for kernel isolation.

    // consecutive_errors_ resets on every successful frame, after a successful
    // reinit, and on init() (new session). reinit_attempts_ is lifetime-bounded:
    // only the in-class initializer below sets it; no other path resets it, so
    // the budget cannot refresh across reconnects.
    unsigned int consecutive_errors_ = 0U;
    unsigned int reinit_attempts_    = 0U;
    std::atomic<bool>* sensor_invalid_flag_  = nullptr;
    std::atomic<bool>* permanent_fault_flag_ = nullptr;
    PssReportInvalidFn report_invalid_;
};

}}  // namespace sai::gpu

#endif  // FRAME_QUALITY_ANALYZER_GPU_H
