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
#include "ProximityControl.h"

/* Must match launchProximityControlAlgo validation in ProximityControl.cpp */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;

int main(int argc, char* argv[])
{
    std::string  gatewayIP   = "127.0.0.1";   // NvPSD Gateway address
    unsigned int gatewayPort = 50000;         // NvPSD Gateway port
    std::string  plcIP       = "127.0.0.1";   // PLC destination
    unsigned int plcPort     = 12345;
    std::uint8_t  maxHbFailures = 10U;
    /* PLC decision-repeat period. 5000 ms is a conservative default: long
     * enough to avoid flooding the PLC and the audit log when a decision is
     * held, short enough to recover within one typical ACK window if the
     * previous send was lost. Override with --decision_interval_ms;
     * 0 disables the periodic repeat entirely. */
    std::uint32_t decisionIntervalMs = 5000U;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--gateway_ip") == 0 && i + 1 < argc)
            gatewayIP = argv[++i];
        else if (strcmp(argv[i], "--gateway_port") == 0 && i + 1 < argc)
        {
            char* end = nullptr;
            errno = 0;
            long p = std::strtol(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || p <= 0 || p > 65535)
            {
                std::cerr << "gateway_port: invalid number (use 1..65535)" << std::endl;
                return 1;
            }
            gatewayPort = static_cast<unsigned int>(p);
        }
        else if (strcmp(argv[i], "--cmd_rx_ip") == 0 && i + 1 < argc)
            plcIP = argv[++i];
        else if (strcmp(argv[i], "--cmd_rx_port") == 0 && i + 1 < argc)
        {
            char* end = nullptr;
            errno = 0;
            long p = std::strtol(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || p <= 0 || p > 65535)
            {
                std::cerr << "cmd_rx_port: invalid number (use 1..65535)" << std::endl;
                return 1;
            }
            plcPort = static_cast<unsigned int>(p);
        }
        else if (strcmp(argv[i], "--max_hb_failures") == 0 && i + 1 < argc)
        {
            char* end = nullptr;
            errno = 0;
            unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || *end != '\0' || v < 1UL || v > 255UL)
            {
                std::cerr << "max_hb_failures: use 1..255" << std::endl;
                return 1;
            }
            maxHbFailures = static_cast<std::uint8_t>(v);
        }
        else if (strcmp(argv[i], "--decision_interval_ms") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --decision_interval_ms requires a value" << std::endl;
                return 1;
            }
            const char* num = argv[++i];
            char* end = nullptr;
            errno = 0;
            const unsigned long raw = std::strtoul(num, &end, 10);
            if (errno == ERANGE || end == num || *end != '\0')
            {
                std::cerr << "decision_interval_ms: invalid integer" << std::endl;
                return 1;
            }
            /* strtoul returns unsigned long which on LP64 is 64-bit; the
             * library validates against a uint32 range so clip the upper
             * bound before narrowing to avoid silent wrap. */
            if (raw > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max()))
            {
                std::cerr << "decision_interval_ms: must be 0 or "
                          << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax
                          << std::endl;
                return 1;
            }
            const std::uint32_t v = static_cast<std::uint32_t>(raw);
            if (v > kDecisionRepeatIntervalMsMax
                || (v != 0U && v < kDecisionRepeatIntervalMsMinNonZero))
            {
                std::cerr << "decision_interval_ms: must be 0 or "
                          << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax
                          << std::endl;
                return 1;
            }
            decisionIntervalMs = v;
        }
    }

    /* Blocks until shutdown; non-zero if initialization failed. */
    return launchProximityControlAlgo(gatewayIP, gatewayPort, plcIP, plcPort,
                                      maxHbFailures, decisionIntervalMs);
}