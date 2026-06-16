/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_SHARED_STATE_HPP
#define MDX_CLIENT_SHARED_STATE_HPP

#include <map>
#include <string>
#include <mutex>
#include <cstdint>

namespace MDXClient {

struct SharedState {
    std::map<std::string, bool> restrictedViolState;
    std::map<std::string, bool> confinedViolState;
    std::map<std::string, std::string> lastRestrictedObjectType;
    std::map<std::string, bool> socialDistancingViolState;
    std::map<std::string, uint64_t> frameCountPerSensor;
    std::string currentFrameSensorId;
    uint64_t currentFrameCount = 0;
    std::mutex mtx;
};

} // namespace MDXClient

#endif
