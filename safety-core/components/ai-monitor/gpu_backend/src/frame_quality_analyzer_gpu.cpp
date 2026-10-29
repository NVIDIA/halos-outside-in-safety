/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "frame_quality_analyzer_gpu.h"
#include "saim_kernels.h"
#include "sai_common.h"
#include "tile_geometry.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <climits>
#include <cuda_runtime.h>

namespace sai { namespace gpu {

bool FrameQualityAnalyzer::allocateGpuResources() noexcept
{
    size_t total = (size_t)W_ * (size_t)H_;
    const size_t plane = (size_t)W_ * (size_t)(H_ + 2 * kBandPad) * sizeof(float);

    if (cudaMalloc(&d_acc_,         sizeof(GpuTileAccum))       != cudaSuccess ||
        cudaMalloc(&d_result_,      sizeof(FrameQualityResult)) != cudaSuccess ||
        cudaMalloc(&d_blurred_,     total)                      != cudaSuccess ||
        cudaMalloc(&d_grad_mag_,    total * sizeof(int16_t))    != cudaSuccess ||
        cudaMalloc(&d_grad_dir_,    total)                      != cudaSuccess ||
        cudaMalloc(&d_edges_,       total)                      != cudaSuccess ||
        cudaMalloc(&d_hyst_labels_, total * sizeof(uint32_t))   != cudaSuccess ||
        cudaMalloc(&d_prev_,        2 * total)                  != cudaSuccess ||
        cudaMalloc(&d_bandA_,       plane)                      != cudaSuccess ||
        cudaMalloc(&d_bandB_,       plane)                      != cudaSuccess ||
        !upload_band_weights())
    {
        std::cerr << "[QA] GPU buffer allocation failed\n";
        cleanup();
        return false;
    }

    // Fresh device memory holds no previous frame to compare against.
    first_ = true;

    // Pinned host buffer so cudaMemcpyAsync DMAs directly without a pageable
    // staging copy.
    if (cudaMallocHost(reinterpret_cast<void**>(&h_result_),
                       sizeof(FrameQualityResult)) != cudaSuccess) {
        std::cerr << "[QA] cudaMallocHost for h_result_ failed\n";
        cleanup();
        return false;
    }

    cudaError_t streamErr = cudaStreamCreate(&stream_);
    if (streamErr != cudaSuccess) {
        std::cerr << "[QA-GPU] cudaStreamCreate failed: "
                  << cudaGetErrorString(streamErr) << "\n";
        cleanup();
        return false;
    }

    initialized_ = true;
    return true;
}

bool FrameQualityAnalyzer::init(int width, int height, LumaLines lines)
{
    NVTX_RANGE("InitAnalyzerBuffers", 0xFFCC8800);

    // Validate args first — a malformed SPS must not wipe a working analyzer.
    if (width <= 0 || height <= 0 || width > MAX_DIM || height > MAX_DIM) {
        std::cerr << "[QA] Invalid dimensions: " << width << "x" << height
                  << " (must be 1.." << MAX_DIM << ")\n";
        return false;
    }
    if (!lumaLinesValid(lines)) {
        std::cerr << "[QA] Invalid luma lines: black " << lines.dark << ", white " << lines.bright << "\n";
        return false;
    }

    // The grid must divide the frame evenly and leave every CUDA block inside a
    // single tile. There is no approximation to fall back on: a block spanning
    // two tiles would credit its pixels to whichever tile it flushed to, so an
    // incompatible resolution is refused here rather than scored wrongly.
    const TileGeometry geom = computeTileGeometry(width, height);
    if (!geom.valid) {
        std::cerr << "[QA] " << width << "x" << height
                  << " is not compatible with the " << kTileRows << "x" << kTileCols
                  << " analysis grid\n";
        return false;
    }
    // The hysteresis counts edges per Canny block, so a tile must hold whole blocks.
    if (geom.tile_w % kCannyBlockPxW != 0 || geom.tile_h % kCannyBlockPxH != 0) {
        std::cerr << "[QA] " << width << "x" << height << " tiles are not whole multiples of the "
                  << kCannyBlockPxW << "x" << kCannyBlockPxH << " edge-detection block\n";
        return false;
    }
    // The band's column step sums whole runs of kBandColRows rows of one tile.
    if (geom.tile_h % kBandColRows != 0) {
        std::cerr << "[QA] " << width << "x" << height << " tiles are not a whole multiple of "
                  << kBandColRows << " rows high\n";
        return false;
    }

    consecutive_errors_ = 0U;

    // LEARN-only: same-dim re-entry is a no-op so a mid-LEARN reconnect does
    // not reset LEARN progress; the luma lines stay the ones LEARN started on.
    if (initialized_
        && mode_ == RunMode::LEARN
        && width == W_ && height == H_) {
        return true;
    }

    // Different dimensions: restarting LEARN is unavoidable because the
    // histogram and edge stats are resolution-dependent.
    if (initialized_ && mode_ == RunMode::LEARN && learn_frames_processed_ > 0) {
        std::cerr << "[QA] WARNING: stream parameters changed mid-LEARN ("
                  << W_ << "x" << H_ << " -> " << width << "x" << height
                  << "). Discarding " << learn_frames_processed_
                  << " accumulated frames; restarting baseline accumulation.\n";
    }

    // Not left to cleanup(): a failed tryReinitPipeline() leaves the count set
    // with initialized_ false, and the caller reads a count that did not drop as
    // "LEARN continues" and keeps the previous resolution's frames.
    learn_frames_processed_ = 0;

    if (initialized_) cleanup();

    W_ = width;  H_ = height;
    lines_ = lines;

    if (!allocateGpuResources()) {
        return false;
    }

#ifdef DEBUG
    std::cout << "[QA-GPU] Analyzer initialized (" << W_ << "x" << H_ << ")\n";
#endif
    return true;
}

bool FrameQualityAnalyzer::tryReinitPipeline() noexcept
{
    if (mode_ == RunMode::LEARN && learn_frames_processed_ > 0) {
        std::cerr << "[QA] reinit during LEARN; keeping "
                  << learn_frames_processed_ << " accepted frames\n";
    }

    // Rebuilds at the unchanged W_/H_, so frames either side are one population.
    // Carried over cleanup(), which zeroes the count for init()'s config-change
    // restart - the only path where discarding is correct.
    const uint64_t learn_frames = learn_frames_processed_;

    cleanup();
    const bool allocated = allocateGpuResources();
    // Restored on failure too, so a dying stream is not read as a LEARN restart.
    learn_frames_processed_ = learn_frames;
    return allocated;
}

void FrameQualityAnalyzer::handleFrameFailure(const char* where) noexcept
{
    std::cerr << "[QA] frame error in " << where
              << " (consecutive=" << (consecutive_errors_ + 1U)
              << "/" << kMaxConsecutiveAnalyzerErrors << ")\n";

    ++consecutive_errors_;
    if (consecutive_errors_ < kMaxConsecutiveAnalyzerErrors) {
        return;
    }
    // Edge-trigger sensor_invalid_ so only the first producer reports SENSOR_INVALID
    bool gainedInvalidEdge = true;
    if (sensor_invalid_flag_ != nullptr) {
        bool expected = false;
        gainedInvalidEdge = sensor_invalid_flag_->compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }

    // One report_invalid_ call per incident: gates the threshold-trip and
    // permanent-fault emits so the same SENSOR_INVALID is not raised twice.
    bool alreadyReported = false;

    auto markPermanentFault = [this, gainedInvalidEdge, &alreadyReported](const char* reason) {
        if (gainedInvalidEdge && !alreadyReported && report_invalid_) {
            report_invalid_(reason);
            alreadyReported = true;
        }
        if (permanent_fault_flag_ != nullptr) {
            permanent_fault_flag_->store(true, std::memory_order_release);
        }
    };

    if (reinit_attempts_ >= kMaxAnalyzerReinitAttempts) {
        std::cerr << "[QA] reinit budget exhausted ("
                  << reinit_attempts_ << "/" << kMaxAnalyzerReinitAttempts
                  << "); flagging permanent fault for this stream\n";
        markPermanentFault("frame analyzer permanent fault — reinit budget exhausted");
        return;
    }

    if (gainedInvalidEdge && report_invalid_) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "frame analyzer threshold tripped — %u consecutive errors",
                 kMaxConsecutiveAnalyzerErrors);
        report_invalid_(msg);
        alreadyReported = true;
    }

    ++reinit_attempts_;
    std::cerr << "[QA] threshold tripped; analyzer reinit ("
              << reinit_attempts_ << "/" << kMaxAnalyzerReinitAttempts << ")\n";

    // Recoverable errors come back here on success; unrecoverable CUDA errors
    // make tryReinitPipeline() fail and we flag the permanent fault below.
    if (tryReinitPipeline()) {
        consecutive_errors_ = 0U;
        std::cerr << "[QA] reinit succeeded; resuming\n";
        return;
    }

    std::cerr << "[QA] reinit failed; flagging permanent fault for this stream\n";
    markPermanentFault("frame analyzer permanent fault — reinit failed");
}

void FrameQualityAnalyzer::cleanup() {
    NVTX_RANGE("CleanupAnalyzer", 0xFF880000);
    auto F = [](void*& p) {
        if (p) {
            cudaError_t err = cudaFree(p);
            if (err != cudaSuccess)
                std::cerr << "[QA-GPU] cudaFree failed: " << cudaGetErrorString(err) << "\n";
            p = nullptr;
        }
    };
    if (stream_) {
        cudaError_t serr = cudaStreamSynchronize(stream_);
        if (serr != cudaSuccess) {
            std::cerr << "[QA-GPU] cudaStreamSynchronize on cleanup failed: "
                      << cudaGetErrorString(serr) << "\n";
        }
        cudaError_t derr = cudaStreamDestroy(stream_);
        if (derr != cudaSuccess) {
            std::cerr << "[QA-GPU] cudaStreamDestroy failed: "
                      << cudaGetErrorString(derr) << "\n";
        }
        stream_ = nullptr;
    }
    F(d_acc_); F(d_result_);
    F(d_blurred_); F(d_grad_mag_); F(d_grad_dir_); F(d_edges_); F(d_hyst_labels_);
    F(d_prev_); F(d_bandA_); F(d_bandB_);

    if (h_result_) {
        cudaError_t err = cudaFreeHost(h_result_);
        if (err != cudaSuccess)
            std::cerr << "[QA-GPU] cudaFreeHost failed: " << cudaGetErrorString(err) << "\n";
        h_result_ = nullptr;
    }

    learn_frames_processed_ = 0;
    initialized_ = false;
}

FrameQualityResult FrameQualityAnalyzer::analyze(const unsigned char* d_y, int y_pitch)
{
    NVTX_RANGE("FrameAnalyze", 0xFF00FF00);
    FrameQualityResult res{};

    // Decoder polls permanent_fault_flag_ and exits at the next NAL boundary;
    // until then short-circuit to avoid relaunching kernels that will fail.
    if (permanent_fault_flag_ != nullptr &&
        permanent_fault_flag_->load(std::memory_order_acquire)) {
        return res;
    }

    if (!initialized_) return res;
    // The CbCr plane is read as byte pairs, which need an even address: an odd
    // pitch or base would fault and poison the CUDA context every camera shares.
    if (!d_y || y_pitch < W_ || (y_pitch % 2) != 0 ||
        (reinterpret_cast<uintptr_t>(d_y) % 2U) != 0U) {
        std::cerr << "[QA-GPU] Invalid frame surface: d_y="
                  << (const void*)d_y << " y_pitch=" << y_pitch
                  << " W_=" << W_ << "\n";
        handleFrameFailure("invalid frame surface");
        return res;
    }

    GpuTileAccum* acc = (GpuTileAccum*)d_acc_;
    const int first = first_ ? 1 : 0;
    if (cudaMemsetAsync(acc, 0, sizeof(GpuTileAccum), stream_) != cudaSuccess) {
        handleFrameFailure("accumulator clear");
        return res;
    }

    runEdgeDetection(d_y, y_pitch);

    // NV12 chroma plane immediately follows the luma plane at pitch*H.
    // cw = W/2 pairs per row, ch = H/2 rows; same pitch as luma.
    {
        NVTX_RANGE("ChromaStats", 0xFF00AAFF);
        const unsigned char* d_uv = d_y + (size_t)y_pitch * (size_t)H_;
        launch_chroma_stats(d_uv, y_pitch, W_ / 2, H_ / 2, acc, stream_);
    }
    {
        NVTX_RANGE("LumaStats", 0xFF00CCFF);
        unsigned char* planes = (unsigned char*)d_prev_;
        const size_t px = (size_t)W_ * (size_t)H_;
        launch_luma_stats(d_y, y_pitch, W_, H_, planes + prev_plane_ * px, planes + (prev_plane_ ^ 1) * px,
                          first, lines_, acc, stream_);
    }
    {
        NVTX_RANGE("Band", 0xFF00DDFF);
        launch_fine_band(d_y, y_pitch, W_, H_, (float*)d_bandA_, (float*)d_bandB_, acc, stream_);
    }
    {
        NVTX_RANGE("QualityMetrics", 0xFF00FFAA);
        launch_quality_metrics(acc, W_ * H_, (W_ / 2) * (H_ / 2),
                               (FrameQualityResult*)d_result_, stream_);
    }

    // Non-blocking launch-error probe. Async kernel errors are surfaced by
    // the cudaStreamSynchronize below. All errors (transient or sticky) go
    // through handleFrameFailure's 30-frame budget: transient ones clear and
    // we resume; sticky ones persist, threshold trips, tryReinitPipeline()
    // fails, and we escalate to permanent fault.
    {
        cudaError_t lastErr = cudaGetLastError();
        if (lastErr != cudaSuccess) {
            std::cerr << "[QA-GPU] Kernel launch error: "
                      << cudaGetErrorString(lastErr) << "\n";
            handleFrameFailure("kernel launch (cudaGetLastError)");
            return res;
        }
    }

    // One D2H + sync per frame: cudaMemcpyAsync is ordered after the kernel
    // on the same stream, and the post-copy sync observes both kernel and
    // copy errors. The pinned h_result_ buffer lets cudaMemcpyAsync DMA
    // directly without a pageable staging copy.
    {
        NVTX_RANGE("ResultD2HCopy", 0xFFFF00FF);
        cudaError_t err = cudaMemcpyAsync(h_result_, d_result_,
                                          sizeof(FrameQualityResult),
                                          cudaMemcpyDeviceToHost, stream_);
        if (err != cudaSuccess) {
            std::cerr << "[QA-GPU] cudaMemcpyAsync result D2H failed: "
                      << cudaGetErrorString(err) << "\n";
            handleFrameFailure("cudaMemcpyAsync result D2H");
            return res;
        }
        cudaError_t syncErr = cudaStreamSynchronize(stream_);
        if (syncErr != cudaSuccess) {
            std::cerr << "[QA-GPU] GPU pipeline error at result sync: "
                      << cudaGetErrorString(syncErr) << "\n";
            handleFrameFailure("cudaStreamSynchronize");
            return res;
        }
        res = *h_result_;
        // The plane just written holds this frame, the previous one from now on.
        first_ = false;
        prev_plane_ ^= 1U;
    }

    // The CUDA checks above only cover faults the driver reported; an unwritten
    // reduction or corrupt copy returns cudaSuccess with a NaN or a negative in it.
    // The tile sums are checked by tile_numbers() where they are used.
    bool finite_ok = true;
    for (int m = 0; m < QM_COUNT; ++m)
        finite_ok = finite_ok && sai_metrics_valid({res.raw[m]});
    if (!finite_ok) {
        std::cerr << "[QA-GPU] non-finite or negative value in kernel result\n";
        handleFrameFailure("invalid value in kernel result");
        return FrameQualityResult{};
    }

    // A corrupt edge count stays finite, so the check above misses it. Its
    // hard bound: no more edges than pixels.
    if (res.raw[QM_EDGE_DENSITY] > 1.f) {
        std::cerr << "[QA-GPU] edge density out of range\n";
        handleFrameFailure("value out of range in kernel result");
        return FrameQualityResult{};
    }

    consecutive_errors_ = 0U;

    if (mode_ == RunMode::LEARN) {
        ++learn_frames_processed_;
    }

    res.valid = true;
    return res;
}

BaselineValues FrameQualityAnalyzer::getLearnedBaseline() {
    // Frame count and has_chroma only. The mu/sigma fields stay zero here and
    // are filled by the caller's LearnFilter::finalize(), which recomputes them
    // from the retained per-frame metrics after outlier rejection. total_frames
    // is what gates that call, so it must be exact.
    BaselineValues b{};
    if (learn_frames_processed_ == 0U) return b;

    b.has_chroma   = true;
    b.total_frames = (learn_frames_processed_ <= static_cast<uint64_t>(INT_MAX))
                   ? static_cast<int>(learn_frames_processed_)
                   : INT_MAX;
    return b;
}

void FrameQualityAnalyzer::runEdgeDetection(const unsigned char* d_y,
                                            int y_pitch)
{
    NVTX_RANGE("EdgeDetectionPipeline", 0xFFFF8800);
    launch_edge_detection(
        d_y, y_pitch, W_, H_,
        d_blurred_, d_grad_mag_, d_grad_dir_,
        d_edges_, d_hyst_labels_,
        cfg_.canny_low_thresh, cfg_.canny_high_thresh,
        (GpuTileAccum*)d_acc_, stream_);
}

}}  // namespace sai::gpu
