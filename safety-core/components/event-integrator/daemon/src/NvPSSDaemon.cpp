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
#include <cmath>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <atomic>

#include "pss_daemon.h"

#include "NvPSSDRPC.hpp"
#include "NvPSSDeliveryFailSafe.hpp"
#include "NvPSSSafetyEventManager.hpp"
#include "NvPSSStatusNoop.hpp"
#include "NvPSB.h"
#include "NvPSSConfigParser.hpp"
#include "sensor_config_parser.h"

#define MAX_CLIENTS 8
#define MAX_PENDING_CLIENTS 2

#define INPUT_SAFETYEVENT_Q_PERIOD_US 100
#define SAFETYEVENT_FUSION_PERIOD_US 2000
#define THRESHOLD_CONFIDENCE_FOR_PSD_REPORT 0.6f
#define EARLY_TERMINATION_THRESHOLD 0.9f
#define ENABLE_EARLY_TERMINATION true

/* Heartbeat fail-safe policy (see nvpss.conf max_hb_failures). WARN_THRESHOLD = max/2 (integer division).
 * Values set after validateConfiguration(); maxHbFailures is already validated when loaded from config. */
std::atomic<uint32_t> g_pssMaxHbFailures{10U};
/* Published here for the same reason as the heartbeat thresholds: the RPC
 * object is constructed after configuration is loaded, and this is how a
 * configured value reaches it. */
std::atomic<uint32_t> g_clientRejectErrorThreshold{NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_DEFAULT};
/* PSS_PRESENCE_* set of required sources, from nvpss.conf. */
std::atomic<uint32_t> g_requiredSourceMask{0U};
std::atomic<uint32_t> g_pssWarnThreshold{5U};

/* AF_UNIX path for the PSS RPC server. /run is the canonical per-boot runtime
 * directory on modern Linux; a dedicated /run/nvpsf subtree keeps the Safety
 * Core sockets out of /tmp and lets the container bind-mount only this one
 * narrow path instead of the entire host /tmp tree. */
const std::string socketPath = PSS_DAEMON_RPC_SOCKET_PATH;

#define MAX_TEMPORAL_TOLERANCE_MS 60000 // 60 seconds max tolerance

/* One fallback per fusion key. It is the value used when the key is not a
 * number (the getFloat default, taken from the initialisers below) and when it
 * is out of range or the weights do not sum to 1.0 (validateConfiguration).
 * These were once separate literals and drifted apart, so an unparsable value
 * and an out-of-range one fell back to different numbers. The warnings in
 * validateConfiguration quote these values; keep them in step. */
#define FUSION_THRESHOLD_FALLBACK 0.7f
#define FUSION_WEIGHT_FALLBACK 0.33f

struct NvPSSConfiguration {
    std::chrono::milliseconds timeWindowSize{200};        // Default: 200ms
    float fusionThreshold{FUSION_THRESHOLD_FALLBACK};     // Default: 0.7
    float alpha{FUSION_WEIGHT_FALLBACK};                  // Default: 0.33
    float beta{FUSION_WEIGHT_FALLBACK};                   // Default: 0.33
    float gamma{FUSION_WEIGHT_FALLBACK};                  // Default: 0.33
    std::chrono::milliseconds temporalTolerance{5};       // Default: 5ms
    uint8_t trajectoryCount{4};                           // Default: 4
    uint8_t maxPipelines{2};                              // Default: 2
    /* Mandatory in nvpss.conf. No default is honoured: the daemon refuses to
     * start rather than select an event pipeline the configuration does not
     * state. The initialiser only keeps the struct well-formed. */
    bool enableFusion{true};
    /* Deployment topology. Mandatory, and DECLARED rather than inferred from
     * whether a group file happens to exist: an operator reading nvpss.conf must
     * be able to see which topology is live without cross-referencing files.
     * The initialiser only keeps the struct well-formed. */
    bool is3D{false};
    std::string sensorGroupConfigPath{};
    /*
     * PSS -> PSD transport. Not configurable: the PSSDToPSDComBackend key
     * is rejected outright, so nothing reads a value into this field -- it is a
     * constant wearing a struct member's clothes
     * it still selects between.
     *
     * It names UNIX_SOCKET. That single value is
     * what actually moves the link: SafetyEventManager::StartSafetyEventManager
     * branches on it to decide between the legacy message-queue client and
     * initializePSSDServer(), and the gateway now takes the matching socket path
     * through NvPSDSocketInitialize.
     */
    NvPSFTransportKind PSSDToPSDComBackend{NVPSF_TRANSPORT_UNIX_SOCKET};
    uint32_t maxHbFailures{10U};                          /* consecutive HB misses before SW_FAIL; range 1..255 (default 10) */
    uint32_t pssToPsdRetryBudget{NVPSS_PSS_TO_PSD_RETRY_BUDGET_DEFAULT};
    uint32_t pssToPsdResponseTimeoutMs{NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_DEFAULT};
    uint32_t statusNoopIntervalMs{NVPSS_STATUS_NOOP_INTERVAL_MS_DEFAULT};
    /* Depth of the PSS -> PSD operational staging queue. Sized against frame
     * fan-out rather than event rate; see
     * NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_DEFAULT. */
    uint32_t psdOperationalQueueDepth{NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_DEFAULT};
    uint32_t psdCriticalQueueDepth{NVPSS_PSD_CRITICAL_QUEUE_DEPTH_DEFAULT};
    uint32_t fusionFailureErrorMs{NVPSS_FUSION_FAILURE_ERROR_MS_DEFAULT};
    uint32_t clientRejectErrorThreshold{NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_DEFAULT};
    uint32_t requiredSafetyMonitor{0U};
    uint32_t requiredPerceptionMonitor{0U};
    uint32_t requiredMdxInstances{0U};
};

std::atomic<bool> shutdownRequested;
std::condition_variable eventMonitorTerminationCV;
std::condition_variable msgListenerTerminationCV;
std::condition_variable fusionTerminationCV;
std::atomic<bool> heartbeatMonitorRunning{false};
std::condition_variable heartbeatTerminationCV;
std::shared_ptr<nvpss::NvPSSDRPC> g_NvPSSDRPC;
std::mutex g_rpcMutex;
volatile std::sig_atomic_t g_signalReceived = 0;
void signalHandler(int signal);
void requestShutdown();
void signalMonitor();
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

/** C-style required-source query: forwards to
 *  SafetyEventManager::IsRequiredSourcePipeline. Returns false when unavailable,
 *  which keeps the rejected-report escalation off rather than firing on an unknown source. */
static bool isRequiredSourceFromRPC(void* ctx, uint8_t pipelineId)
{
    if (ctx != nullptr)
    {
        return static_cast<nvpss::SafetyEventManager*>(ctx)->IsRequiredSourcePipeline(pipelineId);
    }
    return false;
}

void signalHandler(int signal)
{
    (void)signal;
    g_signalReceived = 1;
}

void requestShutdown()
{
    shutdownRequested.store(true, std::memory_order_relaxed);
    heartbeatMonitorRunning.store(false, std::memory_order_relaxed);
    msgListenerTerminationCV.notify_all();
    eventMonitorTerminationCV.notify_all();
    fusionTerminationCV.notify_all();
    heartbeatTerminationCV.notify_all();
}

void signalMonitor()
{
    while (!shutdownRequested.load(std::memory_order_relaxed))
    {
        if (g_signalReceived != 0)
        {
            requestShutdown();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void msgListener(std::unique_ptr<nvpss::SafetyEventManager>& h_safetyEventManager)
{
    std::mutex msgListenerTerminationCV_Mtx;
    std::unique_lock<std::mutex> lock(msgListenerTerminationCV_Mtx);

    {
        std::lock_guard<std::mutex> rpcLock(g_rpcMutex);
        g_NvPSSDRPC = std::make_shared<nvpss::NvPSSDRPC>(SOCKET, socketPath, MAX_CLIENTS,
                        MAX_PENDING_CLIENTS);
        g_NvPSSDRPC->setClientRejectErrorThreshold(
            g_clientRejectErrorThreshold.load(std::memory_order_relaxed));
        g_NvPSSDRPC->setRequiredSourceMask(
            g_requiredSourceMask.load(std::memory_order_relaxed));
    }

    if (g_NvPSSDRPC->NvPSSDInitRPCServer() != NVPSSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Error in initializing PSS RPC server", "");
        requestShutdown();
        goto cleanup;
    }

    {
        if (g_NvPSSDRPC->NvPSSDStartRPCServer(
                h_safetyEventManager->getInputSafetyEventQueRef(),
                h_safetyEventManager->getInputSafetyEventQueMutexRef(),
                THRESHOLD_CONFIDENCE_FOR_PSD_REPORT,
                msgListenerTerminationCV,
                onTrustReportFromRPC,
                h_safetyEventManager.get(),
                isRequiredSourceFromRPC,
                h_safetyEventManager.get()) != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Error in starting RPC server", "");
            requestShutdown();
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

/* Checks for the fusion-only keys, run only when enableFusion is true. In
 * passthrough these keys are not read, so there is nothing of theirs to check
 * and nothing to report. */
static bool validateFusionConfiguration(NvPSSConfiguration& config)
{
    bool valid = true;

    /* The range checks below are written as !(in range), not (below || above):
     * every ordered comparison with NaN is false, so the second form lets NaN
     * through, and a NaN threshold or weight leaves fusion silently off.
     * getFloat already rejects non-finite input; these stay correct on their
     * own if a non-finite value ever arrives another way. */
    if (!(config.fusionThreshold >= 0.0f && config.fusionThreshold <= 1.0f))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "fusionThreshold out of range; falling back to default 0.7", "");
        config.fusionThreshold = FUSION_THRESHOLD_FALLBACK;
        valid = false;
    }

    if (!(config.alpha >= 0.0f && config.alpha <= 1.0f))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "alpha out of range; falling back to default 0.33", "");
        config.alpha = FUSION_WEIGHT_FALLBACK;
        valid = false;
    }

    if (!(config.beta >= 0.0f && config.beta <= 1.0f))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "beta out of range; falling back to default 0.33", "");
        config.beta = FUSION_WEIGHT_FALLBACK;
        valid = false;
    }

    if (!(config.gamma >= 0.0f && config.gamma <= 1.0f))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "gamma out of range; falling back to default 0.33", "");
        config.gamma = FUSION_WEIGHT_FALLBACK;
        valid = false;
    }

    // Check if weights sum to approximately 1.0 (NaN-safe, as above)
    float weightSum = config.alpha + config.beta + config.gamma;
    if (!(std::abs(weightSum - 1.0f) <= 0.01f))
    {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "Weight sum (alpha + beta + gamma = " + std::to_string(weightSum) +
                ") should be close to 1.0; changing to default value",
            "");
        config.alpha = config.beta = config.gamma = FUSION_WEIGHT_FALLBACK;
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

    return valid;
}

bool validateConfiguration(NvPSSConfiguration& config)
{
    bool valid = true;

    if (config.enableFusion && !validateFusionConfiguration(config))
    {
        valid = false;
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

    if (config.maxPipelines == 0 || config.maxPipelines > MAX_SUPPORTED_PIPELINES) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "maxPipelines out of range; falling back to default 2",
            "");
        config.maxPipelines = 2;
        valid = false;
    }

    if (!NvPSSDeliveryRetryBudgetIsValid(config.pssToPsdRetryBudget)) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "pss_to_psd_retry_budget out of range; falling back to default " +
                std::to_string(NVPSS_PSS_TO_PSD_RETRY_BUDGET_DEFAULT),
            "");
        config.pssToPsdRetryBudget = NVPSS_PSS_TO_PSD_RETRY_BUDGET_DEFAULT;
        valid = false;
    }

    if (!NvPSSDeliveryResponseTimeoutMsIsValid(config.pssToPsdResponseTimeoutMs)) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "pss_to_psd_response_timeout_ms out of range; falling back to default " +
                std::to_string(NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_DEFAULT),
            "");
        config.pssToPsdResponseTimeoutMs = NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_DEFAULT;
        valid = false;
    }

    if (!NvPSSDeliveryRetryWindowMsIsValid(config.pssToPsdRetryBudget,
                                           config.pssToPsdResponseTimeoutMs)) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "PSS-to-PSD retry window out of range; falling back to default retry budget and timeout",
            "retryBudget=" + std::to_string(config.pssToPsdRetryBudget) +
                ", responseTimeoutMs=" + std::to_string(config.pssToPsdResponseTimeoutMs) +
                ", retryWindowMs=" + std::to_string(NvPSSDeliveryRetryWindowMs(
                    config.pssToPsdRetryBudget, config.pssToPsdResponseTimeoutMs)) +
                ", maxRetryWindowMs=" +
                std::to_string(NVPSS_PSS_TO_PSD_RETRY_WINDOW_MS_MAX));
        config.pssToPsdRetryBudget = NVPSS_PSS_TO_PSD_RETRY_BUDGET_DEFAULT;
        config.pssToPsdResponseTimeoutMs = NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_DEFAULT;
        valid = false;
    }

    if (!NvPSSStatusNoopIntervalMsIsValid(config.statusNoopIntervalMs)) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "status_noop_interval_ms out of range; falling back to default " +
                std::to_string(NVPSS_STATUS_NOOP_INTERVAL_MS_DEFAULT),
            "");
        config.statusNoopIntervalMs = NVPSS_STATUS_NOOP_INTERVAL_MS_DEFAULT;
        valid = false;
    }

    return valid;
}

EventType parseEventTypeFromString(const std::string& name)
{
    /* Must cover every EventType nameable in nvpss.conf. A name missing here is
     * not inert: the caller inserts whatever comes back, so a gap both fails to
     * exempt the intended event from fusion and adds one that was never listed. */
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
        {"EVENT_11", EventType::EVENT_11},
        {"EVENT_12", EventType::EVENT_12},
        {"EVENT_13", EventType::EVENT_13},
        {"EVENT_14", EventType::EVENT_14},
        {"EVENT_15", EventType::EVENT_15},
        {"EVENT_16", EventType::EVENT_16},
        {"EVENT_17", EventType::EVENT_17},
        {"EVENT_18", EventType::EVENT_18},
        {"EVENT_19", EventType::EVENT_19},
        {"EVENT_20", EventType::EVENT_20},
        {"EVENT_21", EventType::EVENT_21},
        {"EVENT_22", EventType::EVENT_22},
        {"EVENT_23", EventType::EVENT_23},
        {"SW_FAIL", EventType::SW_FAIL},
        {"SENSOR_INVALID", EventType::SENSOR_INVALID},
        {"SENSOR_VALID", EventType::SENSOR_VALID},
        {"AI_PIPELINE_INVALID", EventType::AI_PIPELINE_INVALID},
        {"AI_PIPELINE_VALID", EventType::AI_PIPELINE_VALID}
    };

    auto it = eventTypeMap.find(name);
    if (it != eventTypeMap.end())
    {
        return it->second;
    }

    /* Report the name as unrecognised rather than substituting a real event.
     * Coercing to EVENT_0 here would silently grant fusion bypass to an event
     * the operator never listed; the caller drops EVENT_UNKNOWN instead. */
    NvPSBWriteData(NVPSB_LOG_ERR,
        "Unknown event type '" + name + "' in bypassFusionEvents; entry ignored", "");
    return EventType::EVENT_UNKNOWN;
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

/*
 * A required-source key: strictly parsed, never
 * defaulted. Presence is enforced with the other required keys; this refuses a
 * duplicate, anything but plain decimal digits, and a value above maxValue.
 *
 * Why not getBoundedUint32Config(): it falls back to the default on an
 * unparsable value and clamps an out-of-range one, with a WARNING. For these keys
 * the fallback is 0 -- "not required" -- so a typo would silently switch off the
 * detection the key exists for. The launchers read the same keys to decide which
 * monitors to start, so a value they cannot agree on must stop the daemon too.
 */
static bool parseRequiredSourceKey(const nvpss::PSSConfigParser& parser,
                                   const std::string& filename,
                                   const char* key,
                                   uint32_t maxValue,
                                   uint32_t* out)
{
    const std::string raw = normalizeConfigScalar(parser.getString(key, ""));
    const char* why = nullptr;
    if (parser.isDuplicated(key)) {
        why = "is defined more than once";
    } else if (raw.empty() ||
               raw.find_first_not_of("0123456789") != std::string::npos ||
               raw.size() > 3U) {
        why = "is not a plain decimal integer";
    } else if (std::strtoul(raw.c_str(), nullptr, 10) > maxValue) {
        why = "is out of range";
    }
    if (why != nullptr) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            std::string(key) + " " + why + "; refusing to start",
            "file=" + filename + ", value='" + raw + "', accepted=0.." + std::to_string(maxValue));
        std::cerr << "error: " << filename << ": " << key << " " << why
                  << " (value '" << raw << "', accepted 0.." << maxValue << ").\n"
                     "       It declares which safety sources this deployment requires, and\n"
                     "       the launchers read it to decide which monitors to start, so it\n"
                     "       is never defaulted.\n";
        return false;
    }
    *out = static_cast<uint32_t>(std::strtoul(raw.c_str(), nullptr, 10));
    return true;
}

static uint32_t getBoundedUint32Config(const nvpss::PSSConfigParser& parser,
                                       const std::string& key,
                                       uint32_t defaultValue,
                                       uint32_t minValue,
                                       uint32_t maxValue)
{
    const std::string raw = normalizeConfigScalar(parser.getString(key, ""));
    if (raw.empty())
        return defaultValue;

    char* end = nullptr;
    errno = 0;
    unsigned long value = std::strtoul(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0' || errno == ERANGE) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            "Invalid " + key + " value (expected integer " +
                std::to_string(minValue) + ".." + std::to_string(maxValue) +
                "); using default " + std::to_string(defaultValue),
            "");
        return defaultValue;
    }

    if (value < minValue) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            key + " value " + std::to_string(value) +
                " clamped to " + std::to_string(minValue),
            "");
        value = minValue;
    } else if (value > maxValue) {
        NvPSBWriteData(NVPSB_LOG_WARNING,
            key + " value " + std::to_string(value) +
                " clamped to " + std::to_string(maxValue),
            "");
        value = maxValue;
    }

    return static_cast<uint32_t>(value);
}

static void printUsage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s [-h|--help]\n\n"
        "PSS Daemon — Platform Safety Services daemon.\n\n"
        "  Reads configuration from " NVPSS_DAEMON_DEFAULT_CONF " unless --conf is given.\n"
        "\n"
        "  --conf <PATH>   Read configuration from PATH instead of the default.\n"
        "                  The default is unchanged; this exists so the daemon can be\n"
        "                  brought up in a test fixture without editing a system file.\n"
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

    /*
     * --conf, and why it exists.
     *
     * The config path was a hard-coded absolute string with no override: no
     * flag, no environment variable, not even a path relative to the binary. A
     * safety daemon readable from exactly one absolute path cannot be brought up
     * in a test fixture without editing a system file, and that is why the full
     * PSF stack had never been run end to end -- every attempt ran
     * into it. The default is unchanged, so deployments are unaffected.
     *
     * Deliberately NOT an environment variable. The config selects safety
     * behaviour, and an env var is inherited silently by anything the daemon is
     * launched from; an explicit flag appears in the process listing and in
     * whatever launched it.
     */
    std::string confPath = NVPSS_DAEMON_DEFAULT_CONF;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            printUsage(prog);
            return EXIT_SUCCESS;
        }
        if (std::strcmp(argv[i], "--conf") == 0)
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "error: --conf requires a path\n");
                printUsage(prog);
                return EXIT_FAILURE;
            }
            confPath = argv[++i];
            if (confPath.empty())
            {
                std::fprintf(stderr, "error: --conf path must not be empty\n");
                return EXIT_FAILURE;
            }
            continue;
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
    g_signalReceived = 0;
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

    const std::string filename = confPath;
    // Define required configuration keys.
    //
    // PSSDToPSDComBackend is deliberately absent: it is retired, and is now
    // a rejected key rather than a required one (see below).
    /* Keys required in BOTH modes. The fusion-only keys are appended at step 6
     * once enableFusion is known. */
    std::vector<std::string> requiredKeys = {
        "timeWindowSize", "maxPipelines", "sensorConfig", "enableFusion",
        "deploymentMode",
        "required_safety_monitor", "required_perception_monitor", "required_mdx_instances"
    };

    std::vector<std::string> bypassEventsStr = {};
    std::unordered_set<EventType> bypassFusionEvents = {};
    const bool configLoaded = parser.loadFromFile(filename);

    /*
     * The PSS -> PSD transport is not selectable, so PSSDToPSDComBackend
     * has no meaning. Its presence is a fatal startup error rather than an ignored
     * key, because silently ignoring it would let an operator believe they had
     * selected a transport the daemon was not using -- a defect whose
     * consequence lands on a safety
     * link. There is no fallback and no warn-and-continue.
     *
     * Checked before the load-failure branch below, and on key presence rather
     * than value, so neither an unrelated malformed line nor a trailing inline
     * comment can carry the key past this point. Requires the release
     * migration note: an operator-supplied nvpss.conf retaining this key will
     * not start.
     */
    if (parser.hasKey("PSSDToPSDComBackend")) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Retired configuration key present; refusing to start",
            "file=" + filename + ", key=PSSDToPSDComBackend"
                ", remedy=delete the line. The PSS-to-PSD transport is no longer"
                " selectable and this key no longer has any effect.");
        std::cerr << "error: " << filename
                  << ": PSSDToPSDComBackend is a retired configuration key and is no longer\n"
                     "       selectable. Delete the line to start the daemon. The PSS-to-PSD\n"
                     "       transport is now fixed and cannot be configured.\n";
        return nvPsbExitEarlyFailure();
    }

    /*
     * Startup step 4 (design.md S5): a config file that cannot be read is fatal.
     *
     * Previously this warned and fell through to `init:` on struct defaults.
     * That cannot survive a mandatory key: the mode would come from a default
     * rather than from the operator's file, which is the silent-selection
     * failure the whole design exists to prevent. Taken BEFORE `init:` so the
     * true cause is reported here rather than surfacing later as the
     * sensorConfig check complaining about a key in a file that was never read.
     */
    if (!configLoaded) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Cannot read configuration file; refusing to start",
            "file=" + filename + ", remedy=create it, make it readable, or pass --conf <file>");
        std::cerr << "error: " << filename << ": configuration file is missing, unreadable or empty.\n"
                     "       The PSS daemon has no usable default configuration. Create the file,\n"
                     "       make it readable, or pass --conf <file>.\n";
        return nvPsbExitEarlyFailure();
    }

    /*
     * Startup step 5a: parse deploymentMode BEFORE enableFusion, because 3D
     * constrains what enableFusion may be (step 5b) and selects whether
     * sensorGroupConfig is required (step 5c). Parsing them in the other order
     * would let a 3D deployment accept enableFusion=true and only discover the
     * conflict after the mode was already applied.
     */
    {
        if (parser.isDuplicated("deploymentMode")) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "deploymentMode defined more than once; refusing to start",
                "file=" + filename + ", remedy=keep exactly one deploymentMode line");
            std::cerr << "error: " << filename << ": deploymentMode is defined more than once.\n"
                         "       The last definition would silently win. Keep exactly one.\n";
            return nvPsbExitEarlyFailure();
        }

        const std::string mode = normalizeConfigScalar(parser.getString("deploymentMode", ""));
        if (mode == "2D" || mode == "2d") {
            config.is3D = false;
        } else if (mode == "3D" || mode == "3d") {
            config.is3D = true;
        } else {
            const char* why = mode.empty() ? "is required but missing"
                                           : "has an unrecognised value";
            NvPSBWriteData(NVPSB_LOG_ERR,
                std::string("deploymentMode ") + why + "; refusing to start",
                "file=" + filename + ", accepted=2D|3D");
            std::cerr << "error: " << filename << ": deploymentMode " << why << ".\n"
                         "       Accepted values are 2D and 3D. It is mandatory and is not\n"
                         "       inferred from which other files are present: the topology a\n"
                         "       deployment runs must be readable from this file alone.\n";
            return nvPsbExitEarlyFailure();
        }
    }

    /*
     * Startup step 5: parse enableFusion BEFORE validating required keys,
     * because it selects WHICH keys are required (step 6). Validating first
     * would silently reinstate the fixed key list this replaces.
     */
    {
        if (parser.isDuplicated("enableFusion")) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "enableFusion defined more than once; refusing to start",
                "file=" + filename + ", remedy=keep exactly one enableFusion line");
            std::cerr << "error: " << filename << ": enableFusion is defined more than once.\n"
                         "       The last definition would silently win while the earlier line\n"
                         "       remained visible above it. Keep exactly one.\n";
            return nvPsbExitEarlyFailure();
        }

        nvpss::BoolParse outcome = nvpss::BoolParse::Absent;
        const bool value = parser.getBool("enableFusion", outcome);
        if (outcome != nvpss::BoolParse::Ok) {
            const char* why = (outcome == nvpss::BoolParse::Absent)
                ? "is required but missing"
                : "has an unrecognised value";
            NvPSBWriteData(NVPSB_LOG_ERR,
                std::string("enableFusion ") + why + "; refusing to start",
                "file=" + filename + ", accepted=true|false");
            std::cerr << "error: " << filename << ": enableFusion " << why << ".\n"
                         "       Accepted values are true and false. It is mandatory: the daemon\n"
                         "       will not choose an event pipeline that the configuration does not\n"
                         "       state.\n";
            return nvPsbExitEarlyFailure();
        }
        config.enableFusion = value;
    }

    /*
     * Startup step 5b: in 3D, perception is fused upstream by BEV before PSS
     * sees it, so daemon-side fusion has nothing left to correlate. A 3D
     * configuration that sets enableFusion=true is FATAL, not silently
     * overridden: an override would run one pipeline while the file states
     * another, which is the objection that made enableFusion mandatory in the
     * first place.
     */
    if (config.is3D && config.enableFusion) {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "deploymentMode=3D requires enableFusion=false; refusing to start",
            "file=" + filename);
        std::cerr << "error: " << filename << ": deploymentMode=3D requires enableFusion=false.\n"
                     "       In 3D the BEV pipeline fuses perception before PSS receives it, so\n"
                     "       daemon-side fusion has nothing to correlate. This is not overridden\n"
                     "       silently: the file must state the pipeline that actually runs.\n";
        return nvPsbExitEarlyFailure();
    }

    /*
     * Startup step 5c: sensorGroupConfig is required in 3D and unread in 2D.
     * Being unread is not the same as being harmless -- an operator who switches
     * a deployment to 2D and leaves the key set could believe groups are still
     * active. Name it in the log, exactly as the fusion-only keys are named at
     * step 9.
     */
    if (config.is3D) {
        config.sensorGroupConfigPath =
            normalizeConfigScalar(parser.getString("sensorGroupConfig", ""));
        if (config.sensorGroupConfigPath.empty()) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "sensorGroupConfig is required when deploymentMode=3D; refusing to start",
                "file=" + filename);
            std::cerr << "error: " << filename << ": sensorGroupConfig is required when"
                         " deploymentMode=3D.\n"
                         "       It names the file describing which cameras constitute the BEV\n"
                         "       group. Without it the daemon cannot derive group health.\n";
            return nvPsbExitEarlyFailure();
        }
    } else if (parser.hasKey("sensorGroupConfig")) {
        NvPSBWriteData(NVPSB_LOG_INFO,
            "deploymentMode=2D: sensorGroupConfig present and ignored",
            "file=" + filename + ", key=sensorGroupConfig");
    }

    /*
     * Startup step 6: the required-key set depends on the mode. The fusion
     * tuning keys are meaningless when events are not fused, so they are not
     * required and are not evaluated. timeWindowSize and maxPipelines look like
     * fusion keys but are NOT -- they drive the staleness threshold and the
     * pipeline bound, which apply in both modes (see ConfigureEventStream).
     */
    if (config.enableFusion) {
        requiredKeys.insert(requiredKeys.end(),
            {"fusionThreshold", "alpha", "beta", "gamma", "temporalTolerance"});
    }

    /* Startup step 7: a missing required key is now fatal, not a warning. */
    if (!parser.validateRequiredKeys(requiredKeys))
    {
        NvPSBWriteData(NVPSB_LOG_ERR,
            "Required configuration keys are missing; refusing to start",
            "file=" + filename + ", mode=" + (config.enableFusion ? "enableFusion=true"
                                                                  : "enableFusion=false"));
        std::cerr << "error: " << filename << ": required configuration keys are missing"
                     " (listed above).\n"
                     "       Defaults are not substituted for required keys.\n";
        return nvPsbExitEarlyFailure();
    }

    /* Startup step 9: name the fusion-only keys that are present but inert, so a
     * malformed one is not masked until the operator later flips the key to
     * true. */
    if (!config.enableFusion) {
        std::string inert;
        for (const char* k : {"fusionThreshold", "alpha", "beta", "gamma",
                              "temporalTolerance", "trajectoryCount", "bypassFusionEvents"}) {
            if (parser.hasKey(k)) {
                if (!inert.empty()) inert += ", ";
                inert += k;
            }
        }
        if (!inert.empty()) {
            NvPSBWriteData(NVPSB_LOG_INFO,
                "enableFusion=false: fusion-only keys present and ignored", "keys=" + inert);
        }
    }

    // Load values with fallback to defaults
    config.timeWindowSize = parser.getMilliseconds("timeWindowSize", config.timeWindowSize);
    config.maxPipelines = parser.getUint("maxPipelines", config.maxPipelines);
    /* The fusion-only keys (step 9's list) are read only when events are fused.
     * With enableFusion=false they are named as ignored above and nothing uses
     * them, so parsing them here would report errors and fallbacks for values
     * the daemon has just said it ignores. */
    if (config.enableFusion) {
        config.fusionThreshold = parser.getFloat("fusionThreshold", config.fusionThreshold);
        config.alpha = parser.getFloat("alpha", config.alpha);
        config.beta = parser.getFloat("beta", config.beta);
        config.gamma = parser.getFloat("gamma", config.gamma);
        config.temporalTolerance = parser.getMilliseconds("temporalTolerance", config.temporalTolerance);
        config.trajectoryCount = parser.getUint("trajectoryCount", config.trajectoryCount);
        bypassEventsStr = parser.getBypassFusionEvents("bypassFusionEvents");
    }
    for (const auto& eventStr : bypassEventsStr)
    {
        EventType type = parseEventTypeFromString(eventStr);
        if (type == EventType::EVENT_UNKNOWN)
        {
            continue; // unrecognised name already logged; never guess a substitute
        }
        bypassFusionEvents.insert(type);
    }
    /* Fault reports must not wait for fusion, even if omitted from the config.
     * The fusion loop correlates only generic events, not SW_FAIL. */
    bypassFusionEvents.insert(SW_FAIL);
    /* Trust-report events always bypass fusion and are sent to PSD as daemon-owned critical evidence. */
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

    config.pssToPsdRetryBudget = getBoundedUint32Config(
        parser,
        "pss_to_psd_retry_budget",
        config.pssToPsdRetryBudget,
        NVPSS_PSS_TO_PSD_RETRY_BUDGET_MIN,
        NVPSS_PSS_TO_PSD_RETRY_BUDGET_MAX);
    config.pssToPsdResponseTimeoutMs = getBoundedUint32Config(
        parser,
        "pss_to_psd_response_timeout_ms",
        config.pssToPsdResponseTimeoutMs,
        NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_MIN,
        NVPSS_PSS_TO_PSD_RESPONSE_TIMEOUT_MS_MAX);
    config.statusNoopIntervalMs = getBoundedUint32Config(
        parser,
        "status_noop_interval_ms",
        config.statusNoopIntervalMs,
        NVPSS_STATUS_NOOP_INTERVAL_MS_MIN,
        NVPSS_STATUS_NOOP_INTERVAL_MS_MAX);
    config.psdOperationalQueueDepth = getBoundedUint32Config(
        parser,
        "psd_operational_queue_depth",
        config.psdOperationalQueueDepth,
        NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MIN,
        NVPSS_PSD_OPERATIONAL_QUEUE_DEPTH_MAX);
    config.psdCriticalQueueDepth = getBoundedUint32Config(
        parser,
        "psd_critical_queue_depth",
        config.psdCriticalQueueDepth,
        NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MIN,
        NVPSS_PSD_CRITICAL_QUEUE_DEPTH_MAX);
    config.fusionFailureErrorMs = getBoundedUint32Config(
        parser,
        "fusion_failure_error_ms",
        config.fusionFailureErrorMs,
        NVPSS_FUSION_FAILURE_ERROR_MS_MIN,
        NVPSS_FUSION_FAILURE_ERROR_MS_MAX);
    config.clientRejectErrorThreshold = getBoundedUint32Config(
        parser,
        "client_reject_error_threshold",
        config.clientRejectErrorThreshold,
        NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_MIN,
        NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_MAX);
    if (!parseRequiredSourceKey(parser, filename, "required_safety_monitor", 1U,
                                &config.requiredSafetyMonitor) ||
        !parseRequiredSourceKey(parser, filename, "required_perception_monitor", 1U,
                                &config.requiredPerceptionMonitor) ||
        !parseRequiredSourceKey(parser, filename, "required_mdx_instances", PSS_MDX_INSTANCE_MAX,
                                &config.requiredMdxInstances)) {
        return nvPsbExitEarlyFailure();
    }

    /* No PSS -> PSD transport selection is read from configuration. The
     * value stays at the NvPSSConfiguration default, which is today's behaviour;
     * the default names UNIX_SOCKET. Removing the
     * selection here and changing the transport there are kept separate on
     * purpose, so this task alters no runtime behaviour on the link. */

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

/* The `init:` label is gone with the last `goto init`. Both jumps to it -- an
 * unreadable config file and a missing required key -- now exit non-zero, so
 * there is no longer a path that reaches this point on struct defaults. */
    g_pssMaxHbFailures.store(config.maxHbFailures, std::memory_order_relaxed);
    g_clientRejectErrorThreshold.store(config.clientRejectErrorThreshold,
                                       std::memory_order_relaxed);
    {
        uint32_t mask = 0U;
        if (config.requiredSafetyMonitor != 0U)
            mask |= PSS_PRESENCE_SAFETY_MONITOR;
        if (config.requiredPerceptionMonitor != 0U)
            mask |= PSS_PRESENCE_PERCEPTION_MONITOR;
        for (uint32_t k = 1U; k <= config.requiredMdxInstances; ++k)
            mask |= PSS_PRESENCE_MDX_INSTANCE(k);
        g_requiredSourceMask.store(mask, std::memory_order_relaxed);
    }
    /* Always recorded, including all-zero, so the log states what this daemon
     * will hold the site to. Sources that register are tracked either way. */
    NvPSBWriteData(NVPSB_LOG_INFO, "PSS required sources",
        "required_safety_monitor=" + std::to_string(config.requiredSafetyMonitor) +
            ", required_perception_monitor=" + std::to_string(config.requiredPerceptionMonitor) +
            ", required_mdx_instances=" + std::to_string(config.requiredMdxInstances) +
            " (a required source not registered within the heartbeat budget of PSS start"
            " is reported lost; sources that register are tracked either way)");
    g_pssWarnThreshold.store(g_pssMaxHbFailures.load(std::memory_order_relaxed) / 2U,
                             std::memory_order_relaxed);

    /* Trust-report events always bypass fusion; ensure they are in the set. */
    bypassFusionEvents.insert(SENSOR_INVALID);
    bypassFusionEvents.insert(SENSOR_VALID);
    bypassFusionEvents.insert(AI_PIPELINE_INVALID);
    bypassFusionEvents.insert(AI_PIPELINE_VALID);

    /*Init the SafetyEventManager so that its reference can be passed to NvPSSDRPC */
    std::unique_ptr<nvpss::SafetyEventManager> mSafetyEventManager =
        std::make_unique<nvpss::SafetyEventManager>(INPUT_SAFETYEVENT_Q_PERIOD_US,
                                            SAFETYEVENT_FUSION_PERIOD_US,
                                            config.PSSDToPSDComBackend,
                                            config.pssToPsdRetryBudget,
                                            config.pssToPsdResponseTimeoutMs,
                                            config.statusNoopIntervalMs,
                                            config.psdOperationalQueueDepth,
                                            config.psdCriticalQueueDepth,
                                            config.fusionFailureErrorMs);
    /* Startup record of the PSS -> PSD transport actually in use. Evidence accepts
     * the safe-state injectors' --transport as a declared label on the strength
     * of component startup logs being the authority, and the injector banner
     * tells its reader to read this link off the daemon log -- so this line has
     * to exist for that instruction to be followable. */
    NvPSBWriteData(NVPSB_LOG_INFO,
        "PSS->PSD transport selected",
        std::string("transport=") + NvPSFTransportKindName(config.PSSDToPSDComBackend) +
            " (not configurable)");

    NvPSBWriteData(NVPSB_LOG_INFO,
        "PSS-to-PSD delivery fail-safe config",
        "retryBudget=" + std::to_string(config.pssToPsdRetryBudget) +
            ", responseTimeoutMs=" + std::to_string(config.pssToPsdResponseTimeoutMs) +
            ", retryWindowMs=" + std::to_string(NvPSSDeliveryRetryWindowMs(
                config.pssToPsdRetryBudget, config.pssToPsdResponseTimeoutMs)) +
            ", statusNoopIntervalMs=" + std::to_string(config.statusNoopIntervalMs) +
            ", psdOperationalQueueDepth=" + std::to_string(config.psdOperationalQueueDepth) +
            ", psdCriticalQueueDepth=" + std::to_string(config.psdCriticalQueueDepth));

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

        /*
         * 3D: load the BEV group and MERGE it into the identity map before
         * handing the map over.
         *
         * Identity and health are separate concerns. For identity a group is
         * just another named pipelineId, so merging it here lets
         * ValidateEventSource accept a group event with no change at all. Only
         * health is derived, and that uses the membership passed separately.
         *
         * The camera rows stay in the map. They must: SAIM reports health per
         * camera, and a SENSOR_VALID whose pipelineId is not in this map is
         * dropped by ValidateEventSource -- which would leave the failed-sensor
         * mask permanently set and the safe-state latch unreleasable.
         */
        if (config.is3D) {
            SensorGroupConfig group{};
            const SensorConfigErr gErr =
                sensorGroupConfigParse(config.sensorGroupConfigPath.c_str(), &group);
            if (gErr != SENSOR_CONFIG_OK) {
                NvPSBWriteData(NVPSB_LOG_ERR,
                    "Failed to load sensor group config; refusing to start",
                    "file=" + config.sensorGroupConfigPath +
                        ", reason=" + sensorConfigErrStr(gErr));
                std::cerr << "error: " << config.sensorGroupConfigPath << ": "
                          << sensorConfigErrStr(gErr) << ".\n";
                return nvPsbExitEarlyFailure();
            }

            /* The group id must not collide with a camera: one pipelineId cannot
             * name both a stream and the group that contains it. */
            if (idToName.find(group.groupPipelineId) != idToName.end()) {
                NvPSBWriteData(NVPSB_LOG_ERR,
                    "Sensor group pipelineId collides with a camera; refusing to start",
                    "groupPipelineId=" + std::to_string(group.groupPipelineId) +
                        ", camera=" + idToName[group.groupPipelineId]);
                std::cerr << "error: group pipelineId " << static_cast<unsigned>(group.groupPipelineId)
                          << " is already used by camera '" << idToName[group.groupPipelineId]
                          << "' in " << sensorConfigPath << ".\n";
                return nvPsbExitEarlyFailure();
            }

            /* Every member must exist in sensor_config, or its health can never
             * be reported and the group verdict would be derived from a sensor
             * nothing observes. */
            std::vector<uint8_t> members;
            for (size_t i = 0U; i < group.memberCount; ++i) {
                const uint8_t mid = group.memberPipelineIds[i];
                if (idToName.find(mid) == idToName.end()) {
                    NvPSBWriteData(NVPSB_LOG_ERR,
                        "Sensor group member is not in sensor_config; refusing to start",
                        "member=" + std::to_string(mid) + ", file=" + sensorConfigPath);
                    std::cerr << "error: sensor group member pipelineId "
                              << static_cast<unsigned>(mid) << " is not present in "
                              << sensorConfigPath << ".\n";
                    return nvPsbExitEarlyFailure();
                }
                members.push_back(mid);
            }

            /* The group id must be reachable through ValidateEventSource, which
             * rejects anything above maxPipelines before the membership check. */
            if (group.groupPipelineId > config.maxPipelines) {
                NvPSBWriteData(NVPSB_LOG_ERR,
                    "Sensor group pipelineId exceeds maxPipelines; refusing to start",
                    "groupPipelineId=" + std::to_string(group.groupPipelineId) +
                        ", maxPipelines=" + std::to_string(config.maxPipelines));
                std::cerr << "error: group pipelineId "
                          << static_cast<unsigned>(group.groupPipelineId)
                          << " exceeds maxPipelines " << static_cast<unsigned>(config.maxPipelines)
                          << "; every group event would be rejected as an unregistered sensor.\n";
                return nvPsbExitEarlyFailure();
            }

            idToName[group.groupPipelineId] = group.groupName;
            mSafetyEventManager->SetSensorGroup(group.groupPipelineId, members);

            std::string memberList;
            for (size_t i = 0U; i < members.size(); ++i) {
                if (i != 0U) memberList += "|";
                memberList += std::to_string(members[i]);
            }
            NvPSBWriteData(NVPSB_LOG_INFO,
                "deploymentMode=3D: BEV sensor group loaded",
                "group=" + std::string(group.groupName) +
                    ", pipelineId=" + std::to_string(group.groupPipelineId) +
                    ", members=" + memberList);
        }

        mSafetyEventManager->SetSensorConfig(idToName);
        NvPSBWriteData(NVPSB_LOG_INFO,
            "Loaded sensor config with " + std::to_string(entries.size()) + " sensors from " + sensorConfigPath,
            std::string("deploymentMode=") + (config.is3D ? "3D" : "2D"));
    }

    /* Event-stream configuration first: the pipeline bound and the staleness
     * threshold are needed whether or not events are fused, so they must not
     * live inside fusion initialisation. */
    if (mSafetyEventManager->ConfigureEventStream(
            config.timeWindowSize, config.maxPipelines) != NVPSSD_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to configure event stream", "");
        return nvPsbExitEarlyFailure();
    }

    /*
     * Startup steps 12-13: select the event pipeline.
     *
     * The mode is fixed here for the process lifetime. There is no runtime
     * toggle and no re-read: an operator changing the file must restart the
     * daemon, so the mode a log line reports is the mode that ran.
     */
    mSafetyEventManager->EnableFusion(config.enableFusion);

    if (config.enableFusion)
    {
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
            /*
             * Fatal, not a downgrade to bypass. The previous behaviour disabled
             * fusion and continued, which delivered a different event pipeline
             * from the one the configuration stated -- with nothing but a log
             * line to say so. A configuration that cannot be honoured is a
             * startup failure.
             */
            NvPSBWriteData(NVPSB_LOG_ERR,
                "enableFusion=true but the fusion pipeline failed to initialise; refusing to start",
                "remedy=correct the fusion parameters in nvpss.conf, or set enableFusion=false");
            std::cerr << "error: enableFusion=true but the fusion pipeline could not be\n"
                         "       initialised. Refusing to start rather than running the\n"
                         "       non-fusing pipeline the configuration did not ask for.\n";
            return nvPsbExitEarlyFailure();
        }
    }
    /* When disabled, ConfigureMultiCameraFusion is not called at all. Calling it
     * would fail its own precondition and log an initialisation error against a
     * perfectly valid configuration. ConfigureEventStream above has already set
     * everything the non-fusing path needs. */

    NvPSBWriteData(NVPSB_LOG_INFO,
        "Event pipeline selected",
        std::string("enableFusion=") + (config.enableFusion ? "true (multi-camera fusion)"
                                                            : "false (per-camera passthrough)"));

    /*Spawn msgListener thread*/
    std::thread msgListenerThread(msgListener, std::ref(mSafetyEventManager));

    /*Spawn eventMonitor thread*/
    std::thread eventMonitorThread(eventMonitor, std::ref(mSafetyEventManager));

    /*Spawn fusionMonitor Thread*/
    std::thread fusionMonitorThread(fusionMonitor, std::ref(mSafetyEventManager));

    /*Spawn heartbeatMonitor Thread*/
    heartbeatMonitorRunning.store(true);
    std::thread heartbeatMonitorThread(heartbeatMonitor);

    /* Monitor POSIX signal flags from normal thread context. */
    std::thread signalMonitorThread(signalMonitor);

    msgListenerThread.join();
    eventMonitorThread.join();
    fusionMonitorThread.join();
    heartbeatMonitorThread.join();
    signalMonitorThread.join();

    /*Exit NvPSB*/
    if(NvPSBExit() != NVPSB_SUCCESS)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to exit NvPSB", "");
    }

    NvPSBWriteData(NVPSB_LOG_INFO, "PSS Daemon is terminated", "");

    return 0;
}
