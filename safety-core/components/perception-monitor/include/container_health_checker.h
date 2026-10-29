/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_CONTAINER_HEALTH_CHECKER_H
#define PCM_CONTAINER_HEALTH_CHECKER_H

#include "docker_client.h"
#include "pcm_fault_aggregator.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

/*
 * Container-health check: inspects each allow-listed container by name over the
 * Docker Engine API and turns Docker lifecycle / HEALTHCHECK state into one
 * verdict per poll. Perception-data validation is handled by the DVM.
 *
 * Only containers that have been seen in this run are judged. A listed container
 * that has never existed on this host is skipped (and reported after the startup
 * grace); once seen, its disappearance is a fault. A Docker socket or API problem
 * gives no verdict (Off), never a fault.
 */
class ContainerHealthChecker
{
public:
    struct ContainerSnapshot
    {
        std::string name;
        DockerClient::ContainerHealth health;
        DockerClient::Result inspectResult = DockerClient::Result::CommandError;
    };

    /* Loads the [health_monitor] allow-list from pcm_monitor.cfg; invalid lines
     * are skipped with a warning. @p startupGraceMs: how long listed containers
     * may stay unseen before PCM reports them. @p containerStartupGraceMs:
     * allowance from first observed HEALTHCHECK starting until it is bad. */
    ContainerHealthChecker(DockerClient& client, int startupGraceMs,
                           int containerStartupGraceMs = 30000);

    /* Number of valid monitor_container entries loaded; zero turns the check off. */
    std::size_t containerCount() const { return containers_.size(); }

    /*
     * Inspect every listed container once:
     *   Bad  - a seen required producer is stopped (any exit code), paused,
     *          restarting, dead, unhealthy, gone, or starting past its allowance
     *   Good - every seen container is healthy and none is awaiting startup
     *   Off  - Docker is unreachable or returned an API error, or no listed
     *          container has been seen yet, or a container is still starting
     *          within its allowance and no other container is bad
     * Logs each change of cause (Docker state, container faults, appearances).
     */
    PcmCheckVerdict check(std::chrono::steady_clock::time_point now =
                             std::chrono::steady_clock::now());

    /* Immediate lifecycle faults; the per-container starting deadline is in check(). */
    static bool isLifecycleFault(const ContainerSnapshot& snapshot);

private:
    enum class DockerState { Unknown, Ok, SocketUnreachable, ApiError };

    struct MonitoredContainer
    {
        std::string name;
        bool        seen = false;    /* present at least once in this run */
        bool        faulted = false; /* last judged faulty (for change logging) */
        bool        starting = false;
        std::chrono::steady_clock::time_point startingAt{};
    };

    void setDockerState(DockerState state);
    void reportNeverSeenAfterGrace();

    DockerClient&                               client_;
    std::vector<MonitoredContainer>             containers_;
    const std::chrono::steady_clock::time_point createdAt_;
    const int                                   startupGraceMs_;
    const int                                   containerStartupGraceMs_;
    bool                                        graceReported_ = false;
    DockerState                                 dockerState_ = DockerState::Unknown;
};

#endif /* PCM_CONTAINER_HEALTH_CHECKER_H */
