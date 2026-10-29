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
#include "sdm_decision_freshness.hpp"
#include "sdm_reviewed_sensor_set.hpp"
#include "nvpsf_transport.h"

/* Must match launchProximityControlAlgo validation in ProximityControl.cpp */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                      = 100U;
static constexpr std::uint32_t kHbTimingMsMax                      = 600000U;

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [OPTIONS]\n\n"
              << "Proximity Safety Decision Maker "
                 "(configured machine<->Person pairs).\n\n"
              << "Options:\n"
              << "  --config <PATH>              REQUIRED. This instance's startup event binding.\n"
              << "  --log_file <PATH>            This instance's log (default: pxc_sdm.log).\n"
              << "  --gateway_ip <IP>            PSD Gateway IP (default: 127.0.0.1).\n"
              << "  --gateway_port <PORT>        PSD Gateway port, 1-65535 (default: 50000).\n"
              << "  --cmd_rx_ip <IP>             Command receiver IP (default: 127.0.0.1).\n"
              << "  --cmd_rx_port <PORT>         Command receiver port, 1-65535 (default: 12345).\n"
              << "  --halo_ip <IP>               VST/VIOS halo listener IP; enables fan-out of\n"
              << "                                every decision command to VST's halo overlay\n"
              << "                                (disabled by default; requires --halo_port too).\n"
              << "  --halo_port <PORT>           VST/VIOS halo listener port, 1-65535\n"
              << "                                (default: 12346).\n"
              << "  --max_hb_failures <N>        Heartbeat miss limit, 1-255 (default: 10).\n"
              << "  --decision_interval_ms <MS>  PLC repeat period ms; 0=off, else "
              << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax
              << " (default: 5000).\n"
              << "  --hb_stale_ms <MS>           Gateway HB stale grace, "
              << kHbTimingMsMin << ".." << kHbTimingMsMax << " (default: 5000).\n"
              << "  --hb_period_ms <MS>          Gateway HB miss period, "
              << kHbTimingMsMin << ".." << kHbTimingMsMax << " (default: 5500).\n"
              << "  --decision_freshness_timeout_ms <MS>\n"
              << "                                DecisionRequest freshness timeout, "
              << SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN << ".."
              << SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX
              << " (default: " << SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT << ").\n"
              << "  --sensor_pipelines_config <PATH>\n"
              << "                                Reviewed sensor set behind the\n"
              << "                                all-safety-relevant-sensors-failed latch\n"
              << "                                (default: " << SDM_SENSOR_PIPELINES_CONFIG_DEFAULT << ").\n"
              << "                                Missing or malformed is fatal.\n"
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

static bool parseUint32Range(const char* arg,
                             std::uint32_t& out,
                             std::uint32_t min,
                             std::uint32_t max,
                             const char* name,
                             const char* prog)
{
    if (!arg || arg[0] == '\0')
    {
        std::cerr << "error: " << name << ": invalid integer\n";
        printUsage(prog);
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long raw = std::strtoul(arg, &end, 10);
    if (errno == ERANGE || end == arg || *end != '\0'
        || raw < static_cast<unsigned long>(min)
        || raw > static_cast<unsigned long>(max))
    {
        std::cerr << "error: " << name << " must be in " << min << ".." << max << "\n";
        printUsage(prog);
        return false;
    }
    out = static_cast<std::uint32_t>(raw);
    return true;
}

int main(int argc, char* argv[])
{
    std::string  gatewayIP   = "127.0.0.1";   // NvPSD Gateway address
    std::uint16_t gatewayPort = 50000;        // NvPSD Gateway port
    std::string  plcIP       = "127.0.0.1";   // PLC destination
    std::uint16_t plcPort     = 12345;
    std::string  haloIP;                      // VST PFP halo listener (empty = disabled)
    std::uint16_t haloPort    = 12346;
    std::uint8_t  maxHbFailures = 10U;
    /* PLC decision-repeat period. 5000 ms is a conservative default: long
     * enough to avoid flooding the PLC and the audit log when a decision is
     * held, short enough to recover within one typical ACK window if the
     * previous send was lost. Override with --decision_interval_ms;
     * 0 disables the periodic repeat entirely. */
    std::uint32_t decisionIntervalMs = 5000U;
    std::uint32_t hbStaleMs = 5000U;
    std::uint32_t hbPeriodMs = 5500U;
    std::uint32_t decisionFreshnessTimeoutMs = SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT;
    std::string  sensorPipelinesConfigPath = SDM_SENSOR_PIPELINES_CONFIG_DEFAULT;
    std::string  configPath;
    std::string  logPath = "pxc_sdm.log";

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (strcmp(arg, "--config") == 0 || strcmp(arg, "--log_file") == 0)
        {
            if (!requireOptionValue(argc, i, arg, argv[0]))
                return 1;
            const std::string value = argv[++i];
            if (value.empty() || value.compare(0U, 2U, "--") == 0)
            {
                std::cerr << "error: " << arg << " requires a non-empty path\n";
                return 1;
            }
            if (strcmp(arg, "--config") == 0)
                configPath = value;
            else
                logPath = value;
        }
        else if (strcmp(arg, "--gateway_ip") == 0)
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
        else if (strcmp(arg, "--halo_ip") == 0)
        {
            if (!requireOptionValue(argc, i, "--halo_ip", argv[0]))
                return 1;
            haloIP = argv[++i];
        }
        else if (strcmp(arg, "--halo_port") == 0)
        {
            if (!requireOptionValue(argc, i, "--halo_port", argv[0]))
                return 1;
            if (!parsePort(argv[++i], haloPort, "--halo_port", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--max_hb_failures") == 0)
        {
            if (!requireOptionValue(argc, i, "--max_hb_failures", argv[0]))
                return 1;
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
            if (!requireOptionValue(argc, i, "--decision_interval_ms", argv[0]))
                return 1;
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
            /* strtoul returns unsigned long which on LP64 is 64-bit; the
             * library validates against a uint32 range so clip the upper
             * bound before narrowing to avoid silent wrap. */
            if (raw > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max()))
            {
                std::cerr << "error: decision_interval_ms must be 0 or "
                          << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax
                          << "\n";
                printUsage(argv[0]);
                return 1;
            }
            const std::uint32_t v = static_cast<std::uint32_t>(raw);
            if (v > kDecisionRepeatIntervalMsMax
                || (v != 0U && v < kDecisionRepeatIntervalMsMinNonZero))
            {
                std::cerr << "error: decision_interval_ms must be 0 or "
                          << kDecisionRepeatIntervalMsMinNonZero << ".." << kDecisionRepeatIntervalMsMax
                          << "\n";
                printUsage(argv[0]);
                return 1;
            }
            decisionIntervalMs = v;
        }
        else if (strcmp(arg, "--hb_stale_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--hb_stale_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], hbStaleMs, kHbTimingMsMin,
                                  kHbTimingMsMax, "--hb_stale_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--hb_period_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--hb_period_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], hbPeriodMs, kHbTimingMsMin,
                                  kHbTimingMsMax, "--hb_period_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--decision_freshness_timeout_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--decision_freshness_timeout_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], decisionFreshnessTimeoutMs,
                                  SDM_DECISION_FRESHNESS_TIMEOUT_MS_MIN,
                                  SDM_DECISION_FRESHNESS_TIMEOUT_MS_MAX,
                                  "--decision_freshness_timeout_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--sensor_pipelines_config") == 0)
        {
            if (!requireOptionValue(argc, i, "--sensor_pipelines_config", argv[0]))
                return 1;
            sensorPipelinesConfigPath = argv[++i];
            if (sensorPipelinesConfigPath.empty())
            {
                std::cerr << "error: --sensor_pipelines_config: empty path\n";
                printUsage(argv[0]);
                return 1;
            }
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

    if (configPath.empty())
    {
        std::cerr << "error: --config is required; it states this PXC instance's event binding\n";
        return 1;
    }
    ProximitySdmConfig config = {};
    std::string error;
    if (!proximitySdmConfigLoad(configPath, config, error))
    {
        std::cerr << "error: --config " << configPath << ": " << error << "\n";
        return 1;
    }
    if (!proximitySetSubscribedEvents(config) || !proximitySetLogFile(logPath))
    {
        std::cerr << "error: PXC startup binding or log path rejected\n";
        return 1;
    }

    /* The gateway offers the SDM link as AF_UNIX SOCK_SEQPACKET, so this is
     * fixed rather than exposed as a flag: a --gateway_transport option would
     * have no effect on the link actually used. --gateway_ip/--gateway_port are
     * still accepted and reported, but only an inet-kind transport consults
     * them. */
    const NvPSFTransportKind gatewayTransport = NVPSF_TRANSPORT_UNIX_SOCKET;
    const std::string        gatewaySocketPath = NVPSF_SOCKET_PATH_PSD_GATEWAY;

    std::cout << "  Gateway transport: " << NvPSFTransportKindName(gatewayTransport) << "\n"
              << "  Gateway endpoint:  "
              << (gatewayTransport == NVPSF_TRANSPORT_UNIX_SOCKET
                      ? "socket=" + gatewaySocketPath
                      : gatewayIP + ":" + std::to_string(gatewayPort))
              << "\n";

    /* Blocks until shutdown; non-zero if initialization failed. */
    return launchProximityControlAlgo(gatewayIP, gatewayPort, plcIP, plcPort,
                                      maxHbFailures, decisionIntervalMs,
                                      hbStaleMs, hbPeriodMs,
                                      decisionFreshnessTimeoutMs,
                                      haloIP, haloPort,
                                      gatewayTransport, gatewaySocketPath,
                                      sensorPipelinesConfigPath);
}
