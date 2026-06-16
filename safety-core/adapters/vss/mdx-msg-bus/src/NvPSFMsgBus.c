/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "NvPSFMsgBus.h"
#include <rdkafka.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>

// Configuration: Set to 1 to use callback-based mode internally, 0 for direct polling
#define USE_CALLBACK_MODE 1
#define MAX_PAYLOAD_SIZE 65536
#define MESSAGE_QUEUE_CAPACITY 1000
#define CONSECUTIVE_ERROR_THRESHOLD 100
#define KAFKA_RECONNECT_BACKOFF_MS       "1000"
#define KAFKA_RECONNECT_BACKOFF_MAX_MS   "10000"
#define KAFKA_RETRY_BACKOFF_MS           "1000"
#define KAFKA_METADATA_MAX_AGE_MS        "30000"
#define KAFKA_MAX_POLL_INTERVAL_MS       "86400000"
#define POLL_THREAD_TIMEOUT_MS           100
#define KAFKA_FLUSH_TIMEOUT_MS           1000
#define DEQUEUE_TIMEOUT_MS               5
#define CONSUMER_POLL_TIMEOUT_MS         5
#define SEEK_PARTITIONS_TIMEOUT_MS       5000
#define SEEK_MAX_RETRIES                 100
#define SEEK_POLL_TIMEOUT_MS             100

#if USE_CALLBACK_MODE

typedef struct {
    char payload[MAX_PAYLOAD_SIZE];
    size_t len;
    int err;
    bool valid;
} MessageBuffer;

typedef struct {
    MessageBuffer* buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} NvPSFMsgBusMsgQueue;
#endif

struct NvPSFMsgBusHandle_t
{
    rd_kafka_t* rk;
    rd_kafka_topic_t* rkt;
    char* topic;
    NvPSFMsgBusEndpointType type;
    time_t last_successful_poll;
    int consecutive_failures;
    pthread_mutex_t stats_mutex;
    bool initialized;

#if USE_CALLBACK_MODE
    NvPSFMsgBusMsgQueue* msg_queue;
    pthread_t poll_thread;
    volatile bool running;
#endif
};

static NvPSFMsgBusStatus make_status(NvPSFMsgBusErr err, int retCode, size_t recvd_bytes)
{
    NvPSFMsgBusStatus status;
    status.err = err;
    status.retCode = retCode;
    status.recvd_bytes = recvd_bytes;
    return status;
}

#if USE_CALLBACK_MODE
// Message queue implementation
static NvPSFMsgBusMsgQueue* create_message_queue(size_t capacity)
{
    NvPSFMsgBusMsgQueue* queue = NULL;
    int ret = 0;

    queue = (NvPSFMsgBusMsgQueue*)calloc(1, sizeof(NvPSFMsgBusMsgQueue));
    if (!queue)
    {
        return NULL;
    }

    queue->buffer = (MessageBuffer*)calloc(capacity, sizeof(MessageBuffer));
    if (!queue->buffer)
    {
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    ret = pthread_mutex_init(&queue->mutex, NULL);
    if (ret != 0)
    {
        free(queue->buffer);
        free(queue);
        return NULL;
    }

    ret = pthread_cond_init(&queue->not_empty, NULL);
    if (ret != 0)
    {
        ret = pthread_mutex_destroy(&queue->mutex);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("pthread_mutex_destroy failed: %d\n", ret);
#endif
        }
        free(queue->buffer);
        free(queue);
        return NULL;
    }

    ret = pthread_cond_init(&queue->not_full, NULL);
    if (ret != 0)
    {
        ret = pthread_cond_destroy(&queue->not_empty);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("pthread_cond_destroy failed: %d\n", ret);
#endif
        }
        ret = pthread_mutex_destroy(&queue->mutex);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("pthread_mutex_destroy failed: %d\n", ret);
#endif
        }
        free(queue->buffer);
        free(queue);
        return NULL;
    }

    return queue;
}

static void destroy_message_queue(NvPSFMsgBusMsgQueue* queue)
{
    int ret = 0;
    size_t i = 0;

    if (!queue)
    {
        return;
    }

    ret = pthread_mutex_lock(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to lock mutex during queue destruction: %d\n", ret);
#endif
    }

    for (i = 0; i < queue->capacity; i++)
    {
        queue->buffer[i].valid = false;
    }

    ret = pthread_mutex_unlock(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to unlock mutex during queue destruction: %d\n", ret);
#endif
    }

    ret = pthread_mutex_destroy(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("pthread_mutex_destroy failed: %d\n", ret);
#endif
    }

    ret = pthread_cond_destroy(&queue->not_empty);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("pthread_cond_destroy failed: %d\n", ret);
#endif
    }

    ret = pthread_cond_destroy(&queue->not_full);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("pthread_cond_destroy failed: %d\n", ret);
#endif
    }

    free(queue->buffer);
    free(queue);
}

static int enqueue_message(NvPSFMsgBusMsgQueue* queue, const void* payload, size_t len, int err, const volatile bool* shutdown_flag)
{
    int ret = 0;
    int retval = -1;
    MessageBuffer* msg = NULL;
    void* memcpy_ret = NULL;

    if (!queue || !payload || len == 0 || len > MAX_PAYLOAD_SIZE)
    {
        return retval;
    }

    ret = pthread_mutex_lock(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to lock mutex in enqueue: %d\n", ret);
#endif
        return retval;
    }

    while (queue->count >= queue->capacity)
    {
        if (shutdown_flag && !(*shutdown_flag))
        {
            ret = pthread_mutex_unlock(&queue->mutex);
            return retval;
        }
        ret = pthread_cond_wait(&queue->not_full, &queue->mutex);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("Failed to wait on condition variable: %d\n", ret);
#endif
            ret = pthread_mutex_unlock(&queue->mutex);
            if (ret != 0)
            {
#ifdef NVPSF_DBG
                printf("Failed to unlock mutex in enqueue: %d\n", ret);
#endif
            }
            return retval;
        }
    }

    msg = &queue->buffer[queue->tail];

    memcpy_ret = memcpy(msg->payload, payload, len);
    if (memcpy_ret != msg->payload)
    {
#ifdef NVPSF_DBG
        printf("memcpy failed in enqueue\n");
#endif
        ret = pthread_mutex_unlock(&queue->mutex);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("Failed to unlock mutex in enqueue: %d\n", ret);
#endif
        }
        return retval;
    }

    msg->len = len;
    msg->err = err;
    msg->valid = true;

    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    ret = pthread_cond_signal(&queue->not_empty);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to signal condition variable: %d\n", ret);
#endif
    }

    ret = pthread_mutex_unlock(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to unlock mutex in enqueue: %d\n", ret);
#endif
        return retval;
    }

    retval = 0;
    return retval;
}

static int dequeue_message(NvPSFMsgBusMsgQueue* queue, void* buffer, size_t bufferLen, size_t* outLen, int timeout_ms)
{
    int ret = 0;
    int retval = -1;
    struct timespec ts;
    MessageBuffer* msg = NULL;
    int err = 0;
    size_t copyLen = 0;
    void* memcpy_ret = NULL;

    if (!queue || !buffer || bufferLen == 0)
    {
        if (outLen)
        {
            *outLen = 0;
        }
        return retval;
    }

    ret = pthread_mutex_lock(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to lock mutex in dequeue: %d\n", ret);
#endif
        if (outLen)
        {
            *outLen = 0;
        }
        return retval;
    }

    if (queue->count == 0)
    {
        if (timeout_ms > 0)
        {
            ret = clock_gettime(CLOCK_REALTIME, &ts);
            if (ret != 0)
            {
#ifdef NVPSF_DBG
                printf("clock_gettime failed: %d\n", ret);
#endif
                ret = pthread_mutex_unlock(&queue->mutex);
                if (ret != 0)
                {
#ifdef NVPSF_DBG
                    printf("Failed to unlock mutex in dequeue: %d\n", ret);
#endif
                }
                if (outLen)
                {
                    *outLen = 0;
                }
                return retval;
            }

            ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
            ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000L;
            ts.tv_nsec = ts.tv_nsec % 1000000000L;

            ret = pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &ts);
            if (ret != 0 || queue->count == 0)
            {
                ret = pthread_mutex_unlock(&queue->mutex);
                if (ret != 0)
                {
#ifdef NVPSF_DBG
                    printf("Failed to unlock mutex in dequeue: %d\n", ret);
#endif
                }
                if (outLen)
                {
                    *outLen = 0;
                }
                retval = 0;
                return retval; // Timeout, no message
            }
        }
        else
        {
            ret = pthread_mutex_unlock(&queue->mutex);
            if (ret != 0)
            {
#ifdef NVPSF_DBG
                printf("Failed to unlock mutex in dequeue: %d\n", ret);
#endif
            }
            if (outLen)
            {
                *outLen = 0;
            }
            retval = 0;
            return retval;
        }
    }

    msg = &queue->buffer[queue->head];
    if (!msg->valid)
    {
        ret = pthread_mutex_unlock(&queue->mutex);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("Failed to unlock mutex in dequeue: %d\n", ret);
#endif
        }
        return retval;
    }

    err = msg->err;

    /* Fail if buffer is too small to hold the full message */
    if (msg->len > bufferLen)
    {
#ifdef NVPSF_DBG
        printf("dequeue_message: buffer too small (%zu < %zu)\n", bufferLen, msg->len);
#endif
        msg->valid = false;
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
        ret = pthread_cond_signal(&queue->not_full);
        ret = pthread_mutex_unlock(&queue->mutex);
        if (outLen)
        {
            *outLen = 0;
        }
        return -1;
    }

    copyLen = msg->len;

    memcpy_ret = memcpy(buffer, msg->payload, copyLen);
    if (memcpy_ret != buffer)
    {
#ifdef NVPSF_DBG
        printf("memcpy failed in dequeue\n");
#endif
        ret = pthread_mutex_unlock(&queue->mutex);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("Failed to unlock mutex in dequeue: %d\n", ret);
#endif
        }
        return retval;
    }

    if (outLen)
    {
        *outLen = copyLen;
    }

    msg->valid = false;

    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    ret = pthread_cond_signal(&queue->not_full);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to signal condition variable: %d\n", ret);
#endif
    }

    ret = pthread_mutex_unlock(&queue->mutex);
    if (ret != 0)
    {
#ifdef NVPSF_DBG
        printf("Failed to unlock mutex in dequeue: %d\n", ret);
#endif
        return retval;
    }

    retval = err;
    return retval;
}

static void* poll_thread_func(void* arg)
{
    NvPSFMsgBusHandle* handle = NULL;
    rd_kafka_message_t* msg = NULL;
    time_t current_time = 0;

    handle = (NvPSFMsgBusHandle*)arg;

    if (!handle)
    {
#ifdef NVPSF_DBG
        printf("poll_thread_func: handle is NULL\n");
#endif
        return NULL;
    }

    while (handle->running)
    {
        msg = rd_kafka_consumer_poll(handle->rk, POLL_THREAD_TIMEOUT_MS);

        if (!msg)
        {
            continue;
        }

        if (msg->err == RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            // Valid message - enqueue it
            if (msg->len <= MAX_PAYLOAD_SIZE)
            {
                int ret = enqueue_message(handle->msg_queue, msg->payload, msg->len, 0, &handle->running);
                if (ret == 0)
                {
                    current_time = time(NULL);
                    pthread_mutex_lock(&handle->stats_mutex);
                    if (current_time != (time_t)(-1))
                    {
                        handle->last_successful_poll = current_time;
                    }
                    handle->consecutive_failures = 0;
                    pthread_mutex_unlock(&handle->stats_mutex);
                }
            }
        }
        else if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            pthread_mutex_lock(&handle->stats_mutex);
            handle->consecutive_failures++;
            pthread_mutex_unlock(&handle->stats_mutex);
        }

        rd_kafka_message_destroy(msg);
    }

    return NULL;
}
#endif

NvPSFMsgBusStatus NvPSFMsgBusCreate(const char* brokers, const char* topic, NvPSFMsgBusEndpointType endpointType, const char* group_id, NvPSFMsgBusHandle** out_handle)
{
    NvPSFMsgBusStatus retval;
    char unique_group_id[256];
    char errstr[512];
    rd_kafka_conf_t* conf = NULL;
    NvPSFMsgBusHandle* handle = NULL;
    rd_kafka_type_t rk_type;
    rd_kafka_t* rk = NULL;
    time_t current_time = 0;
    rd_kafka_resp_err_t err;
    rd_kafka_topic_partition_list_t* topics = NULL;
    rd_kafka_topic_partition_t* partition = NULL;
    NvPSFMsgBusStatus seek_status;
    int ret = 0;

    retval = make_status(NvPSFMSGBUS_FAIL, -1, 0);

    if (!out_handle || !brokers || !topic)
    {
        return retval;
    }
    *out_handle = NULL;

    if (endpointType == MSGBUS_CONSUMER && !group_id)
    {
        return retval;
    }

    conf = rd_kafka_conf_new();
    if (!conf)
    {
        return retval;
    }

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
    {
        rd_kafka_conf_destroy(conf);
        return retval;
    }

    if (endpointType == MSGBUS_CONSUMER)
    {
        ret = snprintf(unique_group_id, sizeof(unique_group_id), "%s-%ld", group_id, time(NULL));
        if (ret < 0 || ret >= (int)sizeof(unique_group_id))
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }

        /* Unique consumer group ID to avoid sharing offsets with other instances */
        if (rd_kafka_conf_set(conf, "group.id", unique_group_id, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
        /* Start consuming from latest offset, ignoring old messages on startup */
        if (rd_kafka_conf_set(conf, "auto.offset.reset", "latest", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }

        /* Initial backoff time before reconnecting to broker after disconnect */
        if (rd_kafka_conf_set(conf, "reconnect.backoff.ms", KAFKA_RECONNECT_BACKOFF_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
        /* Maximum backoff time for exponential backoff during reconnection */
        if (rd_kafka_conf_set(conf, "reconnect.backoff.max.ms", KAFKA_RECONNECT_BACKOFF_MAX_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
        /* Backoff time before retrying failed requests to broker */
        if (rd_kafka_conf_set(conf, "retry.backoff.ms", KAFKA_RETRY_BACKOFF_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
        /* Enable TCP keepalive to detect dead connections */
        if (rd_kafka_conf_set(conf, "socket.keepalive.enable", "true", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
        /* How often to refresh broker metadata (topic/partition info) */
        if (rd_kafka_conf_set(conf, "metadata.max.age.ms", KAFKA_METADATA_MAX_AGE_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
        /* Max time between poll() calls before consumer is considered dead (24h for long idle periods) */
        if (rd_kafka_conf_set(conf, "max.poll.interval.ms", KAFKA_MAX_POLL_INTERVAL_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            rd_kafka_conf_destroy(conf);
            return retval;
        }
    }

    handle = (NvPSFMsgBusHandle*)calloc(1, sizeof(NvPSFMsgBusHandle));
    if (!handle)
    {
        rd_kafka_conf_destroy(conf);
        return retval;
    }

    handle->initialized = false;

#if USE_CALLBACK_MODE
    if (endpointType == MSGBUS_CONSUMER)
    {
        handle->msg_queue = create_message_queue(MESSAGE_QUEUE_CAPACITY);
        if (!handle->msg_queue)
        {
            rd_kafka_conf_destroy(conf);
            free(handle);
            return retval;
        }
    }
#endif

    rk_type = (endpointType == MSGBUS_PRODUCER) ? RD_KAFKA_PRODUCER : RD_KAFKA_CONSUMER;
    rk = rd_kafka_new(rk_type, conf, errstr, sizeof(errstr));
    if (!rk)
    {
#if USE_CALLBACK_MODE
        if (endpointType == MSGBUS_CONSUMER && handle->msg_queue)
        {
            destroy_message_queue(handle->msg_queue);
        }
#endif
        rd_kafka_conf_destroy(conf);
        free(handle);
        return retval;
    }

    handle->rk = rk;
    handle->type = endpointType;
    handle->topic = strdup(topic);
    if (!handle->topic)
    {
#if USE_CALLBACK_MODE
        if (endpointType == MSGBUS_CONSUMER && handle->msg_queue)
        {
            destroy_message_queue(handle->msg_queue);
        }
#endif
        rd_kafka_destroy(rk);
        free(handle);
        return retval;
    }

    current_time = time(NULL);
    if (current_time == (time_t)(-1))
    {
#if USE_CALLBACK_MODE
        if (endpointType == MSGBUS_CONSUMER && handle->msg_queue)
        {
            destroy_message_queue(handle->msg_queue);
        }
#endif
        rd_kafka_destroy(rk);
        free(handle->topic);
        free(handle);
        return retval;
    }
    handle->last_successful_poll = current_time;
    handle->consecutive_failures = 0;
    ret = pthread_mutex_init(&handle->stats_mutex, NULL);
    if (ret != 0)
    {
        rd_kafka_destroy(rk);
        free(handle->topic);
        free(handle);
        return retval;
    }

    if (endpointType == MSGBUS_PRODUCER)
    {
        handle->rkt = rd_kafka_topic_new(rk, topic, NULL);
        if (!handle->rkt)
        {
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            return retval;
        }
    }
    else
    {
        err = rd_kafka_poll_set_consumer(rk);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
#if USE_CALLBACK_MODE
            if (handle->msg_queue)
            {
                destroy_message_queue(handle->msg_queue);
            }
#endif
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            retval = make_status(NvPSFMSGBUS_FAIL, err, 0);
            return retval;
        }

        topics = rd_kafka_topic_partition_list_new(1);
        if (!topics)
        {
#if USE_CALLBACK_MODE
            if (handle->msg_queue)
            {
                destroy_message_queue(handle->msg_queue);
            }
#endif
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            return retval;
        }

        partition = rd_kafka_topic_partition_list_add(topics, topic, -1);
        if (!partition)
        {
            rd_kafka_topic_partition_list_destroy(topics);
#if USE_CALLBACK_MODE
            if (handle->msg_queue)
            {
                destroy_message_queue(handle->msg_queue);
            }
#endif
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            return retval;
        }

        err = rd_kafka_subscribe(rk, topics);
        rd_kafka_topic_partition_list_destroy(topics);

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
#if USE_CALLBACK_MODE
            if (handle->msg_queue)
            {
                destroy_message_queue(handle->msg_queue);
            }
#endif
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            retval = make_status(NvPSFMSGBUS_FAIL, err, 0);
            return retval;
        }

        seek_status = NvPSFMsgBusSeekToEnd(handle);
        if (seek_status.err != NvPSFMSGBUS_SUCCESS)
        {
#if USE_CALLBACK_MODE
            if (handle->msg_queue)
            {
                destroy_message_queue(handle->msg_queue);
            }
#endif
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            return seek_status;
        }

#if USE_CALLBACK_MODE
        // Start background polling thread
        handle->running = true;
        ret = pthread_create(&handle->poll_thread, NULL, poll_thread_func, handle);
        if (ret != 0)
        {
            destroy_message_queue(handle->msg_queue);
            rd_kafka_destroy(rk);
            free(handle->topic);
            free(handle);
            retval = make_status(NvPSFMSGBUS_FAIL, ret, 0);
            return retval;
        }
#endif
    }

    handle->initialized = true;
    *out_handle = handle;
    retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
    return retval;
}

NvPSFMsgBusStatus NvPSFMsgBusDestroy(NvPSFMsgBusHandle* handle)
{
    NvPSFMsgBusStatus retval;
    int ret = 0;
    rd_kafka_resp_err_t err;

    retval = make_status(NvPSFMSGBUS_FAIL, -1, 0);

    if (!handle || !handle->initialized)
    {
        return retval;
    }

#if USE_CALLBACK_MODE
    if (handle->type == MSGBUS_CONSUMER)
    {
        handle->running = false;
        /* Wake poll thread if blocked in enqueue_message waiting for queue space */
        if (handle->msg_queue)
        {
            pthread_mutex_lock(&handle->msg_queue->mutex);
            pthread_cond_broadcast(&handle->msg_queue->not_full);
            pthread_mutex_unlock(&handle->msg_queue->mutex);
        }
        ret = pthread_join(handle->poll_thread, NULL);
        if (ret != 0)
        {
#ifdef NVPSF_DBG
            printf("pthread_join failed: %d\n", ret);
#endif
        }
        destroy_message_queue(handle->msg_queue);
    }
#endif

    if (handle->type == MSGBUS_PRODUCER)
    {
        if (handle->rkt)
        {
            rd_kafka_topic_destroy(handle->rkt);
        }
        err = rd_kafka_flush(handle->rk, KAFKA_FLUSH_TIMEOUT_MS);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
#ifdef NVPSF_DBG
            printf("rd_kafka_flush failed: %s\n", rd_kafka_err2str(err));
#endif
        }
    }
    else
    {
        err = rd_kafka_consumer_close(handle->rk);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
#ifdef NVPSF_DBG
            printf("rd_kafka_consumer_close failed: %s\n", rd_kafka_err2str(err));
#endif
        }
    }
    rd_kafka_destroy(handle->rk);
    pthread_mutex_destroy(&handle->stats_mutex);
    free(handle->topic);
    free(handle);
    retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
    return retval;
}

NvPSFMsgBusStatus NvPSFMsgBusSend(NvPSFMsgBusHandle* handle, const void* msg, size_t msgLen)
{
    NvPSFMsgBusStatus retval;
    int ret = 0;
    rd_kafka_resp_err_t err;

    retval = make_status(NvPSFMSGBUS_FAIL, -1, 0);

    if (!handle || !handle->initialized || handle->type != MSGBUS_PRODUCER || !msg || msgLen == 0)
    {
        return retval;
    }

    if (msgLen > MAX_PAYLOAD_SIZE)
    {
#ifdef NVPSF_DBG
        printf("Message size %zu exceeds MAX_PAYLOAD_SIZE %d\n", msgLen, MAX_PAYLOAD_SIZE);
#endif
        return retval;
    }

    ret = rd_kafka_produce(
            handle->rkt, RD_KAFKA_PARTITION_UA,
            RD_KAFKA_MSG_F_COPY,
            (void*)msg, msgLen,
            NULL, 0, NULL);

    if (ret == -1)
    {
        err = rd_kafka_last_error();
#ifdef NVPSF_DBG
        printf("rd_kafka_produce failed: %s\n", rd_kafka_err2str(err));
#endif
        retval = make_status(NvPSFMSGBUS_FAIL, err, 0);
        return retval;
    }

    (void)rd_kafka_poll(handle->rk, 0);
    retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
    return retval;
}

NvPSFMsgBusStatus NvPSFMsgBusReceive(NvPSFMsgBusHandle* handle, void* buffer, size_t bufferLen, size_t* outLen)
{
    NvPSFMsgBusStatus retval;
    time_t current_time = 0;
    int err = 0;
#if !USE_CALLBACK_MODE
    rd_kafka_message_t* msg = NULL;
    size_t copyLen = 0;
    void* memcpy_ret = NULL;
#ifdef NVPSF_DBG
    rd_kafka_timestamp_type_t ts_type;
    int64_t ts = 0;
    struct timespec current_time_spec;
    int ret = 0;
    int64_t current_ts_ms = 0;
    int64_t latency_ms = 0;
#endif
#endif

    retval = make_status(NvPSFMSGBUS_FAIL, -1, 0);

    if (!handle || !handle->initialized || handle->type != MSGBUS_CONSUMER || !buffer || bufferLen == 0)
    {
        return retval;
    }

    // Check if consecutive error threshold has been reached
    pthread_mutex_lock(&handle->stats_mutex);
    if (handle->consecutive_failures >= CONSECUTIVE_ERROR_THRESHOLD)
    {
        pthread_mutex_unlock(&handle->stats_mutex);
        retval = make_status(NvPSFMSGBUS_FAIL, -2, 0);
        return retval;
    }
    pthread_mutex_unlock(&handle->stats_mutex);

    current_time = time(NULL);
    if (current_time == (time_t)(-1))
    {
        return retval;
    }

#if USE_CALLBACK_MODE
    err = dequeue_message(handle->msg_queue, buffer, bufferLen, outLen, DEQUEUE_TIMEOUT_MS);
    if (err < 0)
    {
        retval = make_status(NvPSFMSGBUS_FAIL, err, 0);
        return retval;
    }

    if (outLen && *outLen == 0)
    {
        retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
        return retval;
    }

    retval = make_status(NvPSFMSGBUS_SUCCESS, 0, outLen ? *outLen : 0);
    return retval;
#else
    // In direct polling mode
    msg = rd_kafka_consumer_poll(handle->rk, CONSUMER_POLL_TIMEOUT_MS);
    if (!msg)
    {
        if (outLen)
        {
            *outLen = 0;
        }
        retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
        return retval;
    }

    if (msg->err)
    {
        if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            pthread_mutex_lock(&handle->stats_mutex);
            handle->consecutive_failures++;
            pthread_mutex_unlock(&handle->stats_mutex);
        }
        err = msg->err;
        rd_kafka_message_destroy(msg);
        if (err == RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            if (outLen)
            {
                *outLen = 0;
            }
            retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
        }
        else
        {
            retval = make_status(NvPSFMSGBUS_FAIL, err, 0);
        }
        return retval;
    }

    // Valid message received - reset consecutive failures
    pthread_mutex_lock(&handle->stats_mutex);
    handle->last_successful_poll = current_time;
    handle->consecutive_failures = 0;
    pthread_mutex_unlock(&handle->stats_mutex);

#ifdef NVPSF_DBG
    ts = rd_kafka_message_timestamp(msg, &ts_type);

    ret = clock_gettime(CLOCK_REALTIME, &current_time_spec);
    if (ret != 0)
    {
        printf("clock_gettime failed: %d\n", ret);
    }
    else
    {
        current_ts_ms = (int64_t)current_time_spec.tv_sec * 1000 + current_time_spec.tv_nsec / 1000000;

        if (ts_type == RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME)
        {
            latency_ms = current_ts_ms - ts;
            printf("Received log append time: %ld, Current time: %ld, Latency: %ld ms\n",
                   ts, current_ts_ms, latency_ms);
        }
        else if (ts_type == RD_KAFKA_TIMESTAMP_CREATE_TIME)
        {
            latency_ms = current_ts_ms - ts;
            printf("Received create time: %ld, Current time: %ld, Latency: %ld ms\n",
                   ts, current_ts_ms, latency_ms);
        }
        else
        {
            printf("No timestamp or unknown type received from Kafka\n");
        }
    }
#endif

    if (msg->len > MAX_PAYLOAD_SIZE || msg->len > bufferLen)
    {
#ifdef NVPSF_DBG
        printf("Message size issue: len=%zu, MAX=%d, bufferLen=%zu\n", msg->len, MAX_PAYLOAD_SIZE, bufferLen);
#endif
        rd_kafka_message_destroy(msg);
        return retval;
    }

    copyLen = (msg->len < bufferLen) ? msg->len : bufferLen;
    memcpy_ret = memcpy(buffer, msg->payload, copyLen);
    if (memcpy_ret != buffer)
    {
#ifdef NVPSF_DBG
        printf("memcpy failed in receive\n");
#endif
        rd_kafka_message_destroy(msg);
        return retval;
    }

    if (outLen)
    {
        *outLen = copyLen;
    }
    rd_kafka_message_destroy(msg);
    retval = make_status(NvPSFMSGBUS_SUCCESS, 0, copyLen);
    return retval;
#endif
}

NvPSFMsgBusStatus NvPSFMsgBusSeekToEnd(NvPSFMsgBusHandle* handle)
{
    NvPSFMsgBusStatus retval;
    rd_kafka_topic_partition_list_t *assignment = NULL;
    int retry_count = 0;
    const int max_retries = SEEK_MAX_RETRIES;
    const int poll_timeout_ms = SEEK_POLL_TIMEOUT_MS;
    rd_kafka_message_t* msg = NULL;
    rd_kafka_resp_err_t err;
    int i = 0;
    rd_kafka_error_t *error = NULL;

    retval = make_status(NvPSFMSGBUS_FAIL, -1, 0);

    if (!handle || handle->type != MSGBUS_CONSUMER)
    {
        return retval;
    }

    while (retry_count < max_retries)
    {
        msg = rd_kafka_consumer_poll(handle->rk, poll_timeout_ms);
        if (msg)
        {
            rd_kafka_message_destroy(msg);
        }

        err = rd_kafka_assignment(handle->rk, &assignment);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
#ifdef NVPSF_DBG
            printf("rd_kafka_assignment failed: %s\n", rd_kafka_err2str(err));
#endif
            retry_count++;
            continue;
        }

        if (assignment && assignment->cnt > 0)
        {
            break;
        }

        if (assignment)
        {
            rd_kafka_topic_partition_list_destroy(assignment);
            assignment = NULL;
        }

        retry_count++;
    }

    if (!assignment || assignment->cnt == 0)
    {
        if (assignment)
        {
            rd_kafka_topic_partition_list_destroy(assignment);
        }
        return retval;
    }

    for (i = 0; i < assignment->cnt; i++)
    {
        assignment->elems[i].offset = RD_KAFKA_OFFSET_END;
    }

    error = rd_kafka_seek_partitions(handle->rk, assignment, SEEK_PARTITIONS_TIMEOUT_MS);
    if (error)
    {
#ifdef NVPSF_DBG
        printf("Failed to seek partitions to end: %s\n", rd_kafka_error_string(error));
#endif
        rd_kafka_error_destroy(error);
        rd_kafka_topic_partition_list_destroy(assignment);
        return retval;
    }

    rd_kafka_topic_partition_list_destroy(assignment);
    retval = make_status(NvPSFMSGBUS_SUCCESS, 0, 0);
    return retval;
}
