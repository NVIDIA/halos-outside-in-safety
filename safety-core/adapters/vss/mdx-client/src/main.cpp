/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Config-driven MDX client. Usage:
 *   mdx_client -c|--config <event_mapping_config.pb> [-b|--broker <brokers>] [-d|--debug]
 * The config file can be protobuf binary or text format.
 * -c, --config  Path to event mapping config (required).
 * -b, --broker  Message broker(s), e.g. localhost:9092. Default: localhost:9092 or MDX_MSGBUS_BROKERS.
 * -d, --debug   Print matched events to stdout instead of reporting to PSS.
 * -i, --instance-id  PSS instance id 1..8 (see printUsage).
 */

#include <csignal>
#include <cstdlib>
#include <string>
#include <iostream>
#include <cstring>
#include <cerrno>
#include "common.hpp"
#include "MDXClient.hpp"
#include "pss_daemon.h"

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [-d|--debug] -c|--config <config.pb>\n"
              << "       (-s|--sensor-config <sensor_config.conf>\n"
              << "        | -g|--sensor-pipelines-config <sensor_pipelines_config.conf>)\n"
              << "       [-b|--broker <brokers>] [-i|--instance-id <1..8>]\n\n"
              << "Config-driven MDX client.\n\n"
              << "Options:\n"
              << "  -c, --config <PATH>          Path to event mapping config (required).\n"
              << "                               Protobuf binary or text format.\n"
              << "  -s, --sensor-config <PATH>   2D deployments. CSV: pipelineId, sensorName,\n"
              << "                               rtspUrl. The cameras are the event sources.\n"
              << "  -g, --sensor-pipelines-config <PATH>\n"
              << "                               3D/BEV deployments. CSV: pipelineId, sensorName.\n"
              << "                               Names the BEV group, NOT the cameras.\n"
              << "                               EXACTLY ONE of -s and -g is required. Supplying\n"
              << "                               both is refused: holding two identity maps would\n"
              << "                               let a stray per-camera event in a 3D deployment\n"
              << "                               resolve and be gated against one camera's health\n"
              << "                               instead of the group's.\n"
              << "  -b, --broker <ADDR>          Message broker(s) (default: localhost:9092 or\n"
              << "                               MDX_MSGBUS_BROKERS env var).\n"
              << "  -i, --instance-id <N>        PSS instance id, 1.." << PSS_MDX_INSTANCE_MAX << ". Makes this\n"
              << "                               client identifiable: if it stops, every SDM\n"
              << "                               latches until THIS instance is back, and\n"
              << "                               nvpss.conf required_mdx_instances can require\n"
              << "                               it. Omitted = anonymous: PSS cannot report\n"
              << "                               this client's loss. The launchers pass 1.\n"
              << "  -d, --debug                  Print matched events to stdout instead of\n"
              << "                               reporting to PSS.\n"
              << "  -h, --help                   Show this help message.\n\n"
              << "Positional arguments are accepted for backward compatibility:\n"
              << "  first positional  = config path (if -c not given)\n"
              << "  second positional = broker address (if -b not given)\n";
}

int main(int argc, char* argv[]) {
    if (argv == nullptr) return EXIT_FAILURE;

    struct sigaction sa = {};
    sa.sa_handler = sig_segv_handler;
    if (sigemptyset(&sa.sa_mask) == -1) {
        std::cerr << "Failed to init signal mask: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }
    if (sigaction(SIGSEGV, &sa, nullptr) == -1) {
        std::cerr << "Failed to set SIGSEGV handler: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    const char* prog = (argc > 0 && argv[0] != nullptr) ? argv[0] : "mdx_client";
    bool debugMode = false;
    std::string configPath;
    std::string sensorConfigPath;
    std::string sensorPipelinesPath;
    std::string brokerOverride;
    uint8_t instanceId = 0U;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(prog);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debugMode = true;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--broker") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: -b/--broker requires a value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            ++i;
            if (argv[i][0] == '\0' || argv[i][0] == '-') {
                std::cerr << "error: -b/--broker: invalid value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            brokerOverride = argv[i];
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: -c/--config requires a value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            ++i;
            if (argv[i][0] == '\0' || argv[i][0] == '-') {
                std::cerr << "error: -c/--config: invalid value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            configPath = argv[i];
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sensor-config") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: -s/--sensor-config requires a value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            ++i;
            if (argv[i][0] == '\0' || argv[i][0] == '-') {
                std::cerr << "error: -s/--sensor-config: invalid value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            sensorConfigPath = argv[i];
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--instance-id") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: -i/--instance-id requires a value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            ++i;
            char* end = nullptr;
            errno = 0;
            const unsigned long v = std::strtoul(argv[i], &end, 10);
            if (argv[i][0] == '\0' || argv[i][0] == '-' || end == nullptr || *end != '\0' ||
                errno == ERANGE || v < 1UL || v > PSS_MDX_INSTANCE_MAX) {
                std::cerr << "error: -i/--instance-id: expected an integer 1.."
                          << PSS_MDX_INSTANCE_MAX << "\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            instanceId = static_cast<uint8_t>(v);
        } else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--sensor-pipelines-config") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: -g/--sensor-pipelines-config requires a value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            ++i;
            if (argv[i][0] == '\0' || argv[i][0] == '-') {
                std::cerr << "error: -g/--sensor-pipelines-config: invalid value\n";
                printUsage(prog);
                return EXIT_FAILURE;
            }
            sensorPipelinesPath = argv[i];
        } else if (argv[i][0] == '-') {
            std::cerr << "error: unknown option (see --help)\n";
            printUsage(prog);
            return EXIT_FAILURE;
        } else if (configPath.empty()) {
            configPath = argv[i];
        } else if (brokerOverride.empty()) {
            brokerOverride = argv[i];
        } else {
            std::cerr << "error: unexpected positional argument (see --help)\n";
            printUsage(prog);
            return EXIT_FAILURE;
        }
    }

    if (configPath.empty()) {
        std::cerr << "error: config path is required (use -c or positional argument)\n";
        printUsage(prog);
        return EXIT_FAILURE;
    }
    /*
     * Exactly one identity file. Both or neither is fatal, and neither case is
     * given a default.
     *
     * BOTH is refused because two maps would resolve two topologies at once. In
     * a 3D deployment a per-camera Kafka event would then find a pipelineID and
     * be reported as a decision-driving identity, gated against that single
     * camera's health rather than the group's -- accepted silently, and wrong in
     * the direction that matters. With one map a topology mismatch is
     * unresolvable, and an unresolvable event is dropped.
     *
     * NEITHER is refused because falling back to "no map" would mean every event
     * is dropped, which looks identical to a quiet deployment.
     */
    const bool haveCameras   = !sensorConfigPath.empty();
    const bool havePipelines = !sensorPipelinesPath.empty();
    if (haveCameras && havePipelines) {
        std::cerr << "error: -s/--sensor-config and -g/--sensor-pipelines-config are mutually "
                     "exclusive; supply exactly one.\n"
                     "       -s is the 2D form (the cameras are the event sources); -g is the\n"
                     "       3D/BEV form (only the group is). A deployment is one or the other.\n";
        printUsage(prog);
        return EXIT_FAILURE;
    }
    if (!haveCameras && !havePipelines) {
        std::cerr << "error: a sensor identity file is required: -s/--sensor-config for 2D, or\n"
                     "       -g/--sensor-pipelines-config for 3D/BEV.\n";
        printUsage(prog);
        return EXIT_FAILURE;
    }
    if (brokerOverride.empty()) {
        const char* env = std::getenv("MDX_MSGBUS_BROKERS");
        if (env && env[0] != '\0')
            brokerOverride = env;
    }

    const std::string identityPath = haveCameras ? sensorConfigPath : sensorPipelinesPath;
    const bool identityIsPipelines = havePipelines;

    std::cout << "Starting MDX Client with config: " << configPath
              << (identityIsPipelines ? ", sensor-pipelines-config: " : ", sensor-config: ")
              << identityPath
              << (debugMode ? " (debug mode)" : "")
              << (brokerOverride.empty() ? "" : ", brokers: " + brokerOverride)
              << ", PSS instance: "
              << (instanceId == 0U ? std::string("anonymous") : std::to_string(instanceId))
              << std::endl;
    int ret = launchMDXClient(configPath, identityPath, identityIsPipelines,
                              debugMode, brokerOverride, instanceId);
    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
