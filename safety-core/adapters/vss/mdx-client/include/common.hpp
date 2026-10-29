/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MDX_CLIENT_COMMON_HPP
#define MDX_CLIENT_COMMON_HPP

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <cstdint>

#define IDENTIFIER_NAME_LENGTH 64
#define MAX_OBJECT_TYPE_LENGTH 32
#define MAX_DIRECTION_LENGTH 32
#define MAX_EVENT_TYPE_LENGTH 32
#define MAX_MESSAGE_SOURCE_LENGTH 16
#define MAX_END_TIMESTAMP_LENGTH 32
#define MAX_COORDINATES_COUNT 10

void sig_segv_handler(int32_t sig);

struct ObjectInfo {
    float confidence;
    char type[MAX_OBJECT_TYPE_LENGTH];
};

struct Coordinate {
    float x;
    float y;
};

// Internal candidate category. It is never serialized into the PSS event.
enum class AlertCandidateKind : uint8_t {
    kNone = 0,
    kRestrictedRoi,
    kConfinedRoi,
    kFrameSocial,
    kProximityPair,
    // One object is being carried by another. Like kProximityPair it describes
    // two physical objects, but it is a judgement over a window of frames rather
    // than a measurement of the current one, and it is kept separate so a
    // proximity rule can never consume it or vice versa.
    kCoMovementPair,
};

// Internal pair outcome. It is never serialized into the PSS event.
enum class ProximitySelection : uint8_t {
    kNone = 0,
    kUnknown,
    kNoViolation,
    kThresholdViolation,
};

// What a co-movement window measured, carried on the candidate for logging.
// Meaningful only when candidateKind is kCoMovementPair.
struct CoMovementEvidence {
    float offsetSd;
    float carrierTravel;
    float boxContainment;
    bool travelWaivedByBox;
};

// Unified alert message from mdx-events or mdx-frames (same as ATL, with optional violation flags)
struct AlertMessage {
    char sensorId[IDENTIFIER_NAME_LENGTH];
    char type[MAX_OBJECT_TYPE_LENGTH];
    char direction[MAX_DIRECTION_LENGTH];
    char ruleId[IDENTIFIER_NAME_LENGTH];
    char endTimestamp[MAX_END_TIMESTAMP_LENGTH];
    uint32_t id;
    uint64_t frameOrdinal;  /* mdx-frames parser ordinal; zero for mdx-events */
    uint32_t objectId;
    uint32_t objectId2;  /* Second object ID for proximity (pair); 0 when single-object */
    ObjectInfo object;
    ObjectInfo object2;  /* Second object info for proximity (pair); type used for objectType[1] */
    float speed;
    char eventType[MAX_EVENT_TYPE_LENGTH];
    char messageSource[MAX_MESSAGE_SOURCE_LENGTH];
    bool restrictedAreaViolation;   // mdx-frames: from rois (TypeCount)
    bool confinedAreaViolation;     // mdx-frames: from rois (TypeCount)
    bool socialDistancingViolation; // mdx-frames: from FrameMessage.socialDistancing (SD.proximityDetections)
    AlertCandidateKind candidateKind;
    ProximitySelection proximitySelection;
    double proximityThreshold;      // Valid only when proximitySelection is kThresholdViolation
    CoMovementEvidence coMovement;  // Valid only when candidateKind is kCoMovementPair
    Coordinate coordinates[MAX_COORDINATES_COUNT];
    int coordCount;
};

inline void sig_segv_handler(int32_t sig) {
    const char msg[] = "SEGFAULT: Terminating application\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(128 + sig);
}

#endif
