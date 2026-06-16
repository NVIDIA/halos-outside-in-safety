/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROTOAPI
#define PROTOAPI

#include <cstdint>
#include <cstdbool>

namespace google { namespace protobuf { class Message; } }

typedef enum
{
    VALUE_TYPE_INT32,
    VALUE_TYPE_INT64,
    VALUE_TYPE_UINT32,
    VALUE_TYPE_UINT64,
    VALUE_TYPE_FLOAT,
    VALUE_TYPE_DOUBLE,
    VALUE_TYPE_STRING,
    VALUE_TYPE_BOOL,
    VALUE_TYPE_TIMESTAMP,
    VALUE_TYPE_ERROR,
    VALUE_TYPE_MAPVALUE
} Type;

typedef struct
{
    int64_t seconds;
    int32_t nanos;
} TimeStamp;

typedef struct
{
    Type type;
    union
    {
        int32_t i32;
        int64_t i64;
        uint32_t u32;
        uint64_t u64;
        float f;
        double d;
        const char *s;
        bool b;
        const char *mapValue;
        TimeStamp timestamp;
    } data;
} Result;

typedef struct
{
    Type type;
    union
    {
        int32_t i32;
        int64_t i64;
        uint32_t u32;
        uint64_t u64;
        float f;
        double d;
        const char *s;
        bool b;
        const char *mapValue;
        TimeStamp *timestamp;
    } data;
} FieldData;

/*
 * C linkage is required for symbol export compatibility with the .export file.
 * These functions accept C++ reference types; C linkage only affects name
 * mangling and is valid per C++ standard [dcl.link].
 */
#ifdef __cplusplus
extern "C" {
#endif

Result getFieldValue(const google::protobuf::Message &, char *);

bool getFieldPresence(const google::protobuf::Message &, char *);

void setFieldValue(google::protobuf::Message &, char *, FieldData);

bool checkProtobufVersion();

Result getFieldValueFromFile(google::protobuf::Message &, char *, char *);

#ifdef __cplusplus
}
#endif

#endif