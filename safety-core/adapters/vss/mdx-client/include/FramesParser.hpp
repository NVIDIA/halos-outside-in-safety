/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_FRAMES_PARSER_HPP
#define MDX_CLIENT_FRAMES_PARSER_HPP

#include <string>
#include <vector>
#include <functional>
#include "common.hpp"
#include "NvPSFMsgCodec.h"

namespace MDXClient {

struct SharedState;

class FramesParser {
public:
    using NextEventIdFn = std::function<uint32_t()>;

    explicit FramesParser(NextEventIdFn nextEventId, bool debugMode = false);

    std::vector<AlertMessage> parseFramesMessage(const std::string& data,
        const NvPSFMsgCodecMsg* config, SharedState& state);

private:
    NextEventIdFn nextEventId_;
    bool debugMode_;

    AlertMessage buildAlertFromFrameRoi(const NvPSFMsgCodecMsg* frameMsg,
        const NvPSFMsgCodecMsg* roi, uint32_t objectId);
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
