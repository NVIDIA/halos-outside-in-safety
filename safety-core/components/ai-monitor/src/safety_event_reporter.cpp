/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "safety_event_reporter.h"

#include "pss_daemon.h"
#include "pss_protocol.h"
#include "sai_common.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <new>
#include <utility>
#include <vector>

#include <pthread.h>

constexpr std::chrono::seconds      SafetyEventReporter::DEFAULT_REINFORCE_INTERVAL;
constexpr std::chrono::milliseconds SafetyEventReporter::kInitialRetryBackoff;
constexpr std::chrono::milliseconds SafetyEventReporter::kMaxRetryBackoff;

SafetyEventReporter::SafetyEventReporter(uint32_t pssClientId,
                                         std::chrono::seconds reinforce_interval)
    : pssClientId_(pssClientId),
      reinforceInterval_(reinforce_interval)
{}

SafetyEventReporter::~SafetyEventReporter() {
    stop();
}

// start()/requestStop() hold cvMtx_ across both the running_ CAS and the
// stopFlag_ write so a concurrent stop cannot land between them and have
// its stopFlag_=true silently clobbered by start()'s reset.
void SafetyEventReporter::start() {
    std::lock_guard<std::mutex> lk(cvMtx_);
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopFlag_ = false;
    haveWork_ = false;
    nextRetryDeadline_ = std::chrono::steady_clock::time_point::max();
    retryBackoff_      = kInitialRetryBackoff;
    thread_ = std::thread(&SafetyEventReporter::runLoop, this);
}

void SafetyEventReporter::requestStop() {
    {
        std::lock_guard<std::mutex> lk(cvMtx_);
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) return;
        stopFlag_ = true;
        haveWork_ = true;
    }
    cv_.notify_all();
}

void SafetyEventReporter::stop() {
    requestStop();
    // Move thread_ out under cvMtx_ to serialize with start(); detach on
    // self-join (terminate handler firing inside runLoop) to avoid UB.
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(cvMtx_);
        t = std::move(thread_);
    }
    if (!t.joinable()) return;
    if (t.get_id() == std::this_thread::get_id()) {
        t.detach();
        return;
    }
    t.join();
}

bool SafetyEventReporter::registerSensor(const std::string& sensorIdentifier,
                                         const std::atomic<bool>* invalidFlag,
                                         uint8_t pipelineId,
                                         std::function<uint32_t()> allocEventId) {
    if (!allocEventId) {
        std::cerr << "[SAI reporter] registerSensor: allocEventId callback "
                     "is empty for sensor " << sensorIdentifier << "\n";
        return false;
    }
    if (invalidFlag == nullptr) {
        std::cerr << "[SAI reporter] registerSensor: invalidFlag is null "
                     "for sensor " << sensorIdentifier << "\n";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(registryMtx_);
        auto it = sensors_.find(sensorIdentifier);
        if (it != sensors_.end()) {
            // Re-registration: rebind callbacks only; live state preserved.
            std::lock_guard<std::mutex> slk(it->second->mtx);
            it->second->pipelineId   = pipelineId;
            it->second->invalidFlag  = invalidFlag;
            it->second->allocEventId = std::move(allocEventId);
            return true;
        }
        std::unique_ptr<Slot> slot(new (std::nothrow) Slot());
        if (!slot) {
            std::cerr << "[SAI reporter] Failed to allocate slot for sensor "
                      << sensorIdentifier << "\n";
            return false;
        }
        slot->pipelineId   = pipelineId;
        slot->invalidFlag  = invalidFlag;
        slot->allocEventId = std::move(allocEventId);
        // Fail-safe seed: PSS gets SENSOR_INVALID on the next drain pass
        // instead of waiting reinforceInterval_ for the first reinforcement.
        slot->state        = SENSOR_INVALID;
        snprintf(slot->eventCause, MAX_INDENTIFIER_LENGTH, "%s", SAIM_INIT); // pre-frame fail-safe cause
        slot->timestamp_ns = monotonic_now_ns();
        slot->confidence   = 1.0f;
        slot->lastEventId  = slot->allocEventId();
        slot->dirty.store(true, std::memory_order_release);
        sensors_.emplace(sensorIdentifier, std::move(slot));
    }
    // Notify outside registryMtx_ to avoid contention with worker snapshot.
    {
        std::lock_guard<std::mutex> lk(cvMtx_);
        haveWork_ = true;
    }
    cv_.notify_one();
    return true;
}

// Public name-keyed entry for external producers; the _impl below is the
// shared mutation+wake body that the reporter's reinforcement pass calls
// directly with cached Slot* pointers to skip the per-sensor lookup.
void SafetyEventReporter::update_slot(const std::string& sensorIdentifier,
                                      EventType state,
                                      uint64_t  ts_monotonic_ns,
                                      float     confidence,
                                      uint32_t  eventId,
                                      const char* eventCause) {
    // Safe to dereference `slot` after releasing registryMtx_: sensors_ is
    // append-only (no erase/clear anywhere) and unique_ptr keeps each Slot's
    // address stable.
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lk(registryMtx_);
        auto it = sensors_.find(sensorIdentifier);
        if (it == sensors_.end()) return;
        slot = it->second.get();
    }
    update_slot_impl(slot, state, ts_monotonic_ns, confidence, eventId, eventCause);
}

void SafetyEventReporter::update_slot_impl(Slot* slot,
                                           EventType state,
                                           uint64_t  ts_monotonic_ns,
                                           float     confidence,
                                           uint32_t  eventId,
                                           const char* eventCause) {
    if (slot == nullptr) return;
    {
        std::lock_guard<std::mutex> slk(slot->mtx);

        // ts == 0 signals a clock_gettime failure. Clamp to the slot's
        // last-good timestamp so the (safety-critical) state transition
        // still reaches PSS without polluting the monotonic ordering
        // invariant for subsequent updates.
        const uint64_t ts = (ts_monotonic_ns == 0) ? slot->timestamp_ns
                                                   : ts_monotonic_ns;

        if (ts < slot->timestamp_ns) {
            staleAtProducer_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        slot->state        = state;
        slot->timestamp_ns = ts;
        slot->confidence   = confidence;
        slot->lastEventId  = eventId;
        // Copy into the slot's owned buffer; nullptr preserves the current cause.
        if (eventCause != nullptr)
            snprintf(slot->eventCause, MAX_INDENTIFIER_LENGTH, "%s", eventCause);
        slot->dirty.store(true, std::memory_order_release);
    }

    // Self-notify from runLoop (reinforcement) is a no-op, but routing
    // reinforcement through this same path lets a racing producer overwrite
    // the reinforcement state and coalesce the two into one event.
    {
        std::lock_guard<std::mutex> lk(cvMtx_);
        haveWork_ = true;
    }
    cv_.notify_one();
}

SafetyEvent SafetyEventReporter::buildEventFromSlotLocked(const std::string& name,
                                              const Slot& s) {
    SafetyEvent evt = {};
    evt.id        = s.lastEventId;
    evt.type      = s.state;
    evt.timestamp = s.timestamp_ns;
    evt.confidenceLevel = s.confidence;
    evt.fusionMetadata.pipelineID = s.pipelineId;
    evt.fusionMetadata.clientID   = static_cast<uint8_t>(pssClientId_);
    evt.processed = false;
    snprintf(evt.sensorIdentifier, MAX_INDENTIFIER_LENGTH, "%s", name.c_str());
    // VALID always reports the healthy cause; INVALID keeps its real cause, else SAIM_UNKNOWN (never healthy).
    const char* cause = s.eventCause;
    if (s.state == SENSOR_VALID) {
        cause = SAIM_SENSOR_HEALTHY;
    } else if (cause[0] == '\0' ||
               std::strcmp(cause, SAIM_SENSOR_HEALTHY) == 0) {
        cause = SAIM_UNKNOWN;
    }
    snprintf(evt.ruleIdentifier, MAX_INDENTIFIER_LENGTH, "%s", cause);
    return evt;
}

bool SafetyEventReporter::sendWithRetry(const SafetyEvent& evt) {
    for (int attempt = 0; attempt <= MAX_PSS_REPORT_RETRIES; ++attempt) {
        if (NvPSSReportSafetyEvent(pssClientId_, &evt) == NVPSSD_SUCCESS)
            return true;
        if (attempt < MAX_PSS_REPORT_RETRIES) {
            std::cerr << "[SAI reporter] NvPSSReportSafetyEvent failed, retry "
                      << (attempt + 1) << "/" << MAX_PSS_REPORT_RETRIES << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(kPssReportRetryDelayMs));
        }
    }
    return false;
}

void SafetyEventReporter::emitReinforcement() {
    // Slot*-only snapshot; update_slot_impl takes the pointer directly.
    reinforceSnap_.clear();
    {
        std::lock_guard<std::mutex> lk(registryMtx_);
        reinforceSnap_.reserve(sensors_.size());
        for (auto it = sensors_.begin(); it != sensors_.end(); ++it) {
            reinforceSnap_.push_back(it->second.get());
        }
    }

    const uint64_t now_ns = monotonic_now_ns();
    for (size_t i = 0; i < reinforceSnap_.size(); ++i) {
        Slot* slot = reinforceSnap_[i];
        // Snapshot the flag pointer and the id allocator under slot->mtx
        // so a re-registration racing with this read sees a consistent
        // view; the pointed-to atomic and the captured decoder both
        // outlive the reporter.
        const std::atomic<bool>* flagPtr = nullptr;
        std::function<uint32_t()> alloc;
        {
            std::lock_guard<std::mutex> slk(slot->mtx);
            flagPtr = slot->invalidFlag;
            alloc   = slot->allocEventId;
        }
        if (flagPtr == nullptr || !alloc) continue;
        const bool inv = flagPtr->load(std::memory_order_acquire);
        // Direct _impl entry: Slot* is from our snapshot, no name lookup
        // needed. The self-notify inside is a harmless no-op on this thread.
        update_slot_impl(slot,
                         inv ? SENSOR_INVALID : SENSOR_VALID,
                         now_ns,
                         1.0f,
                         alloc(),
                         /*eventCause=*/nullptr);  // preserve the slot's cause
        reinforcement_.fetch_add(1, std::memory_order_relaxed);
    }
}

void SafetyEventReporter::runLoop() {
    const int nameErr = pthread_setname_np(pthread_self(), "saim-reporter");
    if (nameErr != 0) {
        std::cerr << "[SAI reporter] Failed to set thread name: "
                  << std::strerror(nameErr) << "\n";
    }

    auto lastReinforce = std::chrono::steady_clock::now();
    size_t rrOffset = 0;

    while (true) {
        // Sleep until the earliest of: (a) producer signal, (b) reinforcement
        // deadline, (c) backoff retry deadline (set after a failed send), or
        // (d) stop(). nextRetryDeadline_ defaults to time_point::max() so the
        // min selects reinforceDeadline whenever no retry is pending.
        {
            std::unique_lock<std::mutex> lk(cvMtx_);
            const auto reinforceDeadline = lastReinforce + reinforceInterval_;
            const auto deadline = std::min(reinforceDeadline, nextRetryDeadline_);
            cv_.wait_until(lk, deadline,
                           [this]{ return stopFlag_ || haveWork_; });
            haveWork_ = false;
        }

        // Snapshot the registry for the drain pass.
        drainSnap_.clear();
        {
            std::lock_guard<std::mutex> lk(registryMtx_);
            drainSnap_.reserve(sensors_.size());
            for (auto it = sensors_.begin(); it != sensors_.end(); ++it) {
                drainSnap_.push_back({&it->first, it->second.get()});
            }
        }

        bool anyFailedThisPass    = false;
        bool anySucceededThisPass = false;
        const size_t n = drainSnap_.size();
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = (rrOffset + i) % n;
            const std::string& name = *drainSnap_[idx].first;
            Slot* slot = drainSnap_[idx].second;

            // Snapshot under slot->mtx, then send outside the lock.
            // sendWithRetry performs blocking I/O (NvPSSReportSafetyEvent
            // plus short retry sleeps).
            SafetyEvent evt;
            bool hasWork = false;
            {
                std::lock_guard<std::mutex> slk(slot->mtx);
                if (slot->dirty.exchange(false, std::memory_order_acq_rel)) {
                    evt = buildEventFromSlotLocked(name, *slot);
                    hasWork = true;
                }
            }
            if (!hasWork) continue;

            if (sendWithRetry(evt)) {
                sent_.fetch_add(1, std::memory_order_relaxed);
                anySucceededThisPass = true;
            } else {
                reportFailed_.fetch_add(1, std::memory_order_relaxed);
                // Re-arm so the next drain pass retries this slot. retryBackoff_
                //  below schedules a sooner retry without spinning.
                {
                    std::lock_guard<std::mutex> slk(slot->mtx);
                    slot->dirty.store(true, std::memory_order_release);
                }
                anyFailedThisPass = true;
                std::cerr << "[SAI reporter] " << name
                          << ": send failed after " << MAX_PSS_REPORT_RETRIES
                          << " retries; will retry on backoff schedule\n";
            }
        }
        if (n > 0) rrOffset = (rrOffset + 1) % n;

        // nextRetryDeadline_ is the forward-looking next-retry wake (not a
        // last-attempt timestamp): on failure, shorten the wait to 100ms->5s
        // backoff instead of the 30s reinforce interval. Accelerates recovery.
        {
            std::lock_guard<std::mutex> lk(cvMtx_);
            if (anyFailedThisPass) {
                const auto bo = retryBackoff_;
                nextRetryDeadline_ = std::chrono::steady_clock::now() + bo;
                retryBackoff_ = std::min(bo * 2, kMaxRetryBackoff);
            } else if (anySucceededThisPass) {
                nextRetryDeadline_ =
                    std::chrono::steady_clock::time_point::max();
                retryBackoff_ = kInitialRetryBackoff;
            }
        }

        bool stopRequested;
        {
            std::lock_guard<std::mutex> lk(cvMtx_);
            stopRequested = stopFlag_;
        }

        // Skip reinforcement during shutdown to keep stop() bounded.
        if (!stopRequested &&
            std::chrono::steady_clock::now() - lastReinforce >= reinforceInterval_) {
            emitReinforcement();
            lastReinforce = std::chrono::steady_clock::now();
        }

        // Bounded shutdown: exit after one drain pass once stopFlag_ is
        // observed. We do not wait for haveWork_ to drain because producers
        // racing with stop() could keep haveWork_ true indefinitely.
        if (stopRequested) break;
    }
}
