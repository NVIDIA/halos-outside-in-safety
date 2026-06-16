/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <memory>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <atomic>

#include "pss_daemon.h"

#include "NvPSSDRPC.hpp"
#include "NvPSSSafetyEventManager.hpp"
#include "NvPSB.h"
#include "NvPSSConfigParser.hpp"
#include "sensor_config_parser.h"

#define MAX_CLIENTS 8
#define MAX_PENDING_CLIENTS 2

#define CRITICAL_PRIO_Q_PERIOD_US 1000
#define HIGH_PRIO_Q_PERIOD_US 2500
#define MEDIUM_PRIO_Q_PERIOD_US 5000
#define LOW_PRIO_Q_PERIOD_US 10000
#define INPUT_SAFETYEVENT_Q_PERIOD_US 100
#define SAFETYEVENT_FUSION_PERIOD_US 2000
#define THRESHOLD_CONFIDENCE_FOR_PSD_REPORT 0.6f
#define EARLY_TERMINATION_THRESHOLD 0.9f
#define ENABLE_EARLY_TERMINATION true

/* Heartbeat fail-safe policy (see nvpss.conf max_hb_failures). WARN_THRESHOLD = max/2 (integer division).
 * Values set after validateConfiguration(); maxHbFailures is already validated when loaded from config. */
std::atomic<uint32_t> g_pssMaxHbFailures{10U};
std::atomic<uint32_t> g_pssWarnThreshold{5U};

/* AF_UNIX path for the PSS RPC server. /run is the canonical per-boot runtime
 * directory on modern Linux; a dedicated /run/nvpsf subtree keeps the Safety
 * Core sockets out of /tmp and lets the container bind-mount only this one
 * narrow path instead of the entire host /tmp tree. */
const std::string socketPath = "/run/nvpsf/nvpssd";

#define MAX_TEMPORAL_TOLERANCE_MS 60000 // 60 seconds max tolerance

typedef struct {
    std::chrono::milliseconds timeWindowSize{200};        // Default: 200ms
    float fusionThreshold{0.5f};                          // Default: 0.5
    float alpha{0.35f};                                   // Default: 0.35
    float beta{0.45f};                                    // Default: 0.45
    float gamma{0.20f};                                   // Default: 0.20
    std::chrono::milliseconds temporalTolerance{5};       // Default: 5ms
    uint8_t trajectoryCount{4};                           // Default: 4
    uint8_t maxPipelines{2};                              // Default: 2
    NvPSDChannelBackend PSSDToPSDComBackend{NvPSDChannelBackend::POSIX_MSG_QUE}; // Default: POSIX_MSG_QUE
    uint32_t maxHbFailures{10U};                          /* consecutive HB misses before SW_FAIL; range 1..255 (default 10) */
} NvPSSConfiguration;

std::atomic<bool> shutdownRequested;
std::condition_variable eventMonitorTerminationCV;
std::condition_variable msgListenerTerminationCV;
std::condition_variable fusionTerminationCV;
std::atomic<bool> heartbeatMonitorRunning{false};
std::condition_variable heartbeatTerminationCV;
std::shared_ptr<nvpss::NvPSSDRPC> g_NvPSSDRPC;
std::mutex g_rpcMutex;
void signalHandler(int signal);
void heartbeatMonitor();
bool validateConfiguration(NvPSSConfiguration& config);
void msgListener(std::unique_ptr<nvpss::SafetyEventManager>&);
void eventMonitor(std::unique_ptr<nvpss::SafetyEventManager>&);
void fusionMonitor(std::unique_ptr<nvpss::SafetyEventManager>&);
EventType parseEventTypeFromString(const std::string& name);

/** C-style trust-report callback: forwards to SafetyEventManager::OnTrustReport. Returns true if accepted. */
static bool onTrustReportFromRPC(void* ctx, uint32_t clientId, uint8_t reporterClientType, const SafetyEvent* event)
{
    if (ctx != nullptr && event != nullptr)
    {
        return static_cast<nvpss::SafetyEventManager*>(ctx)->OnTrustReport(clientId, reporterClientType, *event);
    }
    return false;
}

void signalHandler(int signal)
{
    shutdownRequested.store(true);
    heartbeatMonitorRunning.store(false);
    msgListenerTerminationCV.notify_all();
    eventMonitorTerminationCV.notify_all();
    fusionTerminationCV.notify_all();
    heartbeatTerminationCV.notify_all();
}

void msgListener(std::unique_ptr<nvpss::SafetyEventManager>& h_safetyEventManager)
{
    std::mutex msgListenerTerminationCV_Mtx;
    std::unique_lock<std::mutex> lock(msgListenerTerminationCV_Mtx);

    {
        std::lock_guard<std::mutex> rpcLock(g_rpcMutex);
        g_NvPSSDRPC = std::make_shared<nvpss::NvPSSDRPC>(SOCKET, socketPath, MAX_CLIENTS,
                        MAX_PENDING_CLIENTS);
    }

    if (g_NvPSSDRPC->NvPSSDInitRPCServer() != NVPSSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error in initializing PSS RPC server", "");
        shutdownRequested.store(true);
        heartbeatMonitorRunning.store(false);
        msgListenerTerminationCV.notify_all();
        eventMonitorTerminationCV.notify_all();
        fusionTerminationCV.notify_all();
        heartbeatTerminationCV.notify_all();
        goto cleanup;
    }

    {
        if (g_NvPSSDRPC->NvPSSDStartRPCServer(
                h_safetyEventManager->getInputSafetyEventQueRef(),
                h_safetyEventManager->getInputSafetyEventQueMutexRef(),
                THRESHOLD_CONFIDENCE_FOR_PSD_REPORT,
                msgListenerTerminationCV,
                onTrustReportFromRPC,
                h_safetyEventManager.get()) != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Error in starting RPC server", "");
            shutdownRequested.store(true);
            heartbeatMonitorRunning.store(false);
            msgListenerTerminationCV.notify_all();
            eventMonitorTerminationCV.notify_all();
            fusionTerminationCV.notify_all();
            heartbeatTerminationCV.notify_all();
            goto cleanup;
        }
        h_safetyEventManager->SetRpcForOperationalMode(g_NvPSSDRPC.get());
    }

    while (!shutdownRequested.load())
    {
        auto status = msgListenerTerminationCV.wait_for(lock, std::chrono::milliseconds(100));
        if (status == std::cv_status::no_timeout || shutdownRequested.load())
            break;
    }

cleanup:
    /* Must run before NvPSSDCloseRPCServer: mutex in SafetyEventManager blocks until no concurrent
     * decisionRequestOperationalMode / getSafetyMonitorOperationalMode on this RPC finishes. */
    h_safetyEventManager->SetRpcForOperationalMode(nullptr);
    {
        std::lock_guard<std::mutex> rpcLock(g_rpcMutex);
        if (g_NvPSSDRPC)
        {
            g_NvPSSDRPC->NvPSSDCloseRPCServer();
            g_NvPSSDRPC.reset();
        }
    }
    NvPSBWriteData(NVPSB_LOG_INFO, "Stopped RPCServer", "");
    return;
}

void heartbeatMonitor()
{
    std::mutex heartbeatMonitorMtx;
    std::unique_lock<std::mutex> lock(heartbeatMonitorMtx);
#ifdef NVPSF_DBG
    uint32_t checkCount = 0;

    NvPSBWriteData(NVPSB_LOG_INFO, "Heartbeat monitor started, checking every 5s, timeout=5s", "");
#endif

    while (heartbeatMonitorRunning.load() && !shutdownRequested.load())
    {
#ifdef NVPSF_DBG
        checkCount++;
#endif

        /* Acquire a shared_ptr copy under g_rpcMutex so the NvPSSDRPC object
           stays alive after releasing the mutex, preventing use-after-free if
           msgListener calls g_NvPSSDRPC.reset() concurrently. */
        std::shared_ptr<nvpss::NvPSSDRPC> rpc;
        {
            std::lock_guard<std::mutex> rpcLock(g_rpcMutex);
            rpc = g_NvPSSDRPC;
        }

        if (rpc)
        {
#ifdef NVPSF_DBG
            if (checkCount % 100 == 0)
            {
                const size_t activeCount = rpc->getActiveClientCount();
                if (activeCount > 0)
                {
                    NvPSBWriteData(NVPSB_LOG_INFO,
                        "Heartbeat monitor: " + std::to_string(checkCount) + " checks, " +
                        std::to_string(activeCount) + " active clients", "");
                }
            }
#endif
            rpc->heartbeatMonitorTick(g_pssMaxHbFailures.load(std::memory_order_relaxed),
                                      g_pssWarnThreshold.load(std::memory_order_relaxed));
        }

        if (!rpc)
        {
            /* RPC server not yet initialized or already shut down */
            auto status = heartbeatTerminationCV.wait_for(
                lock, std::chrono::milliseconds(HB_INTERVAL_MS));

            if (status == std::cv_status::no_timeout || shutdownRequested.load())
            {
                break;
            }

            continue;
        }

        auto status = heartbeatTerminationCV.wait_for(lock,
            std::chrono::milliseconds(HB_INTERVAL_MS));
        if (status == std::cv_status::no_timeout || shutdownRequested.load())
        {
            break;
        }
    }

#ifdef NVPSF_DBG
    NvPSBWriteData(NVPSB_LOG_INFO, "Heartbeat monitor stopped after " + std::to_string(checkCount) + " checks", "");
#endif
}

void eventMonitor(std::unique_ptr<nvpss::SafetyEventManager>& h_safetyEventManager)
{
    std::mutex eventMonitorTerminationCV_Mtx;
    std::unique_lock<std::mutex> lock(eventMonitorTerminationCV_Mtx);

    if(h_safetyEventManager->StartSafetyEventManager() != NVPSSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error on starting PSS Event manager", "");
        return;
    }

    while(!shutdownRequested.load()) {
        auto status = eventMonitorTerminationCV.wait_for(lock, std::chrono::milliseconds(100));
        if (status == std::cv_status::no_timeout || shutdownRequested.load()) {
            break;
        }
    }

    h_safetyEventManager->StopSafetyEventManager();
    NvPSBWriteData(NVPSB_LOG_INFO, "SafetyEventManager stopped", "");
}

void fusionMonitor(std::unique_ptr<nvpss::SafetyEventManager>& h_safetyEventManager)
{
    std::mutex fusionTerminationCV_Mtx;
    std::unique_lock<std::mutex> lock(fusionTerminationCV_Mtx);

    if(h_safetyEventManager->StartFusionProcessing() != NVPSSD_SUCCESS) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error starting fusion processor", "");
        return;
    }

    while(!shutdownRequested.load()) {
        auto status = fusionTerminationCV.wait_for(lock, std::chrono::milliseconds(100));
        if (status == std::cv_status::no_timeout || shutdownRequested.load()) {
            break;
        }
    }

    h_safetyEventManager->StopFusionProcessing();
#ifdef NVPSF_DBG
    NvPSBWriteData(NVPSB_LOG_INFO,"FusionProcessor stopped", "");
#endif
}

bool validateConfiguration(NvPSSConfiguration& config)
{
    bool valid = true;

    if (config.fusionThreshold < 0.0f || config.fusionThreshold > 1.0f)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "fusionThreshold out of range; falling back to default 0.7", "");
        config.fusionThreshold = 0.7f;
        valid = false;
    }

    if (config.alpha < 0.0f || config.alpha > 1.0f)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "alpha out of range; falling back to default 0.33", "");
        config.alpha = 0.33f;
        valid = false;
    }

    if (config.beta < 0.0f || config.beta > 1.0f)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "beta out of range; falling back to default 0.33", "");
        config.beta = 0.33f;
        valid = false;
    }

    if (config.gamma < 0.0f || config.gamma > 1.0f)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "gamma out of range; falling back to default 0.33", "");
        config.gamma = 0.33f;
        valid = false;
    }

    // Check if weights sum to approximately 1.0
    float weightSum = config.alpha + config.beta + config.gamma;
    if (std::abs(weightSum - 1.0f) > 0.01f)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "Weight sum (alpha + beta + gamma = " + std::to_string(weightSum) +
                ") should be close to 1.0; changing to default value",
            "");
        config.alpha = config.beta = config.gamma = 0.33f;
    }

    if (config.timeWindowSize.count() <= 0)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "timeWindowSize must be positive; falling back to default 5 ms", "");
        config.timeWindowSize = std::chrono::milliseconds{5};
        valid = false;
    }

    // Validate that conversion won't overflow
    long long maxSafeWindowSize = UINT64_MAX / 1000000ULL;  // Max before ns conversion overflows
    if (config.timeWindowSize.count() > maxSafeWindowSize) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "timeWindowSize too large (" + std::to_string(config.timeWindowSize.count()) +
                "); falling back to default 200 ms",
            "");
        config.timeWindowSize = std::chrono::milliseconds{200};
        valid = false;
    }

    if (config.temporalTolerance.count() < 0)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "temporalTolerance must be non-negative; falling back to default 5 ms", "");
        config.temporalTolerance = std::chrono::milliseconds{5};
        valid = false;
    }

    // Validate reasonable range
    long long maxTolerance = MAX_TEMPORAL_TOLERANCE_MS;
    if (config.temporalTolerance.count() > maxTolerance) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "temporalTolerance too large (" + std::to_string(config.temporalTolerance.count()) +
                " ms); falling back to max " + std::to_string(maxTolerance) + " ms",
            "");
        config.temporalTolerance = std::chrono::milliseconds{maxTolerance};
        valid = false;
    }

    if (config.trajectoryCount > 10)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "trajectoryCount out of range; falling back to default 4", "");
        config.trajectoryCount = 4;
        valid = false;
    }

    if (config.maxPipelines == 0 || config.maxPipelines > MAX_SUPPORTED_PIPELINES) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "maxPipelines out of range; falling back to default 2",
            "");
        config.maxPipelines = 2;
        valid = false;
    }

    return valid;
}

EventType parseEventTypeFromString(const std::string& name)
{
    static const std::unordered_map<std::string, EventType> eventTypeMap = {
        {"EVENT_0", EventType::EVENT_0},
        {"EVENT_1", EventType::EVENT_1},
        {"EVENT_2", EventType::EVENT_2},
        {"EVENT_3", EventType::EVENT_3},
        {"EVENT_4", EventType::EVENT_4},
        {"EVENT_5", EventType::EVENT_5},
        {"EVENT_6", EventType::EVENT_6},
        {"EVENT_7", EventType::EVENT_7},
        {"EVENT_8", EventType::EVENT_8},
        {"EVENT_9", EventType::EVENT_9},
        {"EVENT_10", EventType::EVENT_10},
        {"ROI_ENTRY", EventType::ROI_ENTRY},
        {"ROI_EXIT", EventType::ROI_EXIT},
        {"TW_CROSSING_ENTRY", EventType::TW_CROSSING_ENTRY},
        {"TW_CROSSING_EXIT", EventType::TW_CROSSING_EXIT},
        {"SW_FAIL", EventType::SW_FAIL},
        {"SENSOR_INVALID", EventType::SENSOR_INVALID},
        {"SENSOR_VALID", EventType::SENSOR_VALID},
        {"AI_PIPELINE_INVALID", EventType::AI_PIPELINE_INVALID},
        {"AI_PIPELINE_VALID", EventType::AI_PIPELINE_VALID}
        // Add more mappings as needed
    };

    auto it = eventTypeMap.find(name);
    if (it != eventTypeMap.end())
    {
        return it->second;
    }

    // Return default event type for unknown strings
    NvPSBWriteData(NVPSB_LOG_WARNING,
        "Unknown event type '" + name + "', using EVENT_0", "");
    return EventType::EVENT_0;
}

/** Trim ASCII whitespace for config values; avoids false rejects on trailing newlines/spaces. */
static std::string trimConfigWhitespace(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1U]))) {
        --j;
    }
    return s.substr(i, j - i);
}

/** Trim, then drop inline `#` comments (rest of line) for scalar config values. */
static std::string normalizeConfigScalar(const std::string& raw)
{
    std::string t = trimConfigWhitespace(raw);
    const size_t hash = t.find('#');
    if (hash != std::string::npos) {
        t = trimConfigWhitespace(t.substr(0, hash));
    }
    return t;
}

static void printUsage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s [-h|--help]\n\n"
        "PSS Daemon — Platform Safety Services daemon.\n\n"
        "  Reads configuration from /opt/nvidia/psf/bin/nvpss.conf.\n"
        "  No runtime flags are required; all behaviour is config-driven.\n\n"
        "Options:\n"
        "  -h, --help    Show this help message and exit.\n",
        prog);
}

/** After successful NvPSBInitialize: tear down PSB when main() exits early (e.g. signal registration failure). */
static int nvPsbExitEarlyFailure()
{
    if (NvPSBExit() != NVPSB_SUCCESS)
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to exit NvPSB", "");
    return EXIT_FAILURE;
}

int main(int argc, char* argv[])
{
    const char* prog = (argc > 0 && argv != nullptr && argv[0] != nullptr) ? argv[0] : "nvpss_daemon";

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            printUsage(prog);
            return EXIT_SUCCESS;
        }
        std::fprintf(stderr, "error: unexpected argument (see --help)\n");
        printUsage(prog);
        return EXIT_FAILURE;
    }
    /**
     * PSS daemon has two main components
     * 1. Listener for incoming messages from clients. Implemented through NvPSSDRPC class
     * 2. Monitoring of reported safety events by the clients and appropriately reporting to PSD.
     * Implemented through NvPSSSafetyEventManager class
     */
    /* Single NvPSBInitialize / NvPSBExit pair for the process lifetime. */
    if (NvPSBInitialize("NVPSB_PSS_DAEMON", NVPSB_PSS_DAEMON) != NVPSB_SUCCESS)
    {
        std::fprintf(stderr, "Failed to initialize PSB.\n");
        return EXIT_FAILURE;
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon Starting...", "");

    /*Register Signal handler*/
    if (std::signal(SIGINT, signalHandler) == SIG_ERR) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register SIGINT handler", "");
        return nvPsbExitEarlyFailure();
    }
    if (std::signal(SIGTERM, signalHandler) == SIG_ERR) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register SIGTERM handler", "");
        return nvPsbExitEarlyFailure();
    }
    /* Stream sends to disconnected peers would raise SIGPIPE by default; MSG_NOSIGNAL also used in NvPSSDRPC. */
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to ignore SIGPIPE", "");
        return nvPsbExitEarlyFailure();
    }

    nvpss::PSSConfigParser parser;
    NvPSSConfiguration config;
    shutdownRequested.store(false);

    const std::string filename = "/opt/nvidia/psf/bin/nvpss.conf";
    std::string pssdtopsdcombackend = "POSIX_MSG_QUE";
    // Define required configuration keys
    std::vector<std::string> requiredKeys = {
        "timeWindowSize", "fusionThreshold", "alpha", "beta", "gamma", "temporalTolerance", "maxPipelines", "PSSDToPSDComBackend", "sensorConfig"
    };

    std::vector<std::string> bypassEventsStr = {};
    std::unordered_set<EventType> bypassFusionEvents = {};

    if (!parser.loadFromFile(filename)) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Failed to load required config file: " + filename, "");
        return nvPsbExitEarlyFailure();
    }

    if (!parser.validateRequiredKeys(requiredKeys))
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "One or more required keys are missing from " + filename, "");
        return nvPsbExitEarlyFailure();
    }

    // Load values with fallback to defaults
    config.timeWindowSize = parser.getMilliseconds("timeWindowSize", config.timeWindowSize);
    config.fusionThreshold = parser.getFloat("fusionThreshold", config.fusionThreshold);
    config.alpha = parser.getFloat("alpha", config.alpha);
    config.beta = parser.getFloat("beta", config.beta);
    config.gamma = parser.getFloat("gamma", config.gamma);
    config.temporalTolerance = parser.getMilliseconds("temporalTolerance", config.temporalTolerance);
    config.trajectoryCount = parser.getUint("trajectoryCount", config.trajectoryCount);
    config.maxPipelines = parser.getUint("maxPipelines", config.maxPipelines);
    bypassEventsStr = parser.getBypassFusionEvents("bypassFusionEvents");
    for (const auto& eventStr : bypassEventsStr)
    {
        EventType type = parseEventTypeFromString(eventStr); // Implement string to enum mapping
        bypassFusionEvents.insert(type);
    }
    /* Trust-report events always bypass fusion and are sent to PSD with their reported severity. */
    bypassFusionEvents.insert(SENSOR_INVALID);
    bypassFusionEvents.insert(SENSOR_VALID);
    bypassFusionEvents.insert(AI_PIPELINE_INVALID);
    bypassFusionEvents.insert(AI_PIPELINE_VALID);

    {
        const std::string smf = normalizeConfigScalar(parser.getString("max_hb_failures", ""));
        uint32_t mf = config.maxHbFailures;
        if (!smf.empty()) {
            char* end = nullptr;
            errno = 0;
            unsigned long v = std::strtoul(smf.c_str(), &end, 10);
            if (end == smf.c_str() || *end != '\0' || errno == ERANGE) {
                NvPSBWriteData(NVPSB_LOG_WARNING,
                    "Invalid max_hb_failures value (expected integer 1..255); using default "
                        + std::to_string(mf),
                    "");
            } else {
                const unsigned long orig = v;
                if (v < 1UL) {
                    v = 1UL;
                } else if (v > 255UL) {
                    v = 255UL;
                }
                if (v != orig) {
                    NvPSBWriteData(NVPSB_LOG_WARNING,
                        "max_hb_failures value " + std::to_string(orig) + " clamped to " + std::to_string(v),
                        "");
                }
                mf = static_cast<uint32_t>(v);
            }
        }
        config.maxHbFailures = mf;
    }

    pssdtopsdcombackend = parser.getString("PSSDToPSDComBackend", "POSIX_MSG_QUE");
    if(pssdtopsdcombackend == "POSIX_MSG_QUE")
    {
        config.PSSDToPSDComBackend = NvPSDChannelBackend::POSIX_MSG_QUE;
    }
    else if(pssdtopsdcombackend == "POSIX_SOCKET")
    {
        config.PSSDToPSDComBackend = NvPSDChannelBackend::POSIX_SOCKET;
    }
    else
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "PSSDToPSDComBackend invalid value '" + pssdtopsdcombackend
                + "'; falling back to POSIX_MSG_QUE",
            "");
        config.PSSDToPSDComBackend = NvPSDChannelBackend::POSIX_MSG_QUE;
    }

    // Validate configuration values; invalid fields are corrected in-place with per-field warnings.
    if (!validateConfiguration(config)) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "One or more configuration parameters were invalid; default or bounded values were applied "
            "(see prior messages).",
            "");
    }

#ifdef NVPSF_DBG
    parser.printLoadedConfig();
    for (const std::string& s : bypassEventsStr) {
        NvPSBWriteData(NVPSB_LOG_DEBUG, s, "");
    }
#endif

    g_pssMaxHbFailures.store(config.maxHbFailures, std::memory_order_relaxed);
    g_pssWarnThreshold.store(g_pssMaxHbFailures.load(std::memory_order_relaxed) / 2U,
                             std::memory_order_relaxed);

    /* Trust-report events always bypass fusion; ensure they are in the set regardless of configured bypass list. */
    bypassFusionEvents.insert(SENSOR_INVALID);
    bypassFusionEvents.insert(SENSOR_VALID);
    bypassFusionEvents.insert(AI_PIPELINE_INVALID);
    bypassFusionEvents.insert(AI_PIPELINE_VALID);

    /*Init the SafetyEventManager so that its reference can be passed to NvPSSDRPC */
    std::unique_ptr<nvpss::SafetyEventManager> mSafetyEventManager =
        std::make_unique<nvpss::SafetyEventManager>(CRITICAL_PRIO_Q_PERIOD_US,HIGH_PRIO_Q_PERIOD_US,
                                            MEDIUM_PRIO_Q_PERIOD_US, LOW_PRIO_Q_PERIOD_US,
                                            INPUT_SAFETYEVENT_Q_PERIOD_US, SAFETYEVENT_FUSION_PERIOD_US, config.PSSDToPSDComBackend);

    mSafetyEventManager->SetBypassFusionEvents(bypassFusionEvents);

    {
        std::string sensorConfigPath = parser.getString("sensorConfig", "");
        sensorConfigPath = normalizeConfigScalar(sensorConfigPath);
        if (sensorConfigPath.empty()) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "sensorConfig is required in nvpss.conf but is missing or empty", "");
            return nvPsbExitEarlyFailure();
        }
        std::string cfgErr;
        auto entries = sensorConfigLoad(sensorConfigPath, &cfgErr);
        if (entries.empty()) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "Failed to load sensor config from " + sensorConfigPath + ": " + cfgErr, "");
            return nvPsbExitEarlyFailure();
        }
        auto idToName = sensorConfigIdToNameMap(entries);
        mSafetyEventManager->SetSensorConfig(idToName);
        NvPSBWriteData(NVPSB_LOG_INFO,
            "Loaded sensor config with " + std::to_string(entries.size()) + " sensors from " + sensorConfigPath, "");
    }

    /* Init Fusion Pipeline */
    mSafetyEventManager->EnableFusion(true);
    if (mSafetyEventManager->ConfigureMultiCameraFusion(
            config.maxPipelines,
            config.timeWindowSize,
            config.fusionThreshold,
            config.alpha,
            config.beta,
            config.gamma,
            config.temporalTolerance,
            config.trajectoryCount,
            EARLY_TERMINATION_THRESHOLD,
            ENABLE_EARLY_TERMINATION) != NVPSSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to initialize Fusion Pipeline; fusion disabled", "");
        mSafetyEventManager->EnableFusion(false);
    }

    /*Spawn msgListener thread*/
    std::thread msgListenerThread(msgListener, std::ref(mSafetyEventManager));

    /*Spawn eventMonitor thread*/
    std::thread eventMonitorThread(eventMonitor, std::ref(mSafetyEventManager));

    /*Spawn fusionMonitor Thread*/
    std::thread fusionMonitorThread(fusionMonitor, std::ref(mSafetyEventManager));

    /*Spawn heartbeatMonitor Thread*/
    heartbeatMonitorRunning.store(true);
    std::thread heartbeatMonitorThread(heartbeatMonitor);

    msgListenerThread.join();
    eventMonitorThread.join();
    fusionMonitorThread.join();
    heartbeatMonitorThread.join();

    /*Exit NvPSB*/
    if(NvPSBExit() != NVPSB_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to exit NvPSB", "");
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon is terminated", "");

    return 0;
}
