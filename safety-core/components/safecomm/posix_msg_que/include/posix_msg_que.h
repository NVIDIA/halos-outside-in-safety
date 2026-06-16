/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file posix_msg_queue.h
 * @brief Interface for POSIX Message Queue operations.
 *
 * This header file provides an interface for creating, sending, receiving,
 * and managing POSIX message queues.
 *
 * The functions defined in this header can be used in both C and C++
 * programs.
 */

#ifndef POSIX_MSG_QUEUE_H
#define POSIX_MSG_QUEUE_H

#include <mqueue.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @def MQ_PERMISSIONS
 *  @brief Default permissions for message queues.
 */
#define MQ_PERMISSIONS 0660

/** @def MQ_MAX_MESSAGES
 *  @brief Maximum number of messages in the queue.
 */
#define MQ_MAX_MESSAGES 10

/** @def MQ_MAX_MSG_SIZE
 *  @brief Maximum size of each message in bytes.
 */
#define MQ_MAX_MSG_SIZE 8192

/** @def MQ_MSG_BUFFER_SIZE
 *  @brief Buffer size for messages including additional space.
 */
#define MQ_MSG_BUFFER_SIZE (MQ_MAX_MSG_SIZE + 32)

/** @def MSG_PRIO_DEFAULT
 *  @brief Default priority for messages.
 */
#define MSG_PRIO_DEFAULT 10

/**
 * @enum NvPSFMsgQueEndpointType_t
 * @brief Types of message queue endpoints,
 * Either receiver end or sender end
 */
typedef enum NvPSFMsgQueEndpointType_t {
    MSG_QUE_RECEIVER = 0, /**< Message queue receiver endpoint */
    MSG_QUE_SENDER,        /**< Message queue sender endpoint */
    MSG_QUE_BIDIRECTIONAL
} NvPSFMsgQueEndpointType;

/**
 * @enum NvPSFMsgQueBlockingMode_t
 * @brief Blocking modes for message queue operations.
 * either blocking or non-blocking.
 * Only Blocking queue is supported right now.
 */
typedef enum NvPSFMsgQueBlockingMode_t {
    BLOCKING = 0, /**< Blocking mode for operations */
    NON_BLOCKING  /**< Non-blocking mode for operations */
} NvPSFMsgQueBlockingMode;

/**
 * @enum NvPSFMsgQueErr_t
 * @brief Error codes for message queue operations.
 */
typedef enum NvPSFMsgQueErr_t {
    NvPSFMSGQ_SUCCESS = 0, /**< Operation was successful */
    NvPSFMSGQ_FAIL = 1     /**< Operation failed */
} NvPSFMsgQueErr;

/**
 * @union NvPSFRetCode_t
 * @brief Return codes from various message queue operations.
 */
typedef union NvPSFMsgQueRetCode_t {
    int mqd;        /**< Message queue descriptor */
    int errCode;   /**< Error code from an operation */
    int recvd_bytes; /**< Number of bytes received from a message */
} NvPSFMsgQueRetCode;

/**
 * @struct NvPSFMsgQStatus_t
 * @brief Status structure returned by message queue operations.
 *
 * This structure contains the result of an operation and any relevant
 * return codes.
 */
typedef struct NvPSFMsgQueStatus_t {
    NvPSFMsgQueErr err;      /**< Error status of the operation */
    NvPSFMsgQueRetCode retCode; /**< Return code associated with the operation */
} NvPSFMsgQueStatus;

/**
 * @brief Create a message queue.
 *
 * This function creates a new POSIX message queue with the specified name
 * and attributes.
 *
 * @param name The name of the message queue (must start with '/').
 * @param endpointType Type of endpoint (sender or receiver).
 * @param blockingMode Blocking mode for the queue operations.
 *
 * @return A status structure indicating success or failure.
 */
NvPSFMsgQueStatus NvPSFMsgQueCreate(const char* name, const NvPSFMsgQueEndpointType endpointType,
                             const NvPSFMsgQueBlockingMode blockingMode);

/**
 * @brief Close a message queue.
 *
 * This function closes the specified message queue descriptor.
 *
 * @param mqdes The message queue descriptor to close.
 *
 * @return A status structure indicating success or failure.
 */
NvPSFMsgQueStatus NvPSFMsgQueClose(mqd_t mqdes);

/**
 * @brief Unlink (delete) a message queue.
 *
 * This function removes the specified message queue. The queue will be
 * deleted once all processes have closed it.
 *
 * @param name The name of the message queue to unlink (must start with '/').
 *
 * @return A status structure indicating success or failure.
 */
NvPSFMsgQueStatus NvPSFMsgQueUnlink(const char* name);

/**
 * @brief Send a message over the message queue.
 *
 * This function sends a message to the specified message queue with a
 * given priority.
 *
 * @param mqdes The message queue descriptor to send the message to.
 * @param msg Pointer to the message to send.
 * @param msgLen Length of the message in bytes.
 * @param priority Priority of the message (higher values indicate higher
 *                 priority).
 *
 * @return A status structure indicating success or failure.
 */
NvPSFMsgQueStatus NvPSFMsgQueSend(mqd_t mqdes, const char* msg, size_t msgLen, unsigned int priority);

/**
 * @brief Receive a message from the message queue.
 *
 * This function receives a message from the specified message queue and
 * stores it in the provided buffer.
 *
 * @a buffer must not be NULL; passing NULL returns NvPSFMSGQ_FAIL with
 * errCode = EINVAL without consuming a message from the queue.
 *
 * Callers should provide a buffer of at least @c MQ_MSG_BUFFER_SIZE bytes.
 * If the received message is larger than @a bufferLen, the function copies
 * only @a bufferLen bytes into @a buffer and returns NvPSFMSGQ_FAIL with
 * errCode = EMSGSIZE to signal truncation.  The message is still consumed
 * from the queue by @c mq_receive, so data beyond @a bufferLen is lost.
 *
 * @param mqdes The message queue descriptor to receive from.
 * @param buffer Pointer to the buffer where the received message will be
 *               stored.  Must not be NULL.
 * @param bufferLen Size of the buffer in bytes.  Should be at least
 *                  MQ_MSG_BUFFER_SIZE to avoid truncation.
 * @param priority Pointer to an unsigned integer where the priority of
 *                 the received message will be stored (can be NULL).
 *
 * @return A status structure indicating success or failure.  On success
 *         retCode.recvd_bytes holds the number of bytes copied.  On
 *         truncation (EMSGSIZE) bufferLen bytes were copied.
 */
NvPSFMsgQueStatus NvPSFMsgQueReceive(mqd_t mqdes, char* buffer, size_t bufferLen, unsigned int* priority);

#ifdef __cplusplus
}
#endif

#endif // POSIX_MSG_QUEUE_H
