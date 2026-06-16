/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file  pss_message_validate.h
 * @brief CRC-32 integrity and field-range validation for SafetyEvent and
 *        DecisionRequest wire messages.
 *
 * Pure C with extern "C" linkage so the same object can be linked into
 * C++ host binaries (PSS daemon, PSD, Gateway) and C firmware (FSI SDm).
 *
 * Typical receiver sequence:
 *   1. pssSafetyEventVerifyCRC()  / pssDecisionRequestVerifyCRC()
 *   2. validateSafetyEvent()      / validateDecisionRequest()
 *
 * Typical sender sequence:
 *   1. Fill struct fields
 *   2. pssSafetyEventSetCRC()     / pssDecisionRequestSetCRC()
 *
 * Any component that modifies a struct after CRC computation (e.g. fusion,
 * filtering) must call the SetCRC function again before forwarding.
 */

#ifndef PSS_MESSAGE_VALIDATE_H
#define PSS_MESSAGE_VALIDATE_H

#include "pss_protocol.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== Validation error flags ======================== */

/** Bitmask returned by validateSafetyEvent / validateDecisionRequest.
 *  Zero (PSS_VALID) means all checks passed. */
#define PSS_VALID                          0U
#define PSS_ERR_NULL_POINTER               (1U << 0)
#define PSS_ERR_SCHEMA_VERSION             (1U << 1)
#define PSS_ERR_CRC                        (1U << 2)
#define PSS_ERR_EVENT_TYPE                 (1U << 3)
#define PSS_ERR_SEVERITY                   (1U << 4)
#define PSS_ERR_CONFIDENCE                 (1U << 5)
#define PSS_ERR_SENSOR_IDENTIFIER          (1U << 6)
#define PSS_ERR_RULE_IDENTIFIER            (1U << 7)
#define PSS_ERR_OBJECT_TYPE                (1U << 8)
#define PSS_ERR_EVENT_STATUS               (1U << 9)
#define PSS_ERR_OPERATIONAL_MODE           (1U << 10)
#define PSS_ERR_SENSOR_DATA_SUMMARY_SIZE   (1U << 11)
#define PSS_ERR_TIMESTAMP                  (1U << 12)

/* ======================== Enum range limits ======================== */

/**
 * Maximum valid wire integer for each protocol enum.  Used by the
 * validation helpers; update these when new enumerators are appended.
 */
#define PSS_EVENTTYPE_MAX              ((int)AI_PIPELINE_VALID)
#define PSS_OBJECTTYPE_MAX             ((int)OBJECT)
#define PSS_SEVERITY_LEVEL_MAX         ((int)CRITICAL)
#define PSS_OPERATIONAL_MODE_MAX       ((int)ERROR)
#define PSS_SAFETY_EVENT_STATUS_MAX    ((int)UNKNOWN)

/* ================ Alignment-safe MessageIntegrity accessors ================ */

/*
 * MessageIntegrity lives inside #pragma pack(push, 1) wire structs, so its
 * uint32_t crc32 field may sit at a misaligned offset.  Direct member
 * read/write is undefined behavior on strict-alignment targets (e.g. ARM
 * Cortex-R52).  These helpers use memcpy, which the compiler is free to
 * lower to an unaligned-safe load/store intrinsic at -O1+.
 */

static inline uint16_t pssGetSchemaVersion(const MessageIntegrity *mi)
{
    uint16_t v;
    memcpy(&v, (const char *)mi + offsetof(MessageIntegrity, schemaVersion),
           sizeof(v));
    return v;
}

static inline void pssSetSchemaVersion(MessageIntegrity *mi, uint16_t val)
{
    memcpy((char *)mi + offsetof(MessageIntegrity, schemaVersion),
           &val, sizeof(val));
}

static inline uint32_t pssGetCrc32(const MessageIntegrity *mi)
{
    uint32_t v;
    memcpy(&v, (const char *)mi + offsetof(MessageIntegrity, crc32),
           sizeof(v));
    return v;
}

static inline void pssSetCrc32(MessageIntegrity *mi, uint32_t val)
{
    memcpy((char *)mi + offsetof(MessageIntegrity, crc32),
           &val, sizeof(val));
}

/* ======================== CRC-32 primitives ======================== */

/**
 * @brief Compute CRC-32 (ISO 3309 / ITU-T V.42, polynomial 0xEDB88320
 *        reflected) over an arbitrary byte buffer.
 *
 * @param data   Pointer to input buffer (must not be NULL if length > 0).
 * @param length Number of bytes.
 * @return       CRC-32 value.
 */
uint32_t pssComputeCRC32(const void *data, size_t length);

/**
 * @brief Set integrity.schemaVersion and compute + store integrity.crc32
 *        for a SafetyEvent.
 *
 * CRC covers bytes [0 .. offsetof(SafetyEvent, integrity.crc32) - 1],
 * i.e. all fields including schemaVersion but excluding crc32 itself.
 */
void pssSafetyEventSetCRC(SafetyEvent *event);

/**
 * @brief Verify the CRC-32 stored in a received SafetyEvent.
 * @return true if the CRC matches, false otherwise.
 */
bool pssSafetyEventVerifyCRC(const SafetyEvent *event);

/**
 * @brief Set integrity.schemaVersion and compute + store integrity.crc32
 *        for a DecisionRequest.
 */
void pssDecisionRequestSetCRC(DecisionRequest *req);

/**
 * @brief Verify the CRC-32 stored in a received DecisionRequest.
 * @return true if the CRC matches, false otherwise.
 */
bool pssDecisionRequestVerifyCRC(const DecisionRequest *req);

/* ======================== Field validation ======================== */

/**
 * @brief Validate all fields of a SafetyEvent (including CRC and schema
 *        version).
 *
 * Checks performed:
 *   - Non-NULL pointer
 *   - Schema version == PSS_SCHEMA_VERSION
 *   - CRC-32 integrity
 *   - EventType, SeverityLevel, ObjectType enum ranges
 *   - confidenceLevel in [0.0f, 1.0f] (NaN is rejected)
 *   - timestamp != 0 (rejects uninitialized events)
 *   - sensorIdentifier / ruleIdentifier NUL-terminated within bounds
 *
 * @return PSS_VALID (0) on success, or a bitmask of PSS_ERR_* flags
 *         indicating which checks failed.
 */
uint32_t validateSafetyEvent(const SafetyEvent *event);

/**
 * @brief Validate all fields of a DecisionRequest (including CRC, schema
 *        version, and nested FusedSafetyEvent entries).
 *
 * Checks performed:
 *   - Non-NULL pointer
 *   - Schema version == PSS_SCHEMA_VERSION
 *   - CRC-32 integrity
 *   - sensorDataSummarySize <= MAX_SENSORS_DATA_SUMMARY_SIZE
 *   - OperationalMode range
 *   - Per-sensor FusedSafetyEvent field validation (types, severity,
 *     confidence, timestamp, status, strings, object types)
 *
 * @return PSS_VALID (0) on success, or a bitmask of PSS_ERR_* flags.
 */
uint32_t validateDecisionRequest(const DecisionRequest *req);

#ifdef __cplusplus
}
#endif

#endif /* PSS_MESSAGE_VALIDATE_H */
