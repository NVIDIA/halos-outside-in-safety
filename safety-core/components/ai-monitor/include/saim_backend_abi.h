/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAIM_BACKEND_ABI_H
#define SAIM_BACKEND_ABI_H

// ABI for backend plugins used by SAI core and backend libraries.
// Ensures all exported symbols have matching signatures.

class IFrameQualityAnalyzer;

// Heap-allocates the backend analyzer and transfers ownership to the caller.
// Returns nullptr on allocation failure (OOM).
extern "C" IFrameQualityAnalyzer* saimCreateAnalyzer();

// One-time backend HW probe, run before any analyzer is constructed. Returns
// true if the backend HW is usable: a no-op true on GPU, whose CUDA device is
// validated by the core CUDA init.
extern "C" bool saimBackendProbe();

// Build-time identity of the linked backend plugin: "GPU".
extern "C" const char* saimBackendName();

#endif  // SAIM_BACKEND_ABI_H
