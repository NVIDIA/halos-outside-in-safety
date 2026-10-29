/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Decides whether one tracked object is being carried by another -- a person
 * riding the forklift they are standing on, rather than a person who happens to
 * be next to it. A consumer suppresses a stop on this verdict, so the test is
 * built to fail closed: every criterion must be met, and anything ambiguous
 * (missing data, a window too short to judge, an inverted bounding box) reports
 * no co-movement and therefore leaves the stop in place.
 *
 * Why the offset vector is the measurement. Distance alone cannot separate the
 * two cases: a forklift driving past a stationary person sweeps around them at a
 * nearly constant distance. The vector from carrier to rider, however, is
 * effectively fixed for someone standing on the vehicle and re-arranges
 * constantly for anyone on foot. Measured on this deployment's frames, riders
 * held the offset to a standard deviation of 0.06-0.13m while pairs on foot sat
 * at 0.42m and above, which is the margin the default tolerance is set against.
 *
 * Why carrier travel is required. A steady offset is equally characteristic of
 * somebody standing beside a parked vehicle, and roughly half of all critical
 * proximity events in this footage occur with the forklift stationary. Requiring
 * the carrier to have covered ground is what keeps those events as stops.
 * Sustained image-space containment of the rider's box within the carrier's may
 * stand in for that travel where configured, since a rider is drawn inside the
 * vehicle whether or not it is moving.
 *
 * Residual limitation, inherent to a geometry-only test: somebody walking in
 * lockstep beside a moving forklift, holding position for longer than the
 * window, is indistinguishable from a rider. Separating those two needs a cue
 * this pipeline's frames do not carry -- per-object speed and direction are
 * unpopulated and z is always zero under the 2D profile.
 *
 * State lives here rather than in the consumer because this is the only stage
 * that sees every frame: sampling at the full frame rate is what makes the
 * offset deviation measurable at all.
 */

#ifndef MDX_CLIENT_CO_MOVEMENT_TRACKER_HPP
#define MDX_CLIENT_CO_MOVEMENT_TRACKER_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace MDXClient {

// Compiled from a co-movement rule once at startup. Every field has a default
// that reflects the measurements described above, so a rule may set none of them.
struct CoMovementParams {
    // Deliberately at the proximity critical threshold rather than at the 1.5m
    // the footage supports: the SDM suppresses an operator pair only in the
    // critical tier, so recognising a rider past that threshold would place them
    // in the warning band and let them slow their own vehicle. Keep this in step
    // with distance_threshold_meters on the critical rule in the event mapping.
    double maxDistanceMeters = 1.0;
    double maxOffsetSdMeters = 0.30;
    int32_t windowMs = 2000;
    int32_t minSamples = 4;
    int32_t minSpanMs = 400;
    double minCarrierTravelMeters = 0.15;
    // Zero disables the substitution, leaving carrier travel unconditionally
    // required.
    double minBoxContainment = 0.0;
    int32_t enterFrames = 3;
    int32_t exitFrames = 5;
    int32_t latchMs = 1500;
};

// One frame's view of a candidate pair, in the caller's units: ground
// coordinates in metres, containment as a fraction of the rider's box area.
struct CoMovementObservation {
    double carrierX = 0.0;
    double carrierY = 0.0;
    double riderX = 0.0;
    double riderY = 0.0;
    double distance = 0.0;
    // Zero when either bounding box was absent, which cannot satisfy a
    // containment requirement.
    double boxContainment = 0.0;
};

// What the window measured, for logging and for the emitted event.
struct CoMovementMeasurement {
    double offsetSd = 0.0;
    double carrierTravel = 0.0;
    double distance = 0.0;
    double boxContainment = 0.0;
    int32_t samples = 0;
    int64_t spanMs = 0;
    bool travelWaivedByBox = false;
};

// Not thread safe: one instance is owned by the frames parser and touched only
// from the frames thread.
class CoMovementTracker {
public:
    // Records one observation of the pair and returns whether it is currently
    // reported as co-moving. Must be called for every frame in which both
    // objects are visible and plausibly close, including frames that fail the
    // criteria, otherwise the window is not continuous and the hysteresis below
    // cannot settle.
    bool observe(const std::string& sensorId, uint32_t carrierId, uint32_t riderId,
                 const CoMovementObservation& observation, const CoMovementParams& params,
                 CoMovementMeasurement* measurement);

    // Bounded so that churn in the tracker's object IDs cannot grow this map
    // without limit. Both are generous relative to the ~5-9 pairs a warehouse
    // camera produces, and reaching either only costs detection latency.
    static constexpr size_t kMaxTrackedPairs = 512U;

private:
    struct Key {
        std::string sensorId;
        uint32_t carrierId = 0U;
        uint32_t riderId = 0U;

        bool operator<(const Key& other) const {
            if (sensorId != other.sensorId) return sensorId < other.sensorId;
            if (carrierId != other.carrierId) return carrierId < other.carrierId;
            return riderId < other.riderId;
        }
    };

    struct Sample {
        int64_t atMs = 0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double carrierX = 0.0;
        double carrierY = 0.0;
        double distance = 0.0;
        double boxContainment = 0.0;
    };

    struct State {
        std::deque<Sample> window;
        int32_t passStreak = 0;
        int32_t failStreak = 0;
        bool reported = false;
        int64_t reportedUntilMs = 0;
        int64_t lastSeenMs = 0;
        // Retention for this pair, from the rule that observed it. Held per
        // entry because one map serves every group: pruning with the caller's
        // rule would let the shortest-window rule in the mapping evict pair
        // history belonging to a longer-window one.
        int64_t staleAfterMs = 0;
    };

    // Tri-state because "not enough evidence yet" must not be counted as a
    // failure: doing so would withdraw a verdict every time detection stutters.
    enum class Verdict : uint8_t { kPass, kFail, kIndeterminate };

    static Verdict evaluate(const State& state, const CoMovementParams& params,
                            CoMovementMeasurement* measurement);
    // Median rather than mean, so that neither a momentary occlusion nor a single
    // dropped box decides whether containment counts as sustained.
    static double medianContainment(const std::deque<Sample>& window);
    void dropStalePairs(int64_t nowMs);

    std::map<Key, State> states_;
};

// Fraction of the rider's box area that lies inside the carrier's. Zero when
// either box is absent or degenerate.
double coMovementBoxContainment(bool carrierHasBox, double carrierLeft, double carrierTop,
                                double carrierRight, double carrierBottom, bool riderHasBox,
                                double riderLeft, double riderTop, double riderRight,
                                double riderBottom);

// Reject a rule whose parameters are outside the ranges the criteria are
// meaningful over, so a mistyped config cannot quietly widen what gets
// suppressed. Applied to present fields only; absent fields keep their default.
bool isValidCoMovementParams(const CoMovementParams& params);

} // namespace MDXClient

#endif
