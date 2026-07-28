/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_FRAMES_PARSER_HPP
#define MDX_CLIENT_FRAMES_PARSER_HPP

#include <string>
#include <vector>
#include <functional>
#include <set>
#include <utility>
#include "FrameOrdinalTracker.hpp"
#include "common.hpp"
#include "NvPSFMsgCodec.h"

namespace MDXClient {

class FramesParser {
public:
    using NextEventIdFn = std::function<uint32_t()>;

    explicit FramesParser(NextEventIdFn nextEventId, bool debugMode = false);

    std::vector<AlertMessage> parseFramesMessage(const std::string& data,
        const NvPSFMsgCodecMsg* config);

private:
    NextEventIdFn nextEventId_;
    bool debugMode_;
    FrameOrdinalTracker frameOrdinalTracker_;
    uint64_t currentFrameOrdinal_ = 0U;

    AlertMessage buildAlertFromFrameRoi(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* roi, const NvPSFMsgCodecMsg* object,
        uint32_t objectId);
    AlertMessage buildRoiStateAlert(const NvPSFMsgCodecMsg* frameMsg,
        const std::string& roiId, const std::string& objectType);
    // Emit per-frame "restricted-area cleared" alerts (EVENT_5) for monitored
    // restricted areas that are present in the frame but contain no violating
    // object of the configured type. VST reports a violation as a per-object ROI
    // entry and omits it once the object leaves (it never sends a false entry),
    // so this is required for the SDM to ever see the violation clear.
    void emitRestrictedAreaClears(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config,
        const std::set<std::string>& presentRestrictedRoiIds,
        const std::vector<std::pair<std::string, std::string>>& restrictedViolations,
        const std::vector<std::pair<std::string, std::string>>& restrictedRoiEmitted,
        std::vector<AlertMessage>& alerts);
    AlertMessage buildAlertFromFrameSocialDistancing(const NvPSFMsgCodecMsg* frameMsg);
    AlertMessage buildAlertFromFrameObject(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* obj, const char* type, const std::string& ruleId, uint32_t assignId);

    void evaluateProximityRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config, std::vector<AlertMessage>& alerts);
    void evaluateObjectPresenceRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* config, std::vector<AlertMessage>& alerts);
};

} // namespace MDXClient

#endif
