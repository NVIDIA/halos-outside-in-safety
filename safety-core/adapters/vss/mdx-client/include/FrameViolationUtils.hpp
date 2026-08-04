/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_FRAME_VIOLATION_UTILS_HPP
#define MDX_CLIENT_FRAME_VIOLATION_UTILS_HPP

#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

namespace MDXClient {

enum class RoiViolationValue {
    kInvalid,
    kFalse,
    kTrue,
};

inline RoiViolationValue parseRoiViolationMapValue(const std::string& value) {
    if (value == "true") {
        return RoiViolationValue::kTrue;
    }
    if (value == "false") {
        return RoiViolationValue::kFalse;
    }
    return RoiViolationValue::kInvalid;
}

inline bool matchesRoiObjectType(const std::string& roiType,
                                 const std::string& objectType) {
    if (roiType.empty() || objectType.empty() || roiType.size() != objectType.size()) {
        return false;
    }
    for (size_t index = 0; index < roiType.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(roiType[index])) !=
            std::tolower(static_cast<unsigned char>(objectType[index]))) {
            return false;
        }
    }
    return true;
}

inline bool insertUniqueRoiObjectId(std::set<std::string>* objectIds,
                                    const std::string& objectId) {
    return objectIds != nullptr && !objectId.empty() && objectIds->insert(objectId).second;
}

inline bool insertUniqueRoiNumericObjectId(std::set<uint32_t>* objectIds,
                                           uint32_t objectId) {
    return objectIds != nullptr && objectIds->insert(objectId).second;
}

inline bool parseStrictObjectId(const std::string& rawObjectId, uint32_t* objectId) {
    if (objectId == nullptr || rawObjectId.empty()) {
        return false;
    }
    uint32_t parsedValue = 0U;
    for (const char character : rawObjectId) {
        if (character < '0' || character > '9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(character - '0');
        if (parsedValue > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
            return false;
        }
        parsedValue = parsedValue * 10U + digit;
    }
    *objectId = parsedValue;
    return true;
}

inline bool isNumericObjectIdAlias(const std::string& resolvedRawObjectId,
                                   uint32_t resolvedNumericObjectId,
                                   const std::string& candidateRawObjectId) {
    if (candidateRawObjectId == resolvedRawObjectId) {
        return false;
    }
    uint32_t candidateNumericObjectId = 0U;
    return parseStrictObjectId(candidateRawObjectId, &candidateNumericObjectId) &&
           candidateNumericObjectId == resolvedNumericObjectId;
}

} // namespace MDXClient

#endif
