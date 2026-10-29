/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAFETY_EVENT_REPORTER_H
#define SAFETY_EVENT_REPORTER_H

#include "pss_protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// SAIM event causes, carried in SafetyEvent.cause so PSS logs and the SDMs can
// attribute why a sensor changed state. The verdict stays in EventType
// (SENSOR_INVALID/VALID).
//
// Addressable objects rather than bare enum constants: the reporter API takes
// a const EventCause*, where nullptr means "preserve the slot's current cause".
// A pointer keeps that meaning without a sentinel enum value that could reach
// the wire.
// SENSOR_INVALID causes:
constexpr EventCause kSaimInputDegraded     = CAUSE_SAIM_INPUT_DEGRADED;    // frame-quality score below threshold
constexpr EventCause kSaimInternalError     = CAUSE_SAIM_INTERNAL_ERROR;    // analyzer init / per-frame compute failure
constexpr EventCause kSaimStreamDisconnect  = CAUSE_SAIM_STREAM_DISCONNECT; // stream lost / reconnect / no keyframe at start-up
constexpr EventCause kSaimFrameDrop         = CAUSE_SAIM_FRAME_DROP;        // FU-A NAL drops
constexpr EventCause kSaimInit              = CAUSE_SAIM_INIT;              // first-registration fail-safe seed (pre-frames)
constexpr EventCause kSaimUnknown           = CAUSE_SAIM_UNKNOWN;           // defensive fallback: INVALID with missing/contradictory cause
// SENSOR_VALID cause:
constexpr EventCause kSaimSensorHealthy     = CAUSE_SAIM_SENSOR_HEALTHY;    // recovery edge (sustained good streak)

// Two constraints a new SAIM cause has to satisfy. Neither is visible from this
// file alone, and both are load-bearing:
//
// 1. kSaimInit is the ONLY value any SDM treats as benign. Every other cause on
//    a SENSOR_INVALID sets the sensor's failed bit and contributes to the
//    all-safety-relevant-sensors-failed latch. Reporting a new cause that is
//    "not really a failure" therefore latches. Emit SENSOR_VALID, or extend the
//    SDM suppression deliberately -- do not assume a new cause is inert.
//
// 2. EventCause is a wire field shared with every SDM. A new value must be
//    added to pss_protocol.h, which bumps PSS_SCHEMA_VERSION and forces all
//    PSF components to ship together. It cannot be added here alone.

/*
 * Per-camera state-mirror reporter. Each registered sensor owns one slot
 * holding its latest state. update_slot() is non-blocking and overwrites
 * the slot; one worker thread round-robin drains dirty slots and forwards
 * to PSS via NvPSSReportSafetyEvent. No queue, no overflow, bounded memory.
 *
 * A 30s reinforcement timer re-asserts each sensor's current state for
 * self-healing after PSS restarts.
 */
class SafetyEventReporter {
public:
    static constexpr std::chrono::seconds DEFAULT_REINFORCE_INTERVAL{30};

    explicit SafetyEventReporter(uint32_t pssClientId,
                                 std::chrono::seconds reinforce_interval = DEFAULT_REINFORCE_INTERVAL);
    ~SafetyEventReporter();

    SafetyEventReporter(const SafetyEventReporter&) = delete;
    SafetyEventReporter& operator=(const SafetyEventReporter&) = delete;
    SafetyEventReporter(SafetyEventReporter&&) = delete;
    SafetyEventReporter& operator=(SafetyEventReporter&&) = delete;

    // Spawn the worker thread. Not safe to call concurrently with stop().
    void start();

    // Idempotent. Wakes the worker for one final drain, then joins.
    // If called from the reporter thread itself (e.g., terminate handler
    // triggered inside runLoop), detaches instead of self-joining (which
    // is undefined behavior).
    void stop();

    // Idempotent. Sets the stop flag and wakes the worker; does NOT join.
    // Use from fatal/emergency paths that cannot block and may run on the
    // reporter thread. The worker exits within one drain pass after this
    // call (runLoop checks stopFlag_ unconditionally).
    void requestStop();

    // invalidFlag must point to the decoder's sensorInvalid_ atomic and
    // outlive this reporter. allocEventId must be a non-empty callable that
    // returns the next unique SafetyEvent.id for this sensor. Returns false
    // if slot allocation fails or the callback is empty.
    // First registration seeds a dirty SENSOR_INVALID and wakes the worker;
    // re-registration only rebinds the callbacks (live state preserved).
    bool registerSensor(const std::string& sensorIdentifier,
                        const std::atomic<bool>* invalidFlag,
                        uint8_t pipelineId,
                        std::function<uint32_t()> allocEventId);

    /* Producer entry point; safe from any thread. Drops the call when the
     * sensor is unregistered, or when ts_monotonic_ns < slot.timestamp_ns
     * (staleAtProducer_++). Otherwise overwrites the slot and wakes the
     * worker. */
    void update_slot(const std::string& sensorIdentifier,
                     EventType state,
                     uint64_t  ts_monotonic_ns,
                     float     confidence,
                     uint32_t  eventId,
                     const EventCause* eventCause);

    // Telemetry counter snapshots.
    uint64_t sentCount()            const { return sent_.load(std::memory_order_relaxed);            }
    uint64_t reportFailedCount()    const { return reportFailed_.load(std::memory_order_relaxed);    }
    uint64_t staleAtProducerCount() const { return staleAtProducer_.load(std::memory_order_relaxed); }
    uint64_t reinforcementCount()   const { return reinforcement_.load(std::memory_order_relaxed);   }

private:
    struct Slot {
        std::mutex mtx;
        EventType  state          = SENSOR_INVALID;
        EventCause eventCause = CAUSE_NONE;
        uint64_t   timestamp_ns   = 0;
        float      confidence     = 1.0f;
        uint32_t   lastEventId    = 0;
        uint8_t    pipelineId     = 0;
        const std::atomic<bool>* invalidFlag = nullptr;
        std::function<uint32_t()> allocEventId;
        std::atomic<bool> dirty{false};
    };

    void runLoop();
    bool sendWithRetry(const SafetyEvent& evt);
    SafetyEvent buildEventFromSlotLocked(const std::string& name, const Slot& s);
    void emitReinforcement();
    // Slot mutation + worker wake. Caller passes a Slot* obtained under
    // registryMtx_ or from a snapshot of sensors_; both stay valid for
    // the reporter's lifetime (sensors_ is append-only). Lets reinforcement
    // reuse its own snapshot without re-locking registryMtx_ per sensor.
    // eventCause == nullptr preserves the slot's current cause (reinforcement
    // re-asserts state without changing why the sensor is in that state).
    void update_slot_impl(Slot* slot,
                          EventType state,
                          uint64_t  ts_monotonic_ns,
                          float     confidence,
                          uint32_t  eventId,
                          const EventCause* eventCause);

    const uint32_t             pssClientId_;
    const std::chrono::seconds reinforceInterval_;

    // Append-only after registration. unique_ptr keeps each Slot's mutex
    // address stable across insertions; slot pointers obtained under
    // registryMtx_ stay valid for the reporter's lifetime
    std::mutex registryMtx_;
    std::map<std::string, std::unique_ptr<Slot>> sensors_;

    // Bounded exponential backoff for transient PSS send failures.
    // kInitial is the first retry delay after a fresh failure run; kMax caps
    // the schedule so a sustained PSS outage never sleeps longer than this
    // before the next retry attempt. Both fields below
    // (nextRetryDeadline_/retryBackoff_) are protected by cvMtx_.
    static constexpr std::chrono::milliseconds kInitialRetryBackoff{100};
    static constexpr std::chrono::milliseconds kMaxRetryBackoff{5000};

    // Protects stopFlag_/haveWork_/nextRetryDeadline_/retryBackoff_;
    // cv_ wakes the worker.
    std::mutex              cvMtx_;
    std::condition_variable cv_;
    bool                    stopFlag_ = false;
    bool                    haveWork_ = false;
    // ::max() == "no retry pending"; runLoop reads this under cvMtx_ to
    // shorten the wait_until deadline when a previous send failed.
    std::chrono::steady_clock::time_point nextRetryDeadline_ =
        std::chrono::steady_clock::time_point::max();
    std::chrono::milliseconds retryBackoff_ = kInitialRetryBackoff;

    std::thread       thread_;
    std::atomic<bool> running_{false};

    // Worker-only reusable snapshot buffers. Storing const std::string*
    // avoids per-wake key copies; the pointee is stable (sensors_ append-only).
    std::vector<std::pair<const std::string*, Slot*>> drainSnap_;
    std::vector<Slot*> reinforceSnap_;

    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> reportFailed_{0};
    std::atomic<uint64_t> staleAtProducer_{0};
    std::atomic<uint64_t> reinforcement_{0};
};

#endif
