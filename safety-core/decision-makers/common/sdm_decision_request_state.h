/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SDM_DECISION_REQUEST_STATE_H
#define SDM_DECISION_REQUEST_STATE_H

#include "pss_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool SdmDecisionRequestIsNoopOnly(const DecisionRequest *req)
{
    return req != 0 &&
           req->sensorDataSummarySize == 1U &&
           req->sensorDataSummary[0].event.type == PSS_STATUS_NOOP;
}

static inline bool SdmDecisionRequestContainsSwFail(const DecisionRequest *req)
{
    if (req == 0) {
        return false;
    }

    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }

    for (uint8_t i = 0U; i < count; i++) {
        if (req->sensorDataSummary[i].event.type == SW_FAIL) {
            return true;
        }
    }

    return false;
}

/*
 * ============================ WHICH RUNTIME CALLS WHICH ============================
 *
 * All SDMs apply the same PSS sub-cause rules. Each SDM calls the three clear
 * predicates below from its own control flow; SdmPssSubCausesEvaluate() is the
 * same rules as one pure transition, built from the same predicates.
 *
 *   SdmDecisionRequestClearsPssModeError     mode != ERROR -- a no-op qualifies
 *   SdmDecisionRequestIsGatewayCommRecovered the gateway's all-client recovery record
 *   SdmDecisionRequestClearsPssSwFail        a usable physical request -- no-op does not
 *
 * The source-lost sub-causes (Safety Monitor, Perception Monitor, numbered MDX
 * instances) follow a different rule, and EVERY SDM calls the same pure
 * function for it, SdmSourcePresenceEvaluate(): PSS's presence report is the only
 * thing that clears one, and a source the SDM has seen disappearing from it
 * raises one.
 *
 * The Crossing-Junction-Safety SDM used to keep a single PSS fault flag cleared
 * only by the last rule (SdmDecisionRequestClearsPssFaultActive), so the first
 * usable request cleared a lost monitor, and on a quiet site a recovered PSS left
 * it release-denied indefinitely. That single-bucket rule is gone; every SDM --
 * ATL, proximity, combined and junction -- applies the rules above.
 * =================================================================================
 */

/*
 * A SW_FAIL the PSD gateway synthesised about its own link to PSS, as opposed to
 * one describing a real fault upstream.
 *
 * These three are the gateway's own observations -- PSS reported ERROR, its
 * request ids jumped, or one was out of range -- so the gateway is the authority
 * on whether they have recovered, and they clear on evidence it supplies. Every
 * other cause, INCLUDING ONE THIS BUILD DOES NOT RECOGNISE, is not in this set
 * and therefore lands in the conservative bucket. That fall-through is the point:
 * a cause added later is treated as a real fault until someone teaches the SDMs
 * otherwise.
 */
static inline bool SdmEventCauseIsGatewayComm(EventCause cause)
{
    return cause == CAUSE_PSS_ERROR ||
           cause == CAUSE_PSS_REQUEST_SEQUENCE_GAP ||
           cause == CAUSE_PSS_REQUEST_ID_INVALID;
}

/*
 * A monitor was lost -- it hung, crashed, unregistered, or was dropped by PSS.
 *
 * Each has its own sub-cause, because the SW_FAIL bucket clears on any usable
 * physical request, and MDX traffic says nothing about whether sensor health or
 * AI-pipeline trust is being watched. Before this, the next ordinary MDX batch
 * cleared the fault and a release was accepted with no monitor running.
 */
static inline bool SdmEventCauseIsSafetyMonitorLost(EventCause cause)
{
    return cause == CAUSE_SAFETY_MONITOR_LOST;
}

static inline bool SdmEventCauseIsPerceptionMonitorLost(EventCause cause)
{
    return cause == CAUSE_PERCEPTION_MONITOR_LOST;
}

/* A numbered MDX client instance was lost, or a required one never came. */
static inline bool SdmEventCauseIsMdxClientLost(EventCause cause)
{
    return cause == CAUSE_MDX_CLIENT_LOST;
}

/* A required source's reports are being rejected (cause 13). */
static inline bool SdmEventCauseIsClientInputUnusable(EventCause cause)
{
    return cause == CAUSE_CLIENT_INPUT_UNUSABLE;
}

/*
 * The gateway's all-client recovery record: a status/no-op carrying
 * CAUSE_PSS_COMM_RECOVERED.
 *
 * It rides a PSS_STATUS_NOOP and never a SW_FAIL, so it cannot latch anything.
 * It exists to reach SDMs that receive no ordinary qualifying traffic and would
 * otherwise stay release-denied after the fault that latched them recovered.
 */
static inline bool SdmDecisionRequestIsGatewayCommRecovered(const DecisionRequest *req)
{
    return SdmDecisionRequestIsNoopOnly(req) &&
           req->pssStatus.mode != ERROR &&
           req->sensorDataSummary[0].event.cause == CAUSE_PSS_COMM_RECOVERED;
}

/*
 * Clears the PSS mode-error sub-cause (all SDMs).
 *
 * A NO-OP QUALIFIES, and that is the substantive change. PSS recomputes
 * pssStatus.mode on every send from its delivery state OR the Safety Monitor
 * heartbeat, so mode != ERROR is an affirmative statement -- transmitted at a
 * bounded interval and validated on receipt -- that neither of those is failed.
 * It is not silence, so acting on it does not infer recovery from an absence.
 *
 * Refusing to act on it is what used to strand a recovered system release-denied
 * on a quiet site: without physical event traffic, nothing else ever arrived to
 * say PSS was healthy again.
 *
 * This clears an ACTIVE sub-cause only. The safe-state latch is untouched and
 * still requires a validated manual release.
 */
static inline bool SdmDecisionRequestClearsPssModeError(const DecisionRequest *req)
{
    return req != 0 && req->pssStatus.mode != ERROR;
}

/*
 * Clears the SW_FAIL sub-cause (all SDMs) -- the historical rule, now scoped to
 * the one sub-cause it was always right for.
 *
 * A no-op does NOT qualify. A no-op says the transport and PSS are alive; it says
 * nothing whatever about a reported SW_FAIL, so it must not clear one.
 */
static inline bool SdmDecisionRequestClearsPssSwFail(const DecisionRequest *req)
{
    return req != 0 &&
           req->pssStatus.mode != ERROR &&
           !SdmDecisionRequestIsNoopOnly(req) &&
           !SdmDecisionRequestContainsSwFail(req);
}


/*
 * ============================== SDM STATE TRANSITIONS ==============================
 *
 * Pure functions: no I/O, no globals. The SDM keeps the state and does the
 * latching, holding and logging; these decide what changed.
 */

/* PSS sub-causes. Bits of one uint8_t so the set fits in a single byte. */
#define SDM_PSS_SUB_MODE_ERROR   0x01U   /* PSS reported pssStatus.mode == ERROR       */
#define SDM_PSS_SUB_GATEWAY_COMM 0x02U   /* gateway-synthesised SW_FAIL about its link */
#define SDM_PSS_SUB_SW_FAIL      0x04U   /* any other SW_FAIL, including unknown causes */
#define SDM_PSS_SUB_MONITOR_LOST 0x08U   /* the Safety Monitor was lost                 */
#define SDM_PSS_SUB_PM_LOST      0x10U   /* the Perception Monitor was lost             */
#define SDM_PSS_SUB_MDX_LOST     0x20U   /* a numbered MDX client instance was lost     */
#define SDM_PSS_SUB_INPUT_UNUSABLE 0x40U /* a source's reports are being rejected (13) */

/*
 * The sub-cause ONE SW_FAIL raises, by its cause. An unrecognised cause lands in
 * the conservative SW_FAIL bucket. Cause 13 gets its own sub-cause only when it
 * names the source (an input-state bit in objectID[0]); without one there is no
 * identity to wait for, so it falls back to SW_FAIL as before.
 */
static inline uint8_t SdmSwFailSubCause(const FusedSafetyEvent *ev)
{
    const EventCause c = (EventCause)ev->cause;
    return SdmEventCauseIsGatewayComm(c)           ? SDM_PSS_SUB_GATEWAY_COMM
         : SdmEventCauseIsSafetyMonitorLost(c)     ? SDM_PSS_SUB_MONITOR_LOST
         : SdmEventCauseIsPerceptionMonitorLost(c) ? SDM_PSS_SUB_PM_LOST
         : SdmEventCauseIsMdxClientLost(c)         ? SDM_PSS_SUB_MDX_LOST
         : (SdmEventCauseIsClientInputUnusable(c) &&
            (ev->fusionMetadata.objectID[0] & PSS_INPUT_STATE_MASK) != 0U)
                                                   ? SDM_PSS_SUB_INPUT_UNUSABLE
                                                   : SDM_PSS_SUB_SW_FAIL;
}

/*
 * The sub-causes of EVERY SW_FAIL in the request, whatever its position, whatever
 * pssStatus.mode, STALE or not. A request can carry several faults, and a fault
 * that rides an ERROR-mode request is still a fault: stopping at the first, or
 * at the ERROR return, lets the others go unlatched, and restoring the first then
 * permits a release while they persist.
 */
static inline uint8_t SdmSwFailSubCausesRaised(const DecisionRequest *req)
{
    uint8_t raised = 0U;
    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }
    for (uint8_t i = 0U; i < count; i++) {
        const FusedSafetyEvent *ev = &req->sensorDataSummary[i].event;
        if (ev->type == SW_FAIL) {
            raised = (uint8_t)(raised | SdmSwFailSubCause(ev));
        }
    }
    return raised;
}

typedef struct
{
    uint8_t raised;    /* sub-causes this request makes active: caller latches PSS and holds */
    uint8_t cleared;   /* sub-causes this request affirmatively refutes */
} SdmPssSubCauseDelta;

/*
 * What one VALIDATED DecisionRequest does to the PSS sub-cause set. The rules are
 * the CCPLEX ones (ATLControl.cpp), in the same order:
 *
 *   mode == ERROR                 raise MODE_ERROR, and the sub-cause of every
 *                                 SW_FAIL in the request; nothing is cleared
 *   mode != ERROR                 clear MODE_ERROR -- a no-op qualifies
 *   no-op carrying cause 14       clear GATEWAY_COMM (the gateway's recovery record)
 *   usable physical request       clear SW_FAIL, and GATEWAY_COMM too: without that
 *                                 floor a gateway restart, or one lost recovery
 *                                 record, would deny release for good.
 *                                 The monitor-lost sub-causes are deliberately NOT
 *                                 cleared here -- see SdmSourcePresenceEvaluate()
 *   request carrying SW_FAIL      raise the sub-cause of every SW_FAIL in it
 *                                 (SdmSwFailSubCause)
 *
 * One difference from the SDM control flows, in the conservative direction: they
 * skip a STALE SW_FAIL; this raises for every SW_FAIL present, STALE or not.
 */
static inline SdmPssSubCauseDelta SdmPssSubCausesEvaluate(const DecisionRequest *req)
{
    SdmPssSubCauseDelta d;
    d.raised = 0U;
    d.cleared = 0U;

    if (req == 0) {
        return d;
    }
    if (req->pssStatus.mode == ERROR) {
        d.raised = (uint8_t)(SDM_PSS_SUB_MODE_ERROR | SdmSwFailSubCausesRaised(req));
        return d;
    }
    if (SdmDecisionRequestClearsPssModeError(req)) {
        d.cleared |= SDM_PSS_SUB_MODE_ERROR;
    }
    if (SdmDecisionRequestIsNoopOnly(req)) {
        if (SdmDecisionRequestIsGatewayCommRecovered(req)) {
            d.cleared |= SDM_PSS_SUB_GATEWAY_COMM;
        }
        return d;
    }
    if (SdmDecisionRequestClearsPssSwFail(req)) {
        d.cleared |= (uint8_t)(SDM_PSS_SUB_SW_FAIL | SDM_PSS_SUB_GATEWAY_COMM);
        return d;
    }

    d.raised = SdmSwFailSubCausesRaised(req);
    return d;
}

/*
 * ================================== SOURCE PRESENCE ================================
 *
 * The sources PSS tracks, in groups, and the sub-cause each group's loss raises.
 * A source appears here only if it is the SOLE reporter of some evidence, so that
 * no other client's traffic can stand in for it: the Safety Monitor, the
 * Perception Monitor, and each NUMBERED MDX client instance -- which are one group
 * with one sub-cause, because a lost instance is lost physical-event supply
 * whichever instance it was.
 */
#define SDM_PRESENCE_GROUP_COUNT 3U
#define SDM_PRESENCE_TRACKED_MASK \
    (PSS_PRESENCE_SAFETY_MONITOR | PSS_PRESENCE_PERCEPTION_MONITOR | PSS_PRESENCE_MDX_MASK)

static inline uint32_t SdmPresenceGroupMask(uint8_t g)
{
    return (g == 0U) ? PSS_PRESENCE_SAFETY_MONITOR
         : (g == 1U) ? PSS_PRESENCE_PERCEPTION_MONITOR
                     : PSS_PRESENCE_MDX_MASK;
}

static inline uint8_t SdmPresenceSubCause(uint8_t g)
{
    return (g == 0U) ? SDM_PSS_SUB_MONITOR_LOST
         : (g == 1U) ? SDM_PSS_SUB_PM_LOST
                     : SDM_PSS_SUB_MDX_LOST;
}

/*
 * Find PSS's presence report: a PSS_STATUS_NOOP carrying CAUSE_SOURCE_PRESENCE.
 * Searched across the WHOLE request, not only a no-op-only one: PSS batches its
 * no-op with ordinary operational events, and on a busy site that may be the only
 * way it ever arrives. Every report in one request is sampled at the same build,
 * so any of them will do; the last is taken.
 *
 * A no-op WITHOUT that cause -- the gateway's recovery record, or a no-op from a
 * PSS that predates presence reports -- is not a report and says nothing about
 * presence either way.
 *
 * presenceMask receives objectID[0], the sources present now; expectedMask, if
 * not null, receives objectID[1], the sources PSS expects.
 */
static inline bool SdmDecisionRequestFindPresenceReport(const DecisionRequest *req,
                                                        uint32_t *presenceMask,
                                                        uint32_t *expectedMask)
{
    bool found = false;

    if (req == 0 || presenceMask == 0) {
        return false;
    }
    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }
    for (uint8_t i = 0U; i < count; i++) {
        const FusedSafetyEvent *ev = &req->sensorDataSummary[i].event;
        if (ev->type == PSS_STATUS_NOOP && ev->cause == CAUSE_SOURCE_PRESENCE) {
            *presenceMask = ev->fusionMetadata.objectID[0];
            if (expectedMask != 0) {
                *expectedMask = ev->fusionMetadata.objectID[1];
            }
            found = true;
        }
    }
    return found;
}

/*
 * The sub-cause a source-lost SW_FAIL raises, by its cause; 0 for any other cause.
 */
static inline uint8_t SdmSourceLostSubCause(EventCause cause)
{
    return SdmEventCauseIsSafetyMonitorLost(cause)     ? SDM_PSS_SUB_MONITOR_LOST
         : SdmEventCauseIsPerceptionMonitorLost(cause) ? SDM_PSS_SUB_PM_LOST
         : SdmEventCauseIsMdxClientLost(cause)         ? SDM_PSS_SUB_MDX_LOST
                                                       : 0U;
}

/*
 * What one request does to the source-lost sub-causes. *seen is the caller's own
 * record of every tracked source (PSS_PRESENCE_* bits) it knows of; it is updated
 * here and must persist for the life of the SDM process. It does NOT survive an
 * SDM restart -- that is what the report's expected set is for.
 *
 * First, EVERY source-lost SW_FAIL (causes 15, 17, 18) in the request raises its
 * group's sub-cause, whatever its position and whatever pssStatus.mode. This
 * path is taken for every request, before the PSS-ERROR return. The SW_FAIL also
 * names the lost source in
 * objectID[0], and that source is marked seen, so the clear waits for THAT source
 * even if this SDM never saw it present.
 *
 * Then PSS's presence report. Its expected set (objectID[1]) is added to *seen
 * first: the sources PSS knows of -- every one that has heartbeated since PSS started, and
 * the required ones once their start-up budget has run out. That is how an SDM
 * that restarted while a source was down relearns it, and how a required
 * source that never came up becomes a loss. Then, per group:
 *
 *   a seen member is missing   raise the group's sub-cause. This catches a loss
 *                              PSS never reported: PSS restarting while the
 *                              source was dead forgets the SW_FAIL, but its
 *                              reports no longer name the source
 *   every seen member present, clear it -- only when mode != ERROR, the one
 *   and at least one named     clear there is. For a one-member group this is
 *                              exactly "the report names it"
 *   nothing seen, none named   nothing. A site that never ran it is not a loss
 *   no report in the request   nothing
 *
 * Raised whenever missing, not only on the edge: the caller logs and holds on its
 * own inactive->active transition. A raise and a clear for the same group cannot
 * come from one report.
 */
static inline SdmPssSubCauseDelta SdmSourcePresenceEvaluate(uint32_t *seen,
                                                            const DecisionRequest *req)
{
    SdmPssSubCauseDelta d;
    uint32_t mask = 0U;

    d.raised = 0U;
    d.cleared = 0U;
    if (seen == 0 || req == 0) {
        return d;
    }

    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }
    for (uint8_t i = 0U; i < count; i++) {
        const FusedSafetyEvent *ev = &req->sensorDataSummary[i].event;
        const uint8_t lost = SdmSourceLostSubCause((EventCause)ev->cause);
        if (ev->type == SW_FAIL && lost != 0U) {
            d.raised = (uint8_t)(d.raised | lost);
            *seen |= ev->fusionMetadata.objectID[0] & SDM_PRESENCE_TRACKED_MASK;
        }
    }

    uint32_t expected = 0U;
    if (!SdmDecisionRequestFindPresenceReport(req, &mask, &expected)) {
        return d;
    }
    *seen |= expected & SDM_PRESENCE_TRACKED_MASK;
    for (uint8_t g = 0U; g < SDM_PRESENCE_GROUP_COUNT; g++) {
        const uint32_t group = SdmPresenceGroupMask(g);
        const uint32_t present = mask & group;
        const uint32_t missing = *seen & group & ~present;
        *seen |= present;
        if (missing != 0U) {
            d.raised = (uint8_t)(d.raised | SdmPresenceSubCause(g));
        } else if (present != 0U && req->pssStatus.mode != ERROR &&
                   (d.raised & SdmPresenceSubCause(g)) == 0U) {
            d.cleared = (uint8_t)(d.cleared | SdmPresenceSubCause(g));
        }
    }
    return d;
}

/*
 * ================================== INPUT STATE ====================================
 *
 * What one request does to the input-unusable sub-cause: a source whose reports
 * PSS keeps rejecting (cause 13). *unusableSeen is the caller's record, in the
 * PSS_INPUT_STATE_MASK layout, of the identities it is holding the cause for; it
 * must persist for the life of the SDM process.
 *
 *   a cause-13 SW_FAIL           adds the identity it names (objectID[0]), at any
 *                                position and in any mode
 *   the presence report          adds its UNUSABLE set (objectID[0] bits 16..31);
 *                                and, only when mode != ERROR, removes each
 *                                identity it shows CONFIRMED USABLE (objectID[1]
 *                                bits 16..31) and not unusable -- except one a
 *                                SW_FAIL in this same request just added
 *
 * Raised while any identity is held; cleared when the last one is removed. Other
 * sources' traffic clears nothing: only PSS saying THIS source has had a report
 * accepted since its escalation does. PSS starts both sets empty, so a PSS
 * restart cannot clear anything either -- the SDM holds until that source's next
 * accepted report. A restarted SDM relearns an episode that is still open from
 * the unusable set.
 */
static inline SdmPssSubCauseDelta SdmSourceInputStateEvaluate(uint32_t *unusableSeen,
                                                              const DecisionRequest *req)
{
    SdmPssSubCauseDelta d;
    uint32_t fromSwFail = 0U;
    uint32_t present = 0U;
    uint32_t expected = 0U;

    d.raised = 0U;
    d.cleared = 0U;
    if (unusableSeen == 0 || req == 0) {
        return d;
    }
    const uint32_t before = *unusableSeen;

    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }
    for (uint8_t i = 0U; i < count; i++) {
        const FusedSafetyEvent *ev = &req->sensorDataSummary[i].event;
        if (ev->type == SW_FAIL && SdmEventCauseIsClientInputUnusable((EventCause)ev->cause)) {
            fromSwFail |= ev->fusionMetadata.objectID[0] & PSS_INPUT_STATE_MASK;
        }
    }
    *unusableSeen |= fromSwFail;

    if (SdmDecisionRequestFindPresenceReport(req, &present, &expected)) {
        const uint32_t unusable = (present >> PSS_INPUT_STATE_SHIFT) & PSS_INPUT_STATE_MASK;
        const uint32_t confirmed = (expected >> PSS_INPUT_STATE_SHIFT) & PSS_INPUT_STATE_MASK;
        *unusableSeen |= unusable;
        if (req->pssStatus.mode != ERROR) {
            *unusableSeen &= ~(confirmed & ~unusable & ~fromSwFail);
        }
    }

    if (*unusableSeen != 0U) {
        d.raised = SDM_PSS_SUB_INPUT_UNUSABLE;
    } else if (before != 0U) {
        d.cleared = SDM_PSS_SUB_INPUT_UNUSABLE;
    }
    return d;
}

/* Result bits of SdmAiPipelineVerdictApply(). */
#define SDM_AI_VERDICT_SAW_INVALID 0x01U   /* set the AI-pipeline latch bit            */
#define SDM_AI_VERDICT_ENTERED     0x02U   /* inactive -> active: send the hold NOW     */
#define SDM_AI_VERDICT_CLEARED     0x04U   /* active -> inactive: latch awaits release  */

/*
 * Apply every AI_PIPELINE_INVALID / AI_PIPELINE_VALID verdict in the request, in
 * order, to *invalidActive.
 *
 * The verdict arrives ALREADY STAMPED UNTRUSTED -- PSS sets the pipeline-wide flag
 * before routing it -- so this must run before anything that skips untrusted
 * events, or it never runs.
 *
 * ENTERED is the edge on which the caller must send the safe hold itself. Under
 * pipeline-wide distrust every later event is skipped as untrusted, so the
 * ordinary decision path is never reached to send it.
 *
 * VALID clears the ACTIVE condition only; the latch waits for a PLC release.
 *
 * STALE entries are skipped, as the CCPLEX SDMs do.
 */
static inline uint8_t SdmAiPipelineVerdictApply(uint8_t *invalidActive, const DecisionRequest *req)
{
    uint8_t result = 0U;

    if (invalidActive == 0 || req == 0) {
        return 0U;
    }

    uint8_t count = req->sensorDataSummarySize;
    if (count > MAX_SENSORS_DATA_SUMMARY_SIZE) {
        count = MAX_SENSORS_DATA_SUMMARY_SIZE;
    }
    for (uint8_t i = 0U; i < count; i++) {
        const FusedSafetyEvent *ev = &req->sensorDataSummary[i].event;
        if (ev->status == STALE) {
            continue;
        }
        if (ev->type == AI_PIPELINE_INVALID) {
            result |= SDM_AI_VERDICT_SAW_INVALID;
            if (*invalidActive == 0U) {
                *invalidActive = 1U;
                result |= SDM_AI_VERDICT_ENTERED;
            }
        } else if (ev->type == AI_PIPELINE_VALID) {
            if (*invalidActive != 0U) {
                *invalidActive = 0U;
                result |= SDM_AI_VERDICT_CLEARED;
            }
        }
    }
    return result;
}

#ifdef __cplusplus
}
#endif

#endif /* SDM_DECISION_REQUEST_STATE_H */
