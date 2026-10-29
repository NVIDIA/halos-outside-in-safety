/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

/*
 * psscom's own backend type, and the reason it exists.
 *
 * This component used to name its backend with NvPSFTransportKind from
 * safecomm/transport -- while never calling a single NvPSFTransport* function. It
 * talks to POSIX message queues directly through safecomm/posix_msg_que, so its
 * queues were never a "PSF transport" in that component's sense at all; it was
 * borrowing a type from a component it does not use.
 *
 * That borrowed reference was the last thing keeping NVPSF_TRANSPORT_POSIX_MSG_QUE
 * alive in the shared enum, which made a retired PSF transport look selectable
 * on links that no longer carry it. Decoupling here
 * is what let that enumerator go.
 *
 * One value, because there is one implementation. If psscom ever gains a second
 * backend, add it here -- not to the PSF transport enum, unless it genuinely
 * goes through NvPSFTransport.
 */
typedef enum NvPSSComBackend_t
{
    NVPSSCOM_BACKEND_POSIX_MSG_QUE = 1
} NvPSSComBackend;

typedef enum NvPSSComEndpoint_t
{
    NVPSSCOM_SRC,
    NVPSSCOM_SINK
}NvPSSComEndpoint;

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
    NvPSSComBackend backend;

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
    NvPSSComErr NvPSSComChannelCreate(NvPSSComBackend backend);
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
    NvPSSComErr NvPSSComChannelClose(NvPSSComBackend backend);
};

}
