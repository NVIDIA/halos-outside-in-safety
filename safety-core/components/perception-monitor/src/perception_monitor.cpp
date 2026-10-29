/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "perception_monitor.h"

#include "container_health_checker.h"
#include "data_validation_monitor.h"
#include "kafka_frame_source.h"
#include "docker_client.h"
#include "pcm_fault_aggregator.h"
#include "pcm_log.h"
#include "pss_daemon.h"
#include "pss_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>
#include <unordered_set>

static constexpr int kHealthIntervalSec = 1;
static constexpr int kDefaultLearnDurationSec = 300;
static constexpr const char* kDefaultBaselinePath = "/opt/nvidia/psf/bin/pcm_baseline/pcm_baseline.cfg";
static constexpr const char* kBaselineEnvVar = "PCM_BASELINE_CONFIG";
/* pcm_monitor.cfg location (same resolution as the health checker uses): the
 * Monitor toggles, fault timing and DVM percentages are read here in ACTIVE. */
static constexpr const char* kMonitorConfigEnvVar      = "PCM_MONITOR_CONFIG";
static constexpr const char* kDefaultMonitorConfigPath = "/opt/nvidia/psf/bin/pcm_monitor.cfg";

/* DVM Kafka data-plane source. Separate consumer group from
 * mdx-client so DVM gets its own copy of every mdx-frames message. The broker is
 * runtime-configurable (--kafka-broker / PerceptionMonitorOptions::kafkaBroker). */
static constexpr const char* kDvmFramesTopic   = "mdx-frames";
static constexpr const char* kDvmConsumerGroup = "nv_perception_monitor_dvm";

/* Stop-flag check granularity so a long poll interval still exits promptly. */
static constexpr std::chrono::milliseconds kStopCheckInterval{100};

namespace {

static std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static std::string defaultBaselinePath() {
    const char* envPath = std::getenv(kBaselineEnvVar);
    if (envPath != nullptr && envPath[0] != '\0') return std::string(envPath);
    return std::string(kDefaultBaselinePath);
}

static std::string trimConfig(const std::string& in) {
    const std::size_t a = in.find_first_not_of(" \t\r\n\f\v");
    if (a == std::string::npos) return std::string();
    const std::size_t b = in.find_last_not_of(" \t\r\n\f\v");
    return in.substr(a, b - a + 1U);
}

static bool parseConfigBool(const std::string& value, bool dflt) {
    const std::string s = lowerCopy(value);
    if (s == "true" || s == "1" || s == "yes" || s == "on")  return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return dflt;  /* unrecognized -> keep the current/default value */
}

static bool parsePositiveInt(const std::string& text, int minValue, int maxValue, int& out) {
    if (text.empty()) return false;
    int result = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (result > (std::numeric_limits<int>::max() - digit) / 10) return false;
        result = (result * 10) + digit;
    }
    if (result < minValue || result > maxValue) return false;
    out = result;
    return true;
}

/* Built-in values for the optional millisecond settings, used (with a warning)
 * when a line is missing or invalid. */
static constexpr int kDefaultMaxReportAgeMs = 4000;
static constexpr int kDefaultInitialValidMs = 4000;
static constexpr int kDefaultStartupGraceMs = 10000;
static constexpr int kDefaultVerdictResendMs = 5000;
static constexpr int kDefaultContainerStartupGraceMs = 30000;

/* One optional positive-integer setting. A missing, invalid or repeated line
 * falls back to the built-in value with a warning; it never stops PCM. */
struct OptionalSetting
{
    const char* section;
    const char* key;
    int*        target;
    int         fallback;
    std::size_t line;    /* first line it appeared on; 0 = missing */
    bool        invalid; /* bad value or repeated */
};

/* Read ACTIVE settings once, before PSS registration. Monitor toggles retain
 * their existing defaults; timing counts and enabled-DVM percentages have no
 * compiled fallback; the optional millisecond settings fall back with a warning.
 * The container allow-list remains owned by ContainerHealthChecker. */
static bool readMonitorConfig(PerceptionMonitorOptions& options) {
    options.containerHealth = true;
    options.dataValidation = true;
    options.badTicksToInvalid = 0;
    options.goodTicksToValid = 0;
    options.badObjectsPercent = 0;
    options.badFramesPercent = 0;
    options.maxReportAgeMs = 0;
    options.initialValidMs = 0;
    options.startupGraceMs = 0;
    options.verdictResendMs = 0;
    options.containerStartupGraceMs = 0;
    OptionalSetting optional[] = {
        {"fault_aggregator", "max_report_age_ms", &options.maxReportAgeMs,
         kDefaultMaxReportAgeMs, 0U, false},
        {"fault_aggregator", "initial_valid_ms", &options.initialValidMs,
         kDefaultInitialValidMs, 0U, false},
        {"data_validation", "startup_grace_ms", &options.startupGraceMs,
         kDefaultStartupGraceMs, 0U, false},
        {"fault_aggregator", "verdict_resend_ms", &options.verdictResendMs,
         kDefaultVerdictResendMs, 0U, false},
        {"health_monitor", "container_startup_grace_ms", &options.containerStartupGraceMs,
         kDefaultContainerStartupGraceMs, 0U, false},
    };
    const char* env = std::getenv(kMonitorConfigEnvVar);
    const std::string path = (env != nullptr && env[0] != '\0')
                                 ? std::string(env)
                                 : std::string(kDefaultMonitorConfigPath);
    std::ifstream file(path.c_str());
    if (!file.is_open()) {
        PcmLogLine(PcmLogLevel::Err) << "cannot read PCM monitor config";
        return false;
    }

    std::string section;
    bool timingSectionSeen = false;
    bool dvmSectionSeen = false;
    std::size_t lineNumber = 0U;
    std::string line;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string t = trimConfig(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = trimConfig(t.substr(1U, t.size() - 2U));
            bool* sectionSeen = nullptr;
            if (section == "fault_aggregator") sectionSeen = &timingSectionSeen;
            else if (section == "data_validation") sectionSeen = &dvmSectionSeen;
            if (sectionSeen != nullptr) {
                if (*sectionSeen) {
                    PcmLogLine(PcmLogLevel::Err) << "duplicate [" << section
                                                 << "] section at line " << lineNumber;
                    return false;
                }
                *sectionSeen = true;
            }
            continue;
        }
        if (section != "monitors" && section != "fault_aggregator" &&
            section != "data_validation" && section != "health_monitor") continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) {
            if (section != "monitors" && section != "health_monitor") {
                PcmLogLine(PcmLogLevel::Err) << "malformed [" << section
                                             << "] setting at line " << lineNumber;
                return false;
            }
            continue;
        }
        const std::string key = trimConfig(t.substr(0U, eq));
        const std::string val = trimConfig(t.substr(eq + 1U));
        if (section == "monitors") {
            if (key == "container_health")
                options.containerHealth = parseConfigBool(val, options.containerHealth);
            else if (key == "data_validation")
                options.dataValidation = parseConfigBool(val, options.dataValidation);
            continue;
        }
        OptionalSetting* setting = nullptr;
        for (OptionalSetting& candidate : optional) {
            if (section == candidate.section && key == candidate.key) {
                setting = &candidate;
                break;
            }
        }
        if (setting != nullptr) {
            if (setting->line != 0U) {
                setting->invalid = true; /* repeated: which value was meant is unclear */
            } else {
                setting->line = lineNumber;
                if (!parsePositiveInt(val, 1, std::numeric_limits<int>::max(), *setting->target)) {
                    setting->invalid = true;
                }
            }
            continue;
        }
        /* The health checker owns allow-list validation and its warn/skip policy. */
        if (section == "health_monitor") continue;
        int* count = nullptr;
        const int maximum = section == "data_validation" ? 100 : std::numeric_limits<int>::max();
        if (section == "fault_aggregator") {
            if (key == "bad_ticks_to_invalid") count = &options.badTicksToInvalid;
            else if (key == "good_ticks_to_valid") count = &options.goodTicksToValid;
        } else {
            if (key == "bad_objects_percent") count = &options.badObjectsPercent;
            else if (key == "bad_frames_percent") count = &options.badFramesPercent;
        }
        if (count == nullptr || *count != 0) {
            PcmLogLine(PcmLogLevel::Err) << "unknown or duplicate [" << section
                                         << "] key at line " << lineNumber;
            return false;
        }
        if (!parsePositiveInt(val, 1, maximum, *count)) {
            PcmLogLine(PcmLogLevel::Err) << key << " at line " << lineNumber
                                         << " must be an integer in 1.." << maximum;
            return false;
        }
    }
    if (!file.eof()) {
        PcmLogLine(PcmLogLevel::Err) << "failed to read PCM monitor config";
        return false;
    }
    for (OptionalSetting& setting : optional) {
        if (setting.line == 0U) {
            PcmLogLine(PcmLogLevel::Warning) << setting.key << " not set in [" << setting.section
                                             << "]; using the built-in default "
                                             << setting.fallback;
            *setting.target = setting.fallback;
        } else if (setting.invalid) {
            PcmLogLine(PcmLogLevel::Warning) << setting.key << " at line " << setting.line
                                             << " must appear once, as a positive whole number "
                                             << "of milliseconds; using the built-in default "
                                             << setting.fallback;
            *setting.target = setting.fallback;
        }
    }
    if (options.badTicksToInvalid == 0 || options.goodTicksToValid == 0) {
        PcmLogLine(PcmLogLevel::Err) << "[fault_aggregator] requires bad_ticks_to_invalid "
                                     << "and good_ticks_to_valid";
        return false;
    }
    if (!options.containerHealth && !options.dataValidation) {
        PcmLogLine(PcmLogLevel::Err) << "both monitors disabled in [monitors]; enable "
                                     << "container_health and/or data_validation";
        return false;
    }
    if (options.dataValidation && (options.badObjectsPercent == 0 || options.badFramesPercent == 0)) {
        PcmLogLine(PcmLogLevel::Err) << "enabled DVM requires bad_objects_percent and "
                                     << "bad_frames_percent in [data_validation]";
        return false;
    }
    return true;
}

static void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " [--mode LEARN|ACTIVE]"
              << " [--learn-duration <seconds>]"
              << " [--baseline-config <file>]"
              << " [--kafka-broker <host:port>]"
              << " [--event-mapping <file>] [--sdm-config <file> ...]\n";
}

static bool parseArgs(int argc, char** argv, PerceptionMonitorOptions& options) {
    options.mode = PcmRuntimeMode::Active;
    options.learnDurationSec = kDefaultLearnDurationSec;
    options.baselinePath = defaultBaselinePath();

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--mode") {
            if (++i >= argc) {
                PcmLogLine(PcmLogLevel::Err) << "--mode requires LEARN or ACTIVE";
                return false;
            }
            const std::string mode = lowerCopy(argv[i]);
            if (mode == "learn") {
                options.mode = PcmRuntimeMode::Learn;
            } else if (mode == "active") {
                options.mode = PcmRuntimeMode::Active;
            } else {
                PcmLogLine(PcmLogLevel::Err) << "invalid --mode '" << argv[i]
                                             << "' (expected LEARN or ACTIVE)";
                return false;
            }
        } else if (arg == "--learn-duration") {
            if (++i >= argc) {
                PcmLogLine(PcmLogLevel::Err) << "--learn-duration requires a value";
                return false;
            }
            int parsed = 0;
            if (!parsePositiveInt(argv[i], 1, 3600, parsed)) {
                PcmLogLine(PcmLogLevel::Err) << "invalid --learn-duration '" << argv[i]
                                             << "' (expected 1..3600 seconds)";
                return false;
            }
            options.learnDurationSec = parsed;
        } else if (arg == "--baseline-config") {
            if (++i >= argc) {
                PcmLogLine(PcmLogLevel::Err) << "--baseline-config requires a path";
                return false;
            }
            options.baselinePath = argv[i];
        } else if (arg == "--event-mapping" || arg == "--sdm-config") {
            if (++i >= argc || argv[i][0] == '\0') {
                PcmLogLine(PcmLogLevel::Err) << arg << " requires a path";
                return false;
            }
            if (arg == "--event-mapping") {
                if (!options.eventMappingPath.empty()) {
                    PcmLogLine(PcmLogLevel::Err) << "--event-mapping may be supplied only once";
                    return false;
                }
                options.eventMappingPath = argv[i];
            } else {
                options.sdmConfigPaths.emplace_back(argv[i]);
            }
        } else if (arg == "--kafka-broker") {
            if (++i >= argc) {
                PcmLogLine(PcmLogLevel::Err) << "--kafka-broker requires an address (host:port)";
                return false;
            }
            options.kafkaBroker = argv[i];
        } else {
            PcmLogLine(PcmLogLevel::Err) << "unknown argument '" << arg << "'";
            return false;
        }
    }

    if (options.baselinePath.empty()) {
        PcmLogLine(PcmLogLevel::Err) << "empty baseline path";
        return false;
    }
    return true;
}

static const char* modeName(PcmRuntimeMode mode) {
    return mode == PcmRuntimeMode::Learn ? "LEARN" : "ACTIVE";
}

} // namespace

PerceptionMonitor::PerceptionMonitor(uint32_t pssClientId,
                                     std::atomic<bool>& stopFlag,
                                     const PerceptionMonitorOptions& options)
    : pssClientId_(pssClientId)
    , stopFlag_(stopFlag)
    , options_(options)
{}

PerceptionMonitor::~PerceptionMonitor() {
    stop();
}

void* PerceptionMonitor::runPollLoopThunk(void* self) {
    static_cast<PerceptionMonitor*>(self)->runPollLoop();
    return nullptr;
}

bool PerceptionMonitor::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    completedOk_.store(false, std::memory_order_release);
    const int rc = pthread_create(&pollThread_, nullptr,
                                  &PerceptionMonitor::runPollLoopThunk, this);
    if (rc != 0) {
        running_.store(false);
        PcmLogLine(PcmLogLevel::Err) << "Failed to start polling thread: "
                                     << std::strerror(rc);
        return false;
    }
    pollThreadValid_ = true;
    PcmLogLine(PcmLogLevel::Info) << "PerceptionMonitor started (mode="
                                  << modeName(options_.mode) << ")";
    return true;
}

void PerceptionMonitor::stop() {
    running_.store(false, std::memory_order_release);
    if (pollThreadValid_) {
        pthread_join(pollThread_, nullptr);
        pollThreadValid_ = false;
    }
    PcmLogLine(PcmLogLevel::Info) << "PerceptionMonitor stopped";
}

bool PerceptionMonitor::isRunning() const {
    return running_.load(std::memory_order_acquire);
}

bool PerceptionMonitor::completedOk() const {
    return completedOk_.load(std::memory_order_acquire);
}

void PerceptionMonitor::runPollLoop() {
    /* LEARN is owned by the DVM ONLY. The container-health check learns nothing, so
     * LEARN runs just the DVM thread (no health checker, no aggregator, no PSS
     * reporting) to observe mdx-frames and write the DVM baseline (the sensor
     * whitelist). */
    if (options_.mode == PcmRuntimeMode::Learn) {
        const auto learnDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(options_.learnDurationSec);
        DataValidationMonitor dvm(DvmMode::Learn, options_.baselinePath, nullptr,
                                  std::unique_ptr<IFrameSource>(new KafkaFrameSource(
                                      options_.kafkaBroker, kDvmFramesTopic, kDvmConsumerGroup)),
                                  0, 0, 0, options_.decisionContract);
        if (!dvm.start()) {
            /* DVM thread failed to spawn -> no frames observed, no baseline.
             * Take the failure path: leave completedOk_ false and bail out. */
            PcmLogLine(PcmLogLevel::Err) << "data validation monitor start failed";
            running_.store(false, std::memory_order_release);
            return;
        }

        while (running_.load(std::memory_order_acquire) &&
               !stopFlag_.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < learnDeadline) {
            std::this_thread::sleep_for(kStopCheckInterval);
        }
        /* Only a LEARN that ran its full duration writes baselines: files from a
         * stopped, shorter observation would replace good learned ones. */
        const bool reachedDeadline = std::chrono::steady_clock::now() >= learnDeadline;

        dvm.stop();
        if (!reachedDeadline) {
            PcmLogLine(PcmLogLevel::Warning) << "LEARN stopped before its "
                                             << options_.learnDurationSec
                                             << " s duration; no baseline files written "
                                             << "(existing files are unchanged)";
            running_.store(false, std::memory_order_release);
            return; /* completedOk_ stays false: LEARN failed */
        }
        const bool dvmBaselineOk = dvm.writeBaseline();
        completedOk_.store(!stopFlag_.load(std::memory_order_relaxed) && dvmBaselineOk,
                           std::memory_order_release);
        running_.store(false, std::memory_order_release);
        return;
    }

    /* ACTIVE: the enabled checks feed the single shared aggregator, which owns the
     * 1 s evaluation, the combine, the hysteresis and the one AI_PIPELINE_* emit.
     * The [monitors] config selects which checks run (both on by default):
     *   - container health: THIS thread inspects the listed containers every ~1 s.
     *     Off when the allow-list has no valid entry. Disable it for a remote
     *     perception stack whose containers are not on this host's Docker.
     *   - the DVM: its own Kafka thread reports one verdict per ~1 s window.
     *     Off when no learned or default baseline exists.
     * A configuration or setup problem turns a check off with a printed cause and
     * never produces INVALID. With no check left PCM stays registered with PSS
     * (heartbeat) but sends no status. (An earlier log-content scanner was removed.) */
    const bool healthEnabled = options_.containerHealth;
    const bool dvmEnabled = options_.dataValidation;
    if (!healthEnabled && !dvmEnabled) {
        /* Switching both checks off is a configuration mistake; readMonitorConfig
         * already rejects it before PSS registration. */
        PcmLogLine(PcmLogLevel::Err) << "both monitors disabled in [monitors]; enable "
                                     << "container_health and/or data_validation";
        running_.store(false, std::memory_order_release);
        return;
    }

    /* Container health: on when enabled and the allow-list has a valid entry. */
    std::unique_ptr<DockerClient> dockerClient;
    std::unique_ptr<ContainerHealthChecker> healthChecker;
    if (healthEnabled) {
        dockerClient.reset(new DockerClient());
        healthChecker.reset(new ContainerHealthChecker(*dockerClient, options_.startupGraceMs,
                                                       options_.containerStartupGraceMs));
        if (healthChecker->containerCount() == 0U) healthChecker.reset();
    }
    const bool healthActive = (healthChecker != nullptr);

    /* The aggregator must know which checks report (an enabled check that stays
     * silent counts as stuck), but whether the DVM can run is known only once its
     * baseline has loaded, and the DVM reports into the aggregator. So build the
     * aggregator for the configured checks and, if the DVM has no baseline,
     * rebuild it without the DVM. Nothing has started yet. */
    PcmAggregatorSettings settings;
    settings.badTicksToInvalid = options_.badTicksToInvalid;
    settings.goodTicksToValid = options_.goodTicksToValid;
    settings.initialValidMs = options_.initialValidMs;
    settings.maxReportAgeMs = options_.maxReportAgeMs;
    settings.verdictResendMs = options_.verdictResendMs;
    settings.healthEnabled = healthActive;
    settings.dvmEnabled = dvmEnabled;
    std::unique_ptr<PcmFaultAggregator> aggregator(new PcmFaultAggregator(pssClientId_, settings));

    std::unique_ptr<DataValidationMonitor> dvm;
    if (dvmEnabled) {
        dvm.reset(new DataValidationMonitor(DvmMode::Active, options_.baselinePath,
                      aggregator.get(),
                      std::unique_ptr<IFrameSource>(new KafkaFrameSource(
                          options_.kafkaBroker, kDvmFramesTopic, kDvmConsumerGroup)),
                      options_.badObjectsPercent, options_.badFramesPercent,
                      options_.startupGraceMs, options_.decisionContract));
        if (!dvm->hasSensorBaseline()) {
            dvm.reset(); /* the DVM logged the cause; data validation is off */
            settings.dvmEnabled = false;
            aggregator.reset(new PcmFaultAggregator(pssClientId_, settings));
        }
    }
    const bool dvmActive = (dvm != nullptr);

    {
        PcmLogLine line(PcmLogLevel::Info);
        line << "checks: container_health=";
        if (healthActive) line << "on (" << healthChecker->containerCount() << " container(s) listed)";
        else if (healthEnabled) line << "off (no valid monitor_container entries)";
        else line << "off (disabled in [monitors])";
        line << " data_validation=";
        if (dvmActive) line << "on (" << dvm->knownSensorCount() << " baseline camera(s))";
        else if (dvmEnabled) line << "off (no baseline)";
        else line << "off (disabled in [monitors])";
    }
    if (!healthActive && !dvmActive) {
        PcmLogLine(PcmLogLevel::Warning) << "nothing is being checked (causes above); PCM "
                                         << "stays registered with PSS but sends no VALID/INVALID";
    }

    PcmFaultAggregator* const aggPtr = aggregator.get();
    if (!aggregator->start()) {
        /* Evaluator thread failed to spawn -> the AI_PIPELINE_INVALID/VALID emit
         * never runs. Do not poll blind to the safety function. */
        PcmLogLine(PcmLogLevel::Err) << "fault aggregator start failed";
        running_.store(false, std::memory_order_release);
        return;
    }
    if (dvmActive && !dvm->start()) {
        /* DVM thread failed to spawn -> data verdict never reported. */
        PcmLogLine(PcmLogLevel::Err) << "data validation monitor start failed";
        aggPtr->stop();
        running_.store(false, std::memory_order_release);
        return;
    }

    while (running_.load(std::memory_order_acquire) &&
           !stopFlag_.load(std::memory_order_relaxed)) {
        if (healthActive) {
            aggPtr->reportHealth(healthChecker->check());
        }

        /* ~1 s cadence; re-check the stop flag finely so shutdown is prompt. */
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(kHealthIntervalSec);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!running_.load(std::memory_order_acquire) ||
                stopFlag_.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(kStopCheckInterval);
        }
    }

    if (dvm) dvm->stop();
    aggPtr->stop();

    /* completedOk_ is meaningful only in LEARN (read by runLearnMode). ACTIVE
     * never reads it -- runActiveMode loops on the stop flag -- so it is set
     * unconditionally here and carries no success semantics for ACTIVE. */
    completedOk_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

/* Lifecycle ordering: start the heartbeat before the watcher (the daemon
 * gates trust reports on a recent heartbeat) and stop it before
 * terminating the PSS client (else the daemon sees a freed slot). */

/* Must be lock-free for async-signal-safe access from the handler. */
static std::atomic<bool> g_stopFlag{false};
static_assert(ATOMIC_BOOL_LOCK_FREE == 2,
              "g_stopFlag must be lock-free for async-signal-safe use");

static void signalHandler(int /*sig*/) {
    /* A lock-free atomic store is async-signal-safe on the target platform
     * (guaranteed by the ATOMIC_BOOL_LOCK_FREE static_assert above). */
    g_stopFlag.store(true, std::memory_order_relaxed);
}

static std::atomic<bool> s_heartbeatRunning{false};

/* pthread_create entry: @p arg points to the registered PSS client id. */
static void* heartbeatLoop(void* arg) {
    const uint32_t pssClientId = *static_cast<const uint32_t*>(arg);
    while (s_heartbeatRunning.load(std::memory_order_acquire) &&
           !g_stopFlag.load(std::memory_order_relaxed))
    {
        if (NvPSSSendHeartbeat(pssClientId, CLIENT_PERCEPTION_MONITOR) != NVPSSD_SUCCESS) {
            PcmLogLine(PcmLogLevel::Err) << "Heartbeat send failed";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(HB_INTERVAL_MS));
    }
    return nullptr;
}

static constexpr int kMaxPssRegisterRetries = 5;
static constexpr int kRegisterRetryDelaySec = 5;

static bool registerPSS(uint32_t& pssClientId) {
    for (int attempt = 0; attempt <= kMaxPssRegisterRetries; ++attempt) {
        if (NvPSSRegisterPSSClient(&pssClientId, CLIENT_PERCEPTION_MONITOR) == NVPSSD_SUCCESS) {
            PcmLogLine(PcmLogLevel::Info) << "Registered with PSS daemon as CLIENT_PERCEPTION_MONITOR, "
                                          << "clientId=" << pssClientId;
            return true;
        }
        if (attempt < kMaxPssRegisterRetries) {
            PcmLogLine(PcmLogLevel::Warning) << "PSS registration failed, retry " << (attempt + 1)
                                             << "/" << kMaxPssRegisterRetries
                                             << " in " << kRegisterRetryDelaySec << "s";
            std::this_thread::sleep_for(std::chrono::seconds(kRegisterRetryDelaySec));
        }
    }
    PcmLogLine(PcmLogLevel::Err) << "Failed to register with PSS daemon after "
                                 << (kMaxPssRegisterRetries + 1) << " attempts. Exiting.";
    return false;
}

static int runLearnMode(const PerceptionMonitorOptions& options) {
    PerceptionMonitor watcher(UINT32_MAX, g_stopFlag, options);
    if (!watcher.start()) {
        PcmLogLine(PcmLogLevel::Err) << "PerceptionMonitor::start() failed";
        return 1;
    }

    while (!g_stopFlag.load(std::memory_order_relaxed) && watcher.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    watcher.stop();
    return watcher.completedOk() ? 0 : 1;
}

static int runActiveMode(const PerceptionMonitorOptions& options) {
    uint32_t pssClientId = UINT32_MAX;
    if (!registerPSS(pssClientId)) {
        return 1;
    }

    s_heartbeatRunning.store(true, std::memory_order_release);
    pthread_t heartbeatThread{};
    if (pthread_create(&heartbeatThread, nullptr, heartbeatLoop, &pssClientId) != 0) {
        PcmLogLine(PcmLogLevel::Err) << "Failed to create heartbeat thread";
        s_heartbeatRunning.store(false, std::memory_order_release);
        if (NvPSSTerminatePSSClient(pssClientId) != NVPSSD_SUCCESS) {
            PcmLogLine(PcmLogLevel::Err) << "NvPSSTerminatePSSClient failed for clientId="
                                         << pssClientId;
        }
        return 1;
    }

    int exitCode = 0;
    {
        PerceptionMonitor watcher(pssClientId, g_stopFlag, options);
        if (!watcher.start()) {
            PcmLogLine(PcmLogLevel::Err) << "PerceptionMonitor::start() failed";
            exitCode = 1;
        } else {
            /* Exit if the monitor thread dies on its own (e.g. a layer failed to
             * start / stopped), not just on a shutdown signal -- same guard as LEARN. */
            while (!g_stopFlag.load(std::memory_order_relaxed) && watcher.isRunning()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            /* Monitor thread stopped without a shutdown signal: fail closed. Exiting
             * non-zero stops the heartbeat so the daemon's disconnect path raises
             * SW_FAIL -> safe state, rather than staying "up" but blind. */
            if (!g_stopFlag.load(std::memory_order_relaxed) && !watcher.isRunning()) {
                PcmLogLine(PcmLogLevel::Err) << "monitor thread stopped unexpectedly; "
                                             << "exiting to trigger safe state";
                exitCode = 1;
            }
        }
        watcher.stop();
    }

    s_heartbeatRunning.store(false, std::memory_order_release);
    pthread_join(heartbeatThread, nullptr);

    if (pssClientId != UINT32_MAX) {
        if (NvPSSTerminatePSSClient(pssClientId) != NVPSSD_SUCCESS) {
            PcmLogLine(PcmLogLevel::Err) << "NvPSSTerminatePSSClient failed for clientId="
                                         << pssClientId;
        } else {
            PcmLogLine(PcmLogLevel::Info) << "Terminated PSS client registration";
        }
    }
    return exitCode;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    PerceptionMonitorOptions options;
    if (!parseArgs(argc, argv, options)) {
        printUsage(argv[0]);
        return 2;
    }
    if (options.mode == PcmRuntimeMode::Active && !readMonitorConfig(options)) {
        return 2;
    }
    if (!loadDvmDecisionContract(options.eventMappingPath, options.sdmConfigPaths,
                                 options.decisionContract)) {
        return 2;
    }

    PcmLogLine(PcmLogLevel::Info) << "nv_perception_monitor starting"
                                  << " mode=" << modeName(options.mode)
                                  << " baseline=" << options.baselinePath;
    if (options.mode == PcmRuntimeMode::Active) {
        PcmLogLine(PcmLogLevel::Info) << "settings: bad_ticks_to_invalid=" << options.badTicksToInvalid
                                      << " good_ticks_to_valid=" << options.goodTicksToValid
                                      << " initial_valid_ms=" << options.initialValidMs
                                      << " max_report_age_ms=" << options.maxReportAgeMs
                                      << " bad_objects_percent=" << options.badObjectsPercent
                                      << " bad_frames_percent=" << options.badFramesPercent
                                      << " startup_grace_ms=" << options.startupGraceMs;
    }

    const int exitCode = options.mode == PcmRuntimeMode::Learn
                             ? runLearnMode(options)
                             : runActiveMode(options);

    PcmLogLine(PcmLogLevel::Info) << "nv_perception_monitor exit (" << exitCode << ")";
    return exitCode;
}
