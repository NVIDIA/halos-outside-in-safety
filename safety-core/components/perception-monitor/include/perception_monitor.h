/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_PERCEPTION_MONITOR_H
#define PCM_PERCEPTION_MONITOR_H

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <vector>
#include "decision_contract.h"

enum class PcmRuntimeMode
{
    Learn,
    Active
};

struct PerceptionMonitorOptions
{
    PcmRuntimeMode mode = PcmRuntimeMode::Active;
    int learnDurationSec = 300;
    std::string baselinePath;
    /* Kafka broker (host:port) for the DVM mdx-frames consumer; set with
     * --kafka-broker so a remote DS_MDX deployment can be reached. */
    std::string kafkaBroker = "localhost:9092";
    std::string eventMappingPath;
    std::vector<std::string> sdmConfigPaths;
    DvmDecisionContract decisionContract;
    /* ACTIVE settings loaded once from pcm_monitor.cfg before PSS registration.
     * Zero means unconfigured: the timing counts and percentages have no compiled
     * fallback; the millisecond settings fall back to built-in values with a
     * warning when missing or invalid. */
    bool containerHealth = true;
    bool dataValidation = true;
    int badTicksToInvalid = 0;
    int goodTicksToValid = 0;
    int badObjectsPercent = 0;
    int badFramesPercent = 0;
    int maxReportAgeMs = 0;  /* a check silent for longer is stuck */
    int initialValidMs = 0;  /* fresh good results before the first VALID */
    int startupGraceMs = 0;  /* unseen cameras/containers are reported after this */
    int verdictResendMs = 0; /* periodic repetition after the first judgement */
    int containerStartupGraceMs = 0; /* Docker health starting allowance */
};

/*
 * Owns the per-run thread that coordinates the PCM detection checks.
 *
 * ACTIVE mode registers with PSS and runs the detection checks -- the container-health
 * check and the data validator (DVM) -- both feeding the shared aggregator that emits
 * AI_PIPELINE_INVALID/VALID. (An earlier log-content scanner was removed.) LEARN mode
 * does not register or report: it runs ONLY the DVM, observing mdx-frames to
 * write the DVM baseline (the sensor whitelist) when the duration completes;
 * the container-health check learns nothing.
 */
class PerceptionMonitor
{
public:
    /*
     * @param pssClientId  Id from NvPSSRegisterPSSClient in ACTIVE mode;
     *                     ignored in LEARN mode.
     * @param stopFlag     Shutdown flag set by signal handlers; must
     *                     outlive this object.
     * @param options      Runtime mode, learn duration, and baseline path.
     */
    PerceptionMonitor(uint32_t pssClientId,
                      std::atomic<bool>& stopFlag,
                      const PerceptionMonitorOptions& options);
    ~PerceptionMonitor();

    PerceptionMonitor(const PerceptionMonitor&) = delete;
    PerceptionMonitor& operator=(const PerceptionMonitor&) = delete;
    PerceptionMonitor(PerceptionMonitor&&) = delete;
    PerceptionMonitor& operator=(PerceptionMonitor&&) = delete;

    /* Spawn the polling thread. Idempotent. False only on spawn failure. */
    bool start();

    /* Signal and join the polling thread. Idempotent. */
    void stop();

    bool isRunning() const;
    bool completedOk() const;

private:
    void runPollLoop();
    /* pthread_create entry trampoline: forwards to runPollLoop(). */
    static void* runPollLoopThunk(void* self);

    const uint32_t                 pssClientId_;
    std::atomic<bool>&             stopFlag_;
    const PerceptionMonitorOptions options_;
    std::atomic<bool>              running_{false};
    std::atomic<bool>              completedOk_{false};
    pthread_t                      pollThread_{};
    bool                           pollThreadValid_ = false;
};

#endif /* PCM_PERCEPTION_MONITOR_H */
