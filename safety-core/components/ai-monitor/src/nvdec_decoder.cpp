/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nvdec_decoder.h"
#include "rtsp_client.h"
#include "safety_event_reporter.h"
#include "i_frame_quality_analyzer.h"
#include "sai_common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

std::atomic<uint32_t> NVDECDecoder::s_nextEventId_{1};

namespace {
// Frame rate exactly as the sequence header reports it, or 0 when it carries
// none or an unusable one.
double sequenceFps(const CUVIDEOFORMAT* fmt)
{
    if (fmt->frame_rate.numerator == 0 || fmt->frame_rate.denominator == 0)
        return 0.0;
    const double fps = (double)fmt->frame_rate.numerator
                     / (double)fmt->frame_rate.denominator;
    return std::isfinite(fps) ? fps : 0.0;
}

// Rate the LEARN frame target and the ACTIVE tile waits are counted in.
// Substitutes a default rather than refusing.
double streamRate(double seqFps, const std::string& sensorName)
{
    if (seqFps <= 0.0) {
        std::cerr << "[" << sensorName
                  << "] WARNING: sequence header carries no frame rate; using"
                     " fallback " << LEARN_FPS_FALLBACK << " fps.\n";
        return LEARN_FPS_FALLBACK;
    }
    if (seqFps > LEARN_FPS_MAX) {
        std::cerr << "[" << sensorName << "] WARNING: stream FPS " << seqFps
                  << " exceeds cap " << LEARN_FPS_MAX << "; clamping.\n";
        return LEARN_FPS_MAX;
    }
    return seqFps;
}

// Slots for the LEARN filter's buffer: the frame target plus ~6% and 64 spare,
// because the target check runs per NAL rather than per frame and LEARN
// overshoots by a few. 300 s @ 30 fps -> 9626 slots, ~600 KB.
//
// Sized from the observed rate, not LEARN_FPS_MAX, so a 30 fps camera does not
// pay 8x for a 240 fps buffer. Safe only because init() never reallocates and
// the caller clamps a later, larger target down to this.
uint64_t learnFilterCapacity(uint64_t targetFrames)
{
    const uint64_t want = targetFrames + targetFrames / 16U + 64U;
    // Clamped rather than passed through: init() refuses an over-large request
    // outright, whereas the caller caps its target at capacity() and logs that
    // LEARN will end short, the better outcome of the two.
    return (want > sai::kLearnMaxSlots) ? sai::kLearnMaxSlots : want;
}
}

NVDECDecoder::NVDECDecoder()
    : cuContext(nullptr), decoder(nullptr), parser(nullptr),
      frameWidth(0), frameHeight(0), displayWidth(0), displayHeight(0),
      frameCount(0)
{}

bool NVDECDecoder::initialize() {
    NVTX_RANGE("CUDAInit", 0xFF0000FF);

    // Construct the per-camera analyzer (from the linked backend plugin) before
    // any CUDA work. Backend runtime init is verified once at startup
    // (loadAnalyzerBackend in main), so a null here is an analyzer allocation
    // failure; bail before doing expensive CUDA setup.
    analyzer_ = createFrameQualityAnalyzer();
    if (!analyzer_) {
        std::cerr << "[" << sensorName_ << "] failed to allocate "
                  << analyzerBackendName()
                  << " frame-quality analyzer\n";
        return false;
    }

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
              << "  Max MB Count: " << decodeCaps.nMaxMBCount << "\n";
#endif
    return true;
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

    // Ensure decode target has even dimensions for NV12 chroma and downstream processing.
    // Odd display sizes are rounded down; even inputs remain unchanged.
    if ((dec->displayWidth & 1) != 0 || (dec->displayHeight & 1) != 0) {
        std::cerr << "[" << dec->sensorName_ << "] Odd display dimensions "
                  << dec->displayWidth << "x" << dec->displayHeight
                  << " rounded down to even for NV12 analysis\n";
        dec->displayWidth  &= ~1;
        dec->displayHeight &= ~1;
    }

#ifdef DEBUG
    std::cout << "  Display Area: " << dec->displayWidth << "x"
              << dec->displayHeight << "\n";
#endif

    // Set before the resolution checks below so their early returns cannot
    // leave streamFps_ and learn_target_frames_ describing different headers.
    double oldFps = 0.0;
    double newFps = 0.0;
    if (dec->mode_ == RunMode::LEARN) {
        oldFps = dec->streamFps_;
        newFps = streamRate(sequenceFps(pFormat), dec->sensorName_);
        dec->streamFps_ = newFps;
    } else {
        dec->tileScorer_.setFps(streamRate(sequenceFps(pFormat), dec->sensorName_));
    }

    // LEARN: on FPS change, recompute the target. Same resolution keeps the
    // frames already counted (remaining-based, holds wall-clock at
    // learn_duration_sec_). Resolution change restarts at the new FPS.
    if (dec->mode_ == RunMode::LEARN && dec->learn_target_frames_ != 0) {
        constexpr double kLearnFpsChangeEpsilon = 1e-3;
        if (std::fabs(newFps - oldFps) > kLearnFpsChangeEpsilon) {
            const uint64_t done = dec->analyzer_->getLearnFramesProcessed();
            const bool resolution_changing =
                (dec->displayWidth  != dec->analyzer_->getWidth()) ||
                (dec->displayHeight != dec->analyzer_->getHeight());

            uint64_t new_target = 0;
            if (resolution_changing) {
                // The analyzer's count and the filter's buffer are both about to
                // be discarded, so restart LEARN with the full duration at the
                // new FPS rather than under-sampling the new resolution.
                new_target = (uint64_t)std::ceil(
                    (double)dec->learn_duration_sec_ * newFps);
                std::cerr << "[" << dec->sensorName_
                          << "] LEARN: FPS+resolution change ("
                          << oldFps << " fps@"
                          << dec->analyzer_->getWidth() << "x"
                          << dec->analyzer_->getHeight()
                          << " -> " << newFps << " fps@"
                          << dec->displayWidth << "x"
                          << dec->displayHeight
                          << "); discarding " << done
                          << " accumulated frames, restarting full LEARN,"
                          << " new_target=" << new_target << " frames\n";
            } else {
                // Same resolution: progress preserved, adjust by remaining
                // wall-clock so total LEARN time stays = learn_duration_sec_.
                const double covered_sec =
                    (oldFps > 0.0) ? (double)done / oldFps : 0.0;
                const double remaining_sec =
                    ((double)dec->learn_duration_sec_ > covered_sec)
                        ? ((double)dec->learn_duration_sec_ - covered_sec)
                        : 0.0;
                new_target = done +
                    (uint64_t)std::ceil(remaining_sec * newFps);
                std::cerr << "[" << dec->sensorName_
                          << "] LEARN: FPS change ("
                          << oldFps << " -> " << newFps
                          << "); done=" << done
                          << " covered=" << covered_sec << "s"
                          << " remaining=" << remaining_sec << "s"
                          << " new_target=" << new_target << " frames\n";
            }

            dec->learn_target_frames_ = new_target;
        }
    }

    // First sequence only: convert duration into the initial frame target
    // using this sequence's FPS, with fallback / clamp on bad headers. After
    // this point learn_target_frames_ is always > 0 and FPS changes are
    // handled by the remaining-based recompute above.
    if (dec->mode_ == RunMode::LEARN && dec->learn_target_frames_ == 0) {
        dec->learn_target_frames_ = (uint64_t)std::ceil(
            (double)dec->learn_duration_sec_ * newFps);
        std::cout << "[" << dec->sensorName_ << "] LEARN target: "
                  << dec->learn_target_frames_ << " frames ("
                  << dec->learn_duration_sec_ << "s @ " << newFps << " fps)\n";
    }

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
    ci.ulNumOutputSurfaces = kNumDecodeOutputSurfaces;
    ci.ulCreationFlags     = cudaVideoCreate_PreferCUVID;
    ci.vidLock             = nullptr;
    ci.display_area.left   = (short)pFormat->display_area.left;
    ci.display_area.top    = (short)pFormat->display_area.top;
    ci.display_area.right  = (short)(pFormat->display_area.left + dec->displayWidth);
    ci.display_area.bottom = (short)(pFormat->display_area.top  + dec->displayHeight);

    CUresult result;
    {
        NVTX_RANGE("CreateDecoder", 0xFFCC0000);
        result = cuvidCreateDecoder(&dec->decoder, &ci);
    }
    if (result != CUDA_SUCCESS) {
        std::cerr << "[" << dec->sensorName_ << "] Failed to create decoder: " << result << "\n";
        return 0;
    }
    // Save LEARN count to detect a restart due to a resolution change.
    const uint64_t learnFramesBefore = dec->analyzer_->getLearnFramesProcessed();

    // NVDEC reports 0 when the stream does not signal its range, which H.264
    // reads as limited (16-235).
    const bool fullRange = pFormat->video_signal_description.video_full_range_flag != 0;
    const LumaLines lines = lumaLines(fullRange);

    // A profile's texture numbers are per pixel, so it scores only the size it
    // was learned at.
    const bool sizeMismatch = dec->mode_ == RunMode::ACTIVE &&
                              (dec->baseline_.frame_width  != dec->displayWidth ||
                               dec->baseline_.frame_height != dec->displayHeight);
    if (sizeMismatch) {
        std::cerr << "[" << dec->sensorName_ << "] profile was learned at "
                  << dec->baseline_.frame_width << "x" << dec->baseline_.frame_height
                  << " but the stream is " << dec->displayWidth << "x"
                  << dec->displayHeight << "; re-run LEARN for this camera\n";
    }

    {
        NVTX_RANGE("InitAnalyzer", 0xFFFF8800);
        if (sizeMismatch ||
            !dec->analyzer_->init(dec->displayWidth, dec->displayHeight, lines)) {
            std::cerr << "[" << dec->sensorName_
                      << "] FATAL: frame quality analyzer cannot start; "
                         "shutting down this stream\n";
            cuvidDestroyDecoder(dec->decoder);
            dec->decoder = nullptr;
            // Set permanent-fault flag directly; sensorInvalid_ is always true at cold start,
            // so edge tests would miss boot failures. sensorInvalid_ is set regardless for
            // mid-session VALID cases.
            dec->sensorInvalid_.store(true, std::memory_order_release);
            bool alreadyFaulted = false;
            if (dec->analyzerPermanentFault_.compare_exchange_strong(
                    alreadyFaulted, true, std::memory_order_acq_rel)) {
                if (dec->eventReporter_ != nullptr) {
                    dec->eventReporter_->update_slot(dec->sensorName_,
                                                     SENSOR_INVALID,
                                                     monotonic_now_ns(), 1.0f,
                                                     dec->allocEventId(),
                                                     &kSaimInternalError);
                    std::cerr << "[" << dec->sensorName_
                              << "] SENSOR_INVALID handed to reporter "
                                 "(analyzer cannot start)\n";
                } else {
                    std::cerr << "[" << dec->sensorName_
                              << "] analyzer cannot start but eventReporter_ "
                                 "not set; no SENSOR_INVALID emitted\n";
                }
            }
            return 0;
        }
    }

    // Per-camera fault-isolation wiring.
    dec->analyzer_->setSensorInvalidFlag(&dec->sensorInvalid_);
    dec->analyzer_->setPermanentFaultFlag(&dec->analyzerPermanentFault_);
    dec->analyzer_->setReportInvalidCallback([dec](const char* reason) {
        if (dec->eventReporter_ == nullptr) {
            std::cerr << "[" << dec->sensorName_
                      << "] analyzer reported INVALID (" << reason
                      << ") but eventReporter_ not set\n";
            return;
        }
        dec->eventReporter_->update_slot(dec->sensorName_, SENSOR_INVALID,
                                         monotonic_now_ns(), 1.0f,
                                         dec->allocEventId(),
                                         &kSaimInternalError);
        std::cerr << "[" << dec->sensorName_
                  << "] SENSOR_INVALID handed to reporter (" << reason << ")\n";
    });

    dec->analyzer_->setMode(dec->mode_);

    if (dec->mode_ == RunMode::LEARN) {
        // Allocates on the first sequence header and is a no-op after, so the
        // capacity reflects the rate seen first.
        const uint64_t slots = learnFilterCapacity(dec->learn_target_frames_);
        if (!dec->learnFilter_.init(slots) ||
            !dec->tileLearn_.init(slots, dec->analyzer_->config())) {
            // Fatal now rather than at save time: an unfiltered baseline is not
            // the one requested, and failing here costs seconds, not a full run.
            std::cerr << "[" << dec->sensorName_
                      << "] LEARN: cannot allocate the LEARN buffers for "
                      << dec->learn_target_frames_ << " frames\n";
            return 0;
        }
        dec->tileLearn_.setFps(dec->streamFps_);

        // An FPS rise raises the target but not the buffer. Cap the target at
        // the buffer so every frame LEARN counts is one the filter kept: the
        // alternative is a target the buffer cannot serve, where the excess
        // lands in overflow_ and thins the baseline invisibly. LEARN then ends
        // short of learn_duration_sec_, which the log states.
        if (dec->learn_target_frames_ > dec->learnFilter_.capacity()) {
            std::cerr << "[" << dec->sensorName_ << "] LEARN: target "
                      << dec->learn_target_frames_
                      << " frames exceeds the buffer allocated at the initial"
                         " rate (" << dec->learnFilter_.capacity()
                      << "); clamping, so LEARN ends short of "
                      << dec->learn_duration_sec_ << "s\n";
            dec->learn_target_frames_ = dec->learnFilter_.capacity();
        }

        // Follow the analyzer's restart. Its metrics and the buffered ones must
        // describe one resolution: median and MAD over two populations
        // characterise neither, and the mixed spread would either widen the
        // baseline or push the discard fraction past finalize()'s limit.
        if (dec->analyzer_->getLearnFramesProcessed() < learnFramesBefore) {
            std::cerr << "[" << dec->sensorName_
                      << "] LEARN: analyzer restarted; discarding "
                      << dec->learnFilter_.frames()
                      << " buffered frame(s) from the previous configuration\n";
            dec->learnFilter_.reset();
            dec->tileLearn_.reset();
        }
    }

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
    // The map copies the frame on output_stream. Without the analyzer's own stream
    // here, a non-blocking analyzer stream reads the surface before the copy lands.
    pp.output_stream     = dec->analyzer_->stream();

    CUdeviceptr dpSrcFrame = 0;
    unsigned int nPitch    = 0;

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

    FrameQualityResult quality = dec->analyzer_->analyze(
        (const unsigned char*)(uintptr_t)dpSrcFrame, (int)nPitch);

    if (dec->mode_ == RunMode::LEARN) {
        // Buffered, not accumulated: which frames belong in the baseline is not
        // decidable until the run is in hand. Kept across a reconnect, which
        // does not invalidate same-resolution measurements of the same scene.
        //
        // Only analysed frames. Both backends set valid on the same path that
        // increments the LEARN count, so buffering exactly these keeps the two
        // equal and the buffer cannot fill before LEARN reaches its target.
        if (quality.valid) {
            dec->learnFilter_.add(quality);
            dec->tileLearn_.add(quality);
        }
        if (dec->frameCount % kLearnProgressLogIntervalFrames == 0)
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
        const ThresholdConfig& tcfg = dec->analyzer_->config();
        // An integer score keeps the counter logic deterministic.
        const int score_int = dec->tileScorer_.update(quality);
        const float conf = static_cast<float>(score_int);

        if (score_int < tcfg.score_low_threshold) {
            const int low  = tcfg.score_low_threshold;
            const int dist = low - score_int;
            /* Quadratic in dist, so patience scales with severity instead of
             * one flat rate: a score just under the threshold takes counter_max
             * frames to latch, a score of 0 takes counter_max / max_increment.
             * Raising max_increment is what buys resolution - reshaping the
             * curve cannot, since the step is an integer in [1, max_increment].
             *
             * The decrement below must keep the same shape. A linear decrement
             * would outpace this increment everywhere except the endpoints, so
             * corruption that stops and restarts would never re-latch.
             *
             * Integer throughout so the decision is bit-exact. max_increment <=
             * counter_max <= COUNTER_MAX_LIMIT and dist < low < 100 keep the
             * product well inside int32; see ThresholdConfig::validate().
             */
            const int increment = std::max(1, (tcfg.max_increment * dist * dist
                                               + (low * low) / 2) / (low * low));
            dec->alertCounter_ = std::min(dec->alertCounter_ + increment, tcfg.counter_max);
        } else if (score_int > tcfg.score_high_threshold) {
            const int range = 100 - tcfg.score_high_threshold;
            const int dist  = score_int - tcfg.score_high_threshold;
            const int decrement = std::max(1, (tcfg.max_decrement * dist * dist
                                               + (range * range) / 2) / (range * range));
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
            if (dec->eventReporter_ != nullptr) {
                dec->eventReporter_->update_slot(dec->sensorName_,
                                                 SENSOR_INVALID,
                                                 monotonic_now_ns(),
                                                 1.0f - (conf / 100.f),
                                                 dec->allocEventId(),
                                                 &kSaimInputDegraded);
            } else {
                std::cerr << "[" << dec->sensorName_ << "] Frame " << dec->frameCount
                          << " | score: " << score_int
                          << " - event reporter not set, cannot report event\n";
            }
        } else if (enterValid &&
                   dec->sensorInvalid_.compare_exchange_strong(expectedTrue, false,
                                                               std::memory_order_acq_rel)) {
            if (dec->eventReporter_ != nullptr) {
                dec->eventReporter_->update_slot(dec->sensorName_,
                                                 SENSOR_VALID,
                                                 monotonic_now_ns(),
                                                 conf / 100.f,
                                                 dec->allocEventId(),
                                                 &kSaimSensorHealthy);
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
    // Fail-safe reset: INVALID + counter at max so SENSOR_VALID requires
    // a sustained good-frame streak to fire.
    alertCounter_ = analyzer_->config().counter_max;
    // Edge-gate: emit only when sensorInvalid_ transitions false->true,
    // i.e. a mid-session reconnect after the sensor had reached VALID.
    // If it was already true the reporter slot still mirrors INVALID, so
    // skipping avoids a redundant PSS send (update_slot_impl would accept
    // and re-dirty the slot since the new timestamp is newer).
    bool wasValid = false;
    if (sensorInvalid_.compare_exchange_strong(wasValid, true,
                                               std::memory_order_acq_rel) &&
        eventReporter_ != nullptr) {
        eventReporter_->update_slot(sensorName_, SENSOR_INVALID,
                                    monotonic_now_ns(), 1.0f,
                                    allocEventId(),
                                    &kSaimStreamDisconnect);
        std::cerr << "[" << sensorName_
                  << "] SENSOR_INVALID handed to reporter "
                     "(reconnect / parser (re)create)\n";
    }

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

// Always false, so callers can tail-call it to retire the tick.
bool NVDECDecoder::reportInitSeedFault(uint64_t now, bool noKeyframe)
{
    gateWatchdogDone_ = true;

    const char* reason = noKeyframe ? "no keyframe from the source"
                                    : "never reached VALID";
    const int   waited = noKeyframe ? kIdrWaitTimeoutSec : kInitSeedGraceSec;

    if (mode_ == RunMode::LEARN) {
        // No reporter in LEARN. Same exit a connection failure takes.
        std::cerr << "[" << sensorName_ << "] LEARN mode: aborting all pipelines, "
                  << reason << " after " << waited << "s\n";
        g_stopFlag.store(true);
        return false;
    }
    if (eventReporter_ == nullptr) return false;

    // Not edge-gated: sensorInvalid_ already true; gateWatchdogDone_ limits to one emit.
    const EventCause cause = noKeyframe ? kSaimStreamDisconnect : kSaimInputDegraded;
    eventReporter_->update_slot(sensorName_, SENSOR_INVALID, now, 1.0f,
                                allocEventId(), &cause);
    std::cerr << "[" << sensorName_ << "] SENSOR_INVALID handed to reporter ("
              << reason << " after " << waited << "s)\n";
    return false;
}

void NVDECDecoder::onStreamSessionStart()
{
    // ACTIVE's budget is process-wide by design; only LEARN gets a fresh one.
    if (mode_ == RunMode::ACTIVE) return;
    idrWaitDeadlineNs_ = 0;
    gateWatchdogDone_  = false;
}

bool NVDECDecoder::noteStreamGateState(bool gateOpen)
{
    constexpr uint64_t kNsPerSec = 1000000000ULL;
    if (gateWatchdogDone_) return false;

    const uint64_t now = monotonic_now_ns();
    if (now == 0) return true;   // clock-failure sentinel; retry on the next NAL

    if (!gateOpen) {
        if (idrWaitDeadlineNs_ == 0) {
            idrWaitDeadlineNs_ = now + (uint64_t)kIdrWaitTimeoutSec * kNsPerSec;
            return true;
        }
        return (now < idrWaitDeadlineNs_) ? true : reportInitSeedFault(now, true);
    }

    // Gate open: the source supplied a random access point, so frames now flow.
    // That is all LEARN needed - it has no seed to clear and no frame scores.
    if (mode_ != RunMode::ACTIVE) {
        gateWatchdogDone_ = true;
        return false;
    }

    // Tested before the deadline, not only at expiry: sensorInvalid_ is a level,
    // so a sensor that cleared the seed and later degraded would otherwise be
    // reported as having never cleared it. First clear observed settles it.
    if (!sensorInvalid_.load(std::memory_order_acquire)) {
        gateWatchdogDone_ = true;
        return false;
    }

    if (initSeedDeadlineNs_ == 0) {
        initSeedDeadlineNs_ = now + (uint64_t)kInitSeedGraceSec * kNsPerSec;
        return true;
    }
    if (now < initSeedDeadlineNs_) return true;
    return reportInitSeedFault(now, false);
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
                  << learn_target_frames_ << " analyzed frames.\n";
    }
#endif
    unsigned int consecutiveErrors = 0U;  // matches kMaxConsecutiveParseErrors' type
    bool hadErrors = false;

    NalUnit nal;
    while (queue.pop(nal)) {
        if (g_stopFlag.load() || learnComplete_.load()) break;

        // Per-camera give-up: analyzer exhausted its reinit budget (or
        // HandleVideoSequence failed to initialize the analyzer). Exit the
        // NAL loop so runStreamPipeline skips reconnect and only this
        // thread terminates. Marking hadErrors keeps decodeStream()'s
        // contract honest: true only for clean EOS / stop / LEARN-complete;
        // false for any fault-driven early termination.
        if (analyzerPermanentFault_.load(std::memory_order_acquire)) {
            std::cerr << "[" << sensorName_
                      << "] analyzer permanent fault detected; stopping decode\n";
            hadErrors = true;
            break;
        }

        // LEARN stops on accumulated frame count, not wall-clock, so disconnect
        // gaps and reconnects do not over-count idle time. learn_target_frames_
        // is 0 until HandleVideoSequence has computed it from the stream FPS.
        if (mode_ == RunMode::LEARN && learn_target_frames_ > 0) {
            uint64_t done = analyzer_->getLearnFramesProcessed();
            if (done >= learn_target_frames_) {
#ifdef DEBUG
                std::cout << "\nLEARN target reached (" << done << "/"
                          << learn_target_frames_ << " frames). Stopping...\n";
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
            if (++consecutiveErrors >= kMaxConsecutiveParseErrors) {
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

    if (mode_ == RunMode::LEARN && learn_target_frames_ > 0 &&
        analyzer_->getLearnFramesProcessed() >= learn_target_frames_) {
        learnComplete_.store(true);
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
    if (!ThresholdConfig::loadFromFile(path, cfg) || !cfg.validate()) {
        std::cerr << "[" << sensorName_ << "] WARNING: threshold config " << path
                  << " is missing or invalid; using built-in defaults\n";
        cfg = ThresholdConfig{};
        if (!cfg.validate()) return false;
    }
    analyzer_->setConfig(cfg);
#ifdef DEBUG
    std::cout << "Loaded threshold config from " << path << ":\n"
              << "  w_histogram=" << cfg.w_histogram
              << "  w_contrast=" << cfg.w_contrast
              << "  w_edge=" << cfg.w_edge
              << "  w_entropy=" << cfg.w_entropy
              << "  w_laplacian=" << cfg.w_laplacian << "\n"
              << "  k=" << cfg.k
              << "  sigma_floor_fraction=" << cfg.sigma_floor_fraction
              << "  score_at_k_sigma=" << cfg.score_at_k_sigma << "\n"
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
    BaselineValues b = analyzer_->getLearnedBaseline();
    if (b.total_frames == 0) {
        std::cerr << "[" << sensorName_ << "] No frames analyzed during learning\n";
        return false;
    }
    // Pin the canny thresholds used during LEARN so ACTIVE reproduces the same
    // edge map.
    b.canny_low_thresh  = analyzer_->config().canny_low_thresh;
    b.canny_high_thresh = analyzer_->config().canny_high_thresh;
    // Overwrites the backend's means, sigmas and total_frames with the
    // outlier-filtered survivors, so every statistic describes one population.
    if (!learnFilter_.finalize(sensorName_, b) || !tileLearn_.finalize(sensorName_, b.tile))
        return false;
    b.frame_width  = analyzer_->getWidth();
    b.frame_height = analyzer_->getHeight();
    // Reject under-sampled LEARN: sigma from too few frames is dominated by sampling noise.
    if (b.total_frames < MIN_LEARN_FRAMES_FOR_SIGMA) {
        std::cerr << "[" << sensorName_ << "] LEARN ended with only "
                  << b.total_frames << " frames (< MIN_LEARN_FRAMES_FOR_SIGMA="
                  << MIN_LEARN_FRAMES_FOR_SIGMA << "); baseline not saved\n";
        return false;
    }
#ifdef DEBUG
    std::cout << "\nLearned baseline from " << b.total_frames << " frames:\n";
    for (int m = 0; m < QM_COUNT; ++m)
        std::cout << "  " << kQualityMetricNames[m] << ": " << b.mu[m] << "  (lo="
                  << b.std_lo[m] << " hi=" << b.std_hi[m] << ")\n";
    std::cout << "  canny_low_thresh:  " << b.canny_low_thresh << "\n"
              << "  canny_high_thresh: " << b.canny_high_thresh << "\n";
#endif
    // Same gate loadBaseline applies: writing a file the loader will refuse
    // defers a diagnosable failure to next startup.
    if (!b.validate()) {
        std::cerr << "[" << sensorName_ << "] computed baseline failed"
                     " validation; not saved\n";
        return false;
    }
    return b.saveToFile(path);
}

bool NVDECDecoder::loadBaseline(const std::string& path) {
    NVTX_RANGE("LoadBaselineFile", 0xFF88FFAA);

    // Pre-seed canny from the current analyzer config so a baseline.cfg that
    // omits canny_* keys (older files) cleanly falls back to thresholds.cfg
    // values, or to CANNY_*_THRESH compile-time defaults when those are also
    // absent. baseline.cfg's canny_* keys (if present) overwrite the pre-seed.
    baseline_.canny_low_thresh  = analyzer_->config().canny_low_thresh;
    baseline_.canny_high_thresh = analyzer_->config().canny_high_thresh;

    if (!BaselineValues::loadFromFile(path, baseline_)) return false;

    // Push the resolved canny back into the analyzer so the edge kernel uses
    // the same thresholds as LEARN (when baseline.cfg pinned them) or the
    // fallback chain (when it didn't).
    ThresholdConfig cfg = analyzer_->config();
    cfg.canny_low_thresh  = baseline_.canny_low_thresh;
    cfg.canny_high_thresh = baseline_.canny_high_thresh;
    analyzer_->setConfig(cfg);

#ifdef DEBUG
    std::cout << "Loaded baseline (" << baseline_.total_frames << " frames):\n";
    for (int m = 0; m < QM_COUNT; ++m)
        std::cout << "  " << kQualityMetricNames[m] << ": " << baseline_.mu[m] << "  (lo="
                  << baseline_.std_lo[m] << " hi=" << baseline_.std_hi[m] << ")\n";
    std::cout << "  canny_low_thresh:  " << baseline_.canny_low_thresh << "\n"
              << "  canny_high_thresh: " << baseline_.canny_high_thresh << "\n";
#endif
    return tileScorer_.configure(baseline_, analyzer_->config());
}

bool NVDECDecoder::validateBaseline() const {
    return baseline_.validate();
}

void NVDECDecoder::cleanup() {
    NVTX_RANGE("CleanupPipeline", 0xFF880000);
    if (cuContext)
        cuCtxSetCurrent(cuContext);
    if (analyzer_) analyzer_->cleanup();
    if (parser)  { cuvidDestroyVideoParser(parser); parser = nullptr; }
    if (decoder) { cuvidDestroyDecoder(decoder);     decoder = nullptr; }
    frameCount = 0;
    alertCounter_ = 0;
    // Cleared together: a stale target with a zeroed rate would send a reused
    // decoder down the FPS-change path on its first sequence header.
    streamFps_           = 0.0;
    learn_target_frames_ = 0;
    // Preserve INVALID on teardown so racing reads never see a stale "valid".
    // alertCounter_=0 is irrelevant here (parser destroyed); createParser()
    // re-seeds if the decoder is reused.
    sensorInvalid_.store(true);
    analyzerPermanentFault_.store(false);
    frameWidth = 0;  frameHeight = 0;
    displayWidth = 0; displayHeight = 0;
    if (cuContext) { cuDevicePrimaryCtxRelease(cuDevice_); cuContext = nullptr; }
}
