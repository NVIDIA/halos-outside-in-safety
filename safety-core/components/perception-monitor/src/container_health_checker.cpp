/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "container_health_checker.h"

#include "pcm_log.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

static constexpr const char* kDefaultConfigPath = "/opt/nvidia/psf/bin/pcm_monitor.cfg";
static constexpr const char* kConfigEnvVar      = "PCM_MONITOR_CONFIG";

enum class ConfigSection
{
    None,
    HealthMonitor,
    Other
};

static std::string trim(const std::string& in) {
    const std::size_t first = in.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) return std::string();
    const std::size_t last = in.find_last_not_of(" \t\r\n\f\v");
    return in.substr(first, last - first + 1U);
}

static std::string configPath() {
    const char* envPath = std::getenv(kConfigEnvVar);
    if (envPath != nullptr && envPath[0] != '\0') return std::string(envPath);
    return std::string(kDefaultConfigPath);
}

/* Read the [health_monitor] allow-list. A bad line (no '=', unknown key, empty or
 * invalid container name, duplicate) is skipped with a warning; the valid entries
 * are still monitored. */
static std::vector<std::string> loadMonitoredContainers() {
    std::vector<std::string> monitored;
    const std::string path = configPath();
    std::ifstream file(path.c_str());
    if (!file.is_open()) {
        PcmLogLine(PcmLogLevel::Warning) << "container-health config unavailable: " << path;
        return monitored;
    }

    std::unordered_set<std::string> names;
    ConfigSection section = ConfigSection::None;
    std::string line;
    std::size_t lineNumber = 0U;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string sectionName = trim(trimmed.substr(1U, trimmed.size() - 2U));
            section = (sectionName == "health_monitor") ? ConfigSection::HealthMonitor
                                                         : ConfigSection::Other;
            continue;
        }

        if (section != ConfigSection::HealthMonitor) continue;

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            PcmLogLine(PcmLogLevel::Warning) << path << " line " << lineNumber
                                             << ": [health_monitor] line without '='; skipped";
            continue;
        }
        const std::string key = trim(trimmed.substr(0U, eq));
        const std::string value = trim(trimmed.substr(eq + 1U));
        /* Parsed and validated by readMonitorConfig before ACTIVE startup. */
        if (key == "container_startup_grace_ms") continue;
        if (key != "monitor_container") {
            PcmLogLine(PcmLogLevel::Warning) << path << " line " << lineNumber
                                             << ": unknown [health_monitor] key '" << key
                                             << "'; skipped";
        } else if (value.empty()) {
            PcmLogLine(PcmLogLevel::Warning) << path << " line " << lineNumber
                                             << ": empty monitor_container; skipped";
        } else if (!DockerClient::isValidContainerName(value)) {
            /* Not echoed: an invalid name may hold control characters. */
            PcmLogLine(PcmLogLevel::Warning) << path << " line " << lineNumber
                                             << ": invalid container name "
                                             << "(allowed: A-Z a-z 0-9 . _ -, max 256); skipped";
        } else if (!names.insert(value).second) {
            PcmLogLine(PcmLogLevel::Warning) << path << " line " << lineNumber
                                             << ": duplicate monitor_container '" << value
                                             << "'; skipped";
        } else {
            monitored.push_back(value);
        }
    }

    if (monitored.empty()) {
        PcmLogLine(PcmLogLevel::Warning) << "no valid monitor_container entries in " << path;
    } else {
        PcmLogLine(PcmLogLevel::Info) << "container-health config " << path
                                      << ": " << monitored.size() << " container(s) listed";
    }
    return monitored;
}

/* Cause text for a present container that has a lifecycle fault. */
static std::string describeLifecycleFault(const DockerClient::ContainerHealth& health) {
    if (health.paused || health.status == "paused") return "is paused";
    if (health.restarting || health.status == "restarting") return "is restarting";
    if (health.dead || health.status == "dead") return "is dead";
    if (!health.running) {
        std::string text = "not running (status=" +
                           (health.status.empty() ? std::string("unknown") : health.status);
        if (health.hasExitCode) text += ", exit code " + std::to_string(health.exitCode);
        return text + ")";
    }
    return "unhealthy (Docker HEALTHCHECK)";
}

} // namespace

ContainerHealthChecker::ContainerHealthChecker(DockerClient& client, int startupGraceMs,
                                               int containerStartupGraceMs)
    : client_(client)
    , createdAt_(std::chrono::steady_clock::now())
    , startupGraceMs_(startupGraceMs)
    , containerStartupGraceMs_(containerStartupGraceMs)
{
    for (const std::string& name : loadMonitoredContainers()) {
        MonitoredContainer container;
        container.name = name;
        containers_.push_back(container);
    }
}

void ContainerHealthChecker::setDockerState(DockerState state) {
    if (state == dockerState_) return;
    if (state == DockerState::SocketUnreachable) {
        PcmLogLine(PcmLogLevel::Warning) << "Docker socket unreachable; container health is "
                                         << "NOT checked until Docker answers again";
    } else if (state == DockerState::ApiError) {
        PcmLogLine(PcmLogLevel::Warning) << "Docker API error; container health is "
                                         << "NOT checked until Docker answers again";
    } else if (state == DockerState::Ok && dockerState_ != DockerState::Unknown) {
        PcmLogLine(PcmLogLevel::Info) << "Docker answering again; container health check resumed";
    }
    dockerState_ = state;
}

void ContainerHealthChecker::reportNeverSeenAfterGrace() {
    if (graceReported_) return;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - createdAt_).count();
    if (elapsedMs < static_cast<std::int64_t>(startupGraceMs_)) return;
    graceReported_ = true;

    std::string missing;
    std::size_t count = 0U;
    for (const MonitoredContainer& container : containers_) {
        if (container.seen) continue;
        missing += (count == 0U ? "" : ", ") + container.name;
        ++count;
    }
    if (count > 0U) {
        PcmLogLine(PcmLogLevel::Warning) << "not checking " << count
                                         << " listed container(s) that are not present: "
                                         << missing << " (checked once they appear)";
    }
}

PcmCheckVerdict ContainerHealthChecker::check(std::chrono::steady_clock::time_point now) {
    if (containers_.empty()) return PcmCheckVerdict::Off;

    /* Inspect every listed container. A socket or API failure leaves health
     * unknown for this poll: no verdict, never a fault. */
    std::vector<ContainerSnapshot> snapshots;
    snapshots.reserve(containers_.size());
    for (const MonitoredContainer& container : containers_) {
        ContainerSnapshot snapshot;
        snapshot.name = container.name;
        snapshot.inspectResult = client_.inspectContainer(container.name, snapshot.health);
        if (snapshot.inspectResult == DockerClient::Result::SocketUnreachable) {
            setDockerState(DockerState::SocketUnreachable);
            return PcmCheckVerdict::Off;
        }
        if (snapshot.inspectResult == DockerClient::Result::CommandError) {
            setDockerState(DockerState::ApiError);
            return PcmCheckVerdict::Off;
        }
        snapshots.push_back(std::move(snapshot));
    }
    setDockerState(DockerState::Ok);

    bool anySeen = false;
    bool anyBad = false;
    bool anyStarting = false;
    for (std::size_t i = 0U; i < containers_.size(); ++i) {
        MonitoredContainer& container = containers_[i];
        const ContainerSnapshot& snapshot = snapshots[i];
        const bool present = (snapshot.inspectResult == DockerClient::Result::Ok);
        if (!container.seen) {
            if (!present) continue; /* never seen in this run: not judged */
            container.seen = true;
            if (graceReported_) {
                PcmLogLine(PcmLogLevel::Info) << "container '" << container.name
                                              << "' is now present; checking it";
            }
        }
        anySeen = true;

        const bool starting = present && !isLifecycleFault(snapshot) &&
                              snapshot.health.health == "starting";
        if (starting && !container.starting) {
            container.startingAt = now;
            PcmLogLine(PcmLogLevel::Warning) << "container '" << container.name
                << "' health is starting; awaiting healthy for up to "
                << containerStartupGraceMs_ << " ms";
        }
        container.starting = starting;

        std::string fault;
        if (!present) {
            fault = "is gone (seen earlier in this run)";
        } else if (isLifecycleFault(snapshot)) {
            fault = describeLifecycleFault(snapshot.health);
        } else if (starting) {
            if (container.faulted) {
                /* A restart cannot turn an existing fault into a good report. */
                fault = "is still starting; awaiting healthy after a fault";
            } else if (now - container.startingAt >=
                       std::chrono::milliseconds(containerStartupGraceMs_)) {
                fault = "health is still starting after the startup grace";
            } else {
                anyStarting = true;
                continue; /* starting is unjudged, never reported as healthy */
            }
        }
        if (!fault.empty()) {
            anyBad = true;
            if (!container.faulted) {
                container.faulted = true;
                PcmLogLine(PcmLogLevel::Warning) << "container '" << container.name
                                                 << "' " << fault;
            }
        } else if (container.faulted) {
            container.faulted = false;
            PcmLogLine(PcmLogLevel::Info) << "container '" << container.name
                                          << "' is healthy again";
        }
    }

    reportNeverSeenAfterGrace();

    if (anyBad) return PcmCheckVerdict::Bad;
    if (!anySeen || anyStarting) return PcmCheckVerdict::Off;
    return PcmCheckVerdict::Good;
}

bool ContainerHealthChecker::isLifecycleFault(const ContainerSnapshot& snapshot) {
    if (snapshot.inspectResult != DockerClient::Result::Ok) return true; /* incl. NotFound = absent */
    /* The allow-list contains required continuous producers, never one-shot jobs. */
    if (snapshot.health.paused || snapshot.health.restarting || snapshot.health.dead ||
        snapshot.health.status == "paused" || snapshot.health.status == "restarting" ||
        snapshot.health.status == "dead") return true;
    if (!snapshot.health.running) return true;
    if (snapshot.health.health == "unhealthy") return true;
    return false;
}
