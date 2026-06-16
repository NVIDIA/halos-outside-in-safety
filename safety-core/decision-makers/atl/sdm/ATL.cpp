/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <limits>
#include <string>
#include "ATLControl.h"

/* Must match launchATLControlAlgo validation in ATLControl.cpp */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [OPTIONS]\n\n"
              << "ATL V2 Safety Decision Maker.\n\n"
              << "Options:\n"
              << "  --gateway_ip <IP>            PSD Gateway IP (default: 127.0.0.1).\n"
              << "  --gateway_port <PORT>        PSD Gateway port, 1-65535 (default: 50000).\n"
              << "  --cmd_rx_ip <IP>             Command receiver IP (default: 127.0.0.1).\n"
              << "  --cmd_rx_port <PORT>         Command receiver port, 1-65535 (default: 12345).\n"
              << "  --max_hb_failures <N>        Heartbeat miss limit, 1-255 (default: 10).\n"
              << "  --decision_interval_ms <MS>  PLC repeat period ms; 0=off, else "
              << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax
              << " (default: 5000).\n"
              << "  -h, --help                   Show this help message.\n";
}

static bool requireOptionValue(int argc, int i, const char* optName, const char* prog)
{
    if (i + 1 >= argc)
    {
        std::cerr << "error: " << optName << " requires a value\n";
        printUsage(prog);
        return false;
    }
    return true;
}

static bool parsePort(const char* arg, std::uint16_t& out, const char* name, const char* prog)
{
    if (!arg || arg[0] == '\0')
    {
        std::cerr << "Invalid " << name << " value: (empty)\n";
        printUsage(prog);
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long val = std::strtol(arg, &end, 10);
    if (errno == ERANGE || end == arg || *end != '\0')
    {
        std::cerr << "Invalid " << name << " value: not a valid decimal port string\n";
        printUsage(prog);
        return false;
    }
    if (val < 1 || val > 65535)
    {
        std::cerr << "Invalid " << name << ": port must be between 1 and 65535\n";
        printUsage(prog);
        return false;
    }
    out = static_cast<std::uint16_t>(val);
    return true;
}

int main(int argc, char* argv[])
{
    std::string    gatewayIP   = "127.0.0.1";
    std::uint16_t  gatewayPort = 50000;
    std::string    plcIP       = "127.0.0.1";
    std::uint16_t  plcPort     = 12345;
    std::uint8_t   maxHbFailures = 10U;
    std::uint32_t  decisionIntervalMs = 5000U;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (strcmp(arg, "--gateway_ip") == 0)
        {
            if (!requireOptionValue(argc, i, "--gateway_ip", argv[0]))
                return 1;
            gatewayIP = argv[++i];
        }
        else if (strcmp(arg, "--gateway_port") == 0)
        {
            if (!requireOptionValue(argc, i, "--gateway_port", argv[0]))
                return 1;
            if (!parsePort(argv[++i], gatewayPort, "--gateway_port", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--cmd_rx_ip") == 0)
        {
            if (!requireOptionValue(argc, i, "--cmd_rx_ip", argv[0]))
                return 1;
            plcIP = argv[++i];
        }
        else if (strcmp(arg, "--cmd_rx_port") == 0)
        {
            if (!requireOptionValue(argc, i, "--cmd_rx_port", argv[0]))
                return 1;
            if (!parsePort(argv[++i], plcPort, "--cmd_rx_port", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--max_hb_failures") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --max_hb_failures requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || end == argv[i] || *end != '\0' || v < 1UL || v > 255UL)
            {
                std::cerr << "error: max_hb_failures must be 1..255\n";
                printUsage(argv[0]);
                return 1;
            }
            maxHbFailures = static_cast<std::uint8_t>(v);
        }
        else if (strcmp(arg, "--decision_interval_ms") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --decision_interval_ms requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            const char* num = argv[++i];
            char* end = nullptr;
            errno = 0;
            const unsigned long raw = std::strtoul(num, &end, 10);
            if (errno == ERANGE || end == num || *end != '\0')
            {
                std::cerr << "error: decision_interval_ms: invalid integer\n";
                printUsage(argv[0]);
                return 1;
            }
            if (raw > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max()))
            {
                std::cerr << "error: decision_interval_ms must be 0 or "
                          << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax << "\n";
                printUsage(argv[0]);
                return 1;
            }
            const std::uint32_t v = static_cast<std::uint32_t>(raw);
            if (v > kDecisionRepeatIntervalMsMax
                || (v != 0U && v < kDecisionRepeatIntervalMsMinNonZero))
            {
                std::cerr << "error: decision_interval_ms must be 0 or "
                          << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax << "\n";
                printUsage(argv[0]);
                return 1;
            }
            decisionIntervalMs = v;
        }
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg[0] == '-')
        {
            std::cerr << "error: unknown option (see --help)\n";
            printUsage(argv[0]);
            return 1;
        }
        else
        {
            std::cerr << "error: unexpected positional argument (see --help)\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    /* Blocks until shutdown; non-zero if initialization failed. */
    return launchATLControlAlgo(gatewayIP, gatewayPort, plcIP, plcPort, maxHbFailures, decisionIntervalMs);
}
