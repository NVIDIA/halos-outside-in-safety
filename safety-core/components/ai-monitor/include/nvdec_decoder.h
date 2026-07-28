/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVDEC_DECODER_H
#define NVDEC_DECODER_H

#include <cuda.h>
#include <memory>
#include "nvcuvid.h"
#include "cuviddec.h"
#include "sai_common.h"
#include "i_frame_quality_analyzer.h"
#include "pss_daemon.h"

class NalQueue;
class SafetyEventReporter;

/*
 * Manages the NVDEC hardware decoder lifecycle: CUDA context creation,
 * capability query, video parser creation, and frame decoding from either
 * a local H.264 file or a NalQueue fed by the RTSP client.
 * Each decoded frame is passed through FrameQualityAnalyzer.
 */
class NVDECDecoder {
public:
    NVDECDecoder();
    ~NVDECDecoder() { cleanup(); }

    // Builds the selected analyzer (via the factory), initializes the CUDA
    // driver API, and creates a context on the assigned GPU. On a false return,
    // analyzer_ may be null, so no other method except cleanup() is safe to call.
    bool initialize();
    // Queries H.264/NV12 decode capabilities (max resolution, histogram support).
    bool queryDecoderCaps();
    // Creates (or re-creates) the NVDEC video parser with the three decode callbacks.
    bool createParser();
    // Consumes NAL units from the queue (fed by RTSPClient) and parses them
    // one at a time. In LEARN mode, auto-stops once the analyzer has processed
    // learn_target_frames_ frames (computed from learn_duration_sec_ * FPS).
    bool decodeStream(NalQueue& queue);

    void setMode(RunMode m) { mode_ = m; }
    void setLearnDuration(int seconds) { learn_duration_sec_ = seconds; }

    // LEARN-mode progress getters used by the RTSP retry loop to decide
    // whether to reconnect (target not yet reached) or exit (target reached).
    bool isLearnComplete() const { return learnComplete_.load(); }
    uint64_t learnFramesTarget() const { return learn_target_frames_; }
    uint64_t learnFramesProcessed() const { return analyzer_ ? analyzer_->getLearnFramesProcessed() : 0; }
    void setSensorName(const std::string& name) { sensorName_ = name; }
    void setPipelineId(uint8_t id) { pipelineId_ = id; }
    void setGpuIndex(int idx) { gpuIndex_ = idx; }
    void setEventReporter(SafetyEventReporter* r) { eventReporter_ = r; }
    const std::string& sensorName() const { return sensorName_; }
    uint8_t pipelineId() const { return pipelineId_; }
    uint32_t allocEventId() { return s_nextEventId_.fetch_add(1); }

    /* Shared trust-report state: both the frame-quality path inside
     * HandlePictureDisplay and out-of-band alert paths (e.g. FU-A drop bursts
     * in the RTSP client) must agree on whether this sensor is currently
     * "invalid" so that they emit SENSOR_INVALID / SENSOR_VALID on transitions
     * only, not on every symptom. Returning the atomic by reference lets
     * external state machines compare_exchange against it. */
    std::atomic<bool>& sensorInvalidFlag() { return sensorInvalid_; }
    const std::atomic<bool>& sensorInvalidFlag() const { return sensorInvalid_; }

    /* Set by the analyzer once its bounded reinit budget is exhausted (or by
     * HandleVideoSequence on a non-recoverable analyzer.init() failure).
     * Polled by decodeStream() and runStreamPipeline() so this camera's
     * thread shuts down without touching g_stopFlag (other cameras keep
     * running). */
    std::atomic<bool>& analyzerPermanentFaultFlag()       { return analyzerPermanentFault_; }
    const std::atomic<bool>& analyzerPermanentFaultFlag() const { return analyzerPermanentFault_; }

    // Loads threshold tuning parameters (weights, k*sigma knobs, alert thresholds) from a config file.
    bool loadThresholdConfig(const std::string& path);
    // Validates the loaded threshold config (range checks, weight sum, etc.).
    bool validateThresholdConfig() const;
    // Retrieves averaged learn-mode stats from the GPU and writes them to a file.
    bool saveBaseline(const std::string& path);
    // Loads a previously saved baseline from file for use in ACTIVE mode scoring.
    bool loadBaseline(const std::string& path);
    // Validates the loaded baseline values (mu and sigma finite & non-negative,
    // total_frames >= MIN_LEARN_FRAMES_FOR_SIGMA).
    bool validateBaseline() const;

    void cleanup();

private:
    // Parser callback: invoked when a new SPS is parsed. Creates (or re-creates)
    // the hardware decoder with the stream's resolution and chroma format.
    static int CUDAAPI HandleVideoSequence(void* pUserData, CUVIDEOFORMAT* pFormat);
    // Parser callback: submits a compressed picture to the hardware decoder.
    static int CUDAAPI HandlePictureDecode(void* pUserData, CUVIDPICPARAMS* pPic);
    // Parser callback: maps a decoded frame, runs quality analysis, and
    // updates the saturating counter for SENSOR_INVALID/VALID transitions.
    static int CUDAAPI HandlePictureDisplay(void* pUserData,
                                            CUVIDPARSERDISPINFO* pDispInfo);

    CUdevice        cuDevice_ = 0;
    CUcontext       cuContext;
    CUvideodecoder  decoder;
    CUvideoparser   parser;

    int  frameWidth, frameHeight;      // Coded resolution from the bitstream.
    int  displayWidth, displayHeight;  // Cropped display resolution (may differ from coded).
    uint64_t  frameCount;
    static std::atomic<uint32_t> s_nextEventId_;  // Globally unique event ID counter across all decoder instances.
    bool histogramEnabled;             // True if the GPU supports NVDEC per-frame histograms.

    CUVIDDECODECAPS decodeCaps{};

    // Built by initialize() via the factory; null until then. Valid only after
    // initialize() returns true -- all parser callbacks and config methods
    // assume non-null;
    std::unique_ptr<IFrameQualityAnalyzer> analyzer_;
    RunMode mode_ = RunMode::ACTIVE;
    int learn_duration_sec_ = DEFAULT_LEARN_DURATION_SEC; // LEARN-mode duration in seconds; converted to a frame target on first sequence.
    uint64_t learn_target_frames_ = 0; // ceil(learn_duration_sec_ * learn_target_fps_); recomputed on first sequence and whenever the new sequence's FPS differs.
    double   learn_target_fps_    = 0.0;
    BaselineValues baseline_;
    std::string sensorName_ = "unknown_sensor";
    uint8_t pipelineId_ = 1;
    int gpuIndex_ = 0;
    // Fail-safe default: starts INVALID until alertCounter_ drains to 0
    // on a good streak. createParser() re-asserts on every (re)connect;
    // cleanup() preserves true on teardown. Initial INVALID is delivered
    // to PSS via SafetyEventReporter::registerSensor() seed.
    std::atomic<bool> sensorInvalid_{true};
    std::atomic<bool> learnComplete_{false};
    std::atomic<bool> analyzerPermanentFault_{false};
    // Saturating counter: counter_max == fully INVALID, 0 == fully VALID.
    // createParser() seeds to counter_max (fail-safe).
    std::atomic<int> alertCounter_{0};
    // Non-owning; null in LEARN. Outlives the decoder via main's teardown order.
    SafetyEventReporter* eventReporter_ = nullptr;
};

#endif
