/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i_frame_quality_analyzer.h"
#include "frame_quality_analyzer_gpu.h"
#include "saim_backend_abi.h"

#include <new>

// Factory exported by libsaim_gpu.so: heap-allocates the GPU analyzer and hands
// ownership to the caller.
extern "C" IFrameQualityAnalyzer* saimCreateAnalyzer()
{
    return new (std::nothrow) sai::gpu::FrameQualityAnalyzer();
}

// GPU backend HW probe. No-op: the GPU analyzer runs on the same CUDA device
// the core already validates for NVDEC decode (cuInit + cuDeviceGetCount in
// main, before this runs), so there is no GPU-specific HW left to check here.
// Kept so every backend plugin satisfies the same ABI.
extern "C" bool saimBackendProbe()
{
    return true;
}

// Build-time backend identity.
extern "C" const char* saimBackendName()
{
    return "GPU";
}