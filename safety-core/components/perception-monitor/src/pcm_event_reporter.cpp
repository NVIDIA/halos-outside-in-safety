/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcm_event_reporter.h"
#include <chrono>

#include "pcm_log.h"
#include "pss_daemon.h"
#include "pss_protocol.h"

#include <cstddef>
#include <ctime>

namespace {

static uint64_t monotonicNowNs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    if (ts.tv_sec < 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

PcmEventReporter::PcmEventReporter(uint32_t pssClientId, int resendIntervalMs)
    : pssClientId_(pssClientId)
    , resendInterval_(resendIntervalMs)
{}

bool PcmEventReporter::reportIfStateChanged(const PcmPipelineState& state,
                                           std::chrono::steady_clock::time_point now) {
    if (!state.judged) return true;
    if (hasEmitted_ && state.invalid == lastEmittedInvalid_ &&
        now - lastSentAt_ < resendInterval_) return true;
    const bool resend = hasEmitted_ && state.invalid == lastEmittedInvalid_;

    SafetyEvent ev{};
    ev.id              = nextEventId_++;
    ev.type            = state.invalid ? AI_PIPELINE_INVALID : AI_PIPELINE_VALID;
    ev.timestamp       = monotonicNowNs();
    ev.confidenceLevel = 1.0f;
    ev.processed       = false;
    ev.fusionMetadata.pipelineID = 0;
    ev.fusionMetadata.clientID   = static_cast<uint8_t>(pssClientId_);

    if (NvPSSReportSafetyEvent(pssClientId_, &ev) != NVPSSD_SUCCESS) {
        PcmLogLine(PcmLogLevel::Err) << "NvPSSReportSafetyEvent failed (type="
                                     << static_cast<int>(ev.type) << "); will retry";
        return false;
    }

    PcmLogLine line(state.invalid ? PcmLogLevel::Err : PcmLogLevel::Info);
    line << "-> " << (state.invalid ? "AI_PIPELINE_INVALID" : "AI_PIPELINE_VALID")
         << (resend ? " (re-sent, unchanged)" : "");
    if (state.invalid) {
        line << "  active=[";
        for (std::size_t i = 0; i < state.activeFaults.size(); ++i) {
            line << (i == 0 ? "" : ", ") << state.activeFaults[i];
        }
        line << "]";
    }

    hasEmitted_ = true;
    lastEmittedInvalid_ = state.invalid;
    lastSentAt_ = now;
    return true;
}
