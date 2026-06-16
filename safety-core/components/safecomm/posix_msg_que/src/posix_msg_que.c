/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>

#include "posix_msg_que.h"

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
  #define NV_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
  #define NV_TLS __thread
#else
  #warning "No thread-local storage support; rxBuf will be plain static (not thread-safe)"
  #define NV_TLS
#endif

// Create a message queue
NvPSFMsgQueStatus NvPSFMsgQueCreate(const char* mq_name, const NvPSFMsgQueEndpointType endpointType,
                    const NvPSFMsgQueBlockingMode blockingMode)
{

    struct mq_attr attr;
    int flags = 0;
    ssize_t bytes_read = 0;
    char readBuf[MQ_MAX_MSG_SIZE];
    bool staleMsgsInQue = true;
    mqd_t mqd;
    NvPSFMsgQueStatus status = {0};

    attr.mq_flags = 0;
    attr.mq_maxmsg = MQ_MAX_MESSAGES;
    attr.mq_msgsize = MQ_MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    /**
     * First open the msg que in non-blocking, readonly mode,
     * read and discard if there are any stale messages.
     * This has to be done as msg queues are persistent and
     * stale messge would be read
    */
    mqd = mq_open(mq_name, O_RDONLY|O_NONBLOCK,MQ_PERMISSIONS,&attr);
    if(mqd == (mqd_t) -1)
    {
        if(errno == ENOENT)
        {
            /*This means this a fresh que, no bother about stale msgs*/
            staleMsgsInQue = false;
        }
        else
        {
            /*Some other error in que read, should exit*/
            status.err = NvPSFMSGQ_FAIL;
            status.retCode.errCode = errno;
            goto exit;
        }
    }

    while(staleMsgsInQue)
    {
        if((bytes_read = mq_receive(mqd, readBuf, MQ_MAX_MSG_SIZE, NULL)) == (mqd_t)-1)
        {
            if(errno == EAGAIN)
            {
                staleMsgsInQue = false;
                mq_close(mqd);
            }
            else
            {
                /*Some other error in que read, should exit*/
                status.err = NvPSFMSGQ_FAIL;
                status.retCode.errCode = errno;
                goto exit;
            }
        }
        else
        {
            /*Stale messages are discarded*/
        }

    }

    if(blockingMode == NON_BLOCKING)
    {
        flags |= O_NONBLOCK;
    }

    switch(endpointType)
    {
        case MSG_QUE_RECEIVER:
            flags |= O_RDONLY;
            break;

        case MSG_QUE_SENDER:
            flags |= O_WRONLY;
            break;

        case MSG_QUE_BIDIRECTIONAL:
            flags |= O_RDWR;
            break;

        default:
            break;
    }

    flags |= O_CREAT;

    mqd = mq_open(mq_name, flags, MQ_PERMISSIONS, &attr);

    if(mqd == (mqd_t) -1)
    {
        printf("Error in creating message que \n");
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = errno;
        goto exit;
    }

    status.err = NvPSFMSGQ_SUCCESS;
    status.retCode.mqd = mqd;

exit:
    return status;
}

// Close the message queue
NvPSFMsgQueStatus NvPSFMsgQueClose(mqd_t mqdes)
{

    NvPSFMsgQueStatus status = {0};

    if(mq_close(mqdes) == 0)
    {
        status.err = NvPSFMSGQ_SUCCESS;
        status.retCode.errCode = 0;
        goto exit;
    }
    else
    {
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = errno;
        goto exit;
    }

exit:
    return status;
}

// Unlink (delete) a message queue
NvPSFMsgQueStatus NvPSFMsgQueUnlink(const char *name)
{

    NvPSFMsgQueStatus status = {0};

    if(mq_unlink(name) == 0)
    {
        status.err = NvPSFMSGQ_SUCCESS;
        status.retCode.errCode = 0;
        goto exit;
    }
    else
    {
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = errno;
        goto exit;
    }

exit:
    return status;

}

// Send a message over the message queue
NvPSFMsgQueStatus NvPSFMsgQueSend(mqd_t mqdes, const char *msg, size_t msgLen, unsigned int priority)
{
    NvPSFMsgQueStatus status = {0};

    if(mq_send(mqdes, msg, msgLen, priority) == 0)
    {
        status.err = NvPSFMSGQ_SUCCESS;
        status.retCode.errCode = 0;
        goto exit;
    }
    else
    {
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = errno;
        goto exit;
    }
exit:
    return status;
}

// Receive a message from the message queue
NvPSFMsgQueStatus NvPSFMsgQueReceive(mqd_t mqdes, char *buffer, size_t bufferLen, unsigned int *priority)
{
    NvPSFMsgQueStatus status = {0};
    /*
     * POSIX requires the length passed to mq_receive() to be >=
     * attr.mq_msgsize, which may be larger than the caller's struct.
     * Receive into a thread-local buffer that satisfies this requirement
     * without consuming ~8 KB of stack on every call, then copy at most
     * bufferLen bytes into the caller's buffer.
     */
    static NV_TLS char rxBuf[MQ_MSG_BUFFER_SIZE];
    ssize_t bytes_read = 0;

    /* Reject NULL buffer before consuming a message from the queue. */
    if (buffer == NULL)
    {
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = EINVAL;
        return status;
    }

    if ((bytes_read = mq_receive(mqdes, rxBuf, sizeof(rxBuf), priority)) == -1)
    {
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = errno;
        return status;
    }

    /*
     * If the received message is larger than the caller's buffer,
     * copy only bufferLen bytes and signal truncation via EMSGSIZE.
     * mq_receive already consumed the message from the queue, so
     * dropping it here would silently lose data.  Callers should
     * provide a buffer of at least MQ_MSG_BUFFER_SIZE to avoid
     * truncation.
     */
    if ((size_t)bytes_read > bufferLen)
    {
        memcpy(buffer, rxBuf, bufferLen);
        status.err = NvPSFMSGQ_FAIL;
        status.retCode.errCode = EMSGSIZE;
        return status;
    }

    memcpy(buffer, rxBuf, (size_t)bytes_read);
    status.err = NvPSFMSGQ_SUCCESS;
    /* Safe: bytes_read is bounded by MQ_MSG_BUFFER_SIZE (fits in int). */
    status.retCode.recvd_bytes = (int)bytes_read;
    return status;
}
