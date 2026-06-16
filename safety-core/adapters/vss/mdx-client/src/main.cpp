/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <csignal>
#include <cstdlib>
#include <string>
#include <iostream>
#include <cstring>
#include <cerrno>
#include "common.hpp"
#include "MDXClient.hpp"

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [-d|--debug] -c|--config <config.pb> -s|--sensor-config <sensor_config.conf>\n"
              << "       [-b|--broker <brokers>]\n\n"
              << "Config-driven MDX client.\n\n"
              << "Options:\n"
              << "  -c, --config <PATH>          Path to event mapping config (required).\n"
              << "                               Protobuf binary or text format.\n"
              << "  -s, --sensor-config <PATH>   Path to sensor config file (required).\n"
              << "                               CSV: pipelineId, sensorName, rtspUrl\n"
              << "  -b, --broker <ADDR>          Message broker(s) (default: localhost:9092 or\n"
              << "                               MDX_MSGBUS_BROKERS env var).\n"
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
    std::string brokerOverride;

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
    if (sensorConfigPath.empty()) {
        std::cerr << "error: sensor config path is required (use -s/--sensor-config)\n";
        printUsage(prog);
        return EXIT_FAILURE;
    }
    if (brokerOverride.empty()) {
        const char* env = std::getenv("MDX_MSGBUS_BROKERS");
        if (env && env[0] != '\0')
            brokerOverride = env;
    }

    std::cout << "Starting MDX Client with config: " << configPath
              << ", sensor-config: " << sensorConfigPath
              << (debugMode ? " (debug mode)" : "")
              << (brokerOverride.empty() ? "" : ", brokers: " + brokerOverride) << std::endl;
    int ret = launchMDXClient(configPath, sensorConfigPath, debugMode, brokerOverride);
    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
