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

#define MAX_POSSIBLE_CLIENTS 100

/** Callback when a trust-report event is received. reporterClientType from registration (e.g. CLIENT_SAFETY_MONITOR). Returns true if accepted. */
typedef bool (*NvPSSDTrustReportCallback)(void* ctx, uint32_t clientId, uint8_t reporterClientType, const SafetyEvent* event);

namespace nvpss
{

class NvPSSDRPC
{

private:
    const NvPSSDRPCBackend backend;
    const std::string channel;
    const uint8_t maxClients;
    const uint8_t maxPendingClients;

    int serverSocket;
    int clientSockets[MAX_POSSIBLE_CLIENTS];
    /** Incremented on each accept(); processPendingDisconnects skips stale entries. */
    std::atomic<uint32_t> slotGeneration_[MAX_POSSIBLE_CLIENTS];

    std::thread rpcServerThread;
    /** True while RPC server thread is running (set true when thread started, false in Close). Run loop and isRpcServerRunning() both use this. */
    std::atomic<bool> runRpcServer{false};

    std::map<uint32_t, std::chrono::steady_clock::time_point> clientLastHeartbeat;
    std::map<uint32_t, uint32_t> clientHeartbeatCount;
    std::map<uint32_t, uint8_t> clientTypeMap;
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
    /** Policy: at most one Safety Monitor client; cache tracks that slot only (no map scan on heartbeat). */
    static constexpr uint32_t kNoSafetyMonitorClientId = UINT32_MAX;
    uint32_t smMonitorClientId_{kNoSafetyMonitorClientId};
    uint32_t smMonitorMiss_{0};
    bool smMonitorFault_{false};

    void refreshSafetyMonitorOperationalCacheLocked();

    std::deque<std::pair<uint32_t, uint32_t>> pendingDisconnectClients;
    std::mutex pendingDisconnectMutex;
    std::unordered_set<uint32_t> pendingDisconnectSeen;

    mutable std::mutex heartbeatMutex;

    void processPendingDisconnects(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                   std::mutex& inputSafetyEventQueMutex);

    NvPSSDErr NvPSSDRunRPCServer(std::deque<std::pair<int, SafetyEvent>>& inputSafetyEventQueRef,
                                std::mutex& inputSafetyEventQueMutex,
                                const float thresholdConfidence,
                                std::condition_variable& rpcServerTerminationCV,
                                NvPSSDTrustReportCallback trustReportCb,
                                void* trustReportCtx);

    /** Validate REGISTER_CLIENT request and, on success, commit the client type to internal state.
     *  Returns REGISTER_ACCEPTED, REGISTER_REJECTED_INVALID_TYPE, or REGISTER_REJECTED_DUPLICATE_TYPE. */
    uint8_t validateAndAcceptRegistration(uint32_t clientSlot, const NvPSSDRPCMsgReq& req);

    /** Clear heartbeat/type state for a slot (caller must hold heartbeatMutex). */
    void clearClientStateLocked(uint32_t clientId);
    /** Clear heartbeat/type state for a slot so slot reuse does not inherit previous client type. */
    void clearClientState(uint32_t clientId);

    /** Close socket, mark slot free, clear HB/type state. Safe to call on already-closed slots. */
    void disconnectClient(uint32_t slot);
    /** Same as disconnectClient but caller must hold heartbeatMutex. */
    void disconnectClientLocked(uint32_t slot);

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
                                void* trustReportCtx = nullptr);
    NvPSSDErr NvPSSDCloseRPCServer();

    void updateClientHeartbeat(uint32_t clientId, uint8_t clientType);
    /** Returns client type from registration (0 if unknown). Used e.g. to allow Safety Monitor to report other clients. */
    uint8_t getClientType(uint32_t clientId) const;
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
};
}