/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Forklift-to-person proximity Safety Decision Maker for the combined ATL +
 * proximity deployment (ATL dataset).
 *
 * This is the second of the two SDMs the combined app runs:
 *   - atl_sdm           unmodified ATL SDM, consumes EVENT_0..EVENT_5
 *   - atl_proximity_sdm this binary, consumes EVENT_8..EVENT_10
 * Both subscribe to the same PSD gateway and both drive the PLC, on separate
 * command-receiver ports so their command streams stay distinguishable.
 *
 * The ATL side is reused as-is: decision-makers/atl is not modified or copied
 * by this app, and the deployment launches the atl_sdm binary built there.
 * Likewise decision-makers/proximity, which is deployed and field-validated,
 * is left untouched; the forklift-to-person decision logic in
 * AtlProximityControl.cpp belongs to this component alone.
 *
 * This file is only the app layer: CLI parsing and defaults. The decision logic
 * is in AtlProximityControl.cpp and atl_proximity_pair_fusion.h.
 *
 * Note for anyone extending the event mapping: a forklift reaches the SDM as
 * the generic ObjectType OBJECT, because mdx-client's stringToObjectType() has
 * no FORKLIFT enumerator. The SDM can therefore check that a pair has exactly
 * one person endpoint, but it cannot tell a forklift from any other non-person
 * object. Restricting these events to forklift-to-person pairs is the event
 * mapping's job. See the scoping note in event_mapping_atl_proximity.pb.txt.
 */

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <string>

#include "AtlProximityControl.h"
#include "atl_proximity_pair_fusion.h"
#include "sdm_decision_freshness.hpp"

/* Default PLC command-receiver port for this SDM.
 *
 * Deliberately not 12345. The ATL SDM defaults to 12345 and the combined
 * deployment runs both SDMs on one host, so sharing the default would have the
 * two command streams arrive at one receiver where the ATL and proximity
 * opcodes overlap. Keeping them apart also lets an operator attribute an
 * emitted command to the safety function that produced it. */
static constexpr std::uint16_t kDefaultPlcPort = 12346U;

/* Parameter ranges are validated by launchAtlProximityControlAlgo(), which
 * documents itself as the authority so a direct library caller cannot install
 * an out-of-range value. This parser therefore checks argument *shape* only
 * (present, numeric, in uint16/uint32) and lets the library reject the rest
 * with its own diagnostics, keeping one validation owner rather than two that
 * can drift. */
static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [OPTIONS]\n\n"
              << "Forklift-to-person proximity Safety Decision Maker (combined ATL + proximity app).\n"
              << "Consumes EVENT_8/9/10 from event_mapping_atl_proximity.pb.txt and emits the\n"
              << "worst-case command across all live forklift/person pairs.\n\n"
              << "Options:\n"
              << "  --gateway_ip <IP>            PSD Gateway IP (default: 127.0.0.1).\n"
              << "  --gateway_port <PORT>        PSD Gateway port, 1-65535 (default: 50000).\n"
              << "  --cmd_rx_ip <IP>             Command receiver IP (default: 127.0.0.1).\n"
              << "  --cmd_rx_port <PORT>         Command receiver port, 1-65535 (default: "
              << kDefaultPlcPort << ").\n"
              << "                               Must differ from the ATL SDM's port.\n"
              << "  --max_hb_failures <N>        Heartbeat miss limit, 1-255 (default: 10).\n"
              << "  --decision_interval_ms <MS>  PLC repeat period ms; 0=off (default: 5000).\n"
              << "  --hb_stale_ms <MS>           Gateway HB stale grace (default: 5000).\n"
              << "  --hb_period_ms <MS>          Gateway HB miss period (default: 5500).\n"
              << "  --decision_freshness_timeout_ms <MS>\n"
              << "                               DecisionRequest freshness timeout (default: "
              << SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT << ").\n"
              << "  --pair_ttl_ms <MS>           How long a forklift/person pair keeps counting\n"
              << "                               toward the worst case after its last sighting, "
              << ATLPXC_PAIR_TTL_MS_MIN << "-" << ATLPXC_PAIR_TTL_MS_MAX
              << "\n                               (default: " << ATLPXC_PAIR_TTL_MS_DEFAULT << ").\n"
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

/* Parse a decimal unsigned value and reject anything that is not a complete,
 * in-range number. Rejecting trailing characters matters: strtoul would
 * otherwise accept "5000x" and silently install 5000. */
static bool parseUnsigned(const char* arg,
                          unsigned long max,
                          unsigned long* out,
                          const char* name,
                          const char* prog)
{
    char* end = nullptr;

    if (arg == nullptr || arg[0] == '\0')
    {
        std::cerr << "error: " << name << ": missing or empty value\n";
        printUsage(prog);
        return false;
    }

    errno = 0;
    const unsigned long raw = std::strtoul(arg, &end, 10);
    if (errno == ERANGE || end == arg || *end != '\0' || raw > max)
    {
        std::cerr << "error: " << name << ": not a valid value in 0.." << max << "\n";
        printUsage(prog);
        return false;
    }

    *out = raw;
    return true;
}

int main(int argc, char* argv[])
{
    std::string   gatewayIP = "127.0.0.1";
    std::uint16_t gatewayPort = 50000U;
    std::string   plcIP = "127.0.0.1";
    std::uint16_t plcPort = kDefaultPlcPort;
    std::uint8_t  maxHbFailures = 10U;
    std::uint32_t decisionIntervalMs = 5000U;
    std::uint32_t hbStaleMs = 5000U;
    std::uint32_t hbPeriodMs = 5500U;
    std::uint32_t decisionFreshnessTimeoutMs = SDM_DECISION_FRESHNESS_TIMEOUT_MS_DEFAULT;
    std::uint32_t pairTtlMs = ATLPXC_PAIR_TTL_MS_DEFAULT;

    for (int i = 1; i < argc; ++i)
    {
        const char*   arg = argv[i];
        unsigned long parsed = 0UL;

        if (std::strcmp(arg, "--gateway_ip") == 0)
        {
            if (!requireOptionValue(argc, i, "--gateway_ip", argv[0]))
                return 1;
            gatewayIP = argv[++i];
        }
        else if (std::strcmp(arg, "--gateway_port") == 0)
        {
            if (!requireOptionValue(argc, i, "--gateway_port", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], 65535UL, &parsed, "--gateway_port", argv[0]))
                return 1;
            gatewayPort = static_cast<std::uint16_t>(parsed);
        }
        else if (std::strcmp(arg, "--cmd_rx_ip") == 0)
        {
            if (!requireOptionValue(argc, i, "--cmd_rx_ip", argv[0]))
                return 1;
            plcIP = argv[++i];
        }
        else if (std::strcmp(arg, "--cmd_rx_port") == 0)
        {
            if (!requireOptionValue(argc, i, "--cmd_rx_port", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], 65535UL, &parsed, "--cmd_rx_port", argv[0]))
                return 1;
            plcPort = static_cast<std::uint16_t>(parsed);
        }
        else if (std::strcmp(arg, "--max_hb_failures") == 0)
        {
            if (!requireOptionValue(argc, i, "--max_hb_failures", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], 255UL, &parsed, "--max_hb_failures", argv[0]))
                return 1;
            maxHbFailures = static_cast<std::uint8_t>(parsed);
        }
        else if (std::strcmp(arg, "--decision_interval_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--decision_interval_ms", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], UINT32_MAX, &parsed, "--decision_interval_ms", argv[0]))
                return 1;
            decisionIntervalMs = static_cast<std::uint32_t>(parsed);
        }
        else if (std::strcmp(arg, "--hb_stale_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--hb_stale_ms", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], UINT32_MAX, &parsed, "--hb_stale_ms", argv[0]))
                return 1;
            hbStaleMs = static_cast<std::uint32_t>(parsed);
        }
        else if (std::strcmp(arg, "--hb_period_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--hb_period_ms", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], UINT32_MAX, &parsed, "--hb_period_ms", argv[0]))
                return 1;
            hbPeriodMs = static_cast<std::uint32_t>(parsed);
        }
        else if (std::strcmp(arg, "--decision_freshness_timeout_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--decision_freshness_timeout_ms", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], UINT32_MAX, &parsed,
                               "--decision_freshness_timeout_ms", argv[0]))
                return 1;
            decisionFreshnessTimeoutMs = static_cast<std::uint32_t>(parsed);
        }
        else if (std::strcmp(arg, "--pair_ttl_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--pair_ttl_ms", argv[0]))
                return 1;
            if (!parseUnsigned(argv[++i], UINT32_MAX, &parsed, "--pair_ttl_ms", argv[0]))
                return 1;
            pairTtlMs = static_cast<std::uint32_t>(parsed);
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            printUsage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "error: unknown or unexpected argument (see --help)\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    /* Blocks until shutdown; non-zero if initialization or validation failed. */
    return launchAtlProximityControlAlgo(gatewayIP, gatewayPort, plcIP, plcPort,
                                         maxHbFailures, decisionIntervalMs,
                                         hbStaleMs, hbPeriodMs,
                                         decisionFreshnessTimeoutMs, pairTtlMs);
}
