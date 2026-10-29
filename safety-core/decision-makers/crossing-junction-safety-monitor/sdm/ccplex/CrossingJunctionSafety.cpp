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
#include <algorithm>
#include <string>
#include "CrossingJunctionSafetyControl.h"
#include "sdm_decision_freshness.hpp"

/* Must match launchCrossingJunctionSafetyControlAlgo validation in CrossingJunctionSafetyControl.cpp */
static constexpr std::uint32_t kDecisionRepeatIntervalMsMinNonZero = 100U;
static constexpr std::uint32_t kDecisionRepeatIntervalMsMax        = 36000U;
static constexpr std::uint32_t kHbTimingMsMin                      = 100U;
static constexpr std::uint32_t kHbTimingMsMax                      = 600000U;

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog
              << " [OPTIONS]\n\n"
              << "Crossing-Junction Safety Decision Maker.\n\n"
              << "Raises the junction siren while an inbound Forklift and a Person occupy the\n"
              << "same crossing: siren = A1 AND A2, where A1 is a Forklift that crossed a\n"
              << "tripwire heading into the junction and has not left the ROI, and A2 is at\n"
              << "least one Person inside that ROI.\n\n"
              << "Options:\n"
              << "  --gateway_ip <IP>            PSD Gateway IP (default: 127.0.0.1).\n"
              << "  --gateway_port <PORT>        PSD Gateway port, 1-65535 (default: 50000).\n"
              << "  --cmd_rx_ip <IP>             Command receiver IP (default: 127.0.0.1).\n"
              << "  --cmd_rx_port <PORT>         Command receiver port, 1-65535 (default: 12345).\n"
              << "  --halo_ip <IP>               VST/VIOS junction halo listener IP; enables fan-out of\n"
              << "                                every decision command to VST's halo overlay\n"
              << "                                (disabled by default; requires --halo_port too).\n"
              << "  --halo_port <PORT>           VST/VIOS junction halo listener port, 1-65535\n"
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
              << "  --tripwire_engage_window_ms <MS>\n"
              << "                                How long an inbound tripwire crossing keeps a\n"
              << "                                Forklift engaged before the ROI must confirm it,\n"
              << "                                " << kJunctionHoldMsMin << ".."
              << kJunctionHoldMsMax << " (default: 5000).\n"
              << "  --person_hold_ms <MS>        How long a sensor stays believed without a Person\n"
              << "                                report of either polarity, "
              << kJunctionHoldMsMin << ".." << kJunctionHoldMsMax << " (default: 1500).\n"
              << "  --forklift_hold_ms <MS>      The same backstop for Forklift reports, "
              << kJunctionHoldMsMin << ".."
              << kJunctionHoldMsMax << " (default: 2000).\n"
              << "                                Both cover a stream that dies; normal release comes\n"
              << "                                from the vacated reports below.\n"
              << "  --roi_entry_pulse_ms <MS>    How long a Forklift first seen inside the junction ROI\n"
              << "                                keeps A1 armed without any tripwire crossing,\n"
              << "                                0 (disabled) or " << kJunctionHoldMsMin << ".."
              << kJunctionHoldMsMax << " (default: 5000).\n"
              << "                                Covers vehicles the wire never reports -- only one\n"
              << "                                camera sees it, and a crossing is a single-shot event.\n"
              << "  --clear_debounce_ms <MS>     How long every sensor must report the class vacated\n"
              << "                                before the level is released, " << kJunctionHoldMsMin
              << ".." << kJunctionHoldMsMax << " (default: 500).\n"
              << "                                Sets release latency. Release only -- the siren is\n"
              << "                                raised on the crossing frame either way.\n"
              << "  --junction_eval_period_ms <MS>\n"
              << "                                State-machine re-evaluation cadence, "
              << kJunctionEvalPeriodMsMin << ".." << kJunctionEvalPeriodMsMax << "\n"
              << "                                (default: 200). Must not exceed the debounce.\n"
              << "  --per_sensor_junctions       Treat each camera as its own crossing. Default is\n"
              << "                                to pool them, which this deployment needs because\n"
              << "                                only one camera sees the tripwire.\n"
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
    std::string  haloIP;                      // VST junction halo listener (empty = disabled)
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
    /* Junction timing. Defaults are sized against this deployment's event rates;
     * see CrossingJunctionSafetyControl.h for what each one governs. */
    std::uint32_t tripwireEngageWindowMs = 5000U;
    std::uint32_t personRoiHoldMs        = 1500U;
    std::uint32_t forkliftRoiHoldMs      = 2000U;
    std::uint32_t clearDebounceMs        = 500U;
    std::uint32_t roiEntryPulseMs        = 5000U;
    std::uint32_t junctionEvalPeriodMs   = 200U;
    bool          perSensorJunctions     = false;

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
        else if (strcmp(arg, "--tripwire_engage_window_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--tripwire_engage_window_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], tripwireEngageWindowMs,
                                  static_cast<std::uint32_t>(kJunctionHoldMsMin),
                                  static_cast<std::uint32_t>(kJunctionHoldMsMax),
                                  "--tripwire_engage_window_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--person_hold_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--person_hold_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], personRoiHoldMs,
                                  static_cast<std::uint32_t>(kJunctionHoldMsMin),
                                  static_cast<std::uint32_t>(kJunctionHoldMsMax),
                                  "--person_hold_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--forklift_hold_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--forklift_hold_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], forkliftRoiHoldMs,
                                  static_cast<std::uint32_t>(kJunctionHoldMsMin),
                                  static_cast<std::uint32_t>(kJunctionHoldMsMax),
                                  "--forklift_hold_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--clear_debounce_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--clear_debounce_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], clearDebounceMs,
                                  static_cast<std::uint32_t>(kJunctionHoldMsMin),
                                  static_cast<std::uint32_t>(kJunctionHoldMsMax),
                                  "--clear_debounce_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--roi_entry_pulse_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--roi_entry_pulse_ms", argv[0]))
                return 1;
            /* 0 disables the backstop, so it is accepted alongside the range
             * rather than inside it. */
            if (strcmp(argv[i + 1], "0") == 0)
            {
                roiEntryPulseMs = 0U;
                ++i;
            }
            else if (!parseUint32Range(argv[++i], roiEntryPulseMs,
                                       static_cast<std::uint32_t>(kJunctionHoldMsMin),
                                       static_cast<std::uint32_t>(kJunctionHoldMsMax),
                                       "--roi_entry_pulse_ms", argv[0]))
            {
                return 1;
            }
        }
        else if (strcmp(arg, "--junction_eval_period_ms") == 0)
        {
            if (!requireOptionValue(argc, i, "--junction_eval_period_ms", argv[0]))
                return 1;
            if (!parseUint32Range(argv[++i], junctionEvalPeriodMs,
                                  kJunctionEvalPeriodMsMin, kJunctionEvalPeriodMsMax,
                                  "--junction_eval_period_ms", argv[0]))
                return 1;
        }
        else if (strcmp(arg, "--per_sensor_junctions") == 0)
        {
            perSensorJunctions = true;
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

    /* Caught here as well as in the library so the message names the two options
     * involved rather than only the resulting constraint. */
    if (junctionEvalPeriodMs > clearDebounceMs)
    {
        std::cerr << "error: --junction_eval_period_ms must not exceed "
                     "--clear_debounce_ms\n";
        printUsage(argv[0]);
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
    return launchCrossingJunctionSafetyControlAlgo(gatewayIP, gatewayPort, plcIP, plcPort,
                                      maxHbFailures, decisionIntervalMs,
                                      hbStaleMs, hbPeriodMs,
                                      decisionFreshnessTimeoutMs,
                                      haloIP, haloPort,
                                      tripwireEngageWindowMs, personRoiHoldMs,
                                      forkliftRoiHoldMs, junctionEvalPeriodMs,
                                      perSensorJunctions, clearDebounceMs,
                                      roiEntryPulseMs,
                                      gatewayTransport, gatewaySocketPath);
}
