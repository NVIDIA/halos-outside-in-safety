/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file NvPSFMsgCodec.h
 * @brief Message codec interface for decoding and accessing structured messages.
 *
 * Provides an opaque handle-based API for decoding binary messages, accessing
 * fields by path, iterating repeated fields via sub-message handles, and
 * loading configuration from files. All serialization details are hidden
 * behind this interface.
 *
 * The functions defined in this header can be used in both C and C++ programs.
 */

#ifndef NVPSF_MSGCODEC_H
#define NVPSF_MSGCODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Error codes ---- */
typedef enum {
    NvPSFMSGCODEC_SUCCESS = 0,
    NvPSFMSGCODEC_FAIL = 1
} NvPSFMsgCodecErr;

/* ---- Value types ---- */
typedef enum {
    NvPSF_VALUE_INT32,
    NvPSF_VALUE_INT64,
    NvPSF_VALUE_UINT32,
    NvPSF_VALUE_UINT64,
    NvPSF_VALUE_FLOAT,
    NvPSF_VALUE_DOUBLE,
    NvPSF_VALUE_STRING,
    NvPSF_VALUE_BOOL,
    NvPSF_VALUE_TIMESTAMP,
    NvPSF_VALUE_ERROR,
    NvPSF_VALUE_MAPVALUE
} NvPSFMsgCodecValueType;

typedef struct {
    int64_t seconds;
    int32_t nanos;
} NvPSFMsgCodecTimeStamp;

/**
 * @brief Result of a field access operation.
 *
 * Ownership rules for pointer members:
 *  - NvPSF_VALUE_STRING  (data.s):        heap-allocated copy. Caller must free().
 *  - NvPSF_VALUE_MAPVALUE (data.mapValue): heap-allocated copy. Caller must free().
 *  - All other types: value types, no free required.
 *
 * If type == NvPSF_VALUE_ERROR, the field was not found or an error occurred;
 * no union member is valid.
 */
typedef struct {
    NvPSFMsgCodecValueType type;
    union {
        int32_t i32;
        int64_t i64;
        uint32_t u32;
        uint64_t u64;
        float f;
        double d;
        const char* s;        /**< Heap-allocated. Caller must free(). */
        bool b;
        const char* mapValue; /**< Heap-allocated. Caller must free(). */
        NvPSFMsgCodecTimeStamp timestamp;
    } data;
} NvPSFMsgCodecFieldResult;

/* ---- Message types ---- */
typedef enum {
    NvPSF_MSG_BEHAVIOR     = 0,  /* mdx-events: Behavior */
    NvPSF_MSG_FRAME        = 1,  /* mdx-frames: FrameMessage */
    NvPSF_MSG_EVENT_MAPPING = 2  /* EventMappingConfig (mdx-client config) */
} NvPSFMsgCodecMsgType;

/* ---- Opaque message handle ---- */
typedef struct NvPSFMsgCodecMsg_t NvPSFMsgCodecMsg;

/* ---- Decode raw bytes into an opaque message handle ---- */
NvPSFMsgCodecErr NvPSFMsgCodecDecode(const void* buf, size_t len,
    NvPSFMsgCodecMsgType type, NvPSFMsgCodecMsg** out);

/* ---- Decode from file (tries binary first, then text format) ---- */
NvPSFMsgCodecErr NvPSFMsgCodecDecodeFromFile(const char* path,
    NvPSFMsgCodecMsgType type, NvPSFMsgCodecMsg** out);

/* ---- Field access by path ---- */
NvPSFMsgCodecFieldResult NvPSFMsgCodecGetField(const NvPSFMsgCodecMsg* msg,
    const char* path);

/* ---- Check if a field is present ---- */
bool NvPSFMsgCodecGetFieldPresence(const NvPSFMsgCodecMsg* msg,
    const char* path);

/* ---- Get count of repeated field elements ---- */
int NvPSFMsgCodecGetRepeatedCount(const NvPSFMsgCodecMsg* msg,
    const char* path);

/**
 * @brief Get a sub-message handle for efficient repeated field iteration.
 *
 * Use this to avoid re-navigating from the root for each field access.
 * The returned handle is a non-owning reference and must be freed with
 * NvPSFMsgCodecFreeMsg(). The parent message must remain valid while
 * the sub-message handle is in use.
 *
 * Example:
 *   NvPSFMsgCodecMsg* obj = NULL;
 *   NvPSFMsgCodecGetSubMsg(msg, "objects[0]", &obj);
 *   NvPSFMsgCodecGetField(obj, "id");
 *   NvPSFMsgCodecFreeMsg(obj);
 */
NvPSFMsgCodecErr NvPSFMsgCodecGetSubMsg(const NvPSFMsgCodecMsg* msg,
    const char* path, NvPSFMsgCodecMsg** out);

/**
 * @brief Get debug string representation of a message or sub-message.
 *
 * Returns a dynamically allocated string. Caller must free() the result.
 * Returns NULL on failure.
 */
char* NvPSFMsgCodecGetDebugString(const NvPSFMsgCodecMsg* msg);

/* ---- Set a field value by path ---- */
typedef struct {
    NvPSFMsgCodecValueType type;
    union {
        int32_t i32;
        int64_t i64;
        uint32_t u32;
        uint64_t u64;
        float f;
        double d;
        const char* s;
        bool b;
        const char* mapValue;
        NvPSFMsgCodecTimeStamp* timestamp;
    } data;
} NvPSFMsgCodecFieldData;

NvPSFMsgCodecErr NvPSFMsgCodecSetField(NvPSFMsgCodecMsg* msg,
    const char* path, NvPSFMsgCodecFieldData fieldData);

/* ---- Cleanup ---- */
void NvPSFMsgCodecFreeMsg(NvPSFMsgCodecMsg* msg);

/* ---- Lifecycle ---- */
bool NvPSFMsgCodecCheckVersion(void);
void NvPSFMsgCodecShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NVPSF_MSGCODEC_H */
