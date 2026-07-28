/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

// Backend factory + plugin loader. The GPU analyzer is built as a plugin shared
// library (libsaim_gpu.so) that is linked into safety_monitor at build time.

#include "i_frame_quality_analyzer.h"
#include "saim_backend_abi.h"  // saimCreateAnalyzer / saimBackendProbe / saimBackendName

#include <memory>
#include <mutex>

namespace {
// loadAnalyzerBackend() runs at startup and again per stream (separate threads).
// call_once runs the plugin probe exactly once and caches the result, so
// concurrent first-callers serialize and later streams reuse the cached value
// (a failed probe is not retried per stream). saimBackendProbe validates
// backend HW; it is a no-op on GPU, whose CUDA device is validated by the core
// CUDA init before this runs.
std::once_flag g_init_once;
bool           g_init_ok = false;
}  // namespace

bool loadAnalyzerBackend()
{
    std::call_once(g_init_once, [] { g_init_ok = saimBackendProbe(); });
    return g_init_ok;
}

const char* analyzerBackendName()
{
    return saimBackendName();
}

std::unique_ptr<IFrameQualityAnalyzer>
createFrameQualityAnalyzer()
{
    // Run the one-time backend HW probe (a no-op on GPU) before constructing.
    // saimCreateAnalyzer may return nullptr on OOM; unique_ptr handles that.
    if (!loadAnalyzerBackend())
        return nullptr;
    return std::unique_ptr<IFrameQualityAnalyzer>(saimCreateAnalyzer());
}
