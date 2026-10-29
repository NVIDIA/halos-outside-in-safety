/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_KAFKA_FRAME_SOURCE_H
#define PCM_KAFKA_FRAME_SOURCE_H

#include "data_validation_monitor.h"

#include <string>

/* Opaque message-bus handle (defined in NvPSFMsgBus.h, included only by the .cpp). */
struct NvPSFMsgBusHandle_t;

/*
 * IFrameSource backed by the Kafka mdx-frames topic, using the same in-house
 * NvPSFMsgBus (consumer) + NvPSFMsgCodec (FrameMessage decode) path that
 * adapters/vss/mdx-client uses -- no raw librdkafka. The consumer is
 * created lazily on the first poll and re-created after a connection failure,
 * so the broker coming up after PCM is handled gracefully.
 *
 * A distinct consumer group (vs. mdx-client's) ensures DVM receives its own
 * copy of every frame and does not steal messages from mdx-client.
 */
class KafkaFrameSource : public IFrameSource
{
public:
    KafkaFrameSource(std::string brokers, std::string topic, std::string groupId);
    ~KafkaFrameSource() override;

    KafkaFrameSource(const KafkaFrameSource&) = delete;
    KafkaFrameSource& operator=(const KafkaFrameSource&) = delete;

    Status poll(DvmFrame& out, int timeoutMs) override;

private:
    /* Create the consumer if not yet connected. False -> Disconnected. */
    bool ensureConnected();

    std::string         brokers_;
    std::string         topic_;
    std::string         groupId_;
    NvPSFMsgBusHandle_t* consumer_ = nullptr;
    bool                createFailLogged_ = false;
    /* Consecutive messages that arrived but could not be decoded into a valid
     * frame. A sustained run means the source yields only unusable data, which is
     * reported as InvalidData (reset on the next decoded frame). */
    unsigned int        consecutiveDecodeFailures_ = 0U;
};

#endif /* PCM_KAFKA_FRAME_SOURCE_H */
