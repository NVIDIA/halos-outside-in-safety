/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file  pss_message_validate.c
 * @brief CRC-32 integrity and field-range validation for PSS wire messages.
 *
 * Compiled as C99 so the same source can be used in Linux host builds
 * (linked into C++ translation units via extern "C") and bare-metal
 * FreeRTOS/FSI firmware (ARM Cortex-R52, armclang -std=c99).
 */

#include "pss_message_validate.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Compile-time layout assertions ----
 * Guarantee that integrity.crc32 is the final wire byte in each top-level
 * message struct.  If a field is ever appended after crc32, or padding
 * changes the layout, the build will fail here rather than silently
 * computing a wrong CRC span.
 */
#define PSS_STATIC_ASSERT(cond, tag) \
    typedef char static_assert_##tag[(cond) ? 1 : -1]

PSS_STATIC_ASSERT(
    offsetof(SafetyEvent, integrity.crc32) + sizeof(uint32_t) == sizeof(SafetyEvent),
    SafetyEvent_crc32_must_be_last_wire_field);

PSS_STATIC_ASSERT(
    offsetof(DecisionRequest, integrity.crc32) + sizeof(uint32_t) == sizeof(DecisionRequest),
    DecisionRequest_crc32_must_be_last_wire_field);

/* ======================== Internal helpers ======================== */

/**
 * Check that a char array contains at least one NUL byte within @p maxLen.
 */
static bool isNulTerminated(const char *s, size_t maxLen)
{
    size_t i;
    for (i = 0; i < maxLen; i++) {
        if (s[i] == '\0')
            return true;
    }
    return false;
}

/**
 * Validate the fields common to both SafetyEvent and FusedSafetyEvent.
 * Centralised so the two structs cannot diverge on range/format checks.
 */
static uint32_t validateCommonEventFields(
    EventType type,
    SeverityLevel severity,
    float confidenceLevel,
    uint64_t timestamp,
    const char *sensorIdentifier,
    const char *ruleIdentifier,
    const EventFusionMetadata *meta)
{
    uint32_t errors = PSS_VALID;
    int k;

    if ((int)type < 0 || (int)type > PSS_EVENTTYPE_MAX)
        errors |= PSS_ERR_EVENT_TYPE;

    if ((int)severity < 0 || (int)severity > PSS_SEVERITY_LEVEL_MAX)
        errors |= PSS_ERR_SEVERITY;

    /* NaN-safe: NaN fails both comparisons so the condition is true. */
    if (!(confidenceLevel >= 0.0f && confidenceLevel <= 1.0f))
        errors |= PSS_ERR_CONFIDENCE;

    /* A zero timestamp indicates an uninitialized or default-constructed
     * event.  Downstream staleness checks divide by 1e6 to get
     * milliseconds, so zero maps to epoch and would always appear stale
     * — reject it at the validation boundary instead. */
    if (timestamp == 0)
        errors |= PSS_ERR_TIMESTAMP;

    if (!isNulTerminated(sensorIdentifier, MAX_INDENTIFIER_LENGTH))
        errors |= PSS_ERR_SENSOR_IDENTIFIER;

    if (!isNulTerminated(ruleIdentifier, MAX_INDENTIFIER_LENGTH))
        errors |= PSS_ERR_RULE_IDENTIFIER;

    for (k = 0; k < 2; k++) {
        if ((int)meta->objectType[k] < 0 ||
            (int)meta->objectType[k] > PSS_OBJECTTYPE_MAX) {
            errors |= PSS_ERR_OBJECT_TYPE;
        }
    }

    return errors;
}

/**
 * Range-check a FusedSafetyEvent: common fields plus FusedSafetyEvent-only
 * status field.  Used by validateDecisionRequest for each sensor entry.
 */
static uint32_t validateFusedEventFields(const FusedSafetyEvent *ev)
{
    if (ev == NULL)
        return PSS_ERR_NULL_POINTER;

    uint32_t errors = validateCommonEventFields(
        ev->type, ev->severity, ev->confidenceLevel,
        ev->timestamp,
        ev->sensorIdentifier, ev->ruleIdentifier, &ev->fusionMetadata);

    if ((int)ev->status < 0 || (int)ev->status > PSS_SAFETY_EVENT_STATUS_MAX)
        errors |= PSS_ERR_EVENT_STATUS;

    return errors;
}

/* ======================== CRC-32 (ISO 3309) ======================== */

/*
 * Bytewise CRC-32 without a lookup table.  Trades ~8x throughput for
 * zero static memory, which is acceptable for the message sizes in this
 * protocol (a few KB at most) and avoids thread-safety concerns around
 * lazy table initialisation on multi-core hosts.
 */
uint32_t pssComputeCRC32(const void *data, size_t length)
{
    const uint8_t *p;
    uint32_t crc = 0xFFFFFFFFU;
    size_t i;
    int j;

    if (data == NULL) {
        /* length == 0 is harmless (loop would not execute), but
         * length > 0 with a NULL pointer is a caller bug.  Return 0
         * rather than crashing, since this function is exported. */
        return (length == 0) ? (crc ^ 0xFFFFFFFFU) : 0U;
    }

    p = (const uint8_t *)data;

    for (i = 0; i < length; i++) {
        crc ^= p[i];
        for (j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ---- SafetyEvent CRC ---- */

void pssSafetyEventSetCRC(SafetyEvent *event)
{
    if (event == NULL)
        return;
    pssSetSchemaVersion(&event->integrity, PSS_SCHEMA_VERSION);
    pssSetCrc32(&event->integrity, 0U);
    pssSetCrc32(&event->integrity,
        pssComputeCRC32(event, offsetof(SafetyEvent, integrity.crc32)));
}

bool pssSafetyEventVerifyCRC(const SafetyEvent *event)
{
    if (event == NULL)
        return false;
    return pssGetCrc32(&event->integrity) ==
           pssComputeCRC32(event, offsetof(SafetyEvent, integrity.crc32));
}

/* ---- DecisionRequest CRC ---- */

void pssDecisionRequestSetCRC(DecisionRequest *req)
{
    if (req == NULL)
        return;
    pssSetSchemaVersion(&req->integrity, PSS_SCHEMA_VERSION);
    pssSetCrc32(&req->integrity, 0U);
    pssSetCrc32(&req->integrity,
        pssComputeCRC32(req, offsetof(DecisionRequest, integrity.crc32)));
}

bool pssDecisionRequestVerifyCRC(const DecisionRequest *req)
{
    if (req == NULL)
        return false;
    return pssGetCrc32(&req->integrity) ==
           pssComputeCRC32(req, offsetof(DecisionRequest, integrity.crc32));
}

/* ======================== Field validation ======================== */

uint32_t validateSafetyEvent(const SafetyEvent *event)
{
    uint32_t errors = PSS_VALID;

    if (event == NULL)
        return PSS_ERR_NULL_POINTER;

    if (pssGetSchemaVersion(&event->integrity) != PSS_SCHEMA_VERSION)
        errors |= PSS_ERR_SCHEMA_VERSION;

    if (!pssSafetyEventVerifyCRC(event))
        errors |= PSS_ERR_CRC;

    errors |= validateCommonEventFields(
        event->type, event->severity, event->confidenceLevel,
        event->timestamp,
        event->sensorIdentifier, event->ruleIdentifier,
        &event->fusionMetadata);

    return errors;
}

uint32_t validateDecisionRequest(const DecisionRequest *req)
{
    uint32_t errors = PSS_VALID;
    uint8_t i;

    if (req == NULL)
        return PSS_ERR_NULL_POINTER;

    if (pssGetSchemaVersion(&req->integrity) != PSS_SCHEMA_VERSION)
        errors |= PSS_ERR_SCHEMA_VERSION;

    if (!pssDecisionRequestVerifyCRC(req))
        errors |= PSS_ERR_CRC;

    if (req->sensorDataSummarySize > MAX_SENSORS_DATA_SUMMARY_SIZE)
        errors |= PSS_ERR_SENSOR_DATA_SUMMARY_SIZE;

    /*
     * sensorDataSummarySize == 0 is only valid for the STOP sentinel
     * (requestId == UINT32_MAX, mode == ERROR).  Any other zero-size
     * request would cause downstream routing code to read uninitialised
     * sensorDataSummary[0] fields.
     */
    if (req->sensorDataSummarySize == 0 &&
        !(req->requestId == UINT32_MAX &&
          (int)req->pssStatus.mode == (int)ERROR))
        errors |= PSS_ERR_SENSOR_DATA_SUMMARY_SIZE;

    if ((int)req->pssStatus.mode < 0 ||
        (int)req->pssStatus.mode > PSS_OPERATIONAL_MODE_MAX) {
        errors |= PSS_ERR_OPERATIONAL_MODE;
    }

    /*
     * Validate each populated SensorData entry.  Cap the loop at the
     * array bound even if sensorDataSummarySize is out of range, so we
     * never read past the array.
     */
    {
        uint8_t count = req->sensorDataSummarySize;
        if (count > MAX_SENSORS_DATA_SUMMARY_SIZE)
            count = MAX_SENSORS_DATA_SUMMARY_SIZE;

        for (i = 0; i < count; i++)
            errors |= validateFusedEventFields(&req->sensorDataSummary[i].event);
    }

    return errors;
}
