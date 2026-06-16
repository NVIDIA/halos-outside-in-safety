/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sai_common.h"
#include "rtsp_client.h"
#include "nvdec_decoder.h"
#include "pss_daemon.h"
#include "pss_protocol.h"
#include "sensor_config_parser.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <csignal>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <memory>

#include <cuda_runtime.h>

struct StreamPipeline {
    std::string url;
    std::string sensorName;
    std::string baselinePath;
    NVDECDecoder decoder;
    std::atomic<bool> connected{false};
    int result = 0;
};

static constexpr int RETRY_DELAY_SEC = 5;
static std::atomic<bool> s_heartbeatRunning{false};

static void heartbeatLoop(uint32_t pssClientId) {
    while (s_heartbeatRunning.load() && !g_stopFlag.load()) {
        if (NvPSSSendHeartbeat(pssClientId, CLIENT_SAFETY_MONITOR) != NVPSSD_SUCCESS)
            std::cerr << "[SAI] Heartbeat send failed\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(HB_INTERVAL_MS));
    }
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
                              uint32_t pssClientId,
                              const std::string& sensorName) {
    client.setStreamLabel(sensorName);

    /* shared_ptr so the lambda's copy-capture does not ODR-duplicate the state
     * across any internal re-binds; the state outlives the lambda only via
     * the RTSPClient that owns the callback. */
    auto state = std::make_shared<FuaAlertState>();
    static constexpr std::chrono::seconds kFuaMinDwell{10};

    client.setFuaDropAlertCallback(
        [pssClientId, &dec, sensorName, state](uint32_t dropCount) {
            if (pssClientId == UINT32_MAX) {
                std::cerr << "[" << sensorName << "] FU-A drop alert: " << dropCount
                          << " dropped NALs (PSS not registered)\n";
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

            /* Edge gate: only emit on the false→true transition. If the
             * frame-quality path has already set sensorInvalid_, this FU-A
             * alert is redundant — skip it. memory_order_acq_rel pairs with
             * the release store in HandlePictureDisplay. */
            bool expected = false;
            if (!dec.sensorInvalidFlag().compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                state->suppressedSinceLastEmit++;
                return;
            }

            SafetyEvent event = {};
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t now_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                                  + static_cast<uint64_t>(ts.tv_nsec);
            event.id = dec.allocEventId();
            event.type = SENSOR_INVALID;
            event.severity = CRITICAL;
            event.fusionMetadata.pipelineID = dec.pipelineId();
            event.fusionMetadata.clientID = static_cast<uint8_t>(pssClientId);
            event.confidenceLevel = 1.0f;
            event.timestamp = now_ns;
            snprintf(event.sensorIdentifier, MAX_INDENTIFIER_LENGTH, "%s",
                     sensorName.c_str());

            if (dec.reportSafetyEvent(pssClientId, &event) == NVPSSD_SUCCESS) {
                const uint32_t suppressed = state->suppressedSinceLastEmit;
                state->suppressedSinceLastEmit = 0;
                state->lastEmit = now;
                std::cerr << "[" << sensorName << "] FU-A drop alert: "
                          << dropCount
                          << " dropped NALs, SENSOR_INVALID reported"
                          << " (suppressed_since_last_emit=" << suppressed
                          << ", dwell=" << kFuaMinDwell.count() << "s)\n";
            } else {
                /* Report failed — roll the edge back so the next attempt can
                 * still transition cleanly, otherwise we'd permanently lock
                 * the sensor as INVALID with no report on the wire. */
                dec.sensorInvalidFlag().store(false, std::memory_order_release);
                std::cerr << "[" << sensorName << "] Failed to report FU-A safety event"
                          << " (rolling back sensorInvalid edge)\n";
            }
        },
        5);
}

static void runStreamPipeline(StreamPipeline& pipeline, RunMode mode,
                              uint32_t pssClientId,
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

    {
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

    pipeline.decoder.setPSSClientId(pssClientId);
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

    while (!g_stopFlag.load()) {
        try {
            NalQueue queue;
            RTSPClient client(pipeline.url, &queue, &g_stopFlag);

            setupFuaCallback(client, pipeline.decoder, pssClientId, sensor);

            if (!client.connectToServer() || !client.setupRTSPSession()) {
                if (++retries > MAX_RTSP_CONNECT_RETRIES) {
                    std::cerr << "[" << sensor << "] RTSP: exceeded "
                              << MAX_RTSP_CONNECT_RETRIES
                              << " connection retries, giving up\n";
                    if (mode == RunMode::LEARN) {
                        std::cerr << "[" << sensor
                                  << "] LEARN mode: aborting all pipelines"
                                     " due to connection failure\n";
                        g_stopFlag.store(true);
                    }
                    if (mode == RunMode::ACTIVE && pssClientId != UINT32_MAX) {
                        SafetyEvent inv = {};
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        inv.id = pipeline.decoder.allocEventId();
                        inv.type = SENSOR_INVALID;
                        inv.severity = CRITICAL;
                        inv.fusionMetadata.pipelineID = pipeline.decoder.pipelineId();
                        inv.fusionMetadata.clientID = static_cast<uint8_t>(pssClientId);
                        inv.confidenceLevel = 1.0f;
                        inv.timestamp = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                                      + static_cast<uint64_t>(ts.tv_nsec);
                        snprintf(inv.sensorIdentifier, MAX_INDENTIFIER_LENGTH,
                                 "%s", sensor.c_str());
                        if (pipeline.decoder.reportSafetyEvent(pssClientId, &inv)
                            == NVPSSD_SUCCESS) {
                            std::cerr << "[" << sensor
                                      << "] SENSOR_INVALID reported to PSS (connection failed)\n";
                        } else {
                            std::cerr << "[" << sensor
                                      << "] Failed to report SENSOR_INVALID to PSS\n";
                        }
                    }
                    pipeline.result = 1;
                    break;
                }
                std::cerr << "[" << sensor << "] RTSP connection failed, retry "
                          << retries << "/" << MAX_RTSP_CONNECT_RETRIES
                          << " in " << RETRY_DELAY_SEC << "s\n";
                std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
                continue;
            }
            retries = 0;
            pipeline.connected.store(true);

            if (!pipeline.decoder.createParser()) {
                std::cerr << "[" << sensor << "] Failed to create decoder parser\n";
                pipeline.result = 1;
                break;
            }

            std::thread rtspThread([&]() {
                client.receiveLoop();
                queue.markFinished();
            });

            bool decodeOk = false;
            try {
                decodeOk = pipeline.decoder.decodeStream(queue);
            } catch (...) {
                queue.markFinished();
                client.requestStop();
                rtspThread.join();
                throw;
            }

            queue.markFinished();
            client.requestStop();
            rtspThread.join();

            if (!decodeOk) {
                std::cerr << "[" << sensor << "] Decode stream completed with errors\n";
            }

            if (g_stopFlag.load()) break;
            if (mode == RunMode::LEARN) {
                if (!decodeOk) pipeline.result = 1;
                break;
            }

            std::cerr << "[" << sensor << "] RTSP stream ended, reconnecting...\n";
        } catch (const std::exception& e) {
            std::cerr << "[" << sensor << "] Pipeline error: " << e.what() << "\n";
            if (g_stopFlag.load()) break;
            if (++retries > MAX_RTSP_CONNECT_RETRIES) {
                std::cerr << "[" << sensor << "] RTSP: exceeded "
                          << MAX_RTSP_CONNECT_RETRIES
                          << " retries, giving up\n";
                if (mode == RunMode::LEARN) {
                    std::cerr << "[" << sensor
                              << "] LEARN mode: aborting all pipelines"
                                 " due to connection failure\n";
                    g_stopFlag.store(true);
                }
                if (mode == RunMode::ACTIVE && pssClientId != UINT32_MAX) {
                    SafetyEvent inv = {};
                    struct timespec tsNow;
                    clock_gettime(CLOCK_MONOTONIC, &tsNow);
                    inv.id = pipeline.decoder.allocEventId();
                    inv.type = SENSOR_INVALID;
                    inv.severity = CRITICAL;
                    inv.fusionMetadata.pipelineID = pipeline.decoder.pipelineId();
                    inv.fusionMetadata.clientID = static_cast<uint8_t>(pssClientId);
                    inv.confidenceLevel = 1.0f;
                    inv.timestamp = static_cast<uint64_t>(tsNow.tv_sec) * 1000000000ULL
                                  + static_cast<uint64_t>(tsNow.tv_nsec);
                    snprintf(inv.sensorIdentifier, MAX_INDENTIFIER_LENGTH,
                             "%s", sensor.c_str());
                    if (pipeline.decoder.reportSafetyEvent(pssClientId, &inv)
                        == NVPSSD_SUCCESS) {
                        std::cerr << "[" << sensor
                                  << "] SENSOR_INVALID reported to PSS (exception retries exhausted)\n";
                    } else {
                        std::cerr << "[" << sensor
                                  << "] Failed to report SENSOR_INVALID to PSS\n";
                    }
                }
                pipeline.result = 1;
                break;
            }
            std::cerr << "[" << sensor << "] Retrying in " << RETRY_DELAY_SEC << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SEC));
        }
    }

    if (mode == RunMode::LEARN && pipeline.connected.load()
        && pipeline.result == 0) {
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Safety AI Monitor - CUDA ONLY\n\n"
                  << "Usage: " << argv[0]
                  << " --mode <LEARN|ACTIVE> --sensor-config <file> [options]\n\n"
                  << "Modes:\n"
                  << "  --mode LEARN   --sensor-config <file>  Learn baselines from all sensors.\n"
                  << "  --mode ACTIVE  --sensor-config <file>  Run analysis on all sensors.\n\n"
                  << "Sensor config format (one line per sensor, CSV):\n"
                  << "  pipelineId, sensorName, rtspUrl\n\n"
                  << "Options:\n"
                  << "  --threshold-config <file>          Threshold config file (default: /opt/nvidia/psf/configs/thresholds.cfg)\n"
                  << "  --learn-duration <sec>             Stream learn duration (default: 300)\n"
                  << "  --gpu <id>                         Pin all streams to GPU <id> (default: round-robin)\n";
        return 1;
    }

    RunMode mode = RunMode::ACTIVE;
    std::string sensorConfigPath;
    std::string thresholdConfigPath = "/opt/nvidia/psf/configs/thresholds.cfg";
    int learnDurationSec = 300;
    int gpuId = -1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            std::string val = argv[++i];
            if (val == "LEARN")       mode = RunMode::LEARN;
            else if (val == "ACTIVE") mode = RunMode::ACTIVE;
            else {
                std::cerr << "Error: --mode must be LEARN or ACTIVE\n";
                return 1;
            }
        }
        else if (arg == "--sensor-config" && i + 1 < argc) sensorConfigPath = argv[++i];
        else if (arg == "--threshold-config" && i + 1 < argc) thresholdConfigPath = argv[++i];
        else if (arg == "--learn-duration" && i + 1 < argc) {
            int parsed;
            if (!safe_stoi(argv[++i], parsed) || parsed < 1 || parsed > 3600) {
                std::cerr << "Error: --learn-duration must be 1-3600 seconds\n";
                return 1;
            }
            learnDurationSec = parsed;
        }
        else if (arg == "--gpu" && i + 1 < argc) {
            if (!safe_stoi(argv[++i], gpuId) || gpuId < 0) {
                std::cerr << "Error: --gpu must be a non-negative integer\n";
                return 1;
            }
        }
    }

    if (sensorConfigPath.empty()) {
        std::cerr << "Error: --sensor-config <file> is required.\n";
        return 1;
    }
    if (thresholdConfigPath.empty()) {
        std::cerr << "Error: --threshold-config <file> is required.\n";
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

    uint32_t pssClientId = UINT32_MAX;
    std::thread heartbeatThread;
    std::vector<std::thread> pipelineThreads;
    std::vector<std::unique_ptr<StreamPipeline>> pipelines;

    try {
        pipelines.reserve(numStreams);
        for (size_t i = 0; i < numStreams; i++) {
            const SensorConfigEntry& entry = sensorEntries[i];
            auto p = std::make_unique<StreamPipeline>();
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
                    std::cerr << "Error: baseline file '" << pipelines[i]->baselinePath
                              << "' not found for sensor '" << pipelines[i]->sensorName
                              << "'.\nRun with --learn first to create baselines"
                                 " for all streams.\n";
                    return 1;
                }
            }
            if (!registerPSS(pssClientId)) return 1;
            s_heartbeatRunning.store(true);
            heartbeatThread = std::thread(heartbeatLoop, pssClientId);
        }

        const char* modeStr = (mode == RunMode::LEARN) ? "LEARN" : "ACTIVE";
        std::cout << "\n=== " << modeStr << " MODE (" << numStreams
                  << " stream" << (numStreams > 1 ? "s" : "")
                  << ", " << gpuCount << " GPU" << (gpuCount > 1 ? "s" : "")
                  << (gpuId >= 0 ? ", pinned to GPU " + std::to_string(gpuId)
                                 : ", round-robin")
                  << ") ===\n";
        for (size_t i = 0; i < numStreams; i++) {
            int assignedGpu = gpuId >= 0 ? gpuId
                                         : static_cast<int>(i % gpuCount);
            std::cout << "  [" << static_cast<int>(sensorEntries[i].pipelineId)
                      << "] " << pipelines[i]->sensorName
                      << "  " << pipelines[i]->url << "\n"
                      << "      baseline: " << pipelines[i]->baselinePath
                      << "  gpu: " << assignedGpu << "\n";
        }
        if (mode == RunMode::LEARN)
            std::cout << "  Duration: " << learnDurationSec << " seconds\n";
        std::cout << "\n";

        pipelineThreads.reserve(numStreams);
        for (size_t i = 0; i < numStreams; i++) {
            pipelineThreads.emplace_back(runStreamPipeline,
                                         std::ref(*pipelines[i]),
                                         mode, pssClientId,
                                         std::cref(thresholdConfigPath),
                                         learnDurationSec);
        }

        for (auto& t : pipelineThreads) {
            t.join();
        }

        g_stopFlag.store(true);
        if (mode == RunMode::ACTIVE) {
            s_heartbeatRunning.store(false);
            if (heartbeatThread.joinable()) heartbeatThread.join();
            if (pssClientId != UINT32_MAX) {
                NvPSSTerminatePSSClient(pssClientId);
                std::cout << "Terminated PSS client registration\n";
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
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        g_stopFlag.store(true);
        for (auto& t : pipelineThreads) {
            if (t.joinable()) t.join();
        }
        if (mode == RunMode::ACTIVE) {
            s_heartbeatRunning.store(false);
            if (heartbeatThread.joinable()) heartbeatThread.join();
            if (pssClientId != UINT32_MAX) NvPSSTerminatePSSClient(pssClientId);
        }
        return 1;
    }
}
