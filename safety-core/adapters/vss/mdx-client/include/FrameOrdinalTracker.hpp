/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_FRAME_ORDINAL_TRACKER_HPP
#define MDX_CLIENT_FRAME_ORDINAL_TRACKER_HPP

#include <cstdint>
#include <limits>
#include <map>
#include <string>

namespace MDXClient {

class FrameOrdinalTracker {
public:
    uint64_t next(const std::string& sensorId) {
        uint64_t& ordinal = ordinals_[sensorId];
        // Saturate rather than wrap so scale-factor selection never reuses an
        // earlier frame ordinal during an exceptionally long client lifetime.
        if (ordinal < std::numeric_limits<uint64_t>::max()) {
            ++ordinal;
        }
        return ordinal;
    }

private:
    std::map<std::string, uint64_t> ordinals_;
};

} // namespace MDXClient

#endif
