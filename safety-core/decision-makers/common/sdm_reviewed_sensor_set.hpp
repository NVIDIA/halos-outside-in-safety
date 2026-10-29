/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SDM_REVIEWED_SENSOR_SET_HPP
#define SDM_REVIEWED_SENSOR_SET_HPP

/*
 * The reviewed sensor set behind the all-safety-relevant-sensors-failed latch,
 * loaded from sensor_pipelines_config.conf at startup.
 *
 * WHY THIS IS A FILE AND NOT A TABLE
 * ----------------------------------
 * ATL and PXC each used to carry a compiled copy of {1,2,3} transcribed by hand
 * from sensor_config.conf. Two copies of one fact in two files, and a third
 * copy of the same fact in the deployed .conf: nothing made them agree. In 3D
 * they cannot agree by construction -- the only identity that appears on a
 * decision-driving event is the BEV group, whose pipelineId is deliberately
 * absent from sensor_config.conf -- so a compiled 2D set would match no event
 * at all and the latch would never arm. The set has to come from the same file
 * the rest of the deployment reads.
 *
 * WHY A MISSING FILE IS FATAL
 * ---------------------------
 * The alternative -- fall back to the old compiled {1,2,3} -- is the failure
 * this task exists to remove. A 3D deployment that forgot to ship the file
 * would start, log nothing unusual, and silently run with a reviewed set that
 * no event can ever match. The latch would be inert with no outward sign. A
 * refusal to start is loud; an inert latch is not.
 */

#include <cstdint>
#include <string>

#include "sensor_config_parser.h"

/*
 * Deployment path, matching NVPSS_DAEMON_DEFAULT_CONF and the sensorGroupConfig
 * example in nvpss.conf. A default rather than a required flag: every launcher
 * and harness would otherwise have to be edited in lockstep to start the SDM at
 * all, and the safety property is carried by the file being mandatory, not by
 * the path being typed out. Override with --sensor_pipelines_config.
 */
#define SDM_SENSOR_PIPELINES_CONFIG_DEFAULT "/opt/nvidia/psf/bin/sensor_pipelines_config.conf"

/* Mask bit (pipelineId - 1) in a uint32_t. 31 rather than 32 so the value
 * matches the bound safetyRelevantSensorBit has always applied, and so the
 * shift stays clear of the sign bit. SENSOR_CONFIG_MAX_ENTRIES caps the file at
 * 8 rows and the parser already refuses an id outside 1..8, so this is a
 * property of the representation rather than a second policy -- but the shift
 * below is only defined while it holds, so it is checked and not assumed. */
static constexpr uint32_t kReviewedSensorMaxPipelineId = 31U;

struct ReviewedSensorSet {
    SensorPipelines entries{};
    /* Bit (pipelineId - 1) set for every member. The latch condition is
     * (failedMask & mask) == mask, so an empty set would make that trivially
     * true; loadReviewedSensorSet refuses an empty file for that reason. */
    uint32_t        mask{0U};
};

/*
 * Load and validate the reviewed set. Returns true on success; on failure
 * `err` carries a diagnostic naming the specific reason, and the caller must
 * treat it as fatal.
 *
 * Validation here is the runtime replacement for the three static_asserts the
 * compiled table carried (one-based, within sensor_config bounds, unique). The
 * parser enforces the range, uniqueness, row count and empty-file cases and
 * reports each with its own SensorConfigErr; this function adds only the mask
 * width check, which is a property of the representation rather than the file.
 */
inline bool loadReviewedSensorSet(const char* path,
                                  ReviewedSensorSet& out,
                                  std::string& err)
{
    if (path == nullptr || path[0] == '\0')
    {
        err = "sensor_pipelines_config path is empty";
        return false;
    }

    SensorPipelines parsed{};
    const SensorConfigErr rc = sensorPipelinesParse(path, &parsed);
    if (rc != SENSOR_CONFIG_OK)
    {
        err = std::string(sensorConfigErrStr(rc)) + ": " + path;
        return false;
    }

    /* Defence in depth: the parser refuses an empty file, so reaching here with
     * zero rows would mean the parser and this check disagree. Treat it as
     * fatal rather than computing mask 0, which the latch reads as
     * "all reviewed sensors failed" on the very first event. */
    if (parsed.count == 0U)
    {
        err = std::string("no sensor entries in ") + path;
        return false;
    }

    uint32_t mask = 0U;
    for (size_t i = 0U; i < parsed.count; ++i)
    {
        const uint8_t id = parsed.entries[i].pipelineId;
        if (id == 0U || id > kReviewedSensorMaxPipelineId)
        {
            err = "pipelineId " + std::to_string(static_cast<unsigned>(id)) +
                  " in " + path + " is outside 1.." +
                  std::to_string(static_cast<unsigned>(kReviewedSensorMaxPipelineId)) +
                  ", which the safety-sensor mask cannot represent";
            return false;
        }
        mask |= (1U << (id - 1U));
    }

    out.entries = parsed;
    out.mask    = mask;
    return true;
}

/*
 * One-line rendering of the resolved set for the startup log. A set that
 * silently shrank -- a row dropped from the file, a 3D deployment handed a 2D
 * file -- is only visible if the runtime states what it actually resolved, so
 * this is logged unconditionally at startup, not under a debug flag.
 */
inline std::string describeReviewedSensorSet(const ReviewedSensorSet& set)
{
    std::string s = "count=" + std::to_string(set.entries.count) + " {";
    for (size_t i = 0U; i < set.entries.count; ++i)
    {
        if (i != 0U)
            s += ", ";
        s += std::to_string(static_cast<unsigned>(set.entries.entries[i].pipelineId));
        s += ":";
        s += set.entries.entries[i].sensorName;
    }
    s += "}";
    return s;
}

/*
 * One-shot gate for "this pipelineId is outside the reviewed set".
 *
 * The startup line states what was resolved; this states what arrives and is
 * skipped, which is the other half of the same question. Together they let a
 * reader who knows the deployment mode see whether the reviewed set matches the
 * traffic -- the failure that motivates it is a 3D deployment handed an
 * un-edited 2D file, where {1,2,3} is resolved while every decision-driving
 * event arrives under the BEV group id, so the latch can never arm.
 *
 * Deliberately NOT phrased or logged as a fault. In 3D the daemon forwards each
 * camera's own health report as well as the synthesised group verdict, and only
 * the group is reviewed, so a correct 3D deployment passes ids through here
 * routinely. The SDM cannot distinguish that from the misconfiguration because
 * it does not read nvpss.conf and has no notion of deployment mode.
 *
 * `warned` is a bitmask of ids already reported, so each id is logged exactly
 * once -- bounded at 31 lines for the life of the process, so a
 * per-DecisionRequest stream cannot flood the log. Returns true when the caller
 * should emit the line.
 *
 * pipelineId 0 never reaches here -- the caller excludes it, because SW_FAIL
 * from the gateway legitimately carries 0.
 */
inline bool shouldWarnUnreviewedSensor(uint32_t& warned, uint32_t pipelineId)
{
    if (pipelineId == 0U || pipelineId > kReviewedSensorMaxPipelineId)
        return false;
    const uint32_t bit = 1U << (pipelineId - 1U);
    if ((warned & bit) != 0U)
        return false;
    warned |= bit;
    return true;
}

#endif /* SDM_REVIEWED_SENSOR_SET_HPP */
