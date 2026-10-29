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
#include "atl_sdm_config.h"
#include "sdm_decision_freshness.hpp"
#include "sdm_reviewed_sensor_set.hpp"
#include "nvpsf_transport.h"

/* Must match launchATLControlAlgo validation in ATLControl.cpp */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                      = 100U;
static constexpr std::uint32_t kHbTimingMsMax                      = 600000U;


static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [OPTIONS]\n\n"
              << "ATL V2 Safety Decision Maker.\n\n"
              << "Options:\n"
              << "  --cmd_rx_ip <IP>             Command receiver IP (default: 127.0.0.1).\n"
              << "  --cmd_rx_port <PORT>         Command receiver port, 1-65535 (default: 12345).\n"
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
              << "  --config <PATH>              REQUIRED. Startup configuration file, and the\n"
              << "                                only source of the event binding. Every key\n"
              << "                                is mandatory and there is no compiled-in\n"
              << "                                fallback. The safety-relevant pipelines are\n"
              << "                                NOT here -- see --sensor_pipelines_config.\n"
              << "                                Event names are\n"
              << "                                EVENT_0..EVENT_" << static_cast<unsigned>(EVENT_23)
              << ", distinct, in any order; see\n"
              << "                                atl_sdm_zone1.conf.sample.\n"
              << "  --log_file <PATH>            This instance's log file (default:\n"
              << "                                atl_sdm.log in the working directory). Give\n"
              << "                                each SDM its own when several run together.\n"
              << "  --sensor_pipelines_config <PATH>\n"
              << "                                Reviewed sensor set behind the\n"
              << "                                all-safety-relevant-sensors-failed latch\n"
              << "                                (default: " << SDM_SENSOR_PIPELINES_CONFIG_DEFAULT << ").\n"
              << "                                Missing or malformed is fatal.\n"
              << "  -h, --help                   Show this help message.\n\n"
              << "The configured event codes must match the output_event codes in the\n"
              << "mdx-client event map, so the gateway routes this deployment's events to\n"
              << "this decision maker. Two ATL SDMs sharing one gateway must not share a\n"
              << "code: the gateway filters by registration, and a shared code would be\n"
              << "delivered to both.\n";
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
    std::string    gatewayIP   = "127.0.0.1";
    std::uint16_t  gatewayPort = 50000;
    std::string    plcIP       = "127.0.0.1";
    std::uint16_t  plcPort     = 12345;
    std::uint8_t   maxHbFailures = 10U;
    std::uint32_t  decisionIntervalMs = 5000U;
    std::uint32_t  hbStaleMs = 5000U;
    std::uint32_t  hbPeriodMs = 5500U;
    std::uint32_t  decisionFreshnessTimeoutMs = SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT;
    std::string    sensorPipelinesConfigPath = SDM_SENSOR_PIPELINES_CONFIG_DEFAULT;

    /* Upstream transport selection. Startup-only and immutable. */
    /* AF_UNIX SOCK_SEQPACKET, not selectable. The gateway hop is UNIX-only by
     * deployment decision; only the out-of-scope PLC link stays UDP, and that is
     * a raw AF_INET datagram socket elsewhere in this process, not this
     * transport. */
    const NvPSFTransportKind gatewayTransport = NVPSF_TRANSPORT_UNIX_SOCKET;
    std::string        gatewaySocketPath = NVPSF_SOCKET_PATH_PSD_GATEWAY;

    /* Collected during parsing and applied afterwards, because options may
     * arrive in any order and --log_file must take effect before the control
     * layer opens its log. */
    std::string configPath;
    std::string logFilePath;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (strcmp(arg, "--cmd_rx_ip") == 0)
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
        else if (strcmp(arg, "--config") == 0)
        {
            if (!requireOptionValue(argc, i, "--config", argv[0]))
                return 1;
            configPath = argv[++i];
            if (configPath.empty())
            {
                std::cerr << "error: --config: path is empty\n";
                printUsage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(arg, "--events") == 0)
        {
            /* Removed rather than aliased. It had no equivalent flag to map to
             * -- the binding is the file -- and accepting it with a warning
             * would silently run the file's codes while the operator believed
             * they had overridden them. Named here instead of falling through
             * to "unknown option" so a stale script says what to do next. */
            std::cerr << "error: --events was removed; the configuration file named by"
                         " --config is now the only source of the event binding. Put the"
                         " six codes in that file.\n";
            printUsage(argv[0]);
            return 1;
        }
        else if (strcmp(arg, "--log_file") == 0)
        {
            if (!requireOptionValue(argc, i, "--log_file", argv[0]))
                return 1;
            logFilePath = argv[++i];
            if (logFilePath.empty())
            {
                std::cerr << "error: --log_file: path is empty\n";
                printUsage(argv[0]);
                return 1;
            }
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


    /* --- Startup bindings, applied before the control layer opens its log --- */

    if (!logFilePath.empty() && !atlSetLogFile(logFilePath))
    {
        std::cerr << "error: --log_file: rejected\n";
        printUsage(argv[0]);
        return 1;
    }

    /* --config is mandatory. There is no compiled-in binding to fall back on,
     * and that is the point: an ATL SDM started without one would otherwise
     * register for whatever the build happened to default to. For a second zone
     * that means silently subscribing to the first zone's codes, tracking the
     * first zone's trailer, and emitting a mute permission on its own PLC port
     * for a trailer it has no evidence about. Same rule, and the same reason, as
     * enableFusion and sensorConfig in nvpss.conf. */
    if (configPath.empty())
    {
        std::cerr << "error: --config is required; it states the event binding"
                     " (the safety-relevant pipelines come from"
                     " --sensor_pipelines_config)\n";
        printUsage(argv[0]);
        return 1;
    }

    {
        AtlSdmConfig cfg = {};
        std::string err;
        if (!atlSdmConfigLoad(configPath, cfg, err))
        {
            std::cerr << "error: --config " << configPath << ": " << err << "\n";
            return 1;
        }
        /* Events only. The safety-relevant pipeline set moved to
         * sensor_pipelines_config.conf, which every CCPLEX SDM and mdx-client
         * read, so one reviewed set serves the whole deployment and the ATL and
         * PXC sets cannot drift apart again. launchATLControlAlgo loads it. */
        if (!atlSetSubscribedEvents(cfg.events, ATL_EVENT_COUNT, "--config"))
        {
            /* The loader validated the list, so reaching here means the two
             * validators have drifted apart. Refuse rather than run on a
             * binding neither of them fully agreed to. */
            std::cerr << "error: --config " << configPath << ": rejected by the control layer\n";
            return 1;
        }
    }

    /* Authority on the transport this run used: the safe-state injectors'
     * --transport flag only declares it, so evidence must be read against this
     * line and the matching one in the gateway log.
     *
     * Kind only. The endpoint address, port and socket path are sensitive per the
     * repo logging rule and are not needed for evidence; they stay behind NVPSF_DBG. */
    std::cout << "[TRANSPORT] gateway upstream: "
              << NvPSFTransportKindName(gatewayTransport) << "\n";
#ifdef NVPSF_DBG
    std::cout << "[TRANSPORT] endpoint: "
              << (gatewayTransport == NVPSF_TRANSPORT_UNIX_SOCKET
                      ? "socket=" + gatewaySocketPath
                      : "ip=" + gatewayIP + " port=" + std::to_string(gatewayPort))
              << "\n";
#endif

    /* Blocks until shutdown; non-zero if initialization failed.
     * --gateway_transport had no effect on the link actually used. */
    return launchATLControlAlgo(gatewayIP, gatewayPort, plcIP, plcPort, maxHbFailures,
                                decisionIntervalMs, hbStaleMs, hbPeriodMs,
                                decisionFreshnessTimeoutMs,
                                gatewayTransport, gatewaySocketPath,
                                sensorPipelinesConfigPath);
}
