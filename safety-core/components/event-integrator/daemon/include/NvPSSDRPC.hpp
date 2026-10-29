/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <deque>
#include <utility>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <map>
#include <vector>
#include <unordered_set>
#include <chrono>

#include "pss_daemon.h"
#include "NvPSSDRPCMsg.h"
#include "nvpsf_transport.h"

#define MAX_POSSIBLE_CLIENTS 100

/** Callback when a trust-report event is received. reporterClientType from registration (e.g. CLIENT_SAFETY_MONITOR). Returns true if accepted. */
typedef bool (*NvPSSDTrustReportCallback)(void* ctx, uint32_t clientId, uint8_t reporterClientType, const SafetyEvent* event);

/** Is this pipelineId one whose loss the SDMs latch on? See
 *  SafetyEventManager::IsRequiredSourcePipeline. Same callback+ctx shape as the
 *  trust-report hook, so the RPC server keeps its one-way dependency on the
 *  manager rather than gaining a direct pointer to it. */
typedef bool (*NvPSSDRequiredSourceCallback)(void* ctx, uint8_t pipelineId);

/*
 * How many CONSECUTIVE rejected reports from one required source raise
 * CAUSE_CLIENT_INPUT_UNUSABLE (nvpss.conf client_reject_error_threshold).
 *
 * Consecutive, not cumulative, and an accepted report resets the count. A client
 * that is merely noisy -- an occasional low-confidence detection, a rare
 * malformed frame -- never accumulates, because the fault this reports is an
 * input that has become UNUSABLE, not one that is imperfect. A cumulative count
 * would eventually fire on every long-running healthy deployment.
 *
 * The default is deliberately well above any plausible burst of bad frames while
 * still being reached in seconds by a client that has genuinely broken: a
 * perception client reports per frame at roughly 30 Hz, so 50 consecutive
 * rejections is under two seconds of continuous garbage.
 */
#define NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_MIN     3U
#define NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_MAX     10000U
#define NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_DEFAULT 50U


namespace nvpss
{

class NvPSSDRPC
{

private:
    const NvPSSDRPCBackend backend;
    const std::string channel;
    const uint8_t maxClients;
    const uint8_t maxPendingClients;

    /*
     * AF_UNIX SOCK_SEQPACKET endpoint replacing the raw listener and the
     * descriptor table. The transport owns accept, credential validation
     * and the bounded pool of accepted-but-unregistered peers, so the
     * FD_SETSIZE ceiling that used to bound this server is gone -- with
     * select() a client whose descriptor happened to land above 1024 was
     * refused for a reason that had nothing to do with capacity.
     */
    NvPSFTransport* transport;
    /** clientId -> transport peer. NvPSFPeerId carries a generation counter, so
     *  an identity naming a recycled slot is detected rather than acted on. */
    NvPSFPeerId clientPeers[MAX_POSSIBLE_CLIENTS];
    /** Incremented on each admission; processPendingDisconnects skips stale entries. */
    std::atomic<uint32_t> slotGeneration_[MAX_POSSIBLE_CLIENTS];

    std::thread rpcServerThread;
    /** True while RPC server thread is running (set true when thread started, false in Close). Run loop and isRpcServerRunning() both use this. */
    std::atomic<bool> runRpcServer{false};

    std::map<uint32_t, std::chrono::steady_clock::time_point> clientLastHeartbeat;
    std::map<uint32_t, uint32_t> clientHeartbeatCount;
    std::map<uint32_t, uint8_t> clientTypeMap;
    /** MDX instance id per slot; absent or 0 = anonymous. Under heartbeatMutex. */
    std::map<uint32_t, uint8_t> clientInstanceMap_;
    uint8_t clientInstanceLocked(uint32_t clientId) const;

    /** PSS_PRESENCE_* set nvpss.conf requires: required_safety_monitor,
     *  required_perception_monitor, and MDX instances 1..required_mdx_instances. */
    uint32_t requiredSourceMask_{0U};
    /** When this PSS started accepting clients: a required source's absence is
     *  counted from here, as a missed heartbeat would be. */
    std::chrono::steady_clock::time_point rpcStartTime_{std::chrono::steady_clock::now()};
    /** PSS_PRESENCE_* bits, all under heartbeatMutex:
     *   everRegisteredMask_      tracked sources registered since this PSS started;
     *                            decides never_registered only
     *   everPresentMask_         tracked sources that have HEARTBEATED since
     *                            registering, at least once since this PSS started.
     *                            This, not the registration, is what makes a source
     *                            expected: a source registered but not yet heartbeated
     *                            cannot be present, so expecting it would let a report
     *                            built in between raise a loss on a healthy start
     *   requiredSourcesDue_      the heartbeat budget since start has run out, so the
     *                            required set now counts as expected
     *   neverRegisteredRaised_   required-but-never-registered already raised
     *   neverRegisteredPending_  raised by the tick, waiting for the RPC thread,
     *                            which owns the input queue */
    uint32_t everRegisteredMask_{0U};
    uint32_t everPresentMask_{0U};
    bool     requiredSourcesDue_{false};
    uint32_t neverRegisteredRaised_{0U};
    uint32_t neverRegisteredPending_{0U};
    /** everPresentMask_ | (requiredSourcesDue_ ? requiredSourceMask_ : 0): the
     *  sources PSS expects. Carried in the presence report so a restarted
     *  SDM relearns what it forgot. Written under heartbeatMutex by
     *  recomputeExpectedMaskLocked(); read lock-free by sourceExpectedMask(). */
    std::atomic<uint32_t> expectedMask_{0U};
    void recomputeExpectedMaskLocked();

    /** Records produced under heartbeatMutex, written by flushDeferredRecords()
     *  after it is released, so no syslog write happens while the heartbeat tick
     *  and the report path are blocked on it. */
    std::vector<std::pair<std::string, std::string>> deferredInfoRecords_;
    void flushDeferredRecords();
    /** Consecutive heartbeat misses while RPC heartbeat is stale (per monitor period). */
    std::map<uint32_t, uint32_t> hbMissCount;
    /** Mirrors hbMissCount per slot; updated under heartbeatMutex with map; lock-free read for REPORT_SAFETY_EVENT degraded gate. */
    std::vector<std::atomic<uint32_t>> hbMissCountFast;
    /** Set when hb_miss_count > MAX; cleared on slot reuse (clearClientState). */
    std::map<uint32_t, bool> hbFaultLatched;
    /** Mirrors hbFaultLatched per slot; lock-free read for REPORT_SAFETY_EVENT tier-3 gate. */
    std::vector<std::atomic<uint32_t>> hbFaultLatchedFast;
    /** Lock-free mirrors of the active Safety Monitor's hb miss tier and fault-latched state (see smMonitorClientId_); updated under heartbeatMutex on tick/HB and rebuilt on Close so getSafetyMonitorOperationalMode() can read without locking. */
    std::atomic<uint32_t> smOperationalCacheMaxMiss_{0};
    std::atomic<bool> smOperationalCacheAnyFault_{false};
    /** PSS_PRESENCE_* set of monitors that are registered, have HEARTBEATED since
     *  registering (registration alone does not count) and are not heartbeat-faulted.
     *  Recomputed under heartbeatMutex by recomputePresenceMaskLocked() at every change
     *  to those inputs; read lock-free by sourcePresenceMask(). */
    std::atomic<uint32_t> presenceMask_{0U};
    void recomputePresenceMaskLocked();
    /** Policy: at most one Safety Monitor client; cache tracks that slot only (no map scan on heartbeat). */
    static constexpr uint32_t kNoSafetyMonitorClientId = UINT32_MAX;
    uint32_t smMonitorClientId_{kNoSafetyMonitorClientId};
    uint32_t smMonitorMiss_{0};
    bool smMonitorFault_{false};

    void refreshSafetyMonitorOperationalCacheLocked();

    /** A tracked source -- the Safety Monitor, the
     *  Perception Monitor, or a numbered MDX instance -- stopped being registered,
     *  whatever the reason, or (MDX only) a required instance never registered.
     *  Raises SW_FAIL with that source's *_LOST cause and its presence bit in
     *  objectID[0]; the SDMs clear it only on a presence report naming it, never on
     *  ordinary traffic. No-op for an untracked client (the gateway, anonymous MDX),
     *  so callers need not test.
     *  @param slot the client's slot, or UINT32_MAX when it never had one.
     *  @param reason short snake_case token for the PSB record. */
    void injectMonitorLostSwFail(uint32_t slot, uint8_t clientType, uint8_t instance,
                                 const char* reason,
                                 std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                 std::mutex& inputSafetyEventQueMutex);

    /** Remove a client from the RPC loop. If it was a registered monitor, raise its
     *  loss first: an orderly unregister, a crash and a protocol-error drop
     *  all leave the site just as unmonitored as a hang does. */
    void dropClient(uint32_t slot, const char* reason,
                    std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                    std::mutex& inputSafetyEventQueMutex);

    std::deque<std::pair<uint32_t, uint32_t>> pendingDisconnectClients;
    std::mutex pendingDisconnectMutex;
    std::unordered_set<uint32_t> pendingDisconnectSeen;

    mutable std::mutex heartbeatMutex;

    /*
     * Repeated rejected reports from a required source.
     *
     * Per slot, all guarded by heartbeatMutex alongside the other per-slot maps,
     * and all cleared by clearClientStateLocked() so a recycled slot cannot
     * inherit the previous client's streak.
     *
     * rejectStreak_      consecutive rejections; any accepted report resets it.
     * establishedPipeline_ the pipelineId of this client's last ACCEPTED report.
     *                    Required-ness is decided from this, never from the
     *                    rejected event: a report rejected for a bad CRC or a
     *                    short payload has no trustworthy pipelineId in it, and
     *                    those are exactly the rejections that matter most.
     *                    A client is ALSO required by its registered identity:
     *                    a numbered MDX instance, or a monitor, that nvpss.conf
     *                    requires. Such a client escalates even if none of its
     *                    reports was ever accepted. An anonymous client that has
     *                    never had a report accepted has no identity at all and
     *                    cannot escalate -- that is what stops a rogue peer
     *                    latching every SDM by sending garbage.
     * rejectFaultRaised_ latches the injection so a client that keeps failing
     *                    produces one SW_FAIL, not one per report.
     * rejectStreakNoted_ the same latch for a source that is NOT in the reviewed
     *                    set: its threshold crossing is recorded once per
     *                    episode, not once per report. Kept separate from
     *                    rejectFaultRaised_ so "recorded" is never read as
     *                    "fault raised".
     */
    std::map<uint32_t, uint32_t> rejectStreak_;
    std::map<uint32_t, uint8_t>  establishedPipeline_;
    std::map<uint32_t, bool>     rejectFaultRaised_;
    std::map<uint32_t, bool>     rejectStreakNoted_;

    /*
     * Input-state report (PSS_INPUT_STATE_MASK layout), under heartbeatMutex.
     *
     * inputConfirmedMask_  identities with an accepted report since the last
     *                      cause-13 escalation for that identity. Set by
     *                      noteReportAccepted(), cleared by the escalation.
     * The unusable set is derived: the identities of the slots whose
     * rejectFaultRaised_ is set. Both are published lock-free by
     * recomputeInputStateLocked() for the presence report.
     */
    uint32_t inputConfirmedMask_{0U};
    std::atomic<uint32_t> inputUnusablePublished_{0U};
    std::atomic<uint32_t> inputConfirmedPublished_{0U};
    /** The client's input identity: its PSS_PRESENCE_* bit, or
     *  PSS_INPUT_ANONYMOUS_CLIENT for an anonymous MDX client; 0 otherwise. */
    uint32_t inputIdentityLocked(uint32_t clientId) const;
    void recomputeInputStateLocked();
    uint32_t clientRejectErrorThreshold_{NVPSS_CLIENT_REJECT_ERROR_THRESHOLD_DEFAULT};

    /** Returns true on the escalation edge, i.e. this rejection is the one that
     *  crosses the threshold for a required source: required by its registered
     *  identity, or by its established pipeline being in the reviewed set. */
    bool noteReportRejected(uint32_t clientId, uint8_t rejectReason,
                            NvPSSDRequiredSourceCallback requiredSourceCb,
                            void* requiredSourceCtx);
    void noteReportAccepted(uint32_t clientId, uint8_t pipelineId, EventType eventType);
    void injectClientInputUnusableSwFail(
        uint32_t clientId,
        std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
        std::mutex& inputSafetyEventQueMutex);

    void processPendingDisconnects(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                   std::mutex& inputSafetyEventQueMutex);

    NvPSSDErr NvPSSDRunRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex,
                                const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb,
                                void* trustReportCtx,
                                NvPSSDRequiredSourceCallback requiredSourceCb,
                                void* requiredSourceCtx);

    /** Validate REGISTER_CLIENT request and, on success, commit the client type to internal state.
     *  Returns REGISTER_ACCEPTED, REGISTER_REJECTED_INVALID_TYPE, or REGISTER_REJECTED_DUPLICATE_TYPE. */
    /** @param outEvictedPeer receives a singleton-eviction's peer, which the
     *  CALLER must drop after this returns; heartbeatMutex is held inside.
     *  @param outEvictedSlot receives the evicted slot (UINT32_MAX if none), and
     *  @param outEvictedType / @param outEvictedInstance its identity, so the
     *  caller can raise the loss of an evicted tracked source outside the lock. */
    uint8_t validateAndAcceptRegistration(uint32_t clientSlot, const NvPSSDRPCMsgReq& req,
                                          NvPSFPeerId* outEvictedPeer,
                                          uint32_t* outEvictedSlot = nullptr,
                                          uint8_t* outEvictedType = nullptr,
                                          uint8_t* outEvictedInstance = nullptr);

    /** Clear heartbeat/type state for a slot (caller must hold heartbeatMutex). */
    void clearClientStateLocked(uint32_t clientId);
    /** Clear heartbeat/type state for a slot so slot reuse does not inherit previous client type. */
    void clearClientState(uint32_t clientId);

    /** Close socket, mark slot free, clear HB/type state. Safe to call on already-closed slots. */
    void disconnectClient(uint32_t slot);
    /** Same as disconnectClient but caller must hold heartbeatMutex. */
    /** Clears the slot and RETURNS the peer the caller must drop after
     *  releasing heartbeatMutex; NVPSF_PEER_INVALID if none. Keeping the
     *  DropPeer must be called outside the lock. */
    NvPSFPeerId disconnectClientLocked(uint32_t slot);

    /** True if last HB within HB_TIMEOUT_MS (= HB_INTERVAL_MS + HB_STALE_GRACE_MS; used to gate trust reports). */
    bool hasRecentHeartbeat(uint32_t clientId) const;

public:
    NvPSSDRPC(NvPSSDRPCBackend backend, std::string channel, uint8_t maxClients,
                uint8_t maxPendingClients);
    ~NvPSSDRPC();
    NvPSSDErr NvPSSDInitRPCServer();
    NvPSSDErr NvPSSDStartRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex,
                                const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb = nullptr,
                                void* trustReportCtx = nullptr,
                                NvPSSDRequiredSourceCallback requiredSourceCb = nullptr,
                                void* requiredSourceCtx = nullptr);

    /** Bounded at the configured limits; an out-of-range value keeps the
     *  default, matching how the other numeric nvpss.conf keys behave. */
    void setClientRejectErrorThreshold(uint32_t threshold);
    /** The PSS_PRESENCE_* set nvpss.conf requires: the Safety and
     *  Perception Monitor bits and MDX instance bits. A required source that has not
     *  registered once the heartbeat budget has run out since this PSS started is
     *  reported lost (reason never_registered), and from then on the required set
     *  is part of the expected set in every presence report. */
    void setRequiredSourceMask(uint32_t mask);
    NvPSSDErr NvPSSDCloseRPCServer();

    void updateClientHeartbeat(uint32_t clientId, uint8_t clientType);
    /** Returns client type from registration (0 if unknown). Used e.g. to allow Safety Monitor to report other clients. */
    uint8_t getClientType(uint32_t clientId) const;
    /** MDX instance id from registration; 0 for anonymous or any other type. */
    uint8_t getClientInstance(uint32_t clientId) const;
    /** Returns the number of clients with active heartbeat tracking (internally synchronized). */
    size_t getActiveClientCount() const;
    /** True after thread started successfully (until CloseRPCServer). For logs/heartbeat. */
    bool isRpcServerRunning() const { return runRpcServer.load(); }

    /**
     * Per HB monitor interval: increment miss counts for stale clients, tiered logging, enqueue disconnect at tier 3.
     * maxFailures / warnThreshold follow global policy (WARN_THRESHOLD = maxFailures / 2, integer division).
     */
    void heartbeatMonitorTick(uint32_t maxFailures, uint32_t warnThreshold);

    /**
     * Operational mode for DecisionRequest.pssStatus when Safety AI Monitor (CLIENT_SAFETY_MONITOR) HB state applies:
     * DEGRADED = active fault tier (warnThreshold < miss < maxFailures); ERROR = failure (miss >= maxFailures or fault latched).
     */
    OperationalMode getSafetyMonitorOperationalMode(uint32_t maxFailures, uint32_t warnThreshold) const;

    /** PSS_PRESENCE_* set of monitors present NOW: registered, heartbeated since
     *  registering, and not heartbeat-faulted. A missed heartbeat inside the budget
     *  does NOT remove a monitor -- the SDMs raise a loss when a seen monitor's bit
     *  clears, so this must not flicker on one late heartbeat. Lock-free; carried in
     *  the CAUSE_SOURCE_PRESENCE report on every status no-op. */
    uint32_t sourcePresenceMask() const;
    /** PSS_PRESENCE_* set PSS expects (see expectedMask_). Lock-free; carried in
     *  objectID[1] of the presence report. */
    uint32_t sourceExpectedMask() const;
    /** Input-state report (PSS_INPUT_STATE_MASK layout, unshifted): identities with
     *  a cause-13 episode open, and identities confirmed usable since their last
     *  escalation. Lock-free; carried in bits 16..31 of the presence report. */
    uint32_t sourceInputUnusableMask() const;
    uint32_t sourceInputConfirmedMask() const;
};
}