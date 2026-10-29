/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_DOCKER_CLIENT_H
#define PCM_DOCKER_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*
 * Talks to the Docker Engine API over /var/run/docker.sock (no `docker`
 * CLI). Each call opens a fresh short-lived connection. Not thread-safe;
 * PCM uses one DockerClient per polling thread.
 */
class DockerClient
{
public:
    enum class Result
    {
        Ok,                 /* HTTP 200; out populated */
        SocketUnreachable,  /* connect to docker.sock failed */
        NotFound,           /* HTTP 404 */
        CommandError,       /* timeout, malformed response, validation failure */
    };

    struct ContainerInfo
    {
        std::string id;
        std::string name;   /* .Names[0], leading '/' stripped */
        std::string image;
        std::string state;
    };

    struct ContainerHealth
    {
        bool running = false;        /* .State.Running */
        bool paused = false;         /* Running may still be true while paused */
        bool restarting = false;
        bool dead = false;
        std::string status;          /* .State.Status */
        bool hasExitCode = false;    /* .State.ExitCode was parsed */
        int exitCode = -1;           /* .State.ExitCode */
        std::string health;          /* .State.Health.Status; empty when no HEALTHCHECK */
        std::string image;           /* .Config.Image */
        std::string composeService;  /* com.docker.compose.service label, when present */
    };

    DockerClient();

    DockerClient(const DockerClient&) = delete;
    DockerClient& operator=(const DockerClient&) = delete;

    Result listContainers(std::vector<ContainerInfo>& out);

    /* @p name is validated against the Docker name/id charset before use. */
    Result inspectContainer(const std::string& name, ContainerHealth& out);

    /* True if @p name passes the same charset/length check inspectContainer
     * applies, so a configured name can be rejected when it is loaded. */
    static bool isValidContainerName(const std::string& name);
};

#endif /* PCM_DOCKER_CLIENT_H */
