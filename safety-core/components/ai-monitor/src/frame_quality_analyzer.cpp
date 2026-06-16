/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "frame_quality_analyzer.h"
#include "saim_kernels.h"

#include <iostream>
#include <cuda_runtime.h>

bool FrameQualityAnalyzer::init(int width, int height, bool nvdec_hist,
                                int hw_hist_bins)
{
    NVTX_RANGE("InitAnalyzerBuffers", 0xFFCC8800);
    if (initialized_) cleanup();

    if (width <= 0 || height <= 0 || width > MAX_DIM || height > MAX_DIM) {
        std::cerr << "[QA] Invalid dimensions: " << width << "x" << height
                  << " (must be 1.." << MAX_DIM << ")\n";
        return false;
    }

    W_ = width;  H_ = height;
    use_nvdec_hist_ = nvdec_hist;
    hist_bins_ = (nvdec_hist && hw_hist_bins > 0) ? hw_hist_bins : 256;
    int hist_alloc = (hist_bins_ > 256) ? hist_bins_ : 256;

    size_t total = (size_t)W_ * (size_t)H_;

    if (cudaMalloc(&d_hist_, hist_alloc * sizeof(unsigned int)) != cudaSuccess ||
        cudaMalloc(&d_result_, sizeof(FrameQualityResult)) != cudaSuccess ||
        cudaMalloc(&d_learn_accum_, sizeof(GpuLearnAccum)) != cudaSuccess ||
        cudaMalloc(&d_blurred_,   total)                    != cudaSuccess ||
        cudaMalloc(&d_grad_mag_,  total * sizeof(int16_t))  != cudaSuccess ||
        cudaMalloc(&d_grad_dir_,  total)                    != cudaSuccess ||
        cudaMalloc(&d_edges_,     total)                    != cudaSuccess ||
        cudaMalloc(&d_edgeCount_, sizeof(uint32_t))         != cudaSuccess)
    {
        std::cerr << "[QA] GPU buffer allocation failed\n";
        cleanup(); return false;
    }

    // Pinned (page-locked) host buffer for the per-frame result copy. Using a
    // pinned destination lets cudaMemcpyAsync DMA directly from device memory
    // without going through a pageable staging buffer, eliminating one host-side
    // memcpy per frame.
    if (cudaMallocHost(reinterpret_cast<void**>(&h_result_),
                       sizeof(FrameQualityResult)) != cudaSuccess) {
        std::cerr << "[QA] cudaMallocHost for h_result_ failed\n";
        cleanup(); return false;
    }

    cudaError_t streamErr = cudaStreamCreate(&stream_);
    if (streamErr != cudaSuccess) {
        std::cerr << "[QA] cudaStreamCreate failed: "
                  << cudaGetErrorString(streamErr) << "\n";
        cleanup(); return false;
    }

    cudaError_t err = cudaMemsetAsync(d_learn_accum_, 0, sizeof(GpuLearnAccum), stream_);
    if (err != cudaSuccess) {
        std::cerr << "[QA] cudaMemsetAsync learn_accum failed: "
                  << cudaGetErrorString(err) << "\n";
        cleanup(); return false;
    }

    initialized_ = true;
#ifdef DEBUG
    std::cout << "[QA] Analyzer initialized (" << W_ << "x" << H_
              << ") - GPU scoring enabled\n";
#endif
    return true;
}

void FrameQualityAnalyzer::cleanup() {
    NVTX_RANGE("CleanupAnalyzer", 0xFF880000);
    auto F = [](void*& p) {
        if (p) {
            cudaError_t err = cudaFree(p);
            if (err != cudaSuccess)
                std::cerr << "[QA] cudaFree failed: " << cudaGetErrorString(err) << "\n";
            p = nullptr;
        }
    };
    if (stream_) { cudaStreamDestroy(stream_); stream_ = nullptr; }
    F(d_hist_); F(d_result_); F(d_learn_accum_);
    F(d_blurred_); F(d_grad_mag_); F(d_grad_dir_); F(d_edges_); F(d_edgeCount_);
    if (h_result_) {
        cudaError_t err = cudaFreeHost(h_result_);
        if (err != cudaSuccess)
            std::cerr << "[QA] cudaFreeHost failed: " << cudaGetErrorString(err) << "\n";
        h_result_ = nullptr;
    }
    initialized_ = false;
}

FrameQualityResult FrameQualityAnalyzer::analyze(
    const unsigned char* d_y, int y_pitch,
    CUdeviceptr dpHistogram, int hist_bins, int counter_bytes)
{
    NVTX_RANGE("FrameAnalyze", 0xFF00FF00);
    FrameQualityResult res{};
    if (!initialized_) { res.overall_confidence = 50.f; return res; }
    if (!d_y || y_pitch < W_) {
        std::cerr << "[QA] Invalid frame surface: d_y="
                  << (const void*)d_y << " y_pitch=" << y_pitch
                  << " W_=" << W_ << "\n";
        return res;
    }

    int statsBins = 0;
    const unsigned int* hist_ptr = prepareHistogram(
        d_y, y_pitch, dpHistogram, hist_bins, counter_bytes, statsBins);
    runEdgeDetection(d_y, y_pitch);

    {
        NVTX_RANGE("QualityScoring", 0xFF00FFAA);
        launch_quality_scoring(
            hist_ptr,
            (const uint32_t*)d_edgeCount_,
            gpu_params_,
            (FrameQualityResult*)d_result_,
            (mode_ == RunMode::LEARN) ? (GpuLearnAccum*)d_learn_accum_ : nullptr,
            statsBins,
            stream_);
    }

    // Non-blocking check for kernel launch errors. This does not serialize on
    // the stream, so it does not add a sync. Any async (runtime) error from the
    // scoring kernel will be surfaced by the single cudaStreamSynchronize below.
    {
        cudaError_t lastErr = cudaGetLastError();
        if (lastErr != cudaSuccess) {
            std::cerr << "[QA] Kernel launch error: "
                      << cudaGetErrorString(lastErr) << "\n";
            return res;
        }
    }

    // Single D2H + sync. The previous code did:
    //   sync -> memcpyAsync -> sync
    // The first sync was redundant: cudaMemcpyAsync on the same stream is
    // strictly ordered after the preceding kernel, and the post-memcpy sync
    // observes any async kernel errors as well as the copy result. Using the
    // pinned h_result_ buffer lets cudaMemcpyAsync DMA directly without a
    // pageable staging copy. Net effect: one sync per frame instead of two.
    {
        NVTX_RANGE("ResultD2HCopy", 0xFFFF00FF);
        cudaError_t err = cudaMemcpyAsync(h_result_, d_result_,
                                          sizeof(FrameQualityResult),
                                          cudaMemcpyDeviceToHost, stream_);
        if (err != cudaSuccess) {
            std::cerr << "[QA] cudaMemcpyAsync result D2H failed: "
                      << cudaGetErrorString(err) << "\n";
            return FrameQualityResult{};
        }
        cudaError_t syncErr = cudaStreamSynchronize(stream_);
        if (syncErr != cudaSuccess) {
            std::cerr << "[QA] GPU pipeline error at result sync: "
                      << cudaGetErrorString(syncErr) << "\n";
            return FrameQualityResult{};
        }
        res = *h_result_;
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
        std::cerr << "[QA] cudaMemcpyAsync learn_accum D2H failed: "
                  << cudaGetErrorString(err) << "\n";
        return b;
    }
    cudaError_t syncErr = cudaStreamSynchronize(stream_);
    if (syncErr != cudaSuccess) {
        std::cerr << "[QA] cudaStreamSynchronize learn_accum D2H failed: "
                  << cudaGetErrorString(syncErr) << "\n";
        return b;
    }

    if (accum.frame_count > 0) {
        b.hist_mean    = (float)(accum.hist_mean_sum    / accum.frame_count);
        b.hist_var     = (float)(accum.hist_var_sum     / accum.frame_count);
        b.rms_contrast = (float)(accum.rms_contrast_sum / accum.frame_count);
        b.edge_density = (float)(accum.edge_density_sum / accum.frame_count);
        b.total_frames = accum.frame_count;
    }
    return b;
}

const unsigned int* FrameQualityAnalyzer::prepareHistogram(
    const unsigned char* d_y, int y_pitch,
    CUdeviceptr dpHistogram, int hist_bins, int counter_bytes,
    int& out_bins)
{
    NVTX_RANGE("PrepareHistogram", 0xFF4488FF);
    if (use_nvdec_hist_ && dpHistogram && hist_bins > 0 && counter_bytes > 0) {
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
    out_bins = 256;
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
    gpu_params_.baseline_mean_margin     = cfg_.baseline_mean_margin;
    gpu_params_.baseline_var_margin      = cfg_.baseline_var_margin;
    gpu_params_.baseline_contrast_margin = cfg_.baseline_contrast_margin;
    gpu_params_.baseline_edge_margin     = cfg_.baseline_edge_margin;
    gpu_params_.baseline_hist_mean       = baseline_.hist_mean;
    gpu_params_.baseline_hist_var        = baseline_.hist_var;
    gpu_params_.baseline_rms_contrast    = baseline_.rms_contrast;
    gpu_params_.baseline_edge_density    = baseline_.edge_density;
    gpu_params_.has_baseline  = (baseline_.total_frames > 0) ? 1 : 0;
    gpu_params_.is_learn_mode = (mode_ == RunMode::LEARN) ? 1 : 0;
    gpu_params_.total_pixels  = W_ * H_;
}
