/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_EVENT_REPORTER_H
#define PCM_EVENT_REPORTER_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

/*
 * Aggregated PCM pipeline state handed to the reporter: the overall valid/invalid
 * verdict plus the sorted set of currently-active fault keys. (The dead event-
 * generator class that formerly held this struct was removed; the live
 * aggregation is owned by PcmFaultAggregator.)
 */
struct PcmPipelineState
{
    bool invalid = false;
    /* A starting value is not a verdict: only the aggregator's completed
     * judgement may clear a previous PCM instance's INVALID at PSS. */
    bool judged = false;
    std::vector<std::string> activeFaults;
};

/*
 * Thin PSS reporting boundary for PCM. The first verdict of a run is always
 * sent (so a restarted PCM republishes its validated state). Changes are sent
 * immediately and an unchanged verdict is repeated on a steady-clock interval.
 */
class PcmEventReporter
{
public:
    explicit PcmEventReporter(uint32_t pssClientId, int resendIntervalMs = 5000);

    /* Ignore unjudged states. Send the first/changed judged verdict or
     * a periodic repeat. A failed send advances neither state nor successful-send
     * time, so the next call retries. @p now permits deterministic timing tests. */
    bool reportIfStateChanged(const PcmPipelineState& state,
                              std::chrono::steady_clock::time_point now =
                                  std::chrono::steady_clock::now());

private:
    uint32_t pssClientId_;
    uint32_t nextEventId_ = 1U;
    bool     hasEmitted_ = false;
    bool     lastEmittedInvalid_ = false;
    const std::chrono::milliseconds resendInterval_;
    std::chrono::steady_clock::time_point lastSentAt_{};
};

#endif /* PCM_EVENT_REPORTER_H */
