/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Startup configuration for the ATL CCPLEX SDM: which event code carries each
 * ATL situation. This was a compiled-in constant, which meant one binary per
 * deployment; reading it at startup lets several ATL SDMs, each covering a
 * different zone, run against one PSD gateway.
 *
 * Scope is the event binding ONLY. The safety-relevant camera pipelines are
 * NOT here -- they come from sensor_pipelines_config.conf, which every CCPLEX
 * SDM and mdx-client read, so one reviewed set serves the whole deployment and
 * the ATL and PXC sets cannot drift apart. A per-SDM copy of that list would
 * reintroduce exactly that drift.
 *
 * Deliberately a plain "key = value" text file rather than the protobuf text
 * format mdx-client uses for its event mapping. mdx_client already links
 * libnvpsf_msgcodec and libprotobuf; atl_sdm links neither, and pulling
 * protobuf into a safety decision maker to read six event names is a
 * dependency this file does not need. The format matches nvpss.conf and
 * sensor_pipelines_config.conf, which are what an operator on this system
 * already edits.
 */

#ifndef ATL_SDM_CONFIG_H
#define ATL_SDM_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "pss_protocol.h"

/* Number of ATL situations, each carrying its own event code. */
#define ATL_EVENT_COUNT 6U


/**
 * @brief One ATL SDM instance's startup configuration.
 *
 * Populated only by atlSdmConfigLoad(), which fills it completely or not at
 * all. A partially applied safety binding is worse than none: the operator
 * would believe the file was in force while some situations still carried
 * their compiled-in codes.
 */
struct AtlSdmConfig {
    /* Positional, in the order the situations are declared in ATLControl.cpp:
     * forklift-entered, forklift-exited, person-entered, person-exited,
     * roi-violation, roi-cleared. */
    EventType events[ATL_EVENT_COUNT];

};

/**
 * @brief Resolve a generic event-type name to its enumerator.
 *
 * Accepts only the generic block, "EVENT_0" .. "EVENT_23". SW_FAIL is reserved:
 * it is always subscribed and handled as a fault, so it cannot carry a
 * situation. The trust-report types are owned by the gateway, which force-adds
 * them to every registration.
 *
 * @param name Candidate name; no leading zeros, no surrounding blanks.
 * @param out  Receives the enumerator on success; untouched on failure.
 * @return true when @p name named an event code this SDM may bind.
 */
bool atlParseEventTypeName(const std::string& name, EventType& out);

/**
 * @brief Render an event type for a log line.
 *
 * Returns "SW_FAIL", "EVENT_n" for the generic block, or "EVENT(n)" for a value
 * outside it, so an unexpected type is still readable rather than dropped.
 */
std::string atlEventTypeName(EventType type);

/**
 * @brief Read an ATL SDM configuration file.
 *
 * Every key is mandatory and every failure is fatal to the caller: an
 * unreadable file, a missing key, a repeated key, an unrecognised key, a name
 * that is not a bindable event code, or two situations sharing a code. None of
 * these fall back to a default, because a safety binding the configuration does
 * not state is one nothing downstream can attribute.
 *
 * @param path Path to the configuration file.
 * @param out  Populated only on success.
 * @param err  Receives an operator-facing reason, with a line number where the
 *             failure belongs to one line.
 * @return true on success.
 */
bool atlSdmConfigLoad(const std::string& path, AtlSdmConfig& out, std::string& err);

#endif /* ATL_SDM_CONFIG_H */
