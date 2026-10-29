/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NVPSD_GATEWAY_CONTRACT_HPP
#define NVPSD_GATEWAY_CONTRACT_HPP

#include "NvPSDGatewayProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>

static constexpr uint8_t NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT = 32U;
static constexpr uint32_t NVPSD_GATEWAY_PSS_REQUEST_ID_MIN = 1U;
static constexpr uint32_t NVPSD_GATEWAY_PSS_REQUEST_ID_MAX = UINT32_MAX - 1U;

static inline void NvPSDGatewayAddMandatoryEventTypes(std::unordered_set<EventType>& types)
{
    types.insert(SW_FAIL);
    types.insert(PSS_STATUS_NOOP);
    /* Sensor-health evidence must reach valid SDM clients
     * even when their physical-event REGR list omits these health events. */
    types.insert(SENSOR_INVALID);
    types.insert(SENSOR_VALID);
    /*
     * AI-pipeline trust, for the same reason and with a sharper consequence.
     *
     * An SDM's REGR list names the physical events it acts on; no SDM asks for
     * these. Without forcing them the verdict never reaches an SDM at all, and
     * its AI-pipeline safe-state latch is unreachable code -- the SDMs saw
     * every downstream event stamped untrusted and none of them explaining why.
     * Distrust of the whole perception pipeline is a safe-state CAUSE, so the
     * evidence for it has to be delivered, not merely applied upstream.
     */
    types.insert(AI_PIPELINE_INVALID);
    types.insert(AI_PIPELINE_VALID);
}

static inline bool NvPSDGatewayIsAllowedHealthEventType(uint32_t raw)
{
    return raw == static_cast<uint32_t>(SENSOR_INVALID) ||
           raw == static_cast<uint32_t>(SENSOR_VALID) ||
           raw == static_cast<uint32_t>(AI_PIPELINE_INVALID) ||
           raw == static_cast<uint32_t>(AI_PIPELINE_VALID);
}

static inline bool NvPSDGatewayBuildRegisteredEventSet(const uint32_t* rawEventTypes,
                                                       uint8_t count,
                                                       std::unordered_set<EventType>* outTypes)
{
    if (outTypes == nullptr)
        return false;

    outTypes->clear();

    if (rawEventTypes == nullptr || count == 0U ||
        count > NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT)
    {
        return false;
    }

    const uint32_t eventUnknownVal = static_cast<uint32_t>(EVENT_UNKNOWN);
    for (uint8_t i = 0U; i < count; ++i)
    {
        const uint32_t raw = rawEventTypes[i];
        if (raw >= eventUnknownVal && !NvPSDGatewayIsAllowedHealthEventType(raw))
            continue;
        outTypes->insert(static_cast<EventType>(raw));
    }

    if (outTypes->empty())
        return false;

    NvPSDGatewayAddMandatoryEventTypes(*outTypes);
    return true;
}

/*
 * A REGR as the gateway reads it, before any client-table decision: the
 * registration-refusal reason, or 0 with *outTypes the set to register.
 * buf/n is the whole message, magic included; the caller has already matched the
 * magic. Wire format: "REGR", one count byte, then count event types, each a
 * network-order uint32.
 */
static inline uint8_t NvPSDGatewayClassifyRegr(const char* buf, size_t n,
                                               std::unordered_set<EventType>* outTypes)
{
    if (outTypes != nullptr)
        outTypes->clear();
    if (buf == nullptr || outTypes == nullptr || n < 5U)
        return NVPSD_REG_REJECT_MALFORMED;
    const uint8_t count = static_cast<uint8_t>(buf[4]);
    if (count == 0U || count > NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT ||
        n < 5U + static_cast<size_t>(count) * sizeof(uint32_t))
        return NVPSD_REG_REJECT_MALFORMED;

    uint32_t raw[NVPSD_GATEWAY_MAX_EVENT_TYPES_PER_CLIENT] = {};
    for (uint8_t i = 0U; i < count; ++i)
    {
        uint32_t val;
        std::memcpy(&val, buf + 5U + static_cast<size_t>(i) * sizeof(uint32_t), sizeof(uint32_t));
        raw[i] = ntohl(val);
    }
    /* Mandatory system/safety types are added only once at least one requested
     * type is valid, so an empty or all-invalid REGR stays refused. */
    if (!NvPSDGatewayBuildRegisteredEventSet(raw, count, outTypes))
        return NVPSD_REG_REJECT_NO_VALID_EVENT_TYPES;
    return 0U;
}

/*
 * Per-peer REGR rate limit: at most maxPerWindow REGRs from one peer per
 * windowMs, a fixed window that opens at the peer's first REGR. Beyond that a
 * REGR is refused UNREAD. The first refusal in a window is answered
 * (REFUSE_NOTIFY), the rest are not (REFUSE_SILENT), so a flooding peer costs one
 * reply per window.
 *
 * Legitimate traffic sits far below any allowed setting: an SDM sends one REGR
 * at launch or on reconnect, then one every 30 s.
 *
 * Not thread-safe: the gateway's registration listener is its only user, and
 * forget() runs on the same thread when a peer goes or is reaped, which bounds
 * the map by the transport's peer slots.
 */
enum class NvPSDGatewayRegrAdmission
{
    ADMIT,
    REFUSE_NOTIFY,
    REFUSE_SILENT
};

static constexpr uint32_t NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_DEFAULT = 4U;
static constexpr uint32_t NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_MIN = 1U;
static constexpr uint32_t NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_MAX = 64U;
static constexpr uint32_t NVPSD_GATEWAY_REGR_WINDOW_MS_DEFAULT = 10000U;
static constexpr uint32_t NVPSD_GATEWAY_REGR_WINDOW_MS_MIN = 1000U;
static constexpr uint32_t NVPSD_GATEWAY_REGR_WINDOW_MS_MAX = 600000U;

class NvPSDGatewayRegrRateLimiter
{
public:
    NvPSDGatewayRegrRateLimiter(uint32_t maxPerWindow = NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_DEFAULT,
                                uint32_t windowMs = NVPSD_GATEWAY_REGR_WINDOW_MS_DEFAULT)
    {
        configure(maxPerWindow, windowMs);
    }

    void configure(uint32_t maxPerWindow, uint32_t windowMs)
    {
        maxPerWindow_ = maxPerWindow;
        windowMs_ = windowMs;
        entries_.clear();
    }

    NvPSDGatewayRegrAdmission admit(uint32_t peer, uint64_t nowMs)
    {
        Entry& e = entries_[peer];
        /* A clock that went backwards opens a new window rather than wedging. */
        if (e.count == 0U || nowMs < e.windowStartMs || nowMs - e.windowStartMs >= windowMs_)
        {
            e.windowStartMs = nowMs;
            e.count = 0U;
            e.notified = false;
        }
        if (e.count < maxPerWindow_)
        {
            ++e.count;
            return NvPSDGatewayRegrAdmission::ADMIT;
        }
        if (!e.notified)
        {
            e.notified = true;
            return NvPSDGatewayRegrAdmission::REFUSE_NOTIFY;
        }
        return NvPSDGatewayRegrAdmission::REFUSE_SILENT;
    }

    void forget(uint32_t peer) { entries_.erase(peer); }
    size_t tracked() const { return entries_.size(); }

private:
    struct Entry
    {
        uint64_t windowStartMs{0U};
        uint32_t count{0U};
        bool notified{false};
    };
    uint32_t maxPerWindow_{NVPSD_GATEWAY_REGR_MAX_PER_WINDOW_DEFAULT};
    uint32_t windowMs_{NVPSD_GATEWAY_REGR_WINDOW_MS_DEFAULT};
    std::unordered_map<uint32_t, Entry> entries_;
};

static inline bool NvPSDGatewayIsStatusNoopOnlyRequest(const DecisionRequest& req)
{
    return req.pssStatus.mode != ERROR &&
           req.sensorDataSummarySize == 1U &&
           req.sensorDataSummary[0].event.type == PSS_STATUS_NOOP;
}

static inline bool NvPSDGatewayShouldSendDecisionRequest(bool sendFullRequest,
                                                         uint8_t eventCount,
                                                         OperationalMode pssMode)
{
    if (sendFullRequest)
    {
        return eventCount > 0U || pssMode == ERROR;
    }
    return eventCount > 0U;
}

/*
 * Classification of a raw sendto()/send() byte count for a gateway
 * DecisionRequest packet.
 *
 * FAILED_SHORT_SEND HAS NO PRODUCER. The gateway send path runs over
 * NvPSFTransport, which is message-oriented: a short send is not a reachable
 * outcome, so the transport's own error code is the finer-grained signal.
 *
 * Retained rather than deleted because this header is installed into the
 * dev package and is therefore a public interface with
 * possible out-of-tree consumers this repo cannot see. Deleting it is the same
 * shape of ownership decision as any other public-interface removal and needs
 * the same kind of sign-off.
 */
enum class NvPSDGatewayPacketSendResult : uint8_t
{
    SENT = 0U,
    FAILED_SYSCALL,
    FAILED_SHORT_SEND
};

static inline NvPSDGatewayPacketSendResult NvPSDGatewayDecisionPacketSendResult(
    ssize_t sentBytes)
{
    if (sentBytes < 0)
        return NvPSDGatewayPacketSendResult::FAILED_SYSCALL;
    if (sentBytes != static_cast<ssize_t>(sizeof(NvPSDGatewayDecisionRequestPacket)))
        return NvPSDGatewayPacketSendResult::FAILED_SHORT_SEND;
    return NvPSDGatewayPacketSendResult::SENT;
}

static inline bool NvPSDGatewayBuildDecisionRequestPacket(
    const DecisionRequest& req,
    uint64_t gatewayEpoch,
    uint32_t gatewayTxSeq,
    NvPSDGatewayDecisionRequestPacket* outPacket)
{
    static_assert(offsetof(NvPSDGatewayDecisionRequestPacket, request) <= UINT16_MAX,
                  "Gateway DecisionRequest packet header size must fit uint16_t");

    if (outPacket == nullptr ||
        !NvPSDGatewayDecisionPacketEpochIsValid(gatewayEpoch) ||
        !NvPSDGatewayDecisionPacketSequenceIsValid(gatewayTxSeq))
    {
        return false;
    }

    std::memset(outPacket, 0, sizeof(*outPacket));
    std::memcpy(outPacket->magic, NVPSD_GATEWAY_DECISION_MAGIC, 4U);
    outPacket->version = NVPSD_GATEWAY_DECISION_PACKET_VERSION;
    outPacket->headerSize = NVPSD_GATEWAY_DECISION_HEADER_SIZE;
    outPacket->gatewayEpoch = gatewayEpoch;
    outPacket->gatewayTxSeq = gatewayTxSeq;
    outPacket->request = req;
    return true;
}

struct NvPSDGatewayPssSequenceState
{
    bool initialized{false};
    uint32_t lastValidRequestId{0U};
};

enum class NvPSDGatewayPssRequestObservation : uint8_t
{
    ACCEPT = 0U,
    GAP,
    DUPLICATE_OR_STALE,
    INVALID
};

static inline bool NvPSDGatewayRequestIdIsSequenceTracked(uint32_t requestId)
{
    return requestId >= NVPSD_GATEWAY_PSS_REQUEST_ID_MIN &&
           requestId <= NVPSD_GATEWAY_PSS_REQUEST_ID_MAX;
}

static inline NvPSDGatewayPssRequestObservation NvPSDGatewayObservePssRequestId(
    NvPSDGatewayPssSequenceState* state,
    uint32_t requestId)
{
    if (state == nullptr || !NvPSDGatewayRequestIdIsSequenceTracked(requestId))
        return NvPSDGatewayPssRequestObservation::INVALID;

    if (!state->initialized)
    {
        state->initialized = true;
        state->lastValidRequestId = requestId;
        return NvPSDGatewayPssRequestObservation::ACCEPT;
    }

    const uint32_t last = state->lastValidRequestId;
    if (requestId == last)
        return NvPSDGatewayPssRequestObservation::DUPLICATE_OR_STALE;

    /* A backward jump can be a PSS daemon restart/reset because PSS starts
     * request ids at 1 on process launch. Treat it as a recoverable gap so
     * Gateway emits fault evidence and still forwards the current request. */
    const uint32_t expected = (last >= NVPSD_GATEWAY_PSS_REQUEST_ID_MAX)
        ? NVPSD_GATEWAY_PSS_REQUEST_ID_MIN
        : (last + 1U);
    const bool gap = requestId != expected;
    state->lastValidRequestId = requestId;
    return gap
        ? NvPSDGatewayPssRequestObservation::GAP
        : NvPSDGatewayPssRequestObservation::ACCEPT;
}

static inline bool NvPSDGatewayShouldForwardObservedPssRequest(
    NvPSDGatewayPssRequestObservation observation)
{
    return observation == NvPSDGatewayPssRequestObservation::ACCEPT ||
           observation == NvPSDGatewayPssRequestObservation::GAP;
}

static inline bool NvPSDGatewayBuildPssSequenceGapSwFailRequest(const DecisionRequest& current,
                                                                DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = current.requestId;
    outRequest->pssStatus = current.pssStatus;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = false;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = SW_FAIL;
    sensorData.event.severity = CRITICAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    sensorData.event.cause = CAUSE_PSS_REQUEST_SEQUENCE_GAP;
    return true;
}

static inline bool NvPSDGatewayBuildInvalidPssRequestIdSwFailRequest(const DecisionRequest& current,
                                                                     DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = NVPSD_GATEWAY_PSS_REQUEST_ID_MIN;
    outRequest->pssStatus = current.pssStatus;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = false;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = SW_FAIL;
    sensorData.event.severity = CRITICAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    sensorData.event.cause = CAUSE_PSS_REQUEST_ID_INVALID;
    return true;
}

/*
 * Recovery-record DEBT, per client.
 *
 * The gateway records, per registered client, the synthetic-fault generation it
 * owes a recovery record for (0 = nothing owed). These two rules are the whole
 * of what the fanout may do with that number.
 */

/* Whether the recovery fanout should send this client the record. */
static inline bool NvPSDGatewayRecoveryRecordOwed(uint64_t recoveryOwedGen)
{
    return recoveryOwedGen != 0U;
}

/*
 * The client's debt after a recovery fanout.
 *
 *   owedNow   the client's debt now, after the sends
 *   sentFor   the debt it had when its record was queued
 *   sent      whether the transport accepted the record for it
 *
 * Paid only when the record actually went out AND no newer synthetic fault
 * re-marked the client in the meantime. A skipped client, a failed send and a
 * newer fault all leave the debt standing for the next accepted request.
 */
static inline uint64_t NvPSDGatewayRecoveryDebtAfterSend(uint64_t owedNow, uint64_t sentFor, bool sent)
{
    return (sent && owedNow == sentFor) ? 0U : owedNow;
}

/*
 * The all-client recovery record.
 *
 * WHY IT IS A NO-OP AND NOT A SW_FAIL. The three synthetic faults above are
 * broadcast to every registered client, but the traffic that would clear them is
 * subscription filtered -- so an SDM that subscribes to no event type which
 * subsequently occurs never receives anything that says the fault recovered, and
 * stays release-denied while its peers move on. This record closes that
 * asymmetry by being deliverable on the same all-client path.
 *
 * It rides PSS_STATUS_NOOP, which is a mandatory subscription for every client,
 * and it must NEVER be a SW_FAIL: a SW_FAIL latches, and a record announcing
 * recovery that latches would be self-defeating. Carrying it on a no-op also
 * means every runtime already does the right thing with it by default --
 * SdmDecisionRequestIsNoopOnly() holds, so freshness refreshes and no physical
 * state is touched. An SDM that does not read the cause at all simply sees an
 * ordinary no-op.
 *
 * It clears an ACTIVE sub-cause only. The safe-state latch still requires a
 * validated manual release.
 */
static inline bool NvPSDGatewayBuildPssCommRecoveredRequest(const DecisionRequest& current,
                                                            DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = current.requestId;
    outRequest->pssStatus = current.pssStatus;
    outRequest->pssStatus.mode = NORMAL;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    /* Healthy, unlike the synthetic faults above: this record reports the ABSENCE
     * of a fault, and stamping it unhealthy would contradict what it says. */
    sensorData.isHealthy = true;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = PSS_STATUS_NOOP;
    sensorData.event.severity = OPERATIONAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    sensorData.event.cause = CAUSE_PSS_COMM_RECOVERED;
    return true;
}

static inline bool NvPSDGatewayBuildPssErrorSwFailRequest(const DecisionRequest& current,
                                                          DecisionRequest* outRequest)
{
    if (outRequest == nullptr)
        return false;

    std::memset(outRequest, 0, sizeof(*outRequest));
    outRequest->requestId = current.requestId;
    outRequest->pssStatus = current.pssStatus;
    outRequest->pssStatus.mode = NORMAL;
    outRequest->sensorDataSummarySize = 1U;

    const uint64_t timestamp = (current.sensorDataSummarySize > 0U &&
                                current.sensorDataSummary[0].event.timestamp != 0U)
        ? current.sensorDataSummary[0].event.timestamp
        : 1ULL;

    SensorData& sensorData = outRequest->sensorDataSummary[0];
    sensorData.clientID = 0U;
    sensorData.isHealthy = false;
    sensorData.isTrustedSource = true;
    sensorData.event.id = current.requestId;
    sensorData.event.type = SW_FAIL;
    sensorData.event.severity = CRITICAL;
    sensorData.event.timestamp = timestamp;
    sensorData.event.confidenceLevel = 1.0F;
    sensorData.event.status = PASSTHROUGH;
    sensorData.event.fusionMetadata.objectType[0] = OBJECT;
    sensorData.event.fusionMetadata.objectType[1] = OBJECT;
    (void)std::snprintf(sensorData.event.sensorIdentifier,
                        sizeof(sensorData.event.sensorIdentifier),
                        "%s", "PSD_GATEWAY");
    sensorData.event.cause = CAUSE_PSS_ERROR;
    return true;
}

#endif /* NVPSD_GATEWAY_CONTRACT_HPP */
