/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Forklift-to-person proximity fusion for the ATL + proximity deployment.
 *
 * Owned by decision-makers/atl_proximity. This is deliberately NOT shared with
 * decision-makers/proximity: that component is deployed and field-validated, so
 * this use case brings its own decision core rather than changing it.
 *
 * WHAT THIS SOLVES
 *
 * A DecisionRequest carries up to MAX_SENSORS_DATA_SUMMARY_SIZE events, and the
 * SDM must collapse them to one PLC command. Events in one batch describe
 * DIFFERENT object pairs, and every event in a batch carries the same
 * DecisionRequest-level severity, so severity cannot arbitrate between them.
 *
 * Collapsing a batch by "last qualifying event by index" is therefore unsafe: a
 * safe reading for pair B silences a still-dangerous pair A that happens to sit
 * at a lower index. The fix has two halves, and both are required:
 *
 *   1. WITHIN one pair, the newest observation wins. A fresh EVENT_8 for pair A
 *      is the current truth about pair A and must be able to lower pair A from
 *      CRITICAL to NORMAL, otherwise the SDM latches STOP forever.
 *
 *   2. ACROSS pairs, the worst case wins. The emitted command is the most
 *      severe tier held by any live pair: CRITICAL > WARNING > NORMAL.
 *
 * Half (2) only works if state is retained per pair between batches, because a
 * batch is a partial view: pair A being absent from this batch does not mean
 * pair A became safe. So observations accumulate into a pair table, and the
 * command is then derived once from a snapshot of that table.
 *
 * ROLE RESOLUTION (the "forklift and person" part)
 *
 * This use case is specifically forklift-to-person proximity, so the pair
 * identity is a role-tagged (forklift, person) tuple rather than an unordered
 * object-ID pair. Roles are resolved from EventFusionMetadata::objectType.
 *
 * IMPORTANT LIMITATION: mdx-client's stringToObjectType() maps "person" to
 * PERSON but has no forklift enumerator, so "forklift" arrives as the generic
 * OBJECT. Role resolution can therefore only assert "exactly one endpoint is a
 * PERSON and the other is not"; it cannot prove the peer is a forklift rather
 * than some other OBJECT. Restricting these events to forklift-to-person pairs
 * is the event mapping layer's job, and this SDM's rule set is scoped that way
 * in event_mapping_atl_proximity.pb.txt. The check here is defense in depth
 * against a mapping change that widens the pair scope without updating the SDM.
 *
 * A pair whose roles do not resolve is still tracked, keyed by its ordered
 * object-ID tuple with role ATLPXC_ROLE_UNRESOLVED, and its severity still
 * counts toward the worst case. Discarding an unresolvable CRITICAL would be a
 * fail-dangerous response to unexpected metadata; the counter
 * AtlPxcPairTable::unresolvedRoleObservations exists so the condition is
 * observable rather than silent.
 *
 * CONCURRENCY: none of this is internally synchronized. The caller owns the
 * lock. In AtlProximityControl.cpp every entry point runs under
 * atlPxcStateMtx.
 */

#ifndef ATL_PROXIMITY_PAIR_FUSION_H
#define ATL_PROXIMITY_PAIR_FUSION_H

#include <cstdint>
#include <cstring>

#include "pss_protocol.h"

/* Proximity event types this deployment consumes. Kept local so a renumbering
 * in the mapping layer shows up as a compile-time mismatch here rather than as
 * a silently misclassified severity tier. */
#define ATLPXC_EVENT_NO_VIOLATION        EVENT_8   /* safe separation           */
#define ATLPXC_EVENT_VIOLATION_WARNING   EVENT_9   /* 1 m < distance < 2 m      */
#define ATLPXC_EVENT_VIOLATION_CRITICAL  EVENT_10  /* distance < 1 m            */

/* Severity ordering used for cross-pair arbitration. Values are ordered so a
 * numeric > comparison is the severity comparison; do not reorder. */
enum AtlPxcSeverityTier : std::uint8_t {
    ATLPXC_TIER_NONE     = 0U,  /* no live observation for this pair */
    ATLPXC_TIER_NORMAL   = 1U,  /* EVENT_8  -> CMD_NORMAL */
    ATLPXC_TIER_WARNING  = 2U,  /* EVENT_9  -> CMD_REDUCE */
    ATLPXC_TIER_CRITICAL = 3U   /* EVENT_10 -> CMD_STOP   */
};

/* How the (forklift, person) roles of a tracked pair were established. */
enum AtlPxcPairRole : std::uint8_t {
    ATLPXC_ROLE_UNRESOLVED = 0U, /* roles could not be determined from metadata */
    ATLPXC_ROLE_RESOLVED   = 1U  /* exactly one endpoint is PERSON              */
};

/* Pair table capacity. Each DecisionRequest contributes at most
 * MAX_SENSORS_DATA_SUMMARY_SIZE (32) pairs, so this holds several batches of
 * distinct pairs. Overflow is handled fail-safe by AtlPxcPairFusionObserve(). */
#define ATLPXC_MAX_TRACKED_PAIRS 128

/* A pair not re-observed within this window is treated as gone and stops
 * contributing to the worst case. Without expiry a CRITICAL pair would hold the
 * PLC in STOP forever once the objects left the scene. Perception republishes
 * live pairs far faster than this, so the window is generous. */
#define ATLPXC_PAIR_TTL_MS_DEFAULT 2000U
#define ATLPXC_PAIR_TTL_MS_MIN      200U
#define ATLPXC_PAIR_TTL_MS_MAX    60000U

/* Object identity and last-known position for one tracked pair, already
 * normalized to (forklift, person) order for the outbound CmdPacket. */
typedef struct {
    std::uint32_t forkliftObjectID;
    std::uint32_t personObjectID;
    ObjectType    forkliftObjectType;
    ObjectType    personObjectType;
    float         forkliftX;
    float         forkliftY;
    float         personX;
    float         personY;
    std::uint8_t  pipelineID;
    AtlPxcPairRole role;
} AtlPxcPairObjects;

/* Retained state for one tracked pair. */
typedef struct {
    bool               inUse;
    AtlPxcSeverityTier tier;        /* newest observation for this pair */
    std::uint64_t      lastSeenMs;  /* monotonic ms of that observation */
    AtlPxcPairObjects  objects;
} AtlPxcPairEntry;

typedef struct {
    AtlPxcPairEntry entries[ATLPXC_MAX_TRACKED_PAIRS];
    std::uint32_t   ttlMs;
    /* Diagnostics; not used for decisions. */
    std::uint32_t   unresolvedRoleObservations;
    std::uint32_t   capacityRejectedObservations;
    std::uint32_t   evictedEntries;
    std::uint32_t   expiredEntries;
} AtlPxcPairTable;

/* Result of collapsing the pair table to one command input. */
typedef struct {
    AtlPxcSeverityTier tier;        /* ATLPXC_TIER_NONE when no live pair */
    AtlPxcPairObjects  objects;     /* metadata of the worst-case pair    */
    std::uint32_t      livePairs;   /* pairs still inside the TTL window  */
    bool               haveObjects; /* false when tier == ATLPXC_TIER_NONE */
} AtlPxcAggregate;

/* ---------------------------------------------------------------- helpers */

static inline void AtlPxcPairFusionInit(AtlPxcPairTable* table, std::uint32_t ttlMs)
{
    if (table == nullptr)
        return;

    std::memset(table, 0, sizeof(*table));

    if (ttlMs < ATLPXC_PAIR_TTL_MS_MIN)
        ttlMs = ATLPXC_PAIR_TTL_MS_MIN;
    else if (ttlMs > ATLPXC_PAIR_TTL_MS_MAX)
        ttlMs = ATLPXC_PAIR_TTL_MS_MAX;
    table->ttlMs = ttlMs;
}

/* Drop all retained pair state, preserving the configured TTL. Used on PLC
 * safe-release so a released latch does not immediately re-trip on pairs
 * observed before the release. */
static inline void AtlPxcPairFusionReset(AtlPxcPairTable* table)
{
    if (table == nullptr)
        return;

    const std::uint32_t ttlMs = table->ttlMs;
    std::memset(table, 0, sizeof(*table));
    table->ttlMs = ttlMs;
}

static inline AtlPxcSeverityTier AtlPxcTierFromEventType(EventType type)
{
    switch (type) {
        case ATLPXC_EVENT_VIOLATION_CRITICAL: return ATLPXC_TIER_CRITICAL;
        case ATLPXC_EVENT_VIOLATION_WARNING:  return ATLPXC_TIER_WARNING;
        case ATLPXC_EVENT_NO_VIOLATION:       return ATLPXC_TIER_NORMAL;
        default:                              return ATLPXC_TIER_NONE;
    }
}

/* Resolve (forklift, person) roles from a proximity event's metadata.
 *
 * Returns true when exactly one endpoint is a PERSON, in which case *out is
 * populated in (forklift, person) order with role ATLPXC_ROLE_RESOLVED.
 *
 * Returns false when the pair is person-to-person, or carries no PERSON at all.
 * *out is still populated, keyed by ascending object ID with role
 * ATLPXC_ROLE_UNRESOLVED, so the caller can keep tracking it. See the file
 * header on why such an observation is retained rather than dropped. */
static inline bool AtlPxcResolvePairRoles(const EventFusionMetadata& meta,
                                          AtlPxcPairObjects* out)
{
    if (out == nullptr)
        return false;

    std::memset(out, 0, sizeof(*out));
    out->pipelineID = meta.pipelineID;

    const bool slot0IsPerson = (meta.objectType[0] == PERSON);
    const bool slot1IsPerson = (meta.objectType[1] == PERSON);

    /* MAX_TRAJECTORY_COORDINATES is 10, so slots 0 and 1 always exist; the
     * static_assert keeps that assumption honest if the protocol shrinks. */
    static_assert(MAX_TRAJECTORY_COORDINATES >= 2,
                  "Pair fusion reads coordinates[0] and coordinates[1]");

    if (slot0IsPerson != slot1IsPerson) {
        const int personSlot   = slot0IsPerson ? 0 : 1;
        const int forkliftSlot = slot0IsPerson ? 1 : 0;

        out->personObjectID     = meta.objectID[personSlot];
        out->personObjectType   = meta.objectType[personSlot];
        out->personX            = meta.coordinates[personSlot].x;
        out->personY            = meta.coordinates[personSlot].y;
        out->forkliftObjectID   = meta.objectID[forkliftSlot];
        out->forkliftObjectType = meta.objectType[forkliftSlot];
        out->forkliftX          = meta.coordinates[forkliftSlot].x;
        out->forkliftY          = meta.coordinates[forkliftSlot].y;
        out->role               = ATLPXC_ROLE_RESOLVED;
        return true;
    }

    /* Roles ambiguous. Key on ascending object ID so repeat observations of the
     * same ambiguous pair land on one entry instead of consuming a slot each. */
    const int lowSlot  = (meta.objectID[0] <= meta.objectID[1]) ? 0 : 1;
    const int highSlot = 1 - lowSlot;

    out->forkliftObjectID   = meta.objectID[lowSlot];
    out->forkliftObjectType = meta.objectType[lowSlot];
    out->forkliftX          = meta.coordinates[lowSlot].x;
    out->forkliftY          = meta.coordinates[lowSlot].y;
    out->personObjectID     = meta.objectID[highSlot];
    out->personObjectType   = meta.objectType[highSlot];
    out->personX            = meta.coordinates[highSlot].x;
    out->personY            = meta.coordinates[highSlot].y;
    out->role               = ATLPXC_ROLE_UNRESOLVED;
    return false;
}

static inline bool AtlPxcSamePair(const AtlPxcPairObjects& a,
                                  const AtlPxcPairObjects& b)
{
    return a.forkliftObjectID == b.forkliftObjectID &&
           a.personObjectID   == b.personObjectID &&
           a.role             == b.role;
}

static inline bool AtlPxcEntryExpired(const AtlPxcPairEntry& entry,
                                      std::uint64_t nowMs,
                                      std::uint32_t ttlMs)
{
    /* Guard against a non-monotonic clock: an entry stamped in the future is
     * treated as fresh rather than as expired by underflow. */
    if (nowMs <= entry.lastSeenMs)
        return false;
    return (nowMs - entry.lastSeenMs) > static_cast<std::uint64_t>(ttlMs);
}

/* ------------------------------------------------------------- observation */

/* Record one proximity observation for one pair.
 *
 * Newest-wins within the pair: an existing entry's tier is overwritten, so a
 * fresh EVENT_8 can lower that pair out of CRITICAL. Cross-pair arbitration is
 * deferred to AtlPxcPairFusionAggregate().
 *
 * Returns true when the observation was recorded. A false return means the
 * table was full of strictly more severe pairs, so the dropped observation
 * could not have raised the emitted command. */
static inline bool AtlPxcPairFusionObserve(AtlPxcPairTable* table,
                                           const AtlPxcPairObjects& objects,
                                           AtlPxcSeverityTier tier,
                                           std::uint64_t nowMs)
{
    if (table == nullptr || tier == ATLPXC_TIER_NONE)
        return false;

    if (objects.role == ATLPXC_ROLE_UNRESOLVED)
        table->unresolvedRoleObservations++;

    int freeSlot = -1;
    int weakestSlot = -1;
    AtlPxcSeverityTier weakestTier = ATLPXC_TIER_CRITICAL;
    std::uint64_t weakestSeenMs = UINT64_MAX;

    for (int i = 0; i < ATLPXC_MAX_TRACKED_PAIRS; ++i) {
        AtlPxcPairEntry& entry = table->entries[i];

        if (!entry.inUse) {
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }

        /* Reclaim expired entries opportunistically so a long-lived table does
         * not report full while holding only stale pairs. */
        if (AtlPxcEntryExpired(entry, nowMs, table->ttlMs)) {
            entry.inUse = false;
            table->expiredEntries++;
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }

        if (AtlPxcSamePair(entry.objects, objects)) {
            entry.tier       = tier;
            entry.lastSeenMs = nowMs;
            entry.objects    = objects;
            return true;
        }

        /* Track the eviction candidate: least severe, oldest among equals. */
        if (entry.tier < weakestTier ||
            (entry.tier == weakestTier && entry.lastSeenMs < weakestSeenMs)) {
            weakestTier   = entry.tier;
            weakestSlot   = i;
            weakestSeenMs = entry.lastSeenMs;
        }
    }

    if (freeSlot >= 0) {
        AtlPxcPairEntry& entry = table->entries[freeSlot];
        entry.inUse      = true;
        entry.tier       = tier;
        entry.lastSeenMs = nowMs;
        entry.objects    = objects;
        return true;
    }

    /* Table full of live pairs. Admit the newcomer only if it is strictly more
     * severe than the weakest resident, evicting that resident. Otherwise drop
     * it: it is no more severe than a pair already represented, so it cannot
     * change the worst case and dropping it cannot lower the command. */
    if (weakestSlot >= 0 && tier > weakestTier) {
        AtlPxcPairEntry& entry = table->entries[weakestSlot];
        entry.inUse      = true;
        entry.tier       = tier;
        entry.lastSeenMs = nowMs;
        entry.objects    = objects;
        table->evictedEntries++;
        return true;
    }

    table->capacityRejectedObservations++;
    return false;
}

/* ------------------------------------------------------------- aggregation */

/* Collapse retained pair state to the single worst-case tier, expiring pairs
 * older than the TTL. Ties are broken toward the most recently observed pair so
 * the reported object metadata is the freshest view of the worst case. */
static inline void AtlPxcPairFusionAggregate(AtlPxcPairTable* table,
                                             std::uint64_t nowMs,
                                             AtlPxcAggregate* out)
{
    if (out == nullptr)
        return;

    std::memset(out, 0, sizeof(*out));
    out->tier = ATLPXC_TIER_NONE;

    if (table == nullptr)
        return;

    int bestSlot = -1;
    for (int i = 0; i < ATLPXC_MAX_TRACKED_PAIRS; ++i) {
        AtlPxcPairEntry& entry = table->entries[i];
        if (!entry.inUse)
            continue;

        if (AtlPxcEntryExpired(entry, nowMs, table->ttlMs)) {
            entry.inUse = false;
            table->expiredEntries++;
            continue;
        }

        out->livePairs++;

        if (bestSlot < 0 ||
            entry.tier > table->entries[bestSlot].tier ||
            (entry.tier == table->entries[bestSlot].tier &&
             entry.lastSeenMs > table->entries[bestSlot].lastSeenMs)) {
            bestSlot = i;
        }
    }

    if (bestSlot >= 0) {
        out->tier        = table->entries[bestSlot].tier;
        out->objects     = table->entries[bestSlot].objects;
        out->haveObjects = true;
    }
}

#endif /* ATL_PROXIMITY_PAIR_FUSION_H */
