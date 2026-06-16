/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nvdec_decoder.h"
#include "rtsp_client.h"

#include <algorithm>
#include <iostream>
#include <cstring>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <thread>

std::atomic<uint32_t> NVDECDecoder::s_nextEventId_{1};

NVDECDecoder::NVDECDecoder()
    : cuContext(nullptr), decoder(nullptr), parser(nullptr),
      frameWidth(0), frameHeight(0), displayWidth(0), displayHeight(0),
      frameCount(0), histogramEnabled(false)
{}

bool NVDECDecoder::initialize() {
    NVTX_RANGE("CUDAInit", 0xFF0000FF);
    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        std::cerr << "Failed to initialize CUDA: " << result << "\n";
        return false;
    }

    int gpuCount = 0;
    result = cuDeviceGetCount(&gpuCount);
    if (result != CUDA_SUCCESS || gpuCount == 0) {
        std::cerr << "[" << sensorName_ << "] No CUDA devices found\n";
        return false;
    }
    if (gpuIndex_ < 0 || gpuIndex_ >= gpuCount) {
        std::cerr << "[" << sensorName_ << "] GPU index " << gpuIndex_
                  << " out of range (0-" << gpuCount - 1 << ")\n";
        return false;
    }

    result = cuDeviceGet(&cuDevice_, gpuIndex_);
    if (result != CUDA_SUCCESS) {
        std::cerr << "[" << sensorName_ << "] Failed to get CUDA device "
                  << gpuIndex_ << "\n";
        return false;
    }

    result = cuDevicePrimaryCtxRetain(&cuContext, cuDevice_);
    if (result != CUDA_SUCCESS) {
        std::cerr << "Failed to retain primary CUDA context\n";
        return false;
    }
    result = cuCtxSetCurrent(cuContext);
    if (result != CUDA_SUCCESS) {
        std::cerr << "Failed to set CUDA context current\n";
        cuDevicePrimaryCtxRelease(cuDevice_);
        cuContext = nullptr;
        return false;
    }

#ifdef DEBUG
    std::cout << "[" << sensorName_ << "] CUDA context created on GPU "
              << gpuIndex_ << "\n";
#endif
    return true;
}

bool NVDECDecoder::queryDecoderCaps() {
    NVTX_RANGE("QueryDecoderCaps", 0xFF0044FF);
    memset(&decodeCaps, 0, sizeof(decodeCaps));
    decodeCaps.eCodecType      = cudaVideoCodec_H264;
    decodeCaps.eChromaFormat   = cudaVideoChromaFormat_420;
    decodeCaps.nBitDepthMinus8 = 0;

    CUresult result = cuvidGetDecoderCaps(&decodeCaps);
    if (result != CUDA_SUCCESS) {
        std::cerr << "Failed to query decoder capabilities\n";
        return false;
    }
    if (!decodeCaps.bIsSupported) {
        std::cerr << "H264 decoding not supported on this GPU\n";
        return false;
    }

#ifdef DEBUG
    std::cout << "Decoder Capabilities:\n"
              << "  Max Width: "  << decodeCaps.nMaxWidth  << "\n"
              << "  Max Height: " << decodeCaps.nMaxHeight << "\n"
              << "  Max MB Count: " << decodeCaps.nMaxMBCount << "\n"
              << "  Histogram Supported: "
              << (decodeCaps.bIsHistogramSupported ? "Yes" : "No") << "\n";

    if (decodeCaps.bIsHistogramSupported) {
        std::cout << "  Histogram Bins: " << decodeCaps.nMaxHistogramBins << "\n"
                  << "  Counter Bit Depth: " << decodeCaps.nCounterBitDepth << "\n";
    }
#endif
    histogramEnabled = decodeCaps.bIsHistogramSupported ? true : false;
    return true;
}

// Retries NvPSSReportSafetyEvent up to MAX_PSS_REPORT_RETRIES
NvPSSDErr NVDECDecoder::reportSafetyEvent(uint32_t clientId,
                                           const SafetyEvent* event) {
    for (int attempt = 0; attempt <= MAX_PSS_REPORT_RETRIES; ++attempt) {
        NvPSSDErr err = NvPSSReportSafetyEvent(clientId, event);
        if (err == NVPSSD_SUCCESS) return NVPSSD_SUCCESS;
        if (attempt < MAX_PSS_REPORT_RETRIES) {
            std::cerr << "[SAI] reportSafetyEvent failed, retry "
                      << (attempt + 1) << "/" << MAX_PSS_REPORT_RETRIES << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    return NVPSSD_FAIL;
}

int CUDAAPI NVDECDecoder::HandleVideoSequence(void* pUserData,
                                              CUVIDEOFORMAT* pFormat)
{
    NVTX_RANGE("HandleVideoSequence", 0xFFFF0000);
    NVDECDecoder* dec = static_cast<NVDECDecoder*>(pUserData);

#ifdef DEBUG
    std::cout << "\nVideo Sequence Callback:\n"
              << "  Codec: H264\n"
              << "  Resolution: " << pFormat->coded_width << "x"
              << pFormat->coded_height << "\n"
              << "  Chroma: " << pFormat->chroma_format << "\n"
              << "  Bit Depth: " << (pFormat->bit_depth_luma_minus8 + 8) << "\n"
              << "  Min Decode Surfaces: " << pFormat->min_num_decode_surfaces << "\n";
#endif
    dec->frameWidth  = pFormat->coded_width;
    dec->frameHeight = pFormat->coded_height;

    if (pFormat->display_area.right && pFormat->display_area.bottom) {
        dec->displayWidth  = pFormat->display_area.right  - pFormat->display_area.left;
        dec->displayHeight = pFormat->display_area.bottom - pFormat->display_area.top;
    } else {
        dec->displayWidth  = pFormat->coded_width;
        dec->displayHeight = pFormat->coded_height;
    }

#ifdef DEBUG
    std::cout << "  Display Area: " << dec->displayWidth << "x"
              << dec->displayHeight << "\n";

    if (pFormat->frame_rate.numerator && pFormat->frame_rate.denominator) {
        double fps = (double)pFormat->frame_rate.numerator
                   / (double)pFormat->frame_rate.denominator;
        std::cout << "  Frame Rate: " << fps << " fps\n";
    }
#endif

    if (pFormat->coded_width  > dec->decodeCaps.nMaxWidth ||
        pFormat->coded_height > dec->decodeCaps.nMaxHeight) {
        std::cerr << "[" << dec->sensorName_ << "] Resolution not supported\n";
        return 0;
    }

    if (dec->displayWidth <= 0 || dec->displayHeight <= 0 ||
        dec->displayWidth > MAX_DIM || dec->displayHeight > MAX_DIM) {
        std::cerr << "Display dimensions " << dec->displayWidth << "x"
                  << dec->displayHeight << " out of valid range (1.."
                  << MAX_DIM << ")\n";
        return 0;
    }

    if (dec->decoder) {
        cuvidDestroyDecoder(dec->decoder);
        dec->decoder = nullptr;
    }

    CUVIDDECODECREATEINFO ci{};
    ci.CodecType           = cudaVideoCodec_H264;
    ci.ulWidth             = pFormat->coded_width;
    ci.ulHeight            = pFormat->coded_height;
    ci.ulNumDecodeSurfaces = pFormat->min_num_decode_surfaces;
    ci.ChromaFormat        = pFormat->chroma_format;
    ci.OutputFormat        = cudaVideoSurfaceFormat_NV12;
    ci.bitDepthMinus8      = pFormat->bit_depth_luma_minus8;
    ci.DeinterlaceMode     = cudaVideoDeinterlaceMode_Weave;
    ci.ulTargetWidth       = dec->displayWidth;
    ci.ulTargetHeight      = dec->displayHeight;
    ci.ulNumOutputSurfaces = 2;
    ci.ulCreationFlags     = cudaVideoCreate_PreferCUVID;
    ci.vidLock             = nullptr;
    ci.display_area.left   = (short)pFormat->display_area.left;
    ci.display_area.top    = (short)pFormat->display_area.top;
    ci.display_area.right  = (short)pFormat->display_area.right;
    ci.display_area.bottom = (short)pFormat->display_area.bottom;
    ci.enableHistogram     = dec->histogramEnabled ? 1 : 0;

    CUresult result;
    {
        NVTX_RANGE("CreateDecoder", 0xFFCC0000);
        result = cuvidCreateDecoder(&dec->decoder, &ci);
    }
    if (result != CUDA_SUCCESS) {
        std::cerr << "[" << dec->sensorName_ << "] Failed to create decoder: " << result << "\n";
        return 0;
    }
#ifdef DEBUG
    std::cout << "Decoder created with histogram "
              << (dec->histogramEnabled ? "enabled" : "disabled") << "\n";
#endif
    {
        NVTX_RANGE("InitAnalyzer", 0xFFFF8800);
        if (!dec->analyzer.init(dec->displayWidth, dec->displayHeight,
                                dec->histogramEnabled,
                                dec->histogramEnabled ? (int)dec->decodeCaps.nMaxHistogramBins : 0)) {
            std::cerr << "Fatal: frame quality analyzer init failed, "
                         "safety monitoring disabled — stopping.\n";
            g_stopFlag.store(true);
            return 0;
        }
    }
    dec->analyzer.setMode(dec->mode_);
    if (dec->mode_ == RunMode::ACTIVE && dec->baseline_.total_frames > 0)
        dec->analyzer.setBaseline(dec->baseline_);

    return pFormat->min_num_decode_surfaces;
}

int CUDAAPI NVDECDecoder::HandlePictureDecode(void* pUserData,
                                              CUVIDPICPARAMS* pPic)
{
    NVTX_RANGE("HandlePictureDecode", 0xFFFF4400);
    NVDECDecoder* dec = static_cast<NVDECDecoder*>(pUserData);
    CUresult result = cuvidDecodePicture(dec->decoder, pPic);
    if (result != CUDA_SUCCESS) {
        std::cerr << "[" << dec->sensorName_ << "] Decode picture failed: " << result << "\n";
        return 0;
    }
    return 1;
}

int CUDAAPI NVDECDecoder::HandlePictureDisplay(void* pUserData,
                                               CUVIDPARSERDISPINFO* pDispInfo)
{
    NVTX_RANGE("HandlePictureDisplay", 0xFF00AAFF);
    NVDECDecoder* dec = static_cast<NVDECDecoder*>(pUserData);

    CUVIDPROCPARAMS pp{};
    pp.progressive_frame = pDispInfo->progressive_frame;
    pp.top_field_first   = pDispInfo->top_field_first;
    pp.second_field      = 0;

    CUdeviceptr dpSrcFrame  = 0;
    unsigned int nPitch     = 0;
    CUdeviceptr dpHistogram = 0;

    if (dec->histogramEnabled)
        pp.histogram_dptr = &dpHistogram;

    CUresult result;
    {
        NVTX_RANGE("MapVideoFrame", 0xFF88CCFF);
        result = cuvidMapVideoFrame(
            dec->decoder, pDispInfo->picture_index,
            &dpSrcFrame, &nPitch, &pp);
    }
    if (result != CUDA_SUCCESS) {
        std::cerr << "[" << dec->sensorName_ << "] Map video frame failed: " << result << "\n";
        return 0;
    }

    FrameQualityResult quality = dec->analyzer.analyze(
        (const unsigned char*)(uintptr_t)dpSrcFrame,
        (int)nPitch,
        dpHistogram,
        dec->histogramEnabled ? dec->decodeCaps.nMaxHistogramBins : 0,
        dec->histogramEnabled ? (int)(dec->decodeCaps.nCounterBitDepth / 8) : 0);

    if (dec->mode_ == RunMode::LEARN) {
        if (dec->frameCount % 100 == 0)
            std::cout << "  [" << dec->sensorName_ << "][Learn] frame " << dec->frameCount << "\n";
        dec->frameCount++;
        {
            NVTX_RANGE("UnmapVideoFrame", 0xFF6699CC);
            if (cuvidUnmapVideoFrame(dec->decoder, dpSrcFrame) != CUDA_SUCCESS)
                std::cerr << "cuvidUnmapVideoFrame failed\n";
        }
        return 1;
    }

    if (!quality.valid) {
        std::cerr << "[" << dec->sensorName_ << "] Frame " << dec->frameCount
                  << " | analysis error, skipping PSS report\n";
    } else {
        const ThresholdConfig& tcfg = dec->analyzer.config();
        float conf = quality.overall_confidence;
        if (conf < 0.f) conf = 0.f;
        if (conf > 100.f) conf = 100.f;
        // Integer truncation ensures deterministic counter logic and absorbs minor GPU floating-point variance
        int score_int = static_cast<int>(conf);

        if (score_int < tcfg.score_low_threshold) {
            int dist = tcfg.score_low_threshold - score_int;
            int increment = std::max(1, (tcfg.max_increment * dist + tcfg.score_low_threshold / 2) / tcfg.score_low_threshold);
            dec->alertCounter_ = std::min(dec->alertCounter_ + increment, tcfg.counter_max);
        } else if (score_int > tcfg.score_high_threshold) {
            int dist  = score_int - tcfg.score_high_threshold;
            int range = 100 - tcfg.score_high_threshold;
            int decrement = std::max(1, (tcfg.max_decrement * dist + range / 2) / range);
            dec->alertCounter_ = std::max(dec->alertCounter_ - decrement, 0);
        }

        /* Use compare_exchange rather than load+store so this edge is atomic
         * against the out-of-band FU-A path in safety_monitor.cpp. Without
         * this, both paths could observe sensorInvalid_ == false and each
         * emit a SENSOR_INVALID during the narrow window between the load
         * and the store. Same rationale for the matching SENSOR_VALID edge
         * below. */
        const bool enterInvalid =
            dec->alertCounter_ >= tcfg.counter_max &&
            score_int < tcfg.score_low_threshold;
        const bool enterValid =
            dec->alertCounter_ <= 0 &&
            score_int > tcfg.score_high_threshold;

        bool expectedFalse = false;
        bool expectedTrue = true;
        if (enterInvalid &&
            dec->sensorInvalid_.compare_exchange_strong(expectedFalse, true,
                                                        std::memory_order_acq_rel)) {
            if (dec->pssClientId_ != UINT32_MAX) {
                SafetyEvent event = {};
                event.id = dec->allocEventId();
                event.type = SENSOR_INVALID;
                event.severity = CRITICAL;
                event.fusionMetadata.pipelineID = dec->pipelineId_;
                event.fusionMetadata.clientID = static_cast<uint8_t>(dec->pssClientId_);
                event.confidenceLevel = 1.0f - (conf / 100.f);
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                event.timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
                snprintf(event.sensorIdentifier, MAX_INDENTIFIER_LENGTH, "%s", dec->sensorName_.c_str());
                event.processed = false;

                if(dec->reportSafetyEvent(dec->pssClientId_, &event) != NVPSSD_SUCCESS) {
                    std::cerr << "[" << dec->sensorName_ << "] Failed to report sensor invalid safety event\n";
                }
            } else {
                std::cerr << "[" << dec->sensorName_ << "] Frame " << dec->frameCount
                          << " | confidence: " << quality.overall_confidence << "%"
                          << " - PSS client not registered, cannot report event\n";
            }
        } else if (enterValid &&
                   dec->sensorInvalid_.compare_exchange_strong(expectedTrue, false,
                                                               std::memory_order_acq_rel)) {
            if (dec->pssClientId_ != UINT32_MAX) {
                SafetyEvent event = {};
                event.id = dec->allocEventId();
                event.type = SENSOR_VALID;
                event.severity = CRITICAL;
                event.fusionMetadata.pipelineID = dec->pipelineId_;
                event.fusionMetadata.clientID = static_cast<uint8_t>(dec->pssClientId_);
                event.confidenceLevel = conf / 100.f;
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                event.timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
                snprintf(event.sensorIdentifier, MAX_INDENTIFIER_LENGTH, "%s", dec->sensorName_.c_str());
                event.processed = false;

                if (dec->reportSafetyEvent(dec->pssClientId_, &event) != NVPSSD_SUCCESS) {
                    std::cerr << "[" << dec->sensorName_ << "] Failed to report sensor valid safety event\n";
                }
            }
        }
    }

    dec->frameCount++;

    {
        NVTX_RANGE("UnmapVideoFrame", 0xFF6699CC);
        if (cuvidUnmapVideoFrame(dec->decoder, dpSrcFrame) != CUDA_SUCCESS)
            std::cerr << "cuvidUnmapVideoFrame failed\n";
    }
    return 1;
}

bool NVDECDecoder::createParser() {
    NVTX_RANGE("CreateParser", 0xFF0088FF);
    if (parser) {
        cuvidDestroyVideoParser(parser);
        parser = nullptr;
    }
    frameCount = 0;
    alertCounter_ = 0;

    CUVIDPARSERPARAMS pp{};
    pp.CodecType              = cudaVideoCodec_H264;
    pp.ulMaxNumDecodeSurfaces = 1;
    pp.ulMaxDisplayDelay      = 0;
    pp.ulErrorThreshold       = 100;
    pp.pUserData              = this;
    pp.pfnSequenceCallback    = HandleVideoSequence;
    pp.pfnDecodePicture       = HandlePictureDecode;
    pp.pfnDisplayPicture      = HandlePictureDisplay;

    CUresult result = cuvidCreateVideoParser(&parser, &pp);
    if (result != CUDA_SUCCESS) {
        std::cerr << "[" << sensorName_ << "] Failed to create parser: " << result << "\n";
        return false;
    }
#ifdef DEBUG
    std::cout << "Parser created successfully\n";
#endif
    return true;
}

bool NVDECDecoder::decodeStream(NalQueue& queue) {
    NVTX_RANGE("DecodeStream", 0xFF00FF88);

    CUresult ctxResult = cuCtxSetCurrent(cuContext);
    if (ctxResult != CUDA_SUCCESS) {
        std::cerr << "[" << sensorName_ << "] Failed to set CUDA context on decode thread: "
                  << ctxResult << "\n";
        return false;
    }

#ifdef DEBUG
    if (mode_ == RunMode::LEARN) {
        std::cout << "[" << sensorName_ << "] Learn mode: will auto-stop after "
                  << learn_duration_sec_ << " seconds.\n";
    }
#endif
    auto streamStart = std::chrono::steady_clock::now();
    int consecutiveErrors = 0;
    bool hadErrors = false;

    NalUnit nal;
    while (queue.pop(nal)) {
        if (g_stopFlag.load() || learnComplete_.load()) break;

        if (mode_ == RunMode::LEARN) {
            auto elapsed = std::chrono::steady_clock::now() - streamStart;
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            if (elapsedSec >= learn_duration_sec_) {
#ifdef DEBUG
                std::cout << "\nLearn duration reached (" << learn_duration_sec_
                          << "s, " << frameCount << " frames). Stopping...\n";
#endif
                learnComplete_.store(true);
                break;
            }
        }

        NVTX_RANGE("ParseNALUnit", 0xFF44DD88);
        CUVIDSOURCEDATAPACKET pkt{};
        pkt.payload      = nal.data.data();
        pkt.payload_size = (unsigned long)nal.data.size();
        pkt.flags        = CUVID_PKT_TIMESTAMP;
        pkt.timestamp    = nal.timestamp;

        CUresult result = cuvidParseVideoData(parser, &pkt);
        if (result != CUDA_SUCCESS) {
            hadErrors = true;
            std::cerr << "[" << sensorName_ << "] Failed to parse streaming video data: "
                      << result << "\n";
            if (++consecutiveErrors >= 30) {
                std::cerr << "[" << sensorName_ << "] Too many consecutive parse errors ("
                          << consecutiveErrors << "), aborting decode\n";
                break;
            }
        } else {
            consecutiveErrors = 0;
        }
    }

    {
        NVTX_RANGE("ParseEOS", 0xFFDD4444);
        CUVIDSOURCEDATAPACKET eos{};
        eos.flags = CUVID_PKT_ENDOFSTREAM;
        if (cuvidParseVideoData(parser, &eos) != CUDA_SUCCESS)
            std::cerr << "[" << sensorName_ << "] EOS parse failed\n";
    }

#ifdef DEBUG
    std::cout << "\nStreaming decode complete!\n"
              << "Total frames decoded: " << frameCount << "\n"
              << "Output resolution: " << displayWidth << "x"
              << displayHeight << "\n";
#endif

    return !hadErrors;
}

bool NVDECDecoder::loadThresholdConfig(const std::string& path) {
    NVTX_RANGE("LoadThresholdConfigFile", 0xFF88FF88);
    ThresholdConfig cfg;
    if (!ThresholdConfig::loadFromFile(path, cfg)) return false;
    analyzer.setConfig(cfg);
#ifdef DEBUG
    std::cout << "Loaded threshold config from " << path << ":\n"
              << "  w_histogram=" << cfg.w_histogram
              << "  w_contrast=" << cfg.w_contrast
              << "  w_edge=" << cfg.w_edge << "\n"
              << "  baseline_mean_margin=" << cfg.baseline_mean_margin
              << "  baseline_var_margin=" << cfg.baseline_var_margin << "\n"
              << "  baseline_contrast_margin=" << cfg.baseline_contrast_margin
              << "  baseline_edge_margin=" << cfg.baseline_edge_margin << "\n"
              << "  canny_low_thresh=" << cfg.canny_low_thresh
              << "  canny_high_thresh=" << cfg.canny_high_thresh << "\n"
              << "  score_low_threshold=" << cfg.score_low_threshold
              << "  score_high_threshold=" << cfg.score_high_threshold << "\n"
              << "  counter_max=" << cfg.counter_max
              << "  max_increment=" << cfg.max_increment
              << "  max_decrement=" << cfg.max_decrement << "\n";
#endif
    return true;
}

bool NVDECDecoder::saveBaseline(const std::string& path) {
    NVTX_RANGE("SaveBaseline", 0xFF44FF88);
    BaselineValues b = analyzer.getLearnedBaseline();
    if (b.total_frames == 0) {
        std::cerr << "[" << sensorName_ << "] No frames analyzed during learning\n";
        return false;
    }
#ifdef DEBUG
    std::cout << "\nLearned baseline from " << b.total_frames << " frames:\n"
              << "  hist_mean:    " << b.hist_mean << "\n"
              << "  hist_var:     " << b.hist_var << "\n"
              << "  rms_contrast: " << b.rms_contrast << "\n"
              << "  edge_density: " << b.edge_density << "\n";
#endif
    return b.saveToFile(path);
}

bool NVDECDecoder::loadBaseline(const std::string& path) {
    NVTX_RANGE("LoadBaselineFile", 0xFF88FFAA);
    if (!BaselineValues::loadFromFile(path, baseline_)) return false;
#ifdef DEBUG
    std::cout << "Loaded baseline (" << baseline_.total_frames << " frames):\n"
              << "  hist_mean:    " << baseline_.hist_mean << "\n"
              << "  hist_var:     " << baseline_.hist_var << "\n"
              << "  rms_contrast: " << baseline_.rms_contrast << "\n"
              << "  edge_density: " << baseline_.edge_density << "\n";
#endif
    return true;
}

bool NVDECDecoder::validateThresholdConfig() const {
    return analyzer.config().validate();
}

bool NVDECDecoder::validateBaseline() const {
    return baseline_.validate();
}

void NVDECDecoder::cleanup() {
    NVTX_RANGE("CleanupPipeline", 0xFF880000);
    if (cuContext)
        cuCtxSetCurrent(cuContext);
    analyzer.cleanup();
    if (parser)  { cuvidDestroyVideoParser(parser); parser = nullptr; }
    if (decoder) { cuvidDestroyDecoder(decoder);     decoder = nullptr; }
    frameCount = 0;
    alertCounter_ = 0;
    sensorInvalid_.store(false);
    frameWidth = 0;  frameHeight = 0;
    displayWidth = 0; displayHeight = 0;
    if (cuContext) { cuDevicePrimaryCtxRelease(cuDevice_); cuContext = nullptr; }
}
