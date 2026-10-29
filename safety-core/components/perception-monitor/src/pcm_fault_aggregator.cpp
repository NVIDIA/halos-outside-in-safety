/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcm_fault_aggregator.h"

#include "pcm_log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

/* Evaluation cadence. The health and DVM threads each report about once per
 * second; the evaluator combines the latest reports every kEvalTickMs. */
static constexpr int kEvalTickMs = 1000; /* 1 s tick */

/* Ticks needed to cover @p ms of good results (rounded up; ms <= 0 -> 0, which
 * start() rejects). Split to avoid overflow for ms near INT_MAX. */
static int ticksForMs(int ms)
{
    if (ms <= 0) return 0;
    return (ms / kEvalTickMs) + ((ms % kEvalTickMs) != 0 ? 1 : 0);
}

} // namespace

PcmFaultAggregator::PcmFaultAggregator(uint32_t pssClientId,
                                       const PcmAggregatorSettings& settings)
    : reporter_(pssClientId, settings.verdictResendMs)
    , settings_(settings)
    , initialValidTicks_(ticksForMs(settings.initialValidMs))
    , createdAt_(std::chrono::steady_clock::now())
{}

PcmFaultAggregator::~PcmFaultAggregator()
{
    stop();
}

void* PcmFaultAggregator::evaluatorThunk(void* self)
{
    static_cast<PcmFaultAggregator*>(self)->evaluatorLoop();
    return nullptr;
}

bool PcmFaultAggregator::start()
{
    if (settings_.badTicksToInvalid <= 0 || settings_.goodTicksToValid <= 0 ||
        settings_.initialValidMs <= 0 || settings_.maxReportAgeMs <= 0 ||
        settings_.verdictResendMs <= 0) {
        PcmLogLine(PcmLogLevel::Err) << "fault aggregator requires positive timing settings";
        return false;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    const int rc = pthread_create(&thread_, nullptr,
                                  &PcmFaultAggregator::evaluatorThunk, this);
    if (rc != 0) {
        running_.store(false);
        PcmLogLine(PcmLogLevel::Err) << "fault aggregator thread spawn failed: "
                                     << std::strerror(rc);
        return false;
    }
    threadValid_ = true;
    PcmLogLine(PcmLogLevel::Info)
        << "fault aggregator started (tick=" << kEvalTickMs << "ms, "
        << settings_.badTicksToInvalid << " bad -> INVALID, "
        << settings_.goodTicksToValid << " good -> VALID, first VALID after "
        << settings_.initialValidMs << " ms (" << initialValidTicks_ << " tick(s)), "
        << "max report age " << settings_.maxReportAgeMs << " ms, verdict resend "
        << settings_.verdictResendMs << " ms; checks: health="
        << (settings_.healthEnabled ? "on" : "off") << " dvm="
        << (settings_.dvmEnabled ? "on" : "off") << ")";
    return true;
}

void PcmFaultAggregator::stop()
{
    running_.store(false, std::memory_order_release);
    if (threadValid_) {
        pthread_join(thread_, nullptr);
        threadValid_ = false;
    }
}

void PcmFaultAggregator::record(CheckReport& report, PcmCheckVerdict verdict)
{
    std::lock_guard<std::mutex> lock(reportMutex_);
    report.hasReport = true;
    report.verdict = verdict;
    report.time = std::chrono::steady_clock::now();
    ++report.generation;
}

void PcmFaultAggregator::reportHealth(PcmCheckVerdict verdict)
{
    record(health_, verdict);
}

void PcmFaultAggregator::reportDvm(PcmCheckVerdict verdict)
{
    record(dvm_, verdict);
}

void PcmFaultAggregator::evaluatorLoop()
{
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kEvalTickMs));
        if (!running_.load(std::memory_order_acquire)) break;
        evaluateTick(std::chrono::steady_clock::now());
    }
}

PcmFaultAggregator::Contribution PcmFaultAggregator::assessCheck(
    const char* name, const CheckReport& report, CheckTickState& tick,
    std::chrono::steady_clock::time_point now)
{
    /* Before its first report a check is timed from aggregator creation, so a
     * thread that never reports is also detected as stuck. */
    const auto since = report.hasReport ? report.time : createdAt_;
    const std::int64_t ageMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count();
    const bool stuck = ageMs > static_cast<std::int64_t>(settings_.maxReportAgeMs);
    if (stuck && !tick.stuck) {
        PcmLogLine(PcmLogLevel::Warning)
            << name << " check has not reported for " << ageMs
            << " ms (max_report_age_ms=" << settings_.maxReportAgeMs
            << "); it is ignored, and PCM cannot send VALID until it reports again";
    } else if (!stuck && tick.stuck) {
        PcmLogLine(PcmLogLevel::Info) << name << " check is reporting again";
    }
    tick.stuck = stuck;

    const bool newReport = report.generation != tick.lastGeneration;
    tick.lastGeneration = report.generation;

    Contribution contribution = Contribution::None;
    if (!stuck && report.hasReport) {
        switch (report.verdict) {
            case PcmCheckVerdict::Bad:
                contribution = Contribution::Bad;
                break;
            case PcmCheckVerdict::Good:
                contribution = newReport ? Contribution::Good : Contribution::GoodNoNewReport;
                break;
            case PcmCheckVerdict::Off:
                contribution = Contribution::None;
                break;
        }
    }

    /* Losing sight of the pipeline does not prove an active fault has cleared: a
     * check that stops contributing while INVALID must report Good again first. */
    const bool contributing = contribution != Contribution::None;
    if (state_ == PipelineState::Invalid && tick.contributing && !contributing &&
        !tick.mustReportGood) {
        tick.mustReportGood = true;
        PcmLogLine(PcmLogLevel::Warning)
            << name << " check has no verdict while the pipeline is INVALID; "
            << "staying INVALID until it reports good again";
    }
    if (tick.mustReportGood && contribution == Contribution::Good) {
        tick.mustReportGood = false;
        PcmLogLine(PcmLogLevel::Info) << name << " check reports good again";
    }
    tick.contributing = contributing;
    return contribution;
}

void PcmFaultAggregator::evaluateTick(std::chrono::steady_clock::time_point now)
{
    CheckReport health;
    CheckReport dvm;
    {
        std::lock_guard<std::mutex> lock(reportMutex_);
        health = health_;
        dvm = dvm_;
    }
    const Contribution h = settings_.healthEnabled
        ? assessCheck("container-health", health, healthTick_, now) : Contribution::None;
    const Contribution d = settings_.dvmEnabled
        ? assessCheck("data-validation", dvm, dvmTick_, now) : Contribution::None;

    const bool anyBad = (h == Contribution::Bad) || (d == Contribution::Bad);
    const bool anyGood = (h == Contribution::Good) || (d == Contribution::Good);
    const bool anyNoNewReport = (h == Contribution::GoodNoNewReport) ||
                                (d == Contribution::GoodNoNewReport);
    /* An enabled check is unknown until its first report; only an explicit Off
     * report may exclude it from good ticks before the stuck timeout. */
    const bool awaitingFirstReport = (settings_.healthEnabled && !health.hasReport) ||
                                     (settings_.dvmEnabled && !dvm.hasReport);
    const bool blocked = awaitingFirstReport || healthTick_.stuck || dvmTick_.stuck ||
                         healthTick_.mustReportGood || dvmTick_.mustReportGood;

    /* Saturate counters at the largest threshold they are compared with, so a
     * long-lived state cannot overflow them. */
    const int badCap = settings_.badTicksToInvalid;
    const int goodCap = std::max(settings_.goodTicksToValid, initialValidTicks_);
    if (anyBad) {
        if (consecutiveBad_ < badCap) ++consecutiveBad_;
        consecutiveGood_ = 0;
    } else {
        /* Fault debounce requires consecutive bad ticks. A check going Off or
         * stuck breaks that streak, but absence of evidence only pauses recovery. */
        consecutiveBad_ = 0;
        if (anyGood && !anyNoNewReport && !blocked) {
            if (consecutiveGood_ < goodCap) ++consecutiveGood_;
        }
    }

    /* A transition always needs at least one counted tick, even if a threshold
     * were zero (start() rejects that; evaluateTick may be driven without it). */
    const PipelineState previous = state_;
    if (state_ != PipelineState::Invalid && consecutiveBad_ > 0 &&
        consecutiveBad_ >= settings_.badTicksToInvalid) {
        state_ = PipelineState::Invalid;
    } else if (state_ == PipelineState::Starting && consecutiveGood_ > 0 &&
               consecutiveGood_ >= initialValidTicks_) {
        state_ = PipelineState::Valid;
    } else if (state_ == PipelineState::Invalid && consecutiveGood_ > 0 &&
               consecutiveGood_ >= settings_.goodTicksToValid) {
        state_ = PipelineState::Valid;
    }

    if (state_ != previous) {
        auto describe = [](bool enabled, Contribution c, const CheckTickState& t) -> const char* {
            if (!enabled) return "disabled";
            if (t.stuck) return "stuck";
            if (c == Contribution::Bad) return "bad";
            if (c == Contribution::None) return "off";
            return "ok";
        };
        PcmLogLine(state_ == PipelineState::Invalid ? PcmLogLevel::Err : PcmLogLevel::Info)
            << "pipeline -> " << (state_ == PipelineState::Invalid ? "INVALID" : "VALID")
            << (previous == PipelineState::Starting ? " (first status)" : "")
            << " (health=" << describe(settings_.healthEnabled, h, healthTick_)
            << " dvm=" << describe(settings_.dvmEnabled, d, dvmTick_)
            << " consecBad=" << consecutiveBad_
            << " consecGood=" << consecutiveGood_ << ")";
    }

    /* No status is sent while starting. After that the reporter sends the first
     * status, every change and periodic repeats. Failed sends retry next tick. */
    if (state_ != PipelineState::Starting) {
        PcmPipelineState status;
        status.invalid = (state_ == PipelineState::Invalid);
        status.judged = true;
        if (status.invalid) {
            if (h == Contribution::Bad) status.activeFaults.push_back("health");
            if (d == Contribution::Bad) status.activeFaults.push_back("dvm");
        }
        (void)reporter_.reportIfStateChanged(status, now);
    }
}
