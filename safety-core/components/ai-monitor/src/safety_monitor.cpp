/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_common.h"
#include "rtsp_client.h"
#include "nvdec_decoder.h"
#include "i_frame_quality_analyzer.h"
#include "pss_daemon.h"
#include "pss_protocol.h"
#include "safety_event_reporter.h"
#include "sensor_config_parser.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <csignal>
#include <algorithm>
#include <chrono>
#include <memory>
#include <new>
#include <exception>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#include <unistd.h>
#include <time.h>

#include <cuda_runtime.h>

struct StreamPipeline {
    std::string url;
    std::string sensorName;
    std::string baselinePath;
    NVDECDecoder decoder;
    int result = 0;
};

static constexpr int RETRY_DELAY_SEC = 5;
static std::atomic<bool> s_heartbeatRunning{false};

static std::atomic<uint32_t> g_emergencyPssClient{UINT32_MAX};
static std::atomic<bool>     g_emergencyInProgress{false};
static std::atomic<SafetyEventReporter*> g_emergencyReporter{nullptr};


// std::bad_alloc / uncaught-exception handler: signal workers, brief drain,
// then _Exit. Skips NvPSSTerminatePSSClient (cannot safely join the heartbeat
// thread here, so a polite Terminate would race a concurrent SendHeartbeat);
// daemon reaps the client via socket EOF on _Exit.
[[noreturn]] static void emergencyShutdown(const char* reason) {
    bool expected = false;
    if (!g_emergencyInProgress.compare_exchange_strong(expected, true)) {
        struct timespec ts{2, 0};
        nanosleep(&ts, nullptr);
        std::_Exit(EXIT_FAILURE);
    }

    if (reason != nullptr)
        (void)write(STDERR_FILENO, reason, std::strlen(reason));

    g_stopFlag.store(true);
    s_heartbeatRunning.store(false);

    // Signal the reporter to stop before terminating the PSS client so
    // its worker cannot call NvPSSReportSafetyEvent() on a freed pid.
    // requestStop() does NOT join: this handler may run on the reporter
    // thread itself (e.g., bad_alloc inside runLoop), and a join would
    // self-deadlock or be defeated by producers churning haveWork_.
    // _Exit() below reaps any remaining threads.
    if (SafetyEventReporter* rep = g_emergencyReporter.exchange(
            nullptr, std::memory_order_acq_rel)) {
        rep->requestStop();
    }

    // Burn the published client id so the normal shutdown path (if it
    // somehow still runs) cannot CAS-claim and Terminate a now-doomed pid.
    g_emergencyPssClient.store(UINT32_MAX);

    struct timespec ts{0, 500 * 1000 * 1000};
    struct timespec rem{};
    while (nanosleep(&ts, &rem) == -1 && errno == EINTR) {
        ts = rem;
    }

    std::_Exit(EXIT_FAILURE);
}

static void heartbeatLoop(uint32_t pssClientId) {
    while (s_heartbeatRunning.load() && !g_stopFlag.load()) {
        if (NvPSSSendHeartbeat(pssClientId, CLIENT_SAFETY_MONITOR) != NVPSSD_SUCCESS)
            std::cerr << "[SAI] Heartbeat send failed\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(HB_INTERVAL_MS));
    }
}

// Retry-exhaustion handler: aborts LEARN, hands SENSOR_INVALID to the reporter in ACTIVE.
static void onRetriesExhausted(StreamPipeline& pipeline,
                               const std::string& sensor,
                               RunMode mode,
                               SafetyEventReporter* safetyEventReporter,
                               const char* contextMsg) {
    std::cerr << "[" << sensor << "] RTSP: exceeded "
              << MAX_RTSP_CONNECT_RETRIES
              << " connection retries (" << contextMsg << "), giving up\n";
    if (mode == RunMode::LEARN) {
        std::cerr << "[" << sensor
                  << "] LEARN mode: aborting all pipelines"
                     " due to connection failure\n";
        g_stopFlag.store(true);
    }
    if (mode == RunMode::ACTIVE && safetyEventReporter != nullptr) {
        // Edge-gate: skip the emit if another producer (analyzer or FU-A path)
        // already moved this sensor into INVALID during the successful initial
        // setup. Consistent with the FU-A and frame-quality paths.
        bool expected = false;
        if (!pipeline.decoder.sensorInvalidFlag().compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            std::cerr << "[" << sensor
                      << "] SENSOR_INVALID already reported by another producer ("
                      << contextMsg << "); skipping duplicate emit\n";
        } else {
            // ts==0 sentinel from monotonic_now_ns is clamped to slot's
            // last-good timestamp by SafetyEventReporter::update_slot_impl.
            safetyEventReporter->update_slot(sensor, SENSOR_INVALID,
                                             monotonic_now_ns(), 1.0f,
                                             pipeline.decoder.allocEventId(),
                                             SAIM_STREAM_DISCONNECT);
            std::cerr << "[" << sensor
                      << "] SENSOR_INVALID handed to reporter (" << contextMsg << ")\n";
        }
    }
    pipeline.result = 1;
}

static bool registerPSS(uint32_t& pssClientId) {
    for (int attempt = 0; attempt <= MAX_PSS_REGISTER_RETRIES; ++attempt) {
        if (NvPSSRegisterPSSClient(&pssClientId, CLIENT_SAFETY_MONITOR) == NVPSSD_SUCCESS) {
#ifdef DEBUG
            std::cout << "Registered with PSS daemon, clientId: "
                      << pssClientId << "\n";
#endif
            return true;
        }
        if (attempt < MAX_PSS_REGISTER_RETRIES) {
            std::cerr << "PSS registration failed, retry " << (attempt + 1)
                      << "/" << MAX_PSS_REGISTER_RETRIES
                      << " in " << RETRY_DELAY_SEC << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
        }
    }
    std::cerr << "Failed to register with PSS daemon after "
              << MAX_PSS_REGISTER_RETRIES + 1 << " attempts. Exiting.\n";
    return false;
}

/* Per-stream hysteresis state for the FU-A drop → SENSOR_INVALID trust-report
 * edge. Two gates are applied in order before a SENSOR_INVALID is emitted:
 *
 *   1. Minimum dwell (kFuaMinDwell): after a SENSOR_INVALID has been emitted,
 *      suppress any further emissions from this stream for a cool-down period.
 *      This prevents a sustained drop burst from tripping the trust-report
 *      channel every threshold-interval (the pre-change behavior that produced
 *      110 SENSOR_INVALID events from ~553 drops in a single 4-minute window).
 *
 *   2. Edge gate on the decoder's shared sensorInvalidFlag(): compare_exchange
 *      false → true so that if the frame-quality saturating counter (in
 *      nvdec_decoder.cpp) has already moved the sensor into INVALID, this FU-A
 *      path does not redundantly re-emit the same state.
 *
 * Recovery (the SENSOR_VALID edge) is left to the frame-quality path in
 * HandlePictureDisplay, which already owns the counter-based hysteresis for
 * validity and flips sensorInvalidFlag() back to false on a clean streak. */
struct FuaAlertState {
    std::chrono::steady_clock::time_point lastEmit{};  /* zero = never emitted */
    uint32_t suppressedSinceLastEmit = 0;              /* observability only */
};

static void setupFuaCallback(RTSPClient& client, NVDECDecoder& dec,
                              SafetyEventReporter* safetyEventReporter,
                              const std::string& sensorName) {
    client.setStreamLabel(sensorName);

    /* shared_ptr so the lambda's copy-capture does not ODR-duplicate the state
     * across any internal re-binds; the state outlives the lambda only via
     * the RTSPClient that owns the callback. */
    auto state = std::make_shared<FuaAlertState>();
    static constexpr std::chrono::seconds kFuaMinDwell{10};

    client.setFuaDropAlertCallback(
        [safetyEventReporter, &dec, sensorName, state](uint32_t dropCount) {
            if (safetyEventReporter == nullptr) {
                std::cerr << "[" << sensorName << "] FU-A drop alert: " << dropCount
                          << " dropped NALs (event reporter not active)\n";
                return;
            }

            const auto now = std::chrono::steady_clock::now();

            /* Dwell gate: suppress duplicate SENSOR_INVALID emissions while
             * the last one is still "fresh". Operators still see the drops
             * themselves via the per-stream histogram; we only gate the
             * trust-report edge, not the raw drop telemetry. */
            if (state->lastEmit.time_since_epoch().count() != 0 &&
                (now - state->lastEmit) < kFuaMinDwell) {
                state->suppressedSinceLastEmit++;
                return;
            }

            /* Edge gate: emit only on false->true. CAS fails when
             * sensorInvalid_ is already true (frame-quality path, startup
             * seed, or retries exhausted) - PSS already knows we are
             * INVALID, so suppression is correct. acq_rel pairs with the
             * release stores in HandlePictureDisplay/createParser. */
            bool expected = false;
            if (!dec.sensorInvalidFlag().compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                state->suppressedSinceLastEmit++;
                return;
            }

            // Reporter's state mirror handles delivery and reinforcement.
            safetyEventReporter->update_slot(sensorName, SENSOR_INVALID,
                                             monotonic_now_ns(), 1.0f,
                                             dec.allocEventId(),
                                             SAIM_FRAME_DROP);
            std::cerr << "[" << sensorName << "] FU-A drop alert: " << dropCount
                      << " dropped NALs, SENSOR_INVALID handed to reporter"
                      << " (suppressed_since_last_emit=" << state->suppressedSinceLastEmit
                      << ", dwell=" << kFuaMinDwell.count() << "s)\n";
            state->suppressedSinceLastEmit = 0;
            state->lastEmit = now;
        },
        kDefaultFuaDropAlertThreshold);
}

static void runStreamPipeline(StreamPipeline& pipeline, RunMode mode,
                              SafetyEventReporter* safetyEventReporter,
                              const std::string& thresholdConfigPath,
                              int learnDurationSec)
{
    const std::string& sensor = pipeline.sensorName;
    pipeline.decoder.setSensorName(sensor);

    if (!pipeline.decoder.initialize()) {
        std::cerr << "[" << sensor << "] CUDA initialization failed\n";
        pipeline.result = 1;
        return;
    }
    if (!pipeline.decoder.queryDecoderCaps()) {
        std::cerr << "[" << sensor << "] Decoder capability query failed\n";
        pipeline.result = 1;
        return;
    }

    if (mode == RunMode::ACTIVE) {
        NVTX_RANGE("LoadThresholdConfig", 0xFF88FF88);
        if (!pipeline.decoder.loadThresholdConfig(thresholdConfigPath)) {
            std::cerr << "[" << sensor << "] Failed to load threshold config from "
                      << thresholdConfigPath << "\n";
            pipeline.result = 1;
            return;
        }
        if (!pipeline.decoder.validateThresholdConfig()) {
            std::cerr << "[" << sensor << "] Threshold config validation failed\n";
            pipeline.result = 1;
            return;
        }
    }

    pipeline.decoder.setEventReporter(safetyEventReporter);
    pipeline.decoder.setMode(mode);

    if (mode == RunMode::LEARN) {
        pipeline.decoder.setLearnDuration(learnDurationSec);
    } else {
        NVTX_RANGE("LoadBaseline", 0xFFAAFF00);
        if (!pipeline.decoder.loadBaseline(pipeline.baselinePath)) {
            std::cerr << "[" << sensor << "] Failed to load baseline from "
                      << pipeline.baselinePath << "\n";
            pipeline.result = 1;
            return;
        }
        if (!pipeline.decoder.validateBaseline()) {
            std::cerr << "[" << sensor << "] Baseline validation failed\n";
            pipeline.result = 1;
            return;
        }
    }

    int retries = 0;
    // Counts post-setup stream interruptions (decodeStream returned after a
    // successful connect+setup). Distinct from `retries`, which tracks
    // init/connect/setup failures and resets on successful setup. LEARN bounds
    // this counter; ACTIVE only uses it to log the sleep-throttled reconnect.
    int interruptionRetries = 0;

    while (!g_stopFlag.load()) {
        NalQueue queue;
        RTSPClient client;

        // init() failure (bad URL / null arg) reuses the connect-fail retry path.
        if (!client.init(pipeline.url, &queue, &g_stopFlag)) {
            std::cerr << "[" << sensor
                      << "] RTSPClient init failed (bad URL or null arg)\n";
            if (++retries > MAX_RTSP_CONNECT_RETRIES) {
                onRetriesExhausted(pipeline, sensor, mode, safetyEventReporter,
                                   "init failed");
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
            continue;
        }

        setupFuaCallback(client, pipeline.decoder, safetyEventReporter, sensor);

        if (!client.connectToServer() || !client.setupRTSPSession()) {
            if (++retries > MAX_RTSP_CONNECT_RETRIES) {
                onRetriesExhausted(pipeline, sensor, mode, safetyEventReporter,
                                   "connection failed");
                break;
            }
            std::cerr << "[" << sensor << "] RTSP connection failed, retry "
                      << retries << "/" << MAX_RTSP_CONNECT_RETRIES
                      << " in " << RETRY_DELAY_SEC << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
            continue;
        }
        retries = 0;

        if (!pipeline.decoder.createParser()) {
            std::cerr << "[" << sensor << "] Failed to create decoder parser\n";
            pipeline.result = 1;
            break;
        }

        std::thread rtspThread([&]() {
            client.receiveLoop();
            queue.markFinished();
        });

        const bool decodeOk = pipeline.decoder.decodeStream(queue);

        // Always wake the queue and join the receive thread before continuing.
        queue.markFinished();
        client.requestStop();
        rtspThread.join();

        if (!decodeOk) {
            std::cerr << "[" << sensor << "] Decode stream completed with errors\n";
        }

        if (g_stopFlag.load()) break;

        // Analyzer permanent fault: stop reconnecting and let only this
        // camera's thread exit. SENSOR_INVALID was already emitted by the
        // analyzer and other streams keep running.
        if (pipeline.decoder.analyzerPermanentFaultFlag().load(
                std::memory_order_acquire)) {
            std::cerr << "[" << sensor
                      << "] analyzer permanent fault; shutting down this thread\n";
            pipeline.result = 1;
            break;
        }

        if (mode == RunMode::LEARN) {
            // Target frames reached -> exit retry loop and save baseline.
            // Otherwise the stream ended early; reconnect after a sleep so we
            // do not hammer the RTSP server, and bound the attempts so it cannot loop forever.
            if (pipeline.decoder.isLearnComplete()) break;
            if (++interruptionRetries > MAX_RTSP_CONNECT_RETRIES) {
                onRetriesExhausted(pipeline, sensor, mode, safetyEventReporter,
                                   "stream interrupted");
                break;
            }
            std::cerr << "[" << sensor << "] LEARN: stream interrupted at "
                      << pipeline.decoder.learnFramesProcessed() << "/"
                      << pipeline.decoder.learnFramesTarget()
                      << " frames, retry " << interruptionRetries << "/"
                      << MAX_RTSP_CONNECT_RETRIES << " in "
                      << RETRY_DELAY_SEC << "s\n";
        } else {
            // ACTIVE keeps reconnecting forever so transient outages recover;
            // the sleep prevents busy-looping the RTSP server.
            std::cerr << "[" << sensor
                      << "] RTSP stream ended, reconnecting in "
                      << RETRY_DELAY_SEC << "s...\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
    }

    // Save only when the target was reached. Partial saves (SIGINT or retry
    // exhaustion mid-LEARN) would produce an unreliable baseline.
    if (mode == RunMode::LEARN && pipeline.decoder.isLearnComplete()) {
        NVTX_RANGE("ComputeAndSaveBaseline", 0xFF00FFAA);
        if (!pipeline.decoder.saveBaseline(pipeline.baselinePath)) {
            std::cerr << "[" << sensor << "] Failed to save baseline to "
                      << pipeline.baselinePath << "\n";
            pipeline.result = 1;
        }
#ifdef DEBUG
        else {
            std::cout << "[" << sensor << "] Baseline saved to "
                      << pipeline.baselinePath << "\n";
        }
#endif
    }
}

static void printUsage(const char* progName, std::ostream& os) {
    os << "Safety AI Monitor - [Image processing on "
       << analyzerBackendName() << "]\n\n"
       << "Usage: " << progName
       << " --mode <LEARN|ACTIVE> --sensor-config <file> [options]\n\n"
       << "Modes:\n"
       << "  --mode LEARN   --sensor-config <file>  Learn baselines from all sensors.\n"
       << "  --mode ACTIVE  --sensor-config <file>  Run analysis on all sensors.\n\n"
       << "Sensor config format (one line per sensor, CSV):\n"
       << "  pipelineId, sensorName, rtspUrl\n\n"
       << "Options:\n"
       << "  --threshold-config <file>          Threshold config file (ACTIVE mode only, default: thresholds.cfg)\n"
       << "  --learn-duration <sec>             Stream learn duration (default: 300)\n"
       << "  --gpu <id>                         Pin all streams to GPU <id> (default: round-robin)\n"
       ;
}

int main(int argc, char** argv) {
    // Route OOM and uncaught exceptions through emergencyShutdown().
    std::set_new_handler([]() {
        emergencyShutdown(
            "[SAI] FATAL: out of memory; performing emergency shutdown\n");
    });
    std::set_terminate([]() {
        emergencyShutdown(
            "[SAI] FATAL: unhandled exception; performing emergency shutdown\n");
    });

    if (argc < 3) {
        printUsage(argv[0], std::cout);
        return 1;
    }

    RunMode mode = RunMode::ACTIVE;
    std::string sensorConfigPath;
    std::string thresholdConfigPath = "thresholds.cfg";
    int learnDurationSec = DEFAULT_LEARN_DURATION_SEC;
    int gpuId = -1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        // Missing-value and unknown-flag paths exit non-zero so the user
        // never silently runs with unintended defaults.
        if (arg == "--mode") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --mode requires a value (LEARN or ACTIVE)\n";
                return 1;
            }
            std::string val = argv[++i];
            if (val == "LEARN")       mode = RunMode::LEARN;
            else if (val == "ACTIVE") mode = RunMode::ACTIVE;
            else {
                std::cerr << "Error: --mode must be LEARN or ACTIVE\n";
                return 1;
            }
        }
        else if (arg == "--sensor-config") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --sensor-config requires a file path\n";
                return 1;
            }
            sensorConfigPath = argv[++i];
        }
        else if (arg == "--threshold-config") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --threshold-config requires a file path\n";
                return 1;
            }
            thresholdConfigPath = argv[++i];
        }
        else if (arg == "--learn-duration") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --learn-duration requires a value in seconds\n";
                return 1;
            }
            int parsed;
            if (!safe_stoi(argv[++i], parsed)
                || parsed < 1
                || parsed > MAX_LEARN_DURATION_SEC) {
                std::cerr << "Error: --learn-duration must be 1-"
                          << MAX_LEARN_DURATION_SEC << " seconds\n";
                return 1;
            }
            learnDurationSec = parsed;
        }
        else if (arg == "--gpu") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --gpu requires a GPU id\n";
                return 1;
            }
            if (!safe_stoi(argv[++i], gpuId) || gpuId < 0) {
                std::cerr << "Error: --gpu must be a non-negative integer\n";
                return 1;
            }
        }
        else {
            std::cerr << "Error: unknown argument: " << arg << "\n";
            printUsage(argv[0], std::cerr);
            return 1;
        }
    }

    if (sensorConfigPath.empty()) {
        std::cerr << "Error: --sensor-config <file> is required.\n";
        return 1;
    }
    if (mode == RunMode::ACTIVE && thresholdConfigPath.empty()) {
        std::cerr << "Error: --threshold-config <file> is required in ACTIVE mode.\n";
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    NVTX_RANGE("MainApplication", 0xFFFFFFFF);

    std::string cfgErr;
    std::vector<SensorConfigEntry> sensorEntries = sensorConfigLoad(sensorConfigPath, &cfgErr);
    if (sensorEntries.empty()) {
        std::cerr << "Error: " << cfgErr << "\n";
        return 1;
    }

    const size_t numStreams = sensorEntries.size();

    CUresult cuRes = cuInit(0);
    if (cuRes != CUDA_SUCCESS) {
        std::cerr << "Error: failed to initialize CUDA: " << cuRes << "\n";
        return 1;
    }
    int gpuCount = 0;
    cuRes = cuDeviceGetCount(&gpuCount);
    if (cuRes != CUDA_SUCCESS || gpuCount == 0) {
        std::cerr << "Error: no CUDA GPUs found\n";
        return 1;
    }
    if (gpuId >= gpuCount) {
        std::cerr << "Error: --gpu " << gpuId << " is out of range (0-"
                  << gpuCount - 1 << ")\n";
        return 1;
    }

    // Set every GPU's primary context to BLOCKING_SYNC so cudaStreamSynchronize
    // sleeps instead of busy-waiting (default CU_CTX_SCHED_AUTO spins when
    // active contexts <= CPU cores, burning ~1 core per pipeline thread per
    // sync). Must run before any pipeline thread initializes the context.
    for (int d = 0; d < gpuCount; ++d) {
        if (cudaSetDevice(d) != cudaSuccess) {
            std::cerr << "Warning: cudaSetDevice(" << d << ") failed; GPU "
                      << d << " may spin during cudaStreamSynchronize\n";
            continue;
        }
        cudaError_t err = cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
        if (err != cudaSuccess && err != cudaErrorSetOnActiveProcess) {
            std::cerr << "Warning: cudaSetDeviceFlags(GPU " << d << ") failed: "
                      << cudaGetErrorString(err) << "; may spin during sync\n";
        }
    }

    // Perform the backend plugin's one-time HW probe before creating analyzers.
    // A no-op on GPU, whose CUDA device is already checked above.
    if (!loadAnalyzerBackend()) {
        std::cerr << "[SAI] FATAL: " << analyzerBackendName()
                  << " analysis backend init failed\n";
        return 1;
    }

    // The linked plugin must identify as the GPU backend. Reject any other name
    // up front so the GPU scheduling/logging wiring below can never silently
    // treat an unrecognised backend as GPU.
    {
        const char* backendName = analyzerBackendName();
        if (std::strcmp(backendName, "GPU") != 0) {
            std::cerr << "[SAI] FATAL: unrecognised analysis backend '"
                      << backendName << "'\n";
            return 1;
        }
    }

    uint32_t pssClientId = UINT32_MAX;
    std::thread heartbeatThread;
    std::unique_ptr<SafetyEventReporter> safetyEventReporter;
    std::vector<std::thread> pipelineThreads;
    std::vector<std::unique_ptr<StreamPipeline>> pipelines;

    pipelines.reserve(numStreams);
    for (size_t i = 0; i < numStreams; i++) {
        const SensorConfigEntry& entry = sensorEntries[i];
        // nothrow: startup OOM returns 1 cleanly, no emergencyShutdown needed.
        auto p = std::unique_ptr<StreamPipeline>(new (std::nothrow) StreamPipeline());
        if (!p) {
            std::cerr << "[SAI] Failed to allocate StreamPipeline " << i
                      << "; aborting startup\n";
            return 1;
        }
        p->url = entry.rtspUrl;
        p->sensorName = entry.sensorName;
        p->baselinePath = std::string(entry.sensorName) + "_baseline.cfg";
        if (mode == RunMode::ACTIVE)
            p->decoder.setPipelineId(entry.pipelineId);
        p->decoder.setGpuIndex(gpuId >= 0 ? gpuId : static_cast<int>(i % gpuCount));
        pipelines.push_back(std::move(p));
    }

    if (mode == RunMode::ACTIVE) {
        for (size_t i = 0; i < numStreams; i++) {
            std::ifstream test(pipelines[i]->baselinePath);
            if (!test.is_open()) {
                // A learned <sensor>_baseline.cfg always wins. When it is absent,
                // fall back to the shipped <sensor>_baseline.cfg.default template.
                const std::string defaultPath = pipelines[i]->baselinePath + ".default";
                std::ifstream defaultTest(defaultPath);
                if (defaultTest.is_open()) {
                    std::cout << "Notice: baseline '" << pipelines[i]->baselinePath
                              << "' not found for sensor '" << pipelines[i]->sensorName
                              << "'; falling back to default '" << defaultPath << "'\n";
                    pipelines[i]->baselinePath = defaultPath;
                } else {
                    std::cerr << "Error: baseline file '" << pipelines[i]->baselinePath
                              << "' (or '" << defaultPath << "') not found for sensor '"
                              << pipelines[i]->sensorName
                              << "'.\nRun with --learn first to create baselines"
                                 " for all streams.\n";
                    return 1;
                }
            }
        }
        if (!registerPSS(pssClientId)) return 1;
        // Publish the client id so emergencyShutdown() can burn it (preventing
        // the normal-shutdown CAS from racing on it) and so the normal
        // shutdown path can CAS-claim it for the polite NvPSSTerminatePSSClient.
        g_emergencyPssClient.store(pssClientId);

        // Allocate the reporter before starting the heartbeat so a startup
        // OOM can return cleanly without leaving a joinable heartbeat thread.
        safetyEventReporter.reset(new (std::nothrow) SafetyEventReporter(pssClientId));
        if (!safetyEventReporter) {
            std::cerr << "[SAI] Failed to allocate SafetyEventReporter\n";
            NvPSSTerminatePSSClient(pssClientId);
            g_emergencyPssClient.store(UINT32_MAX);
            return 1;
        }
        // Publish before start() so emergencyShutdown can stop the reporter
        // even if it fires during the start/registerSensor window.
        g_emergencyReporter.store(safetyEventReporter.get(),
                                  std::memory_order_release);

        s_heartbeatRunning.store(true);
        heartbeatThread = std::thread(heartbeatLoop, pssClientId);

        safetyEventReporter->start();
        for (size_t i = 0; i < numStreams; i++) {
            NVDECDecoder* dec = &pipelines[i]->decoder;
            if (!safetyEventReporter->registerSensor(
                    pipelines[i]->sensorName,
                    &pipelines[i]->decoder.sensorInvalidFlag(),
                    pipelines[i]->decoder.pipelineId(),
                    [dec]{ return dec->allocEventId(); })) {
                std::cerr << "[SAI] registerSensor failed for "
                          << pipelines[i]->sensorName
                          << "; aborting startup\n";
                safetyEventReporter->stop();
                g_emergencyReporter.store(nullptr, std::memory_order_release);
                s_heartbeatRunning.store(false);
                if (heartbeatThread.joinable()) heartbeatThread.join();
                uint32_t expected = pssClientId;
                if (g_emergencyPssClient.compare_exchange_strong(
                        expected, UINT32_MAX)) {
                    NvPSSTerminatePSSClient(pssClientId);
                }
                return 1;
            }
        }
    }

    const char* modeStr = (mode == RunMode::LEARN) ? "LEARN" : "ACTIVE";
    // Analysis is pinned or round-robined across CUDA devices; NVDEC decode uses
    // a CUDA device as well.
    std::cout << "\n=== " << modeStr << " MODE (" << numStreams
              << " stream" << (numStreams > 1 ? "s" : "")
              << ", " << gpuCount << " GPU" << (gpuCount > 1 ? "s" : "")
              << (gpuId >= 0 ? ", pinned to GPU " + std::to_string(gpuId)
                             : ", round-robin")
              << ") ===\n";
    for (size_t i = 0; i < numStreams; i++) {
        std::cout << "  [" << static_cast<int>(sensorEntries[i].pipelineId)
                  << "] " << pipelines[i]->sensorName
                  << "  " << pipelines[i]->url << "\n"
                  << "      baseline: " << pipelines[i]->baselinePath
                  << "  gpu: " << (gpuId >= 0 ? gpuId
                                              : static_cast<int>(i % gpuCount))
                  << "\n";
    }
    if (mode == RunMode::LEARN)
        std::cout << "  Duration: " << learnDurationSec << " seconds\n";
    std::cout << "\n";

    pipelineThreads.reserve(numStreams);
    for (size_t i = 0; i < numStreams; i++) {
        pipelineThreads.emplace_back(runStreamPipeline,
                                     std::ref(*pipelines[i]),
                                     mode, safetyEventReporter.get(),
                                     std::cref(thresholdConfigPath),
                                     learnDurationSec);
    }

    for (auto& t : pipelineThreads) {
        t.join();
    }
    g_stopFlag.store(true);

    if (mode == RunMode::ACTIVE) {
        // Stop reporter before terminating PSS so final transitions reach a live client.
        if (safetyEventReporter) safetyEventReporter->stop();
        g_emergencyReporter.store(nullptr, std::memory_order_release);
        s_heartbeatRunning.store(false);
        if (heartbeatThread.joinable()) heartbeatThread.join();
        if (pssClientId != UINT32_MAX) {
            uint32_t expected = pssClientId;
            if (g_emergencyPssClient.compare_exchange_strong(expected,
                                                             UINT32_MAX)) {
                NvPSSTerminatePSSClient(pssClientId);
                std::cout << "Terminated PSS client registration\n";
            }
        }
    }

    int exitCode = 0;
    for (size_t i = 0; i < numStreams; i++) {
        if (pipelines[i]->result != 0) {
            std::cerr << "[" << pipelines[i]->sensorName
                      << "] Pipeline finished with errors\n";
            exitCode = 1;
        }
    }
    return exitCode;
}
