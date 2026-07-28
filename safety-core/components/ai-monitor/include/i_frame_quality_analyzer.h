/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef I_FRAME_QUALITY_ANALYZER_H
#define I_FRAME_QUALITY_ANALYZER_H

#include <cuda.h>  // CUdeviceptr in the analyze() signature

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "sai_common.h"  // RunMode, ThresholdConfig, BaselineValues, FrameQualityResult

// Backend-agnostic frame-quality analyzer contract.
// The GPU (CUDA) analyzer implements this, so NVDECDecoder holds one by pointer;
// the concrete backend is the one linked at build time and constructed via
// createFrameQualityAnalyzer().
class IFrameQualityAnalyzer {
public:
    // Emits SENSOR_INVALID to PSS with a short human-readable reason.
    using PssReportInvalidFn = std::function<void(const char* reason)>;

    virtual ~IFrameQualityAnalyzer() = default;

    // Allocates per-camera analysis resources for the given input dimensions.
    // nvdec_hist/hw_hist_bins describe the NVDEC hardware histogram.
    virtual bool init(int width, int height, bool nvdec_hist,
                      int hw_hist_bins = 0) = 0;
    virtual void cleanup() = 0;

    // Runs one decoded NV12 Y plane through the backend and returns per-frame
    // scores. dpHistogram/hist_bins/counter_bytes are the NVDEC HW histogram.
    virtual FrameQualityResult analyze(
        const unsigned char* d_y, int y_pitch,
        CUdeviceptr dpHistogram, int hist_bins, int counter_bytes) = 0;

    virtual void setConfig(const ThresholdConfig& cfg) = 0;
    virtual const ThresholdConfig& config() const = 0;

    virtual void setMode(RunMode m) = 0;
    virtual void setBaseline(const BaselineValues& b) = 0;
    virtual BaselineValues getLearnedBaseline() = 0;

    virtual uint64_t getLearnFramesProcessed() const = 0;
    virtual int getWidth()  const noexcept = 0;
    virtual int getHeight() const noexcept = 0;

    virtual void setSensorInvalidFlag(std::atomic<bool>* p)  noexcept = 0;
    virtual void setPermanentFaultFlag(std::atomic<bool>* p) noexcept = 0;
    virtual void setReportInvalidCallback(PssReportInvalidFn fn) noexcept = 0;
};

// Runs the linked backend plugin's one-time HW probe (a no-op on GPU, whose CUDA
// device the core validates separately). Call once at startup, single-threaded,
// before any analyzer is constructed. Returns false if the backend HW is
// unavailable.
bool loadAnalyzerBackend();

// Build-time identity of the linked backend plugin ("GPU").
const char* analyzerBackendName();

// Constructs an analyzer from the linked backend plugin, or nullptr if backend
// init failed or allocation failed. Lazily runs loadAnalyzerBackend() if it was
// not called first. Ownership transfers to the caller, which null-checks and
// fails the camera cleanly.
std::unique_ptr<IFrameQualityAnalyzer>
createFrameQualityAnalyzer();

#endif  // I_FRAME_QUALITY_ANALYZER_H