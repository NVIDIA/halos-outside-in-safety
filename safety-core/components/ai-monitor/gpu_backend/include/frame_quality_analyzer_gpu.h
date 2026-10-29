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
 * Orchestrates per-frame GPU analysis: the per-tile sums, Canny edge
 * detection, and the 12 frame metrics from compute_quality_kernel.
 * Manages all GPU buffer allocations for the analysis pipeline.
 */
class FrameQualityAnalyzer : public IFrameQualityAnalyzer {
public:
    FrameQualityAnalyzer() = default;
    ~FrameQualityAnalyzer() override { cleanup(); }

    // Allocates GPU buffers for all intermediate analysis stages.
    bool init(int width, int height, LumaLines lines) override;
    void cleanup() override;

    /*
     * Runs the full analysis pipeline on one decoded frame.
     *
     * @param d_y           Device pointer to the NV12 Y plane.
     * @param y_pitch       Row pitch of the Y plane in bytes.
     * @return              The frame's metrics and tile sums.
     */
    FrameQualityResult analyze(const unsigned char* d_y, int y_pitch) override;

    void setConfig(const ThresholdConfig& cfg) override { cfg_ = cfg; }
    const ThresholdConfig& config() const override { return cfg_; }

    void setMode(RunMode m) override { mode_ = m; }

    // Frame count and has_chroma only; the caller's LearnFilter fills mu/sigma.
    BaselineValues getLearnedBaseline() override;

    uint64_t getLearnFramesProcessed() const override { return learn_frames_processed_; }

    // Current LEARN-input dimensions. Callers use these to detect whether a
    // pending init() will keep or restart LEARN (init() preserves progress only
    // when both dimensions match the current values).
    int getWidth()  const noexcept override { return W_; }
    int getHeight() const noexcept override { return H_; }

    // Per-camera fault-isolation wiring. The report callback emits SENSOR_INVALID
    void setSensorInvalidFlag(std::atomic<bool>* p)  noexcept override { sensor_invalid_flag_  = p; }
    void setPermanentFaultFlag(std::atomic<bool>* p) noexcept override { permanent_fault_flag_ = p; }

    void setReportInvalidCallback(PssReportInvalidFn fn) noexcept override {
        report_invalid_ = std::move(fn);
    }

    CUstream stream() const noexcept override { return stream_; }

private:
    // Gaussian blur -> Scharr gradients -> NMS + double threshold ->
    // hysteresis linking -> per-tile edge pixel counts.
    void runEdgeDetection(const unsigned char* d_y, int y_pitch);

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
    //  - LEARN progress survives a reinit: the rebuild keeps W_/H_, so the
    //    frames on either side are one population. Only init() restarts it,
    //    and only when the stream's resolution changed.
    bool tryReinitPipeline() noexcept;
    void handleFrameFailure(const char* where) noexcept;

    bool initialized_ = false;
    int  W_ = 0, H_ = 0;
    LumaLines lines_{};  // the stream's black / white lines, set together with W_ / H_

    ThresholdConfig    cfg_;
    RunMode           mode_ = RunMode::ACTIVE;

    uint64_t learn_frames_processed_ = 0;  // Frames accepted into LEARN.

    // No previous frame on the device yet: set by every allocation, cleared
    // by the first frame that completes.
    bool first_ = true;

    // GPU buffers for the analysis pipeline.
    void* d_acc_         = nullptr;  // GpuTileAccum: every per-frame accumulator, one memset per frame.
    void* d_result_      = nullptr;  // Single FrameQualityResult.
    void* d_blurred_     = nullptr;  // Gaussian-blurred Y plane.
    void* d_grad_mag_    = nullptr;  // Scharr gradient magnitude (int16).
    void* d_grad_dir_    = nullptr;  // Quantised gradient direction (uint8, 0-3).
    void* d_edges_       = nullptr;  // NMS + threshold output (uint8).
    void* d_hyst_labels_ = nullptr;  // Hysteresis union-find parent keys (uint32).
    void* d_prev_        = nullptr;  // Two W x H planes of Y (motion): the previous frame, and this one being written.
    unsigned int prev_plane_ = 0U;   // Which plane holds the previous frame (0 or 1); flips when a frame completes.
    void* d_bandA_       = nullptr;  // W x (H + 2 kBandPad) floats: band scratch, rows blurred at sigma 1.
    void* d_bandB_       = nullptr;  // W x (H + 2 kBandPad) floats: band scratch, rows blurred at sigma 4.
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
