/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "CoMovementTracker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace MDXClient {

namespace {

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

double coMovementBoxContainment(bool carrierHasBox, double carrierLeft, double carrierTop,
                                double carrierRight, double carrierBottom, bool riderHasBox,
                                double riderLeft, double riderTop, double riderRight,
                                double riderBottom) {
    if (!carrierHasBox || !riderHasBox) {
        return 0.0;
    }
    const double riderArea = (riderRight - riderLeft) * (riderBottom - riderTop);
    if (!std::isfinite(riderArea) || riderArea <= 0.0) {
        return 0.0;
    }
    const double overlapWidth = std::min(carrierRight, riderRight) -
                                std::max(carrierLeft, riderLeft);
    const double overlapHeight = std::min(carrierBottom, riderBottom) -
                                 std::max(carrierTop, riderTop);
    if (overlapWidth <= 0.0 || overlapHeight <= 0.0) {
        return 0.0;
    }
    const double containment = (overlapWidth * overlapHeight) / riderArea;
    if (!std::isfinite(containment)) {
        return 0.0;
    }
    return std::min(containment, 1.0);
}

bool isValidCoMovementParams(const CoMovementParams& params) {
    // Upper bounds exist because every one of these relaxes the test: a distance
    // ceiling past a few metres, or an offset tolerance past a metre, stops
    // describing carriage at all and would suppress ordinary proximity stops.
    if (!std::isfinite(params.maxDistanceMeters) || params.maxDistanceMeters <= 0.0 ||
        params.maxDistanceMeters > 4.0) {
        return false;
    }
    if (!std::isfinite(params.maxOffsetSdMeters) || params.maxOffsetSdMeters <= 0.0 ||
        params.maxOffsetSdMeters > 1.0) {
        return false;
    }
    if (params.windowMs < 200 || params.windowMs > 30000) {
        return false;
    }
    if (params.minSamples < 2 || params.minSamples > 1000) {
        return false;
    }
    if (params.minSpanMs < 0 || params.minSpanMs > params.windowMs) {
        return false;
    }
    if (!std::isfinite(params.minCarrierTravelMeters) || params.minCarrierTravelMeters < 0.0 ||
        params.minCarrierTravelMeters > 50.0) {
        return false;
    }
    // Zero disables the substitution. A containment requirement below half the
    // rider's box would admit pairs that merely clip each other.
    if (!std::isfinite(params.minBoxContainment) || params.minBoxContainment < 0.0 ||
        params.minBoxContainment > 1.0 ||
        (params.minBoxContainment > 0.0 && params.minBoxContainment < 0.5)) {
        return false;
    }
    if (params.enterFrames < 1 || params.enterFrames > 1000) {
        return false;
    }
    if (params.exitFrames < 1 || params.exitFrames > 1000) {
        return false;
    }
    if (params.latchMs < 0 || params.latchMs > 30000) {
        return false;
    }
    return true;
}

double CoMovementTracker::medianContainment(const std::deque<Sample>& window) {
    if (window.empty()) {
        return 0.0;
    }
    std::vector<double> values;
    values.reserve(window.size());
    for (const Sample& sample : window) {
        values.push_back(sample.boxContainment);
    }
    const size_t middle = values.size() / 2U;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return values[middle];
}

CoMovementTracker::Verdict CoMovementTracker::evaluate(const State& state,
                                                       const CoMovementParams& params,
                                                       CoMovementMeasurement* measurement) {
    const std::deque<Sample>& window = state.window;
    if (window.empty()) {
        return Verdict::kIndeterminate;
    }
    const int64_t spanMs = window.back().atMs - window.front().atMs;
    if (static_cast<int32_t>(window.size()) < params.minSamples || spanMs < params.minSpanMs) {
        return Verdict::kIndeterminate;
    }

    double sumOffsetX = 0.0;
    double sumOffsetY = 0.0;
    double minCarrierX = window.front().carrierX;
    double maxCarrierX = window.front().carrierX;
    double minCarrierY = window.front().carrierY;
    double maxCarrierY = window.front().carrierY;
    bool withinCeiling = true;
    for (const Sample& sample : window) {
        if (sample.distance > params.maxDistanceMeters) {
            withinCeiling = false;
        }
        sumOffsetX += sample.offsetX;
        sumOffsetY += sample.offsetY;
        minCarrierX = std::min(minCarrierX, sample.carrierX);
        maxCarrierX = std::max(maxCarrierX, sample.carrierX);
        minCarrierY = std::min(minCarrierY, sample.carrierY);
        maxCarrierY = std::max(maxCarrierY, sample.carrierY);
    }

    const double count = static_cast<double>(window.size());
    const double meanOffsetX = sumOffsetX / count;
    const double meanOffsetY = sumOffsetY / count;
    double sumSquaredDeviation = 0.0;
    for (const Sample& sample : window) {
        const double deviationX = sample.offsetX - meanOffsetX;
        const double deviationY = sample.offsetY - meanOffsetY;
        sumSquaredDeviation += (deviationX * deviationX) + (deviationY * deviationY);
    }
    // Root-mean-square deviation of the offset vector from its own mean: one
    // number for how much the rider moved relative to the carrier.
    const double offsetSd = std::sqrt(sumSquaredDeviation / count);

    // Diagonal of the box the carrier's path swept. Enough to tell "moved" from
    // "parked" without retaining per-sample deltas.
    const double travelX = maxCarrierX - minCarrierX;
    const double travelY = maxCarrierY - minCarrierY;
    const double carrierTravel = std::sqrt((travelX * travelX) + (travelY * travelY));

    const double containment = medianContainment(window);
    const bool travelSatisfied = carrierTravel >= params.minCarrierTravelMeters;
    const bool containmentSatisfied = params.minBoxContainment > 0.0 &&
                                      containment >= params.minBoxContainment;

    if (measurement != nullptr) {
        measurement->offsetSd = offsetSd;
        measurement->carrierTravel = carrierTravel;
        measurement->distance = window.back().distance;
        measurement->boxContainment = containment;
        measurement->samples = static_cast<int32_t>(window.size());
        measurement->spanMs = spanMs;
        measurement->travelWaivedByBox = !travelSatisfied && containmentSatisfied;
    }

    if (!std::isfinite(offsetSd) || !std::isfinite(carrierTravel)) {
        return Verdict::kFail;
    }
    const bool pass = withinCeiling && offsetSd <= params.maxOffsetSdMeters &&
                      (travelSatisfied || containmentSatisfied);
    return pass ? Verdict::kPass : Verdict::kFail;
}

void CoMovementTracker::dropStalePairs(int64_t nowMs) {
    // A pair is forgotten once it can no longer influence anything: its window
    // has aged out and any latched verdict has expired.
    //
    // Each entry is judged against the retention recorded for it, not against
    // the rule that happens to be calling. One map holds the pairs of every
    // co-movement group, and each group carries its own windowMs and latchMs,
    // so a single sweep using the caller's values would delete another group's
    // still-live history: a rule near the windowMs floor of 200 ms would drop
    // pairs belonging to a 10 s rule after 200 ms unseen, costing that rule its
    // whole accumulated window every time detection stuttered. The type-pair
    // uniqueness check in validateEventMapping keeps two rules off the same
    // pair of types, but it does not make their retention periods agree.
    for (std::map<Key, State>::iterator it = states_.begin(); it != states_.end();) {
        if ((nowMs - it->second.lastSeenMs) > it->second.staleAfterMs) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CoMovementTracker::observe(const std::string& sensorId, uint32_t carrierId,
                                uint32_t riderId, const CoMovementObservation& observation,
                                const CoMovementParams& params,
                                CoMovementMeasurement* measurement) {
    if (!std::isfinite(observation.distance) || !std::isfinite(observation.carrierX) ||
        !std::isfinite(observation.carrierY) || !std::isfinite(observation.riderX) ||
        !std::isfinite(observation.riderY)) {
        return false;
    }

    const int64_t nowMs = steadyNowMs();
    dropStalePairs(nowMs);

    const Key key{sensorId, carrierId, riderId};
    std::map<Key, State>::iterator it = states_.find(key);
    if (it == states_.end()) {
        if (states_.size() >= kMaxTrackedPairs) {
            return false;
        }
        it = states_.insert(std::make_pair(key, State())).first;
    }
    State& state = it->second;
    // Recorded before the window is touched so the entry is never visible to a
    // sweep with the default 0, which would prune it on the next observation.
    state.staleAfterMs = static_cast<int64_t>(params.windowMs) +
                         static_cast<int64_t>(params.latchMs);

    Sample sample;
    sample.atMs = nowMs;
    sample.offsetX = observation.riderX - observation.carrierX;
    sample.offsetY = observation.riderY - observation.carrierY;
    sample.carrierX = observation.carrierX;
    sample.carrierY = observation.carrierY;
    sample.distance = observation.distance;
    sample.boxContainment = observation.boxContainment;

    state.lastSeenMs = nowMs;
    state.window.push_back(sample);
    while (!state.window.empty() &&
           (nowMs - state.window.front().atMs) > static_cast<int64_t>(params.windowMs)) {
        state.window.pop_front();
    }

    const Verdict verdict = evaluate(state, params, measurement);
    switch (verdict) {
        case Verdict::kPass:
            state.failStreak = 0;
            if (state.passStreak < params.enterFrames) {
                ++state.passStreak;
            }
            if (state.passStreak >= params.enterFrames) {
                state.reported = true;
                state.reportedUntilMs = nowMs + static_cast<int64_t>(params.latchMs);
            }
            break;
        case Verdict::kFail:
            state.passStreak = 0;
            if (state.failStreak < params.exitFrames) {
                ++state.failStreak;
            }
            if (state.failStreak >= params.exitFrames) {
                state.reported = false;
                state.reportedUntilMs = 0;
            }
            break;
        case Verdict::kIndeterminate:
        default:
            // Neither confirm nor withdraw. The latch below is what eventually
            // clears a verdict whose pair stopped producing usable windows.
            state.passStreak = 0;
            break;
    }

    if (state.reported && nowMs > state.reportedUntilMs) {
        state.reported = false;
    }
    return state.reported;
}

} // namespace MDXClient
