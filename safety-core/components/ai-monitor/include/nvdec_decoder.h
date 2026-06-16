/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVDEC_DECODER_H
#define NVDEC_DECODER_H

#include <cuda.h>
#include "nvcuvid.h"
#include "cuviddec.h"
#include "sai_common.h"
#include "frame_quality_analyzer.h"
#include "pss_daemon.h"

class NalQueue;

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

    // Initializes the CUDA driver API and creates a context on the assigned GPU.
    bool initialize();
    // Queries H.264/NV12 decode capabilities (max resolution, histogram support).
    bool queryDecoderCaps();
    // Creates (or re-creates) the NVDEC video parser with the three decode callbacks.
    bool createParser();
    // Consumes NAL units from the queue (fed by RTSPClient) and parses them one
    // at a time. In LEARN mode, auto-stops after learn_duration_sec_ seconds.
    bool decodeStream(NalQueue& queue);

    void setMode(RunMode m) { mode_ = m; }
    void setLearnDuration(int seconds) { learn_duration_sec_ = seconds; }
    void setPSSClientId(uint32_t id) { pssClientId_ = id; }
    void setSensorName(const std::string& name) { sensorName_ = name; }
    void setPipelineId(uint8_t id) { pipelineId_ = id; }
    void setGpuIndex(int idx) { gpuIndex_ = idx; }
    const std::string& sensorName() const { return sensorName_; }
    uint8_t pipelineId() const { return pipelineId_; }
    uint32_t allocEventId() { return s_nextEventId_.fetch_add(1); }
    NvPSSDErr reportSafetyEvent(uint32_t clientId, const SafetyEvent* event);

    /* Shared trust-report state: both the frame-quality path inside
     * HandlePictureDisplay and out-of-band alert paths (e.g. FU-A drop bursts
     * in the RTSP client) must agree on whether this sensor is currently
     * "invalid" so that they emit SENSOR_INVALID / SENSOR_VALID on transitions
     * only, not on every symptom. Returning the atomic by reference lets
     * external state machines compare_exchange against it. */
    std::atomic<bool>& sensorInvalidFlag() { return sensorInvalid_; }
    const std::atomic<bool>& sensorInvalidFlag() const { return sensorInvalid_; }

    // Loads threshold tuning parameters (weights, margins, thresholds) from a config file.
    bool loadThresholdConfig(const std::string& path);
    // Validates the loaded threshold config (range checks, weight sum, etc.).
    bool validateThresholdConfig() const;
    // Retrieves averaged learn-mode stats from the GPU and writes them to a file.
    bool saveBaseline(const std::string& path);
    // Loads a previously saved baseline from file for use in ACTIVE mode scoring.
    bool loadBaseline(const std::string& path);
    // Validates the loaded baseline values (finite, non-negative, frame count > 0).
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

    FrameQualityAnalyzer analyzer;
    RunMode mode_ = RunMode::ACTIVE;
    int learn_duration_sec_ = 300;     // Auto-stop duration for LEARN mode on RTSP streams.
    BaselineValues baseline_;
    uint32_t pssClientId_ = UINT32_MAX;
    std::string sensorName_ = "unknown_sensor";
    uint8_t pipelineId_ = 1;
    int gpuIndex_ = 0;
    std::atomic<bool> sensorInvalid_{false};
    std::atomic<bool> learnComplete_{false};
    std::atomic<int> alertCounter_{0};
};

#endif
