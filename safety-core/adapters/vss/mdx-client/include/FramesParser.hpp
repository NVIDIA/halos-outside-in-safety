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
#include "CoMovementTracker.hpp"
#include "EventMapping.hpp"
#include "FrameOrdinalTracker.hpp"
#include "common.hpp"
#include "NvPSFMsgCodec.h"

namespace MDXClient {

// One frame's usable objects, decoded once per frame and shared by the
// proximity and co-movement evaluators. Declared here rather than in the
// translation unit because both evaluators take it as a parameter.
struct ProximityFrameObject {
    std::string rawId;
    std::string type;
    uint32_t numericId;
    double x;
    double y;
    double z;
    // Image-space box, used by co-movement only, and read from the frame only
    // when a co-movement rule is configured. A frame may omit it, so every
    // consumer must check hasBbox first; the proximity path ignores it.
    bool hasBbox;
    double leftX;
    double topY;
    double rightX;
    double bottomY;
};

class FramesParser {
public:
    using NextEventIdFn = std::function<uint32_t()>;

    explicit FramesParser(NextEventIdFn nextEventId, bool debugMode = false);

    std::vector<AlertMessage> parseFramesMessage(const std::string& data,
        const EventMapping& mapping);

private:
    NextEventIdFn nextEventId_;
    bool debugMode_;
    FrameOrdinalTracker frameOrdinalTracker_;
    uint64_t currentFrameOrdinal_ = 0U;
    // Carries per-pair history across frames. Owned here because the frames
    // thread is the only caller, which is also why it needs no lock.
    CoMovementTracker coMovementTracker_;

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
        const EventMapping& mapping,
        const std::set<std::string>& presentRestrictedRoiIds,
        const std::vector<std::pair<std::string, std::string>>& restrictedViolations,
        const std::vector<std::pair<std::string, std::string>>& restrictedRoiEmitted,
        std::vector<AlertMessage>& alerts);
    // A successfully decoded frame with no ROI entries is an approved
    // application-event clear condition. Emit only configured false ROI rules;
    // this does not change downstream fault-latch or manual-release behavior.
    void emitEmptyRoiClears(const NvPSFMsgCodecMsg* frameMsg,
        const EventMapping& mapping, std::vector<AlertMessage>& alerts);
    AlertMessage buildAlertFromFrameSocialDistancing(const NvPSFMsgCodecMsg* frameMsg);
    AlertMessage buildAlertFromFrameObject(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* obj, const char* type, const std::string& ruleId, uint32_t assignId);

    // Both evaluators are handed the frame's objects rather than decoding them:
    // collecting costs a codec sub-message allocation per object plus the field
    // reads, and doing it twice per frame bought nothing.
    void evaluateProximityRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const EventMapping& mapping, const std::vector<ProximityFrameObject>& objects,
        std::vector<AlertMessage>& alerts);
    // Unlike the other evaluators this one is stateful: it must run on every
    // frame, not only frames that produce an event, or the per-pair window it
    // judges over is not continuous.
    void evaluateCoMovementRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const EventMapping& mapping, const std::vector<ProximityFrameObject>& objects,
        std::vector<AlertMessage>& alerts);
    void evaluateObjectPresenceRulesForFrame(const NvPSFMsgCodecMsg* frameMsg,
        const EventMapping& mapping, std::vector<AlertMessage>& alerts);
};

} // namespace MDXClient

#endif
