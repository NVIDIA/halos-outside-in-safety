/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_EVENTS_PARSER_HPP
#define MDX_CLIENT_EVENTS_PARSER_HPP

#include <string>
#include <vector>
#include <functional>
#include "common.hpp"
#include "NvPSFMsgCodec.h"

namespace MDXClient {

class EventsParser {
public:
    using NextEventIdFn = std::function<uint32_t()>;

    explicit EventsParser(NextEventIdFn nextEventId);

    std::vector<AlertMessage> parseEventsMessage(const std::string& data);

private:
    NextEventIdFn nextEventId_;

    bool parseBehaviorToAlertMessage(const NvPSFMsgCodecMsg* msg, AlertMessage& alertMsg);
};

} // namespace MDXClient

#endif
