/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_FAULT_AGGREGATOR_H
#define PCM_FAULT_AGGREGATOR_H

#include "pcm_event_reporter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <pthread.h>

/*
 * One check's verdict for one report period.
 *   Good / Bad : the check observed the pipeline (containers or perception data).
 *   Off        : the check has no verdict this period because of a setup problem
 *                (Docker unreachable or erroring, Kafka unavailable, or nothing it
 *                monitors has been seen yet). Off never trips the pipeline.
 */
enum class PcmCheckVerdict
{
    Good,
    Bad,
    Off
};

/* Aggregator policy, read from pcm_monitor.cfg at ACTIVE startup. */
struct PcmAggregatorSettings
{
    int  badTicksToInvalid = 0;     /* consecutive bad ticks -> INVALID */
    int  goodTicksToValid  = 0;     /* consecutive fresh good ticks: INVALID -> VALID */
    int  initialValidMs    = 0;     /* fresh good results before the first VALID */
    int  maxReportAgeMs    = 0;     /* a check silent for longer is stuck */
    int  verdictResendMs   = 5000;  /* repeat a judged verdict after this interval */
    bool healthEnabled     = false; /* the container-health check reports */
    bool dvmEnabled        = false; /* the data-validation check reports */
};

/*
 * The single shared decision point for PCM's two ACTIVE checks: the container-health
 * check and the DVM (data validation). Each check reports one verdict ~once per
 * second (reportHealth / reportDvm); the evaluator thread combines them every
 * 1 s tick and owns the one emit to PSS.
 *
 * Per tick:
 *   - Any check reporting Bad makes a bad tick. A Bad report keeps counting on
 *     later ticks until the check reports again or goes stuck. Any tick without
 *     a Bad contribution resets the consecutive bad count.
 *   - A good tick needs a NEW Good report since the previous tick from every
 *     check that has a verdict. A Good check with no new report pauses the tick
 *     (good count unchanged) instead of resetting recovery. Every enabled check
 *     must report at least once before good ticks can begin.
 *   - An Off check contributes nothing. A check silent for longer than
 *     maxReportAgeMs is stuck: it contributes nothing and also pauses good ticks.
 *   - A check that stops contributing (Off or stuck) while the pipeline is INVALID
 *     must report Good again before good ticks resume.
 *
 * States: the pipeline starts with no status. badTicksToInvalid consecutive bad
 * ticks give INVALID; initialValidMs of fresh good ticks give the first VALID;
 * from INVALID, goodTicksToValid fresh good ticks give VALID. The first status,
 * later changes and periodic repetitions are sent to PSS. Without a first
 * judgement nothing is sent; periodic reporting never creates a judgement.
 *
 * reportHealth/reportDvm are thread-safe and never emit.
 */
class PcmFaultAggregator
{
public:
    PcmFaultAggregator(uint32_t pssClientId, const PcmAggregatorSettings& settings);
    ~PcmFaultAggregator();

    PcmFaultAggregator(const PcmFaultAggregator&) = delete;
    PcmFaultAggregator& operator=(const PcmFaultAggregator&) = delete;
    PcmFaultAggregator(PcmFaultAggregator&&) = delete;
    PcmFaultAggregator& operator=(PcmFaultAggregator&&) = delete;

    /* Spawn the evaluator/emitter thread. Idempotent. False on invalid settings
     * or spawn failure. */
    bool start();
    /* Stop and join the evaluator thread. Idempotent. */
    void stop();

    /* Latest container-health verdict, reported once per health poll. */
    void reportHealth(PcmCheckVerdict verdict);
    /* Latest DVM (data-validation) verdict, reported once per 1 s window. */
    void reportDvm(PcmCheckVerdict verdict);

    /* One evaluation tick at @p now: combine the latest reports, update the state
     * and send first/changed/due periodic verdicts. The evaluator thread calls it once per tick; it is
     * public so unit tests can drive ticks with a chosen clock. Not thread-safe
     * against itself: call it from one thread only. */
    void evaluateTick(std::chrono::steady_clock::time_point now);

private:
    enum class PipelineState { Starting, Valid, Invalid };
    enum class Contribution { None, Good, GoodNoNewReport, Bad };

    /* Verdict, timestamp and evidence of a new report are one snapshot under
     * reportMutex_, so the evaluator cannot combine fields from different
     * reports. generation counts reports (64-bit: never wraps in practice). */
    struct CheckReport
    {
        bool                                  hasReport = false;
        PcmCheckVerdict                       verdict = PcmCheckVerdict::Off;
        std::chrono::steady_clock::time_point time{};
        std::uint64_t                         generation = 0U;
    };

    /* Evaluator-thread bookkeeping for one check. */
    struct CheckTickState
    {
        std::uint64_t lastGeneration = 0U;
        bool          contributing = false;
        bool          stuck = false;
        bool          mustReportGood = false;
    };

    void evaluatorLoop();
    static void* evaluatorThunk(void* self);
    void record(CheckReport& report, PcmCheckVerdict verdict);
    Contribution assessCheck(const char* name, const CheckReport& report,
                             CheckTickState& tick,
                             std::chrono::steady_clock::time_point now);

    PcmEventReporter            reporter_;
    const PcmAggregatorSettings settings_;
    const int                   initialValidTicks_;
    const std::chrono::steady_clock::time_point createdAt_;

    std::mutex  reportMutex_;
    CheckReport health_;
    CheckReport dvm_;

    /* Evaluator-thread-only state. */
    CheckTickState healthTick_;
    CheckTickState dvmTick_;
    int            consecutiveBad_  = 0;
    int            consecutiveGood_ = 0;
    PipelineState  state_ = PipelineState::Starting;

    std::atomic<bool> running_{false};
    pthread_t         thread_{};
    bool              threadValid_ = false;
};

#endif /* PCM_FAULT_AGGREGATOR_H */
