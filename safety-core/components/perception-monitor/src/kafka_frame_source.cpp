/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kafka_frame_source.h"

#include "NvPSFMsgBus.h"
#include "NvPSFMsgCodec.h"
#include "pcm_log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace {

/* Receive buffer; matches mdx-client's MSGBUS_MSG_BUFFER_SIZE. */
static constexpr int kRecvBufSize       = 8192;
/* Short nap when no message is available, to avoid busy-spinning the loop. */
static constexpr int kIdleSleepMs       = 20;
/* Backoff before retrying a failed consumer creation. */
static constexpr int kReconnectBackoffMs = 1000;
/* Consecutive undecodable messages before reporting a data fault. Isolated
 * corruption is tolerated; DVM/aggregator debounce still governs INVALID. */
static constexpr unsigned int kMaxConsecutiveDecodeFailures = 10U;

/*
 * NvPSFMsgCodec field accessors, mirroring the helpers in mdx-client's
 * FramesParser.cpp. STRING results are heap-allocated by the codec and must be
 * freed by the caller.
 */
static std::string getStringField(const NvPSFMsgCodecMsg* msg, const char* path)
{
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_STRING && r.data.s) {
        std::string s(r.data.s);
        free((void*)r.data.s);
        return s;
    }
    return std::string();
}

static float getFloatField(const NvPSFMsgCodecMsg* msg, const char* path)
{
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_FLOAT)  return r.data.f;
    if (r.type == NvPSF_VALUE_DOUBLE) return static_cast<float>(r.data.d);
    return 0.0F;
}

static double getDoubleField(const NvPSFMsgCodecMsg* msg, const char* path)
{
    const NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_DOUBLE) return r.data.d;
    if (r.type == NvPSF_VALUE_FLOAT) return static_cast<double>(r.data.f);
    /* Do not turn an unreadable decision coordinate into a plausible origin. */
    return std::numeric_limits<double>::quiet_NaN();
}

static bool getMapStringField(const NvPSFMsgCodecMsg* msg, const char* path,
                              std::string& value)
{
    value.clear();
    if (!NvPSFMsgCodecGetFieldPresence(msg, path)) return false;
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type != NvPSF_VALUE_MAPVALUE || r.data.mapValue == nullptr) return false;
    std::unique_ptr<char, decltype(&std::free)> text(
        const_cast<char*>(r.data.mapValue), &std::free);
    value.assign(text.get());
    return true;
}

static int32_t getInt32Field(const NvPSFMsgCodecMsg* msg, const char* path)
{
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_INT32) return r.data.i32;
    return 0;
}

static bool getTimestampMs(const NvPSFMsgCodecMsg* msg, const char* path,
                           int64_t& timestampMs)
{
    timestampMs = 0;
    if (!NvPSFMsgCodecGetFieldPresence(msg, path)) return false;
    NvPSFMsgCodecFieldResult r = NvPSFMsgCodecGetField(msg, path);
    if (r.type == NvPSF_VALUE_TIMESTAMP) {
        const int64_t seconds = r.data.timestamp.seconds;
        const int32_t nanos = r.data.timestamp.nanos;
        /* Current perception timestamps cannot precede the Unix epoch. The
         * protobuf upper bound (9999-12-31) also makes the conversion overflow
         * safe, including the nanosecond remainder. */
        if (seconds < 0 || seconds > INT64_C(253402300799) ||
            nanos < 0 || nanos > 999999999) return false;
        timestampMs = seconds * 1000 + nanos / 1000000;
        return true;
    }
    return false;
}

/* Decode one mdx-frames FrameMessage into a DvmFrame. */
static bool decodeFrameMessage(const void* buf, size_t len, DvmFrame& out)
{
    out = DvmFrame{};
    NvPSFMsgCodecMsg* frameMsg = nullptr;
    if (NvPSFMsgCodecDecode(buf, len, NvPSF_MSG_FRAME, &frameMsg) != NvPSFMSGCODEC_SUCCESS ||
        frameMsg == nullptr) {
        PcmLogLine(PcmLogLevel::Warning) << "DVM: failed to decode mdx-frames message";
        return false;
    }
    using CodecMessage = std::unique_ptr<NvPSFMsgCodecMsg,
                                         decltype(&NvPSFMsgCodecFreeMsg)>;
    CodecMessage frameGuard(frameMsg, &NvPSFMsgCodecFreeMsg);

    out.sensorId    = getStringField(frameMsg, "sensorId");
    out.frameId     = getStringField(frameMsg, "id");
    out.timestampValid = getTimestampMs(frameMsg, "timestamp", out.timestampMs);

    const int objCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "objects");
    if (objCount > 0) out.objects.reserve(static_cast<size_t>(objCount));
    for (int i = 0; i < objCount; ++i) {
        NvPSFMsgCodecMsg* obj = nullptr;
        char path[32];
        std::snprintf(path, sizeof(path), "objects[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, path, &obj) != NvPSFMSGCODEC_SUCCESS || obj == nullptr) {
            continue;
        }
        CodecMessage objectGuard(obj, &NvPSFMsgCodecFreeMsg);
        DvmObject o;
        o.id         = getStringField(obj, "id");
        o.type       = getStringField(obj, "type");
        o.confidence = getFloatField(obj, "confidence");
        o.speed      = getFloatField(obj, "speed");
        o.hasBbox    = NvPSFMsgCodecGetFieldPresence(obj, "bbox");
        if (o.hasBbox) {
            o.bbox.leftX   = getFloatField(obj, "bbox.leftX");
            o.bbox.topY    = getFloatField(obj, "bbox.topY");
            o.bbox.rightX  = getFloatField(obj, "bbox.rightX");
            o.bbox.bottomY = getFloatField(obj, "bbox.bottomY");
        }
        o.hasCoordinate = NvPSFMsgCodecGetFieldPresence(obj, "coordinate");
        if (o.hasCoordinate) {
            /* Scalar zero is a valid proto3 default; test the enclosing
             * coordinate message, not individual x/y/z field presence. */
            o.x = getDoubleField(obj, "coordinate.x");
            o.y = getDoubleField(obj, "coordinate.y");
            o.z = getDoubleField(obj, "coordinate.z");
        }
        out.objects.push_back(std::move(o));
    }

    const int fovCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "fov");
    for (int i = 0; i < fovCount; ++i) {
        NvPSFMsgCodecMsg* tc = nullptr;
        char path[32];
        std::snprintf(path, sizeof(path), "fov[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, path, &tc) != NvPSFMSGCODEC_SUCCESS || tc == nullptr) {
            continue;
        }
        CodecMessage countGuard(tc, &NvPSFMsgCodecFreeMsg);
        DvmTypeCount c;
        c.type  = getStringField(tc, "type");
        c.count = getInt32Field(tc, "count");
        out.fov.push_back(std::move(c));
    }

    const int roiCount = NvPSFMsgCodecGetRepeatedCount(frameMsg, "rois");
    if (roiCount > 0) out.rois.reserve(static_cast<size_t>(roiCount));
    for (int i = 0; i < roiCount; ++i) {
        NvPSFMsgCodecMsg* roi = nullptr;
        char path[32];
        std::snprintf(path, sizeof(path), "rois[%d]", i);
        if (NvPSFMsgCodecGetSubMsg(frameMsg, path, &roi) != NvPSFMSGCODEC_SUCCESS ||
            roi == nullptr) continue;
        CodecMessage roiGuard(roi, &NvPSFMsgCodecFreeMsg);
        DvmRoi decoded;
        decoded.id = getStringField(roi, "id");
        decoded.type = getStringField(roi, "type");
        decoded.hasRestricted = getMapStringField(
            roi, "info['restrictedAreaViolation']", decoded.restricted);
        decoded.hasConfined = getMapStringField(
            roi, "info['confinedAreaViolation']", decoded.confined);
        const int idCount = NvPSFMsgCodecGetRepeatedCount(roi, "objectIds");
        if (idCount > 0) decoded.objectIds.reserve(static_cast<size_t>(idCount));
        for (int j = 0; j < idCount; ++j) {
            std::snprintf(path, sizeof(path), "objectIds[%d]", j);
            decoded.objectIds.push_back(getStringField(roi, path));
        }
        out.rois.push_back(std::move(decoded));
    }

    out.proximity.present = NvPSFMsgCodecGetFieldPresence(frameMsg, "socialDistancing");
    if (out.proximity.present) {
        out.proximity.threshold = getDoubleField(frameMsg, "socialDistancing.threshold");
        out.proximity.detections = getInt32Field(frameMsg, "socialDistancing.proximityDetections");
        out.proximity.hasViolation = getMapStringField(
            frameMsg, "socialDistancing.info['proximityViolation']", out.proximity.violation);
    }
    return true;
}

} // namespace

KafkaFrameSource::KafkaFrameSource(std::string brokers, std::string topic, std::string groupId)
    : brokers_(std::move(brokers))
    , topic_(std::move(topic))
    , groupId_(std::move(groupId))
{}

KafkaFrameSource::~KafkaFrameSource()
{
    if (consumer_ != nullptr) {
        NvPSFMsgBusDestroy(consumer_);
        consumer_ = nullptr;
    }
    NvPSFMsgCodecShutdown();
}

bool KafkaFrameSource::ensureConnected()
{
    if (consumer_ != nullptr) return true;

    NvPSFMsgBusHandle* handle = nullptr;
    NvPSFMsgBusStatus status = NvPSFMsgBusCreate(brokers_.c_str(), topic_.c_str(),
                                                 MSGBUS_CONSUMER, groupId_.c_str(), &handle);
    if (status.err != NvPSFMSGBUS_SUCCESS || handle == nullptr) {
        if (handle != nullptr) NvPSFMsgBusDestroy(handle);
        if (!createFailLogged_) {
            /* Do not log brokers_: it exposes internal hostnames/ports. */
            PcmLogLine(PcmLogLevel::Warning) << "DVM: mdx-frames consumer create failed; will retry";
            createFailLogged_ = true;
        }
        return false;
    }

    /* DVM validates current-time data only (design: auto.offset.reset=latest). */
    (void)NvPSFMsgBusSeekToEnd(handle);
    consumer_ = handle;
    createFailLogged_ = false;
    PcmLogLine(PcmLogLevel::Info) << "DVM: subscribed to '" << topic_ << "' (group "
                                  << groupId_ << ")";
    return true;
}

IFrameSource::Status KafkaFrameSource::poll(DvmFrame& out, int timeoutMs)
{
    if (!ensureConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectBackoffMs));
        return Status::Disconnected;
    }

    char buf[kRecvBufSize];
    size_t len = 0;
    NvPSFMsgBusStatus status = NvPSFMsgBusReceive(consumer_, buf, sizeof(buf), &len);

    if (status.err != NvPSFMSGBUS_SUCCESS) {
        /* The bus distinguishes an empty queue (SUCCESS, zero bytes) from a
         * receive failure, including an unreadable/oversized message. A failed
         * receive provides no evidence that the cameras themselves stopped.
         * Recreate the consumer because the bus can latch repeated failures. */
        PcmLogLine(PcmLogLevel::Warning)
            << "DVM: mdx-frames receive failed; source unavailable, reconnecting";
        NvPSFMsgBusDestroy(consumer_);
        consumer_ = nullptr;
        consecutiveDecodeFailures_ = 0U;
        std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectBackoffMs));
        return Status::Disconnected;
    }

    if (len == 0) {
        /* An empty queue cannot distinguish stopped cameras from an unavailable
         * broker. Keep camera-silence checks active unless the bus explicitly
         * fails; an empty poll cannot clear an earlier source failure. */
        const int nap = timeoutMs < kIdleSleepMs ? timeoutMs : kIdleSleepMs;
        if (nap > 0) std::this_thread::sleep_for(std::chrono::milliseconds(nap));
        return Status::Timeout;
    }

    const bool usable = (len <= sizeof(buf)) && decodeFrameMessage(buf, len, out);
    if (!usable) {
        if (len > sizeof(buf)) {
            PcmLogLine(PcmLogLevel::Warning) << "DVM: mdx-frames message exceeds buffer, skipping";
        }
        /* Received bytes are positive evidence of a data fault, not a broker
         * outage. Saturate so a prolonged corrupt stream never wraps or falls
         * back to Timeout; the next decoded frame resets the tolerance. */
        if (consecutiveDecodeFailures_ < kMaxConsecutiveDecodeFailures) {
            ++consecutiveDecodeFailures_;
            if (consecutiveDecodeFailures_ == kMaxConsecutiveDecodeFailures) {
                PcmLogLine(PcmLogLevel::Warning) << "DVM: mdx-frames: " << consecutiveDecodeFailures_
                    << " consecutive undecodable messages; reporting a data fault";
            }
        }
        return consecutiveDecodeFailures_ >= kMaxConsecutiveDecodeFailures ?
            Status::InvalidData : Status::Timeout;
    }

    consecutiveDecodeFailures_ = 0U;
    return Status::Frame;
}
