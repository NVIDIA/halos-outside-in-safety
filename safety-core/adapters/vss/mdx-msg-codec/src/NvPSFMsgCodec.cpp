/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "NvPSFMsgCodec.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/text_format.h>
#include "schema.pb.h"
#include "ext.pb.h"
#include "event_mapping.pb.h"
#pragma GCC diagnostic pop

/* protobuf_util.h must come AFTER protobuf headers since it references google::protobuf::Message */
#include "protobuf_util.h"

/* ---- Opaque handle definition ---- */
struct NvPSFMsgCodecMsg_t {
    google::protobuf::Message* msg;
    NvPSFMsgCodecMsgType type;
    bool owned; /* true if this handle owns the Message (must delete on free) */
};

/* ---- Helper: allocate a new Message by type ---- */
static google::protobuf::Message* createMessageByType(NvPSFMsgCodecMsgType type) {
    switch (type) {
    case NvPSF_MSG_BEHAVIOR:
        return new nv::Behavior();
    case NvPSF_MSG_FRAME:
        return new nv::Frame();
    case NvPSF_MSG_EVENT_MAPPING:
        return new mdx::client::config::EventMappingConfig();
    default:
        return nullptr;
    }
}

/* ---- Helper: convert protobuf_util Result to NvPSFMsgCodecFieldResult ---- */
static NvPSFMsgCodecFieldResult convertResult(const Result& r) {
    NvPSFMsgCodecFieldResult fr;
    switch (r.type) {
    case VALUE_TYPE_INT32:     fr.type = NvPSF_VALUE_INT32;     fr.data.i32 = r.data.i32; break;
    case VALUE_TYPE_INT64:     fr.type = NvPSF_VALUE_INT64;     fr.data.i64 = r.data.i64; break;
    case VALUE_TYPE_UINT32:    fr.type = NvPSF_VALUE_UINT32;    fr.data.u32 = r.data.u32; break;
    case VALUE_TYPE_UINT64:    fr.type = NvPSF_VALUE_UINT64;    fr.data.u64 = r.data.u64; break;
    case VALUE_TYPE_FLOAT:     fr.type = NvPSF_VALUE_FLOAT;     fr.data.f = r.data.f; break;
    case VALUE_TYPE_DOUBLE:    fr.type = NvPSF_VALUE_DOUBLE;    fr.data.d = r.data.d; break;
    case VALUE_TYPE_STRING:    fr.type = NvPSF_VALUE_STRING;    fr.data.s = r.data.s; break;
    case VALUE_TYPE_BOOL:      fr.type = NvPSF_VALUE_BOOL;      fr.data.b = r.data.b; break;
    case VALUE_TYPE_TIMESTAMP: fr.type = NvPSF_VALUE_TIMESTAMP;
                               fr.data.timestamp.seconds = r.data.timestamp.seconds;
                               fr.data.timestamp.nanos = r.data.timestamp.nanos; break;
    case VALUE_TYPE_MAPVALUE:  fr.type = NvPSF_VALUE_MAPVALUE;  fr.data.mapValue = r.data.mapValue; break;
    default:                   fr.type = NvPSF_VALUE_ERROR;     break;
    }
    return fr;
}

/* ---- Helper: navigate to a field descriptor by dot-separated path ---- */
static const google::protobuf::FieldDescriptor* navigateToField(
    const google::protobuf::Message* msg, const char* path,
    const google::protobuf::Message** outParent) {

    using namespace google::protobuf;
    const Message* current = msg;
    std::string pathStr(path);

    /* Split by dots and navigate */
    size_t start = 0;
    size_t dot;
    std::string token;

    while ((dot = pathStr.find('.', start)) != std::string::npos) {
        token = pathStr.substr(start, dot - start);
        start = dot + 1;

        /* Handle array index in token e.g. "objects[0]" */
        int index = 0;
        size_t bracket = token.find('[');
        std::string fieldName = token;
        if (bracket != std::string::npos) {
            fieldName = token.substr(0, bracket);
            index = std::atoi(token.substr(bracket + 1).c_str());
        }

        const Descriptor* desc = current->GetDescriptor();
        const Reflection* ref = current->GetReflection();
        const FieldDescriptor* fd = desc->FindFieldByName(fieldName);
        if (!fd || fd->type() != FieldDescriptor::TYPE_MESSAGE) return nullptr;

        if (fd->is_repeated()) {
            if (index < 0 || index >= ref->FieldSize(*current, fd)) return nullptr;
            current = &ref->GetRepeatedMessage(*current, fd, index);
        } else {
            if (!ref->HasField(*current, fd)) return nullptr;
            current = &ref->GetMessage(*current, fd);
        }
    }

    /* Final token — strip array index if present, only field name is needed for descriptor lookup */
    token = pathStr.substr(start);
    size_t bracket = token.find('[');
    std::string fieldName = token;
    if (bracket != std::string::npos) {
        fieldName = token.substr(0, bracket);
    }

    const Descriptor* desc = current->GetDescriptor();
    const FieldDescriptor* fd = desc->FindFieldByName(fieldName);
    if (outParent) *outParent = current;
    return fd;
}

/* ======================================================================== */
/* API Implementation                                                       */
/* ======================================================================== */

NvPSFMsgCodecErr NvPSFMsgCodecDecode(const void* buf, size_t len,
    NvPSFMsgCodecMsgType type, NvPSFMsgCodecMsg** out) {

    if (!buf || len == 0 || !out) return NvPSFMSGCODEC_FAIL;
    if (len > static_cast<size_t>(INT32_MAX)) return NvPSFMSGCODEC_FAIL;

    google::protobuf::Message* msg = createMessageByType(type);
    if (!msg) return NvPSFMSGCODEC_FAIL;

    if (!msg->ParseFromArray(buf, static_cast<int>(len))) {
        delete msg;
        return NvPSFMSGCODEC_FAIL;
    }

    *out = new NvPSFMsgCodecMsg_t{msg, type, true};
    return NvPSFMSGCODEC_SUCCESS;
}

NvPSFMsgCodecErr NvPSFMsgCodecDecodeFromFile(const char* path,
    NvPSFMsgCodecMsgType type, NvPSFMsgCodecMsg** out) {

    if (!path || !out) return NvPSFMSGCODEC_FAIL;

    google::protobuf::Message* msg = createMessageByType(type);
    if (!msg) return NvPSFMSGCODEC_FAIL;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        delete msg;
        return NvPSFMSGCODEC_FAIL;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string data = buf.str();
    f.close();

    /* Try binary first */
    if (msg->ParseFromString(data)) {
        *out = new NvPSFMsgCodecMsg_t{msg, type, true};
        return NvPSFMSGCODEC_SUCCESS;
    }

    /* Try text format */
    msg->Clear();
    if (google::protobuf::TextFormat::ParseFromString(data, msg)) {
        *out = new NvPSFMsgCodecMsg_t{msg, type, true};
        return NvPSFMSGCODEC_SUCCESS;
    }

    delete msg;
    return NvPSFMSGCODEC_FAIL;
}

/* Stack-based path copy to avoid heap allocation for typical field paths */
#define STACK_PATH_MAX 256

NvPSFMsgCodecFieldResult NvPSFMsgCodecGetField(const NvPSFMsgCodecMsg* msg,
    const char* path) {

    NvPSFMsgCodecFieldResult errResult;
    errResult.type = NvPSF_VALUE_ERROR;

    if (!msg || !msg->msg || !path) return errResult;

    size_t pathLen = strlen(path) + 1;
    char stackBuf[STACK_PATH_MAX];
    char* pathCopy = (pathLen <= STACK_PATH_MAX) ? stackBuf : strdup(path);
    if (!pathCopy) return errResult;
    if (pathCopy == stackBuf) memcpy(stackBuf, path, pathLen);

    Result r = getFieldValue(*msg->msg, pathCopy);
    if (pathCopy != stackBuf) free(pathCopy);
    return convertResult(r);
}

bool NvPSFMsgCodecGetFieldPresence(const NvPSFMsgCodecMsg* msg,
    const char* path) {

    if (!msg || !msg->msg || !path) return false;

    size_t pathLen = strlen(path) + 1;
    char stackBuf[STACK_PATH_MAX];
    char* pathCopy = (pathLen <= STACK_PATH_MAX) ? stackBuf : strdup(path);
    if (!pathCopy) return false;
    if (pathCopy == stackBuf) memcpy(stackBuf, path, pathLen);

    bool result = getFieldPresence(*msg->msg, pathCopy);
    if (pathCopy != stackBuf) free(pathCopy);
    return result;
}

int NvPSFMsgCodecGetRepeatedCount(const NvPSFMsgCodecMsg* msg,
    const char* path) {

    if (!msg || !msg->msg || !path) return 0;

    using namespace google::protobuf;

    /* Navigate to the parent message and find the field */
    const Message* parent = nullptr;
    const FieldDescriptor* fd = navigateToField(msg->msg, path, &parent);

    if (!fd || !parent) {
        /* path might be a simple field name (no dots) */
        const Descriptor* desc = msg->msg->GetDescriptor();
        fd = desc->FindFieldByName(path);
        parent = msg->msg;
    }

    if (!fd || !fd->is_repeated()) return 0;

    const Reflection* ref = parent->GetReflection();
    return ref->FieldSize(*parent, fd);
}

NvPSFMsgCodecErr NvPSFMsgCodecGetSubMsg(const NvPSFMsgCodecMsg* msg,
    const char* path, NvPSFMsgCodecMsg** out) {

    if (!msg || !msg->msg || !path || !out) return NvPSFMSGCODEC_FAIL;

    using namespace google::protobuf;

    const Message* current = msg->msg;
    std::string pathStr(path);

    /* Tokenize by dots and navigate, handling array indices */
    size_t start = 0;
    while (start < pathStr.size()) {
        size_t dot = pathStr.find('.', start);
        std::string token;
        if (dot != std::string::npos) {
            token = pathStr.substr(start, dot - start);
            start = dot + 1;
        } else {
            token = pathStr.substr(start);
            start = pathStr.size();
        }

        int index = 0;
        size_t bracket = token.find('[');
        std::string fieldName = token;
        if (bracket != std::string::npos) {
            fieldName = token.substr(0, bracket);
            index = std::atoi(token.substr(bracket + 1).c_str());
        }

        const Descriptor* desc = current->GetDescriptor();
        const Reflection* ref = current->GetReflection();
        const FieldDescriptor* fd = desc->FindFieldByName(fieldName);
        if (!fd || fd->type() != FieldDescriptor::TYPE_MESSAGE) return NvPSFMSGCODEC_FAIL;

        if (fd->is_repeated()) {
            if (index < 0 || index >= ref->FieldSize(*current, fd)) return NvPSFMSGCODEC_FAIL;
            current = &ref->GetRepeatedMessage(*current, fd, index);
        } else {
            if (!ref->HasField(*current, fd)) return NvPSFMSGCODEC_FAIL;
            current = &ref->GetMessage(*current, fd);
        }
    }

    /* Return a non-owning handle pointing to the sub-message */
    *out = new NvPSFMsgCodecMsg_t{
        const_cast<google::protobuf::Message*>(current),
        msg->type,
        false /* non-owning: parent still owns the memory */
    };
    return NvPSFMSGCODEC_SUCCESS;
}

char* NvPSFMsgCodecGetDebugString(const NvPSFMsgCodecMsg* msg) {
    if (!msg || !msg->msg) return nullptr;
    std::string dbg = msg->msg->DebugString();
    return strdup(dbg.c_str());
}

NvPSFMsgCodecErr NvPSFMsgCodecSetField(NvPSFMsgCodecMsg* msg,
    const char* path, NvPSFMsgCodecFieldData fieldData) {

    if (!msg || !msg->msg || !path) return NvPSFMSGCODEC_FAIL;

    /* Convert NvPSFMsgCodecFieldData to protobuf_util FieldData */
    FieldData fd;
    switch (fieldData.type) {
    case NvPSF_VALUE_INT32:     fd.type = VALUE_TYPE_INT32;     fd.data.i32 = fieldData.data.i32; break;
    case NvPSF_VALUE_INT64:     fd.type = VALUE_TYPE_INT64;     fd.data.i64 = fieldData.data.i64; break;
    case NvPSF_VALUE_UINT32:    fd.type = VALUE_TYPE_UINT32;    fd.data.u32 = fieldData.data.u32; break;
    case NvPSF_VALUE_UINT64:    fd.type = VALUE_TYPE_UINT64;    fd.data.u64 = fieldData.data.u64; break;
    case NvPSF_VALUE_FLOAT:     fd.type = VALUE_TYPE_FLOAT;     fd.data.f = fieldData.data.f; break;
    case NvPSF_VALUE_DOUBLE:    fd.type = VALUE_TYPE_DOUBLE;    fd.data.d = fieldData.data.d; break;
    case NvPSF_VALUE_STRING:    fd.type = VALUE_TYPE_STRING;    fd.data.s = fieldData.data.s; break;
    case NvPSF_VALUE_BOOL:      fd.type = VALUE_TYPE_BOOL;      fd.data.b = fieldData.data.b; break;
    case NvPSF_VALUE_MAPVALUE:  fd.type = VALUE_TYPE_MAPVALUE;  fd.data.mapValue = fieldData.data.mapValue; break;
    case NvPSF_VALUE_TIMESTAMP: fd.type = VALUE_TYPE_TIMESTAMP;
        if (fieldData.data.timestamp) {
            static thread_local TimeStamp tsConv;
            tsConv.seconds = fieldData.data.timestamp->seconds;
            tsConv.nanos = fieldData.data.timestamp->nanos;
            fd.data.timestamp = &tsConv;
        } else {
            return NvPSFMSGCODEC_FAIL;
        }
        break;
    default: return NvPSFMSGCODEC_FAIL;
    }

    size_t pathLen = strlen(path) + 1;
    char stackBuf[STACK_PATH_MAX];
    char* pathCopy = (pathLen <= STACK_PATH_MAX) ? stackBuf : strdup(path);
    if (!pathCopy) return NvPSFMSGCODEC_FAIL;
    if (pathCopy == stackBuf) memcpy(stackBuf, path, pathLen);

    setFieldValue(*msg->msg, pathCopy, fd);
    if (pathCopy != stackBuf) free(pathCopy);
    return NvPSFMSGCODEC_SUCCESS;
}

void NvPSFMsgCodecFreeMsg(NvPSFMsgCodecMsg* msg) {
    if (!msg) return;
    if (msg->owned && msg->msg) {
        delete msg->msg;
    }
    delete msg;
}

bool NvPSFMsgCodecCheckVersion(void) {
    return checkProtobufVersion();
}

void NvPSFMsgCodecShutdown(void) {
    google::protobuf::ShutdownProtobufLibrary();
}
