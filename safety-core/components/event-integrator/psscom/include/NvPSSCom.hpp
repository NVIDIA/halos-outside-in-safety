/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "posix_msg_que.h"
#include "NvPSSErr.h"
#include "NvPSSCom.h"

namespace nvpss
{

typedef enum NvPSSComEndpoint_t
{
    NVPSSCOM_SRC,
    NVPSSCOM_SINK
}NvPSSComEndpoint;

typedef enum NvPSSComChannelBackend_t
{
    POSIX_MSG_QUE=0,
    NVSCI
}NvPSSComChannelBackend;

typedef struct NvPSSComDataSrcCbInternal_t
{
    NvPSSComErr (*onDataRequest)(NvPSSComPacket* pkt); /**< Callback for data request. */
    NvPSSComErr (*onPause)(void);         /**< Callback for pause request. */
    NvPSSComErr (*onResume)(void);        /**< Callback for resume request. */
    NvPSSComErr (*onStop)(void);           /**< Callback for termination request. */
} NvPSSComDataSrcCbInternal;

typedef struct NvPSSComDataSinkCbInternal_t
{
    NvPSSComErr (*onDataAvailable)(NvPSSComPacket* pkt); /**< Callback for data availability. */
    NvPSSComErr (*onFlowRateChange)(uint8_t flowRate); /**< Callback for flow rate change. */
    NvPSSComErr (*onStop)(void);             /**< Callback for termination request. */
} NvPSSComDataSinkCbInternal;

typedef enum NvPSSComSrcState_t
{
    SRC_UNINITIALIZED=0,
    SRC_INITIALIZED,
    SRC_WAITING_FOR_PSS_RDY,
    SRC_ACTIVE,
    SRC_PAUSED,
    SRC_TERMINATED,
    SRC_NA=-1
}NvPSSComSrcState;

typedef enum NvPSSComSinkState_t
{
    SINK_UNINITIALIZED=0,
    SINK_INITIALIZED,
    SINK_ACTIVE,
    SINK_PAUSED,
    SINK_TERMINATED,
    SINK_NA=-1
}NvPSSComSinkState;

class NvPSSCom
{

private:

    const std::string writeChannel;
    const std::string readChannel;
    const NvPSSComEndpoint endpt;
    int writeChannelMqd;
    int readChannelMqd;

    NvPSSComDataSinkCbInternal sinkCallbacks;
    NvPSSComDataSrcCbInternal srcCallbacks;
    NvPSSComChannelBackend backend;

    std::atomic<NvPSSComSrcState> srcState;
    std::atomic<NvPSSComSinkState> sinkState;

    NvPSSComErr msgQueListener();
    std::thread msgQueListenerThread;
    std::thread msgHandlerThread;
    std::deque<NvPSSComPacket> recvdPackets;
    std::condition_variable deqEmptyCV;
    std::mutex deqEmptyCVMtx;
    bool deqEmpty;
    std::atomic<bool> listenOnMsgQue;

    std::thread dataSenderThread;
    NvPSSComErr requestAndSendData();
    std::atomic<bool> runDataSenderThread;

    NvPSSComErr handleMsgsOnMsgQueSrcEndpt();
    NvPSSComErr handleMsgsOnMsgQueSinkEndpt();
    NvPSSComErr calculateChecksum(NvPSSComPacket* pkt);

    uint8_t flowRate;
    pthread_t msgQueListenerThreadNativeHandle;


public:
    NvPSSCom(std::string writeChannel, std::string readChannel, NvPSSComEndpoint endPt);
    ~NvPSSCom();
    NvPSSComErr NvPSSComChannelCreate(NvPSSComChannelBackend backend);
    NvPSSComErr NvPSSComSetDataSrcCbs(NvPSSComDataSrcCbInternal srcCallbacks);
    NvPSSComErr NvPSSComSetDataSinkCbs(NvPSSComDataSinkCbInternal sinkCallbacks);
    NvPSSComEndpoint NvPSSComGetChannelEndpt();
    NvPSSComErr NvPSSComRequestStart();
    NvPSSComErr NvPSSComChannelListenerStart();
    NvPSSComErr NvPSSComSetFlowRate(uint8_t flowRate);
    NvPSSComErr NvPSSComPushData(NvPSSComPacket* pkt);
    NvPSSComErr NvPSSComPause();
    NvPSSComErr NvPSSComResume();
    NvPSSComErr NvPSSComStop();
    NvPSSComErr NvPSSComChannelClose(NvPSSComChannelBackend backend);
};

}
