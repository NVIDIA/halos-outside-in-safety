/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "frame_quality_analyzer_gpu.h"
#include "saim_kernels.h"

#include <cstdio>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <climits>
#include <cuda_runtime.h>

namespace sai { namespace gpu {

bool FrameQualityAnalyzer::allocateGpuResources() noexcept
{
    // Floor the allocation at kSwHistogramBinCount so the SW fallback kernel
    // (which always emits that many bins) never overruns d_hist_ when NVDEC's
    // HW histogram is unavailable or reports fewer bins.
    int hist_alloc = (hist_bins_ > kSwHistogramBinCount) ? hist_bins_ : kSwHistogramBinCount;
    size_t total   = (size_t)W_ * (size_t)H_;

    if (cudaMalloc(&d_hist_, hist_alloc * sizeof(unsigned int)) != cudaSuccess ||
        cudaMalloc(&d_result_, sizeof(FrameQualityResult)) != cudaSuccess ||
        cudaMalloc(&d_learn_accum_, sizeof(GpuLearnAccum)) != cudaSuccess ||
        cudaMalloc(&d_blurred_,   total)                    != cudaSuccess ||
        cudaMalloc(&d_grad_mag_,  total * sizeof(int16_t))  != cudaSuccess ||
        cudaMalloc(&d_grad_dir_,  total)                    != cudaSuccess ||
        cudaMalloc(&d_edges_,     total)                    != cudaSuccess ||
        cudaMalloc(&d_edgeCount_, sizeof(uint32_t))         != cudaSuccess ||
        cudaMalloc(&d_chroma_sums_, 6 * sizeof(unsigned long long)) != cudaSuccess ||
        cudaMalloc(&d_lap_sums_,    2 * sizeof(unsigned long long)) != cudaSuccess)
    {
        std::cerr << "[QA] GPU buffer allocation failed\n";
        cleanup();
        return false;
    }

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

    cudaError_t err = cudaMemsetAsync(d_learn_accum_, 0,
                                      sizeof(GpuLearnAccum), stream_);
    if (err != cudaSuccess) {
        std::cerr << "[QA-GPU] cudaMemsetAsync learn_accum failed: "
                  << cudaGetErrorString(err) << "\n";
        cleanup();
        return false;
    }

    initialized_ = true;
    return true;
}

bool FrameQualityAnalyzer::init(int width, int height, bool nvdec_hist,
                                int hw_hist_bins)
{
    NVTX_RANGE("InitAnalyzerBuffers", 0xFFCC8800);

    // Validate args first — a malformed SPS must not wipe a working analyzer.
    if (width <= 0 || height <= 0 || width > MAX_DIM || height > MAX_DIM) {
        std::cerr << "[QA] Invalid dimensions: " << width << "x" << height
                  << " (must be 1.." << MAX_DIM << ")\n";
        return false;
    }

    // Normalize once: (nvdec_hist=true, hw_hist_bins=0) is a real NVDEC caps
    // outcome that, compared raw, would mismatch the stored SW-fallback bin
    // count and wipe in-progress LEARN on a same-config reconnect. Use the
    // effective values for both the re-entry check and the store below.
    const bool effective_nvdec_hist = (nvdec_hist && hw_hist_bins > 0);
    int        effective_hist_bins  = kSwHistogramBinCount;
    if (effective_nvdec_hist) {
        effective_hist_bins = hw_hist_bins;
        if (effective_hist_bins > kMaxHistBins) {
            std::cerr << "[QA] WARNING: hw_hist_bins=" << hw_hist_bins
                      << " exceeds supported max " << kMaxHistBins
                      << "; clamping bins to " << kMaxHistBins << "\n";
            effective_hist_bins = kMaxHistBins;
        }
    }
    consecutive_errors_ = 0U;

    // LEARN-only: same-dim re-entry is a no-op so a mid-LEARN reconnect does
    // not wipe the GPU learn accumulator.
    if (initialized_
        && mode_ == RunMode::LEARN
        && width == W_ && height == H_
        && effective_nvdec_hist == use_nvdec_hist_
        && effective_hist_bins  == hist_bins_) {
        return true;
    }

    // Different dimensions/hist setup: dropping the accumulator is unavoidable
    // because histogram and edge stats are resolution-dependent.
    if (initialized_ && mode_ == RunMode::LEARN && learn_frames_processed_ > 0) {
        std::cerr << "[QA] WARNING: stream parameters changed mid-LEARN ("
                  << W_ << "x" << H_ << " -> " << width << "x" << height
                  << "). Discarding " << learn_frames_processed_
                  << " accumulated frames; restarting baseline accumulation.\n";
    }

    if (initialized_) cleanup();  // also resets learn_frames_processed_

    W_ = width;  H_ = height;
    use_nvdec_hist_ = effective_nvdec_hist;
    hist_bins_      = effective_hist_bins;

    if (!allocateGpuResources()) {
        return false;
    }

#ifdef DEBUG
    std::cout << "[QA-GPU] Analyzer initialized (" << W_ << "x" << H_
              << ") - GPU scoring enabled\n";
#endif
    return true;
}

bool FrameQualityAnalyzer::tryReinitPipeline() noexcept
{
    if (mode_ == RunMode::LEARN && learn_frames_processed_ > 0) {
        std::cerr << "[QA] reinit during LEARN; discarding "
                  << learn_frames_processed_ << " accumulated frames\n";
    }

    cleanup();  // also resets learn_frames_processed_
    return allocateGpuResources();
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
    F(d_hist_); F(d_result_); F(d_learn_accum_);
    F(d_blurred_); F(d_grad_mag_); F(d_grad_dir_); F(d_edges_); F(d_edgeCount_);
    F(d_chroma_sums_);
    F(d_lap_sums_);
    if (h_result_) {
        cudaError_t err = cudaFreeHost(h_result_);
        if (err != cudaSuccess)
            std::cerr << "[QA-GPU] cudaFreeHost failed: " << cudaGetErrorString(err) << "\n";
        h_result_ = nullptr;
    }

    learn_frames_processed_ = 0;
    initialized_ = false;
}

FrameQualityResult FrameQualityAnalyzer::analyze(
    const unsigned char* d_y, int y_pitch,
    CUdeviceptr dpHistogram, int hist_bins, int counter_bytes)
{
    NVTX_RANGE("FrameAnalyze", 0xFF00FF00);
    FrameQualityResult res{};

    // Decoder polls permanent_fault_flag_ and exits at the next NAL boundary;
    // until then short-circuit to avoid relaunching kernels that will fail.
    if (permanent_fault_flag_ != nullptr &&
        permanent_fault_flag_->load(std::memory_order_acquire)) {
        return res;
    }

    if (!initialized_) { res.overall_confidence = 50.f; return res; }
    if (!d_y || y_pitch < W_) {
        std::cerr << "[QA-GPU] Invalid frame surface: d_y="
                  << (const void*)d_y << " y_pitch=" << y_pitch
                  << " W_=" << W_ << "\n";
        handleFrameFailure("invalid frame surface");
        return res;
    }

    int statsBins = 0;
    const unsigned int* hist_ptr = prepareHistogram(
        d_y, y_pitch, dpHistogram, hist_bins, counter_bytes, statsBins);
    runEdgeDetection(d_y, y_pitch);

    // NV12 chroma plane immediately follows the luma plane at pitch*H.
    // cw = W/2 pairs per row, ch = H/2 rows; same pitch as luma.
    {
        NVTX_RANGE("ChromaStats", 0xFF00AAFF);
        const unsigned char* d_uv = d_y + (size_t)y_pitch * (size_t)H_;
        launch_chroma_stats(d_uv, y_pitch, W_ / 2, H_ / 2,
                            (unsigned long long*)d_chroma_sums_, stream_);
    }

    // Laplacian variance (sharpness) reduces the raw Y plane to sum(L)/sum(L^2).
    {
        NVTX_RANGE("LaplacianStats", 0xFF00CCFF);
        launch_laplacian_stats(d_y, y_pitch, W_, H_,
                               (unsigned long long*)d_lap_sums_, stream_);
    }

    {
        NVTX_RANGE("QualityScoring", 0xFF00FFAA);
        launch_quality_scoring(
            hist_ptr,
            (const uint32_t*)d_edgeCount_,
            (const unsigned long long*)d_chroma_sums_,
            (const unsigned long long*)d_lap_sums_,
            gpu_params_,
            (FrameQualityResult*)d_result_,
            (mode_ == RunMode::LEARN) ? (GpuLearnAccum*)d_learn_accum_ : nullptr,
            statsBins,
            stream_);
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
    }

    consecutive_errors_ = 0U;

    if (mode_ == RunMode::LEARN) {
        ++learn_frames_processed_;
    }

    res.valid = true;
    return res;
}

BaselineValues FrameQualityAnalyzer::getLearnedBaseline() {
    BaselineValues b;
    if (!d_learn_accum_) return b;

    GpuLearnAccum accum;
    cudaError_t err = cudaMemcpyAsync(&accum, d_learn_accum_, sizeof(GpuLearnAccum),
                                      cudaMemcpyDeviceToHost, stream_);
    if (err != cudaSuccess) {
        std::cerr << "[QA-GPU] cudaMemcpyAsync learn_accum D2H failed: "
                  << cudaGetErrorString(err) << "\n";
        return b;
    }
    cudaError_t syncErr = cudaStreamSynchronize(stream_);
    if (syncErr != cudaSuccess) {
        std::cerr << "[QA-GPU] cudaStreamSynchronize learn_accum D2H failed: "
                  << cudaGetErrorString(syncErr) << "\n";
        return b;
    }

    if (accum.frame_count > 0) {
        // Welford state from the GPU: mu is the running mean directly, M2 is
        // the sum of squared deviations from that running mean.
        b.hist_mean    = (float)accum.hist_mean_mu;
        b.hist_var     = (float)accum.hist_var_mu;
        b.rms_contrast = (float)accum.rms_contrast_mu;
        b.edge_density = (float)accum.edge_density_mu;
        b.entropy      = (float)accum.entropy_mu;
        b.lap_var      = (float)accum.lap_var_mu;

        // Bessel-corrected sample variance: sigma = sqrt(M2 / (N - 1)) is the
        // unbiased estimator of the camera's true frame-to-frame variability
        // and matches the Shewhart control-chart convention the k*sigma band
        // is built on. At N=1 the denominator collapses to N (= 1) to avoid
        // divide-by-zero; Welford's update guarantees M2=0 there, so sigma=0
        // either way. Downstream MIN_LEARN_FRAMES_FOR_SIGMA gates reject N<30.
        const uint64_t divisor =
            (accum.frame_count >= 2) ? (accum.frame_count - 1) : 1;
        const double inv_div = 1.0 / (double)divisor;
        auto sigma_of = [inv_div](double M2) -> double {
            return std::sqrt(std::max(0.0, M2 * inv_div));
        };
        b.hist_mean_std    = (float)sigma_of(accum.hist_mean_M2);
        b.hist_var_std     = (float)sigma_of(accum.hist_var_M2);
        b.rms_contrast_std = (float)sigma_of(accum.rms_contrast_M2);
        b.edge_density_std = (float)sigma_of(accum.edge_density_M2);
        b.entropy_std      = (float)sigma_of(accum.entropy_M2);
        b.lap_var_std      = (float)sigma_of(accum.lap_var_M2);

        b.cb_mean      = (float)accum.cb_mean_mu;
        b.cb_var       = (float)accum.cb_var_mu;
        b.cr_mean      = (float)accum.cr_mean_mu;
        b.cr_var       = (float)accum.cr_var_mu;
        b.sat_mean     = (float)accum.sat_mean_mu;
        b.sat_var      = (float)accum.sat_var_mu;
        b.cb_mean_std  = (float)sigma_of(accum.cb_mean_M2);
        b.cb_var_std   = (float)sigma_of(accum.cb_var_M2);
        b.cr_mean_std  = (float)sigma_of(accum.cr_mean_M2);
        b.cr_var_std   = (float)sigma_of(accum.cr_var_M2);
        b.sat_mean_std = (float)sigma_of(accum.sat_mean_M2);
        b.sat_var_std  = (float)sigma_of(accum.sat_var_M2);
        b.has_chroma   = true;

        b.total_frames = (accum.frame_count <= static_cast<uint64_t>(INT_MAX))
                       ? static_cast<int>(accum.frame_count)
                       : INT_MAX;
    }
    return b;
}

const unsigned int* FrameQualityAnalyzer::prepareHistogram(
    const unsigned char* d_y, int y_pitch,
    CUdeviceptr dpHistogram, int hist_bins, int counter_bytes,
    int& out_bins)
{
    NVTX_RANGE("PrepareHistogram", 0xFF4488FF);
    if (use_nvdec_hist_ && dpHistogram && hist_bins > 0
        && hist_bins <= hist_bins_ && counter_bytes > 0) {
        if (counter_bytes == 4) {
            out_bins = hist_bins;
            return (const unsigned int*)(uintptr_t)dpHistogram;
        }
        NVTX_RANGE("HistConvert", 0xFFAA44FF);
        if (counter_bytes == 8) {
            launch_convert_hist64(
                (const uint64_t*)(uintptr_t)dpHistogram,
                (unsigned int*)d_hist_, hist_bins, stream_);
            out_bins = hist_bins;
            return (const unsigned int*)d_hist_;
        }
    }
    NVTX_RANGE("HistFallback", 0xFFAA44FF);
    launch_histogram_fallback(d_y, y_pitch, W_, H_, (unsigned int*)d_hist_, stream_);
    out_bins = kSwHistogramBinCount;
    return (const unsigned int*)d_hist_;
}

void FrameQualityAnalyzer::runEdgeDetection(const unsigned char* d_y,
                                            int y_pitch)
{
    NVTX_RANGE("EdgeDetectionPipeline", 0xFFFF8800);
    launch_edge_detection(
        d_y, y_pitch, W_, H_,
        d_blurred_, d_grad_mag_, d_grad_dir_,
        d_edges_, d_edgeCount_,
        cfg_.canny_low_thresh, cfg_.canny_high_thresh,
        stream_);
}

void FrameQualityAnalyzer::syncParams() {
    gpu_params_.w_histogram              = cfg_.w_histogram;
    gpu_params_.w_contrast               = cfg_.w_contrast;
    gpu_params_.w_edge                   = cfg_.w_edge;
    gpu_params_.w_entropy                = cfg_.w_entropy;
    gpu_params_.w_laplacian              = cfg_.w_laplacian;
    gpu_params_.uv_weight_cb             = cfg_.uv_weight_cb;
    gpu_params_.uv_weight_cr             = cfg_.uv_weight_cr;
    gpu_params_.uv_weight_sat            = cfg_.uv_weight_sat;
    gpu_params_.k                        = cfg_.k;
    gpu_params_.sigma_floor_fraction     = cfg_.sigma_floor_fraction;
    gpu_params_.baseline_hist_mean       = baseline_.hist_mean;
    gpu_params_.baseline_hist_var        = baseline_.hist_var;
    gpu_params_.baseline_rms_contrast    = baseline_.rms_contrast;
    gpu_params_.baseline_edge_density    = baseline_.edge_density;
    gpu_params_.baseline_entropy         = baseline_.entropy;
    gpu_params_.baseline_lap_var         = baseline_.lap_var;
    gpu_params_.baseline_hist_mean_std    = baseline_.hist_mean_std;
    gpu_params_.baseline_hist_var_std     = baseline_.hist_var_std;
    gpu_params_.baseline_rms_contrast_std = baseline_.rms_contrast_std;
    gpu_params_.baseline_edge_density_std = baseline_.edge_density_std;
    gpu_params_.baseline_entropy_std      = baseline_.entropy_std;
    gpu_params_.baseline_lap_var_std      = baseline_.lap_var_std;

    gpu_params_.baseline_cb_mean      = baseline_.cb_mean;
    gpu_params_.baseline_cb_mean_std  = baseline_.cb_mean_std;
    gpu_params_.baseline_cb_var       = baseline_.cb_var;
    gpu_params_.baseline_cb_var_std   = baseline_.cb_var_std;
    gpu_params_.baseline_cr_mean      = baseline_.cr_mean;
    gpu_params_.baseline_cr_mean_std  = baseline_.cr_mean_std;
    gpu_params_.baseline_cr_var       = baseline_.cr_var;
    gpu_params_.baseline_cr_var_std   = baseline_.cr_var_std;
    gpu_params_.baseline_sat_mean     = baseline_.sat_mean;
    gpu_params_.baseline_sat_mean_std = baseline_.sat_mean_std;
    gpu_params_.baseline_sat_var      = baseline_.sat_var;
    gpu_params_.baseline_sat_var_std  = baseline_.sat_var_std;

    gpu_params_.has_baseline  = (baseline_.total_frames >= MIN_LEARN_FRAMES_FOR_SIGMA) ? 1 : 0;
    gpu_params_.has_chroma_baseline =
        (baseline_.has_chroma &&
         baseline_.total_frames >= MIN_LEARN_FRAMES_FOR_SIGMA) ? 1 : 0;
    gpu_params_.is_learn_mode = (mode_ == RunMode::LEARN) ? 1 : 0;
    gpu_params_.total_pixels  = W_ * H_;
    gpu_params_.chroma_pixels = (W_ / 2) * (H_ / 2);
    gpu_params_.lap_pixels    = (W_ > 0 && H_ > 0) ? W_ * H_ : 0;
}

}}  // namespace sai::gpu
