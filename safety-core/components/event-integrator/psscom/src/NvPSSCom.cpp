/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <string>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "NvPSSCom.hpp"
#include "NvPSB.h"

#define SECOND_TO_MICROSECOND 1000000UL

namespace nvpss
{

/*
* Constructor of NvPSSCom.
* Instantiatation of the NvPSSCom as either source or sink.
* Each of the end would have two channels associated with it,
* readChannel and writeChannel. readChannel for listening for
* messages from the other end and writeChannel for passing messages
* to the other end.
* This means, for a given pair of source and sink connected over
* NvPSSCom, readChannel and writeChannel would be cross-connected.
*/
NvPSSCom::NvPSSCom(std::string writeChannel, std::string readChannel ,
                    NvPSSComEndpoint endpt):
                    writeChannel(writeChannel), readChannel(readChannel),
                    endpt(endpt)
{
    switch(endpt)
    {
        case NVPSSCOM_SRC:
            srcState.store(SRC_UNINITIALIZED);
            sinkState.store(SINK_NA);
            break;
        case NVPSSCOM_SINK:
            srcState.store(SRC_NA);
            sinkState.store(SINK_UNINITIALIZED);
    }

    if(endpt == NVPSSCOM_SRC) {
        if(NvPSBInitialize("NVPSB_PSS_SOURCE", NVPSB_PSS_SOURCE) != NVPSB_SUCCESS)
        {
            std::cerr<<"Failed to initialize PSB.\n";
        }
    } else if (endpt == NVPSSCOM_SINK)
    {
        if(NvPSBInitialize("NVPSB_PSS_SINK", NVPSB_PSS_SINK) != NVPSB_SUCCESS)
        {
            std::cerr<<"Failed to initialize PSB.\n";
        }
    }

    writeChannelMqd = -1;
    readChannelMqd = -1;
    sinkCallbacks = {nullptr, nullptr, nullptr};
    srcCallbacks = {nullptr, nullptr, nullptr, nullptr};
    backend = POSIX_MSG_QUE;
    flowRate = 0;
    deqEmpty = false;
    listenOnMsgQue.store(false);
    runDataSenderThread.store(false);
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/*
Destructor of NvPSSCom
*/
NvPSSCom::~NvPSSCom()
{
    if(NvPSBExit() != NVPSB_SUCCESS)
    {
        std::cerr<<"Failed to exit NvPSB.\n";
    }
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Initialize the communication channels.
 *
 * NvPSSCom supports two IPC backends, POSIX message que and NvSciIPC.
 * Currently only POSIX message que is supported.
 *
 * POSIX Message Que:
 * For a given endpoint (i.e. source or sink), two message queues are created.
 * They both are opened in BLOCKIING mode to keep the messaging simpler.
 * At the user API level, asynchronous messaging is exposed by managing
 * separate threads for send and receive over created queues.
 *
 * NvSciIPC:
 * <TBD>
 *
*/
NvPSSComErr NvPSSCom::NvPSSComChannelCreate(NvPSSComChannelBackend backend)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSFMsgQueStatus mqStatus;
    NvPSFMsgQueEndpointType mqEndptType;

    this->backend = backend;

    if(backend == POSIX_MSG_QUE)
    {

        //For PSS Msg Que endpoint is always bidirectional
        mqEndptType = MSG_QUE_BIDIRECTIONAL;

        //Create the queues
        mqStatus = NvPSFMsgQueCreate(writeChannel.c_str(), mqEndptType, BLOCKING);
        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
            writeChannelMqd = mqStatus.retCode.mqd;

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "MsgQ is opened for read with fd : " + std::to_string(writeChannelMqd), "");
#endif

        }
        else
        {
            std::cerr<<"Error in opening message queue : "<<writeChannel<<"Code : "<<
            mqStatus.retCode.errCode;

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR, "Error in opening message queue " + writeChannel +
                "Code: " + std::to_string(mqStatus.retCode.errCode), "");
#endif
            err = NVPSSCOM_FAIL;
            goto exit;
        }

        mqStatus = NvPSFMsgQueCreate(readChannel.c_str(), mqEndptType, BLOCKING);
        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
            readChannelMqd = mqStatus.retCode.mqd;

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO, "MsgQ is opened for read with fd : " + std::to_string(readChannelMqd), "");
#endif

        }
        else
        {
            std::cerr<<"Error in opening message queue : "<<readChannel<<"Code : "<<
            mqStatus.retCode.errCode;

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR, "Error in opening message queue " + readChannel +
                "Code: " + std::to_string(mqStatus.retCode.errCode), "");
#endif

            err = NVPSSCOM_FAIL;
            goto exit;
        }

    }
    else
    {
        std::cerr<<"IPC backend other than POSIX Message Que is not yet supported. Exiting\n";

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR, "Messaging backend other than posix message que is not yet supported", "");
#endif

        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Register callbacks for source endpoint
 *
 * The souce endpoint of NvPSSCom is required to register four callbacks
 * onDataRequest, onPause, onResume.
 * onDataRequest : When source is required to send data messages
 * onPause : When sink requests source to temporarily pause the messaging
 * onResume : When sink requests source to resume the paused messaging
 * onStop : When sink requests source to stop the messaging and thereby terminating
 * the ongoing session
 *
 * When these callbacks are registered source shifts its state SRC_ UNINITIALIZED => SRC_INITIALIZED
*/

NvPSSComErr NvPSSCom::NvPSSComSetDataSrcCbs(NvPSSComDataSrcCbInternal srcCallbacks)
{
    this->srcCallbacks.onDataRequest = srcCallbacks.onDataRequest;
    this->srcCallbacks.onPause = srcCallbacks.onPause;
    this->srcCallbacks.onResume = srcCallbacks.onResume;
    this->srcCallbacks.onStop = srcCallbacks.onStop;

    srcState = SRC_INITIALIZED;

    return NVPSSCOM_SUCCESS;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Register callbacks for sink endpoint
 *
 * The sink endpoint of NvPSSCom is required to register three callbacks
 * onDataAvailable, onFlowRateChange, onStop
 * onDataAvailable : When data is ready to be accepted and processed by PSS
 * onFlowRateChange : Source is requesting update in the flow rate
 * onStop : When source intends to stop the messaging and thereby terminating the
 * ongoing session
 *
 * When these callbacks are registered sink shifts its state SINK_UNINITIALIZED => SINK_INITIALIZED
*/

NvPSSComErr NvPSSCom::NvPSSComSetDataSinkCbs(NvPSSComDataSinkCbInternal sinkCallbacks)
{

    this->sinkCallbacks.onDataAvailable = sinkCallbacks.onDataAvailable;
    this->sinkCallbacks.onFlowRateChange = sinkCallbacks.onFlowRateChange;
    this->sinkCallbacks.onStop = sinkCallbacks.onStop;

    sinkState.store(SINK_INITIALIZED);

    return NVPSSCOM_SUCCESS;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Get the endpoint for given instance of NvPSSCom
 *
 * This is a helper method to identify whether the given
 * instance of NvPSSCom is acting as a source or sink.
 *
 * This is helpful in access controlling certain methods which
 * are restricted to particular endpoint.
 * e.g. NvPSSComPause is supposed to be called by sink only
 * NvPSSConStart is supposed to be called by source only
*/

NvPSSComEndpoint NvPSSCom::NvPSSComGetChannelEndpt()
{
    return endpt;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Send the request to sink about staring the messaging session
 *
 * This method would be only accessed by source endpoint.
 * Access to this method is controlled at NvPSSCom_interface.
 *
 * This method sends a message packet consisting of cmd START_PSS and
 * awaits for PSS_RDY from sink. Once received, a thread 'dataSenderThread'
 * is spawned which would either
 * 1. keep invoking dataRequestCallback periodically to source if flowRate is set
 * OR
 * 2. If flowRate is set to 0, no callbacks would be invoked and source shall call
 * NvPSSPushData whenver it wished to submit data to PSS
 *
 * TODO:
 * Currently, source waits indefinitely to hear back PSS_RDY. Ideally, it should be
 * a timed wait.
 *
*/

NvPSSComErr NvPSSCom::NvPSSComRequestStart()
{

    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket send_pkt;
    NvPSSComPacket recvd_pkt;
    NvPSFMsgQueStatus mqStatus;

    /*Prepare the msg*/
    memset(&send_pkt, 0, sizeof(NvPSSComPacket));
    send_pkt.cmd = START_PSS;
    send_pkt.size = 0;
    memset(send_pkt.data, 0, MAX_DATA_SIZE);
    calculateChecksum(&send_pkt);

    if(backend == POSIX_MSG_QUE)
    {
        mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt), sizeof(NvPSSComPacket),
                                    MSG_PRIO_DEFAULT);
        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Requested PSS Client to connect to start.", "");
            NvPSBWriteData(NVPSB_LOG_INFO,"srcState transition. SRC_INITIALIZED=>SRC_WAITING_FOR_PSS_RDY","");
#endif
            srcState = SRC_WAITING_FOR_PSS_RDY;

            /*Wait till we hearback from sink about PSS_RDY*/
            mqStatus = NvPSFMsgQueReceive(readChannelMqd, (char*)&recvd_pkt,
                                            MQ_MSG_BUFFER_SIZE, NULL);
            if(mqStatus.err == NvPSFMSGQ_SUCCESS)
            {
                if(recvd_pkt.cmd == PSS_RDY)
                {
                    err = NVPSSCOM_SUCCESS;
                    if(this->flowRate > 0)
                    {
                        /*start the thread*/
                        runDataSenderThread.store(true);
                        dataSenderThread = std::thread(&NvPSSCom::requestAndSendData,this);
                    }
                    srcState = SRC_ACTIVE;

#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"Sink is ready to accept the messages","");
                    NvPSBWriteData(NVPSB_LOG_INFO,"srcState transition, SRC_WAITING_FOR_PSS_RDY=>SRC_ACTIVE","");
#endif

                    goto exit;
                }
                else if (recvd_pkt.cmd == PSS_NOT_RDY)
                {
                    err = NVPSSCOM_FAIL;
                    srcState = SRC_WAITING_FOR_PSS_RDY;
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"Sink is not ready to accept the messages","");
                    NvPSBWriteData(NVPSB_LOG_INFO,"srcState continues to be SRC_WAITING_FOR_PSS_RDY","");
#endif
                    goto exit;
                }
                else
                {

#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_ERR,"Invalid response from sink to PSS_RDY","");
#endif
                    std::cerr<<"Invalid response from sink to PSS_RDY \n";
                    err = NVPSSCOM_FAIL;
                    goto exit;
                }
            }
        }
        else
        {
            std::cerr<<"Error in sending START request to PSS Client :"<<mqStatus.retCode.errCode
                        <<"\n";
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending START request to PSS Client","");
#endif
            goto exit;
        }
    }
    else
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Set Flow Rate of messaging in packets/second
 *
 * Ideally flow rate should be set once before NvPSSStart, however
 * dynamically changing the flow rate is also possible.
 *
*/

NvPSSComErr NvPSSCom::NvPSSComSetFlowRate(uint8_t flowRate)
{

    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket send_pkt;
    NvPSSComPacket recvd_pkt;
    NvPSFMsgQueStatus mqStatus;

    /*Ensure that dataSenderThread is not running while
    setting the flow rate*/
    if(runDataSenderThread.load())
    {
        runDataSenderThread.store(false);
        dataSenderThread.join();
    }

    //First set the class variable flowRate
    this->flowRate = flowRate;

    /*Prepare the msg*/
    memset(&send_pkt, 0, sizeof(NvPSSComPacket));
    send_pkt.cmd = FLOW_RATE;
    send_pkt.size = 0;
    memset(send_pkt.data, 0, MAX_DATA_SIZE);
    send_pkt.data[0] = flowRate;
    calculateChecksum(&send_pkt);

    if(backend == POSIX_MSG_QUE)
    {

        /*If flow rate is being changed runtime, i.e. when srcState = SRC_ACTIVE
          this has to be differently handled.
          1. First change the srcState to SRC_PAUSED just internally
          2. Send the msg to sink
          3. But no need to wait for FLOW_RATE_ACK using msgQueReceive as the msg would
          be received by the listener thread.
          4. It will shift the state to SRC_ACTIVE and then this call returns
        */
        if(srcState.load() == SRC_ACTIVE)
        {

            srcState.store(SRC_PAUSED);

#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Requesting Flow rate update to sink","");
            NvPSBWriteData(NVPSB_LOG_INFO,"srcState change, SRC_ACTIVE=>SRC_PAUSED","");
#endif

            mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt),
                                        sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT);

            if(mqStatus.err == NvPSFMSGQ_SUCCESS)
            {

#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Informed sink about the flowRate update","");
                NvPSBWriteData(NVPSB_LOG_INFO,"Waiting for FLOW_RATE_ACK","");
#endif

                while(srcState.load() != SRC_ACTIVE);
                if(this->flowRate > 0)
                {
                    /*start the thread*/
                    runDataSenderThread.store(true);
                    dataSenderThread = std::thread(&NvPSSCom::requestAndSendData,this);
                }
                err = NVPSSCOM_SUCCESS;
                goto exit;

            }
            else
            {
                std::cerr<<"Error in sending FLOW_RATE request to PSS Client : "<<
                            mqStatus.retCode.errCode<<"\n";

#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Error in sending FLOW_RATE request to PSS Client " +
                    std::to_string(mqStatus.retCode.errCode), "");
#endif

                goto exit;
            }
        }
        else
        {
            mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt),
                                        sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT);
            if(mqStatus.err == NvPSFMSGQ_SUCCESS)
            {
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Informed sink about the flowRate update","");
                NvPSBWriteData(NVPSB_LOG_INFO,"Waiting for FLOW_RATE_ACK","");
#endif
                mqStatus = NvPSFMsgQueReceive(readChannelMqd, (char*)&recvd_pkt,
                                                MQ_MSG_BUFFER_SIZE, NULL);
                if(mqStatus.err == NvPSFMSGQ_SUCCESS)
                {
                    if(recvd_pkt.cmd == FLOW_RATE_ACK)
                    {
                        err = NVPSSCOM_SUCCESS;
                        srcState.store(SRC_ACTIVE);

#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Informed sink about the flowRate update","");
                NvPSBWriteData(NVPSB_LOG_INFO,"Waiting for FLOW_RATE_ACK","");
#endif

                        goto exit;
                    }

                    else
                    {
                        std::cerr<<"Invalid response from sink to FLOW_RATE \n";

#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_ERR,"Invalid response from sink to FLOW_RATE","");
#endif
                        err = NVPSSCOM_FAIL;
                        goto exit;
                    }
                }
            }
            else
            {
                err = NVPSSCOM_FAIL;
                std::cerr<<"Error in sending FLOW_RATE request to PSS Client " <<
                                mqStatus.retCode.errCode<<"\n";

#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending FLOW_RATE request to PSS Client " +
                        std::to_string(mqStatus.retCode.errCode),"");
#endif
                goto exit;
            }
        }
    }
    else
    {
        std::cerr<<"IPC backend other than POSIX Message Que is not yet supported. Exiting\n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Launcher of listener and handler threads
 *
 * This helper function launches two threads,
 *
 * One is a listener which keeps on listening on the message que
 * for messages from the other endpoint. Once the message is received,
 * it is pused onto a deque.
 *
 * The other thread pops the message from the deque, depending on the
 * command, it takes the subsequent action i.e. invoke certain callback or
 * send the acknowledgement etc.
 *
 *
*/

NvPSSComErr NvPSSCom::NvPSSComChannelListenerStart()
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    if(backend == POSIX_MSG_QUE)
    {
        listenOnMsgQue.store(true);
        msgQueListenerThread = std::thread(&NvPSSCom::msgQueListener, this);
        if(endpt == NVPSSCOM_SRC)
        {
            msgHandlerThread = std::thread(&NvPSSCom::handleMsgsOnMsgQueSrcEndpt,this);
        }
        else
        {
            msgHandlerThread = std::thread(&NvPSSCom::handleMsgsOnMsgQueSinkEndpt,this);
        }
    }
    else
    {
        err = NVPSSCOM_FAIL;
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que is not yet supported","");
#endif
    }

    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Listener on the message que
 *
 * Entry point of the listener thread.
 *
 * This thread would keep on listening on the message que for messages from the other endpoint.
 * Once the message is received,it is pused onto a deque
 *
*/

NvPSSComErr NvPSSCom::msgQueListener()
{

    NvPSFMsgQueStatus mqStatus;
    NvPSSComPacket recvd_pkt;
    NvPSSComErr err = NVPSSCOM_SUCCESS;

    while(listenOnMsgQue.load())
    {

        mqStatus = NvPSFMsgQueReceive(readChannelMqd, (char*)&recvd_pkt, MQ_MSG_BUFFER_SIZE, NULL);

        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
            if(recvdPackets.empty())
            {
                recvdPackets.push_back(recvd_pkt);
                deqEmpty = false;
                deqEmptyCV.notify_one();
            }
            else
            {
                recvdPackets.push_back(recvd_pkt);
            }
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Error in receiving on message queue : " + std::to_string(mqStatus.retCode.errCode), "");
#endif

            /**TODO:
             * The error in receiving on message que has been ignored here and next message is
             * being awaited.
             * Perhaps it would be better to count number of read failures and exit the messaging
             * if the read failures cross certain threshold
            */
            continue;
        }
    }
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 *  Data sender thread
 *
 * This thread invokes dataRequest callback at a frequency set by setFlowRate to the source.
 * Source is supposed to fill-in the data in the message packet, then this thread passes the
 * packet to message que to deliver to sink.
 *
 * This thread is only handling DATA packets and not any other control command packet.
*/
NvPSSComErr NvPSSCom::requestAndSendData()
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    uint32_t sleepIntervalUs;
    NvPSSComPacket send_pkt;
    NvPSFMsgQueStatus mqStatus;

    while(runDataSenderThread.load())
    {
        sleepIntervalUs = (SECOND_TO_MICROSECOND/flowRate);
        usleep(sleepIntervalUs);

        memset(&send_pkt,0,sizeof(NvPSSComPacket));
        send_pkt.cmd = DATA;
        srcCallbacks.onDataRequest(&send_pkt);
        if(backend == POSIX_MSG_QUE)
        {
            mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt),
                                        sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT);
            if(mqStatus.err != NvPSFMSGQ_SUCCESS)
            {
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Error in sending data packet to sink","");
#endif
                /**TODO:
                 * The error in sending on message que has been ignored here
                 * Perhaps it would be better to count number of  failures and exit the messaging
                 * if thefailures cross certain threshold
                */
                continue;
            }
        }
        else
        {
            std::cerr<<"messaging backend other than msg que is not supported\n";
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"messaging backend other than msg que is not supported","");
#endif
            break;
        }
    }
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 *  Message handler thread for source endpoint
 *
 *  This thread handles the messages as received at source endpoint.
 *
*/
NvPSSComErr NvPSSCom::handleMsgsOnMsgQueSrcEndpt()
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket pkt;
    NvPSSComPacket respPkt;
    NvPSFMsgQueStatus status;

    while(true)
    {
        std::unique_lock<std::mutex> lock(deqEmptyCVMtx);
        if(recvdPackets.empty())
        {
            deqEmptyCV.wait(lock);
             pkt = recvdPackets.front();
        }
        else
        {
            pkt = recvdPackets.front();
        }
        recvdPackets.pop_front();

        switch(pkt.cmd)
        {
            case PSS_RDY:
                /*When PSS_RDY is received, it means sink is ready to accept
                the data packets.So, start the data sender thread, if non-zero
                flow rate is set. src is now in ACTIVE state*/
                if(this->flowRate > 0)
                {
                    /*start the thread*/
                    runDataSenderThread.store(true);
                    dataSenderThread = std::thread(&NvPSSCom::requestAndSendData,this);
                }
                srcState.store(SRC_ACTIVE);
                break;

            case DATA_ACK:
                /*When DATA_ACK is received, it means than sink has consumed
                the previous packet*/
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Ack received for data pkt : " + std::to_string(pkt.ackSrNo), "");
#endif
                break;

            case PAUSE:

                /*PAUSE would be effective only if source is in active state*/
                if(srcState.load() == SRC_ACTIVE)
                {
                    /*When PAUSE is received, it means that sink is requesting to
                    temporarily pause sending the data packets, so throw onPauseCallback
                    from here and reply with PAUSE_ACK to sink*/

                    err = srcCallbacks.onPause();
                    if(err != NVPSSCOM_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Source is not agreed on PAUSE,ignore the msg","");
#endif
                        break;
                    }

                    /*Source has agreed to pause
                    First stop the dataSenderThread and then send PAUSE_ACK*/
                    runDataSenderThread.store(false);
                    dataSenderThread.join();

                    /*Prepare the resep msg for PAUSE_ACK*/
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = PAUSE_ACK;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    /*TODO : Checksum calculation*/
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the PAUSE_ACK response to sink","");
#endif
                    }
                    else
                    {
                        std::cerr<<"Error in sending the message : "<<status.retCode.errCode<<"\n";
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending PAUSE_ACK  : " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        goto exit;
                    }
                    srcState.store(SRC_PAUSED);
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"srcState change, SRC_ACTIVE=>SRC_PAUSED","");
#endif
                }
                else
                {
                    std::cerr<<"PAUSE is applicable only when source is in ACTIVE state;ignoring\n";
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"PAUSE is applicable only when source is in ACTIVE state ignoring", "");
#endif

                }
                break;

            case RESUME:
                /*RESUME is only effective when source is in PAUSED state*/
                if(srcState.load() == SRC_PAUSED)
                {
                    /*When RESUME is received, it means that sink is requesting source to resume
                    the data packet streaming. So reply with RESUME_ACK and change the
                    state back to ACTIVE */


                    err = srcCallbacks.onResume();
                    if(err != NVPSSCOM_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Source is not agreed on RESUME","");
#endif
                        break;
                    }

                    /*Prepare the resep msg for RESUME_ACK*/
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = RESUME_ACK;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    /*TODO : Checksum calculation*/
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt), sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the RESUME_ACK response successfully","");
#endif
                    }
                    else
                    {

#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending the message : " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        std::cerr<<"Error in sending the message : "<<status.retCode.errCode<<"\n";
                        goto exit;
                    }

                    /*Resume the dataSenderThread for positive flow rates*/
                    if(this->flowRate > 0)
                    {
                        /*start the thread*/
                        runDataSenderThread.store(true);
                        dataSenderThread = std::thread(&NvPSSCom::requestAndSendData,this);
                    }
                    srcState.store(SRC_ACTIVE);
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"srcState change, SRC_PAUSED=>SRC_ACTIVE","");
#endif

                }
                else
                {
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_ERR,"RESUME is applicable only when source is in PAUSE state; ignoring","");
#endif
                    std::cerr<<"RESUME is applicable only when source is in PAUSE state;ignoring\n";
                }
                break;

            case BYE:
                /*BYE is acceptable in any state of the source. It means that sink doesn't want to
                continue receiving the msgs. Throw onTerminate callback and send the BYE_ACK to sink*/

                /*Prepare the resep msg for BYE_ACK*/
                memset(&respPkt, 0, sizeof(NvPSSComPacket));
                respPkt.cmd = BYE_ACK;
                respPkt.size = 0;
                memset(respPkt.data, 0, MAX_DATA_SIZE);
                calculateChecksum(&respPkt);

                status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                            sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                if(status.err == NvPSFMSGQ_SUCCESS)
                {
#ifdef NVPSF_DBG
                   NvPSBWriteData(NVPSB_LOG_INFO,"Sent the BYE_ACK response successfully","");
#endif
                }
                else
                {
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending BYE_ACK :" + std::to_string(status.retCode.errCode), "");
#endif
                    std::cerr<<"Error in sending the message :"<<status.retCode.errCode<<"\n";
                    goto exit;
                }
                {
                    runDataSenderThread.store(false);
                    dataSenderThread.join();
                    srcState.store(SRC_TERMINATED);
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"srcState change, SRC_ACTIVE=>SRC_TERMINATED","");
#endif
                    listenOnMsgQue.store(false);
                    msgQueListenerThreadNativeHandle = msgQueListenerThread.native_handle();
                    int cancel_ret = pthread_cancel(msgQueListenerThreadNativeHandle);
                    if (cancel_ret != 0) {
                        NvPSBWriteData(NVPSB_LOG_ERR, "pthread_cancel failed", "");
                    }
                    /*TODO: Check joining status whether it is CANCELLED*/
                    msgQueListenerThread.join();
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"DEBUG: msgQueListener thread is joined","");
#endif

                    srcCallbacks.onStop();
                    /*TODO : We may want check status of onBye and
                        then decide on whether to send BYE_ACK*/

                    goto exit;
                }

            case BYE_ACK:
                /*BYE_ACK means sink has accepted the bye request earlier sent by the source
                This is a formal end of communication between ongloing src-sink pair. Listener
                thread can be exited now*/
                srcState.store(SRC_TERMINATED);
                listenOnMsgQue.store(false);
                std::cout<<"Terminating the messaging. GoodBye";
                err = NVPSSCOM_SUCCESS;
                goto exit;

            case FLOW_RATE_ACK:
                /*FLOW_RATE_ACK means sink has accepted the modified flow rate
                 source state now shifts to SRC_ACTIVE*/
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"modified flow rate has been acked by sink","");
#endif
                srcState.store(SRC_ACTIVE);
                break;

            default:
                std::cerr<<"Message is not yet handled \n";
                break;
        }
    }
exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 *  Message handler thread for sink
 *     This thread handles the messages as received at source endpoint.
*/
NvPSSComErr NvPSSCom::handleMsgsOnMsgQueSinkEndpt()
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket pkt;
    NvPSSComPacket respPkt;
    NvPSFMsgQueStatus status;

    while(true)
    {
        std::unique_lock<std::mutex> lock(deqEmptyCVMtx);
        if(recvdPackets.empty())
        {
            deqEmptyCV.wait(lock);
             pkt = recvdPackets.front();
        }
        else
        {
            pkt = recvdPackets.front();
        }
        recvdPackets.pop_front();

        switch(pkt.cmd)
        {
            case START_PSS:
                /*Respond to START_PSS with PSS_RDY if sink is ready to rcv data*/
                if(sinkState.load() == SINK_INITIALIZED)
                {
                    /*Prepare the msg*/
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = PSS_RDY;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    /*TODO : Checksum calculation*/
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the PSS_RDY response successfully to source","");
#endif
                    }
                    else
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending PSS_RDY to source : " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        std::cerr<<"Error in sending PSS_RDY message :"<<status.retCode.errCode<<"\n";
                        goto exit;
                    }
                    sinkState.store(SINK_ACTIVE);
                }
                else
                {
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = PSS_NOT_RDY;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the PSS_NOT_RDY response to source","");
#endif
                    }
                    else
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending PSS_NOT_RDY to source : " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        std::cerr<<"Error in sending PSS_NOT_RDY message :"<<status.retCode.errCode<<"\n";
                        goto exit;
                    }
                }
                break;

            case FLOW_RATE:
                if(sinkState.load() == SINK_INITIALIZED || sinkState.load() == SINK_ACTIVE)
                {
                    /*Prepare the msg*/
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = FLOW_RATE_ACK;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    /*TODO : Checksum calculation*/
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the FLOW_RATE_ACK response successfully","");
#endif
                    }
                    else
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending FLOW_RATE_ACK to source : " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        std::cerr<<"Error in sending FLOW_RATE_ACK message :"<<
                                    status.retCode.errCode<<"\n";
                        goto exit;
                    }
                    //sinkState = SINK_ACTIVE;
                }
                else
                {
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = PSS_NOT_RDY;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    /*TODO : Checksum calculation*/
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
                        printf("Sent the PSS_NOT_RDY response successfully\n");
                    }
                    else
                    {
                        std::cerr<<"Error in sending PSS_NOT_RDY message :"<<
                                    status.retCode.errCode<<"\n";
                        goto exit;
                    }
                }
                break;

            case DATA:
                if(sinkState.load() == SINK_ACTIVE)
                {
                    /*Prepare the msg*/
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = DATA_ACK;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    /*TODO : Checksum calculation*/
                    respPkt.ackSrNo = pkt.pktSrNo;
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the DATA_ACK response to source","");
#endif
                    }
                    else
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Error in sending DATA_ACK message: " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        std::cerr<<"Error in sending DATA_ACK message :"<<
                                    status.retCode.errCode<<"\n";
                        goto exit;
                    }
                    sinkState.store(SINK_ACTIVE);

                    /*TODO : We may want check status of onDataAvailable*/
                    sinkCallbacks.onDataAvailable(&pkt);

                }
                else
                {
                    memset(&respPkt, 0, sizeof(NvPSSComPacket));
                    respPkt.cmd = PSS_NOT_RDY;
                    respPkt.size = 0;
                    memset(respPkt.data, 0, MAX_DATA_SIZE);
                    calculateChecksum(&respPkt);

                    status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                                sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                    if(status.err == NvPSFMSGQ_SUCCESS)
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Sent the PSS_NOT_RDY response","");
#endif
                    }
                    else
                    {
#ifdef NVPSF_DBG
                        NvPSBWriteData(NVPSB_LOG_INFO,"Error in sending PSS_NOT_RDY message : " +
                                std::to_string(status.retCode.errCode), "");
#endif
                        std::cerr<<"Error in sending PSS_NOT_RDY message :"<<status.retCode.errCode<<"\n";
                        goto exit;
                    }
                }
                break;

            case PAUSE_ACK:
                /*PAUSE_ACK means source has accepted the PAUSE request sent earlier.*/
                /*Change the state of the sink to PAUSED*/
                sinkState.store(SINK_PAUSED);
                break;

            case RESUME_ACK:
                /*RESUME_ACK means source has accepted the RESUME request sent earlier.*/
                /*Change the state of the sink back to ACTIVE*/
                sinkState.store(SINK_ACTIVE);
                break;

            case BYE:
                /*BYE is acceptable in any state of the sink. It means that source doesn't want to
                continue sending more msgs. Throw onTerminate callback and send the BYE_ACK to source*/

                /*Prepare the resep msg for BYE_ACK*/
                memset(&respPkt, 0, sizeof(NvPSSComPacket));
                respPkt.cmd = BYE_ACK;
                respPkt.size = 0;
                memset(respPkt.data, 0, MAX_DATA_SIZE);
                /*TODO : Checksum calculation*/
                calculateChecksum(&respPkt);

                status = NvPSFMsgQueSend(writeChannelMqd, (char*)(&respPkt),
                                            sizeof(NvPSSComPacket),MSG_PRIO_DEFAULT);
                if(status.err == NvPSFMSGQ_SUCCESS)
                {
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"Sent the BYE_ACK response successfully","");
#endif
                }
                else
                {
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"Error in sending BYE_ACK message : " +
                        std::to_string(status.retCode.errCode),"");
#endif
                    std::cerr<<"Error in sending BYE_ACK message :"<<status.retCode.errCode<<"\n";
                    goto exit;
                }

                {
                    listenOnMsgQue.store(false);
                    msgQueListenerThreadNativeHandle = msgQueListenerThread.native_handle();
                    int cancel_ret = pthread_cancel(msgQueListenerThreadNativeHandle);
                    if (cancel_ret != 0) {
                        NvPSBWriteData(NVPSB_LOG_ERR, "pthread_cancel failed", "");
                    }
                    /*TODO: Check joining status whether it is CANCELLED*/
                    msgQueListenerThread.join();
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO,"msgQueListener thread is joined","");
#endif
                    sinkState.store(SINK_TERMINATED);
                    sinkCallbacks.onStop();

                    break;
                }

            case BYE_ACK:
                /*BYE_ACK means source has accepted the bye request earlier sent by the sink
                This is a formal end of communication between ongloing src-sink pair. Listener
                thread can be exited now*/
                std::cout<<"Received BYE_ACK from source\n";
                sinkState.store(SINK_TERMINATED);
#ifdef NVPSF_DBG
                NvPSBWriteData(NVPSB_LOG_INFO,"Terminating the messaging. GoodBye ","");
#endif

                /*Code to gracefully exit this thread*/
                /*Also close both the msge queues*/
                goto exit;

            default:
                std::cerr<<"Msg  is not yet implemented\n";
        }
    }
exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Calculate checksum of message
 *
 * A helper function to calculate the checksum of contents in NvPSSComPacket.
 * A checksum would be calculated before dispatching every packet over the
 * message que
*/

NvPSSComErr NvPSSCom::calculateChecksum(NvPSSComPacket* pkt)
{

    size_t checksumOffset = 0;
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    unsigned char* pktPtr = (unsigned char*)(pkt);

    if(pkt == nullptr)
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO,"Checksum calculation fail due to invalid packet","");
#endif
        std::cerr<<"Checksum calculation fail due to invalid packet \n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

    checksumOffset = offsetof(NvPSSComPacket, checksum);
    pkt->checksum = 0;
    for(size_t byteSrNo = 0; byteSrNo < checksumOffset; byteSrNo++)
    {
        pkt->checksum += pktPtr[byteSrNo];
    }

exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Terminate the ongoing session
 *
 * Function to terminate the ongoing messaging session
 * This can be invoked by either of the endpoints.
 *
*/

NvPSSComErr NvPSSCom::NvPSSComStop()
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket send_pkt;
    NvPSFMsgQueStatus mqStatus;

    /*Prepare the msg*/
    memset(&send_pkt, 0, sizeof(NvPSSComPacket));
    send_pkt.cmd = BYE;
    send_pkt.size = 0;
    memset(send_pkt.data, 0, MAX_DATA_SIZE);
    calculateChecksum(&send_pkt);

    if(backend == POSIX_MSG_QUE)
    {
        mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt),
                                    sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT);
        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Requesting termination","");
#endif
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Error in sending BYE request: " + std::to_string(mqStatus.retCode.errCode),"");
#endif
            std::cerr<<"Error in sending BYE request: "<<mqStatus.retCode.errCode;
            goto exit;
        }

        /*Wait till BYE_ACK is received*/
        if(endpt == NVPSSCOM_SRC)
        {
            while(srcState.load() != SRC_TERMINATED);
        }
        else
        {
            while(sinkState.load() != SINK_TERMINATED);
        }

        /*BYE_ACK is received means no more messages are expected
        So, terminate listener and handler threads*/
        listenOnMsgQue.store(false);
        msgQueListenerThreadNativeHandle = msgQueListenerThread.native_handle();
        int cancel_ret = pthread_cancel(msgQueListenerThreadNativeHandle);
        if (cancel_ret != 0) {
            NvPSBWriteData(NVPSB_LOG_ERR, "pthread_cancel failed", "");
        }
        /*TODO: Check joining status whether it is CANCELLED*/
        msgQueListenerThread.join();
        msgHandlerThread.join();
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO,"Joining message handler thread","");
        NvPSBWriteData(NVPSB_LOG_INFO,"msgQueListener thread is joined","");
#endif

    }
    else
    {
        std::cerr<<"messaging backend other than msg que is not yet supported \n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Close the Message queues
 *
 * Close and delelte/unlink the open message queues
 * This is the complete closure of messaging between source
 * and sink endpoints of NvPSSCom
 *
*/

NvPSSComErr NvPSSCom::NvPSSComChannelClose(NvPSSComChannelBackend backend)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSFMsgQueStatus mqStatus;

    /*Before closing the communication channel, lets ensure all the forked threads are joined
    and no thread remain running while channls are closing*/
    if(msgQueListenerThread.joinable())
    {
        msgQueListenerThread.join();
    }
    if(msgHandlerThread.joinable())
    {
        msgHandlerThread.join();
    }
    if(endpt == NVPSSCOM_SRC)
    {
        /*Data sender thread is only for the source*/
        if(dataSenderThread.joinable())
        {
            dataSenderThread.join();
        }
    }

    if(backend == POSIX_MSG_QUE)
    {
        //Close and unlink the queues
        mqStatus = NvPSFMsgQueClose(writeChannelMqd);

        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Closed MsgQ with fd : " + std::to_string(writeChannelMqd),"");
#endif
            NvPSFMsgQueUnlink(writeChannel.c_str());
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Error in closing message queue " + std::to_string(writeChannelMqd) +
                    " Code: " + std::to_string(mqStatus.retCode.errCode), "");
#endif
            std::cerr<<"Error in closing message queue : "<<writeChannel<<": "
                        <<mqStatus.retCode.errCode;
            err = NVPSSCOM_FAIL;
            goto exit;
        }

        mqStatus = NvPSFMsgQueClose(readChannelMqd);

        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Closed MsgQ with fd : " + std::to_string(writeChannelMqd),"");
#endif
            NvPSFMsgQueUnlink(readChannel.c_str());
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Error in closing message queue " + std::to_string(writeChannelMqd) +
                    " Code: " + std::to_string(mqStatus.retCode.errCode), "");
#endif
            std::cerr<<"Error in closing message queue : "<<writeChannel<<": "
                        <<mqStatus.retCode.errCode;
            err = NVPSSCOM_FAIL;
            goto exit;
        }
    }
    else
    {
        std::cerr<<"messaging backend other than msg que is not yet supported \n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }
exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 *  Pass data to PSS Sink
 *
 * This funcion is supposed to be called by source.
 * This is intended to be called by source in occurances where
 * source doesn't have any periodic data to be passed to sink.
 *
 * In order to maintain the operational consistency, this function
 * is supposed to be called after setting flow rate to 0. In that case,
 * NvPSSCom shall not invoke onDataRequest callback to source and source
 * is responsible to pass data to sink using this function,
 *
*/

NvPSSComErr NvPSSCom::NvPSSComPushData(NvPSSComPacket* pkt)
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSFMsgQueStatus mqStatus;

    pkt->cmd = DATA;
    calculateChecksum(pkt);

    if(backend == POSIX_MSG_QUE)
    {
        mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(pkt), sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT);
        if(mqStatus.err != NvPSFMSGQ_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Error in pushing data to sink : " + std::to_string(mqStatus.retCode.errCode),"");
#endif
            std::cerr<<"Error in pushing data to sink: "<<mqStatus.retCode.errCode;
            goto exit;
        }
    }
    else
    {
        std::cerr<<"messaging backend other than msg que is not yet supported \n";
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;
}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Pause the messaging session
 *
 * Temporarily pause the ongoing messaging without terminating the session
 * till NvPSSResume is called.
 *
 *
 * This is supposed to be called by sink endpoint only
*/

NvPSSComErr NvPSSCom::NvPSSComPause()
{

    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket send_pkt;
    NvPSFMsgQueStatus mqStatus;

    /*Prepare the msg*/
    memset(&send_pkt, 0, sizeof(NvPSSComPacket));
    send_pkt.cmd = PAUSE;
    send_pkt.size = 0;
    memset(send_pkt.data, 0, MAX_DATA_SIZE);
    calculateChecksum(&send_pkt);

    if(backend == POSIX_MSG_QUE)
    {
        mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt), sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT+10);
        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Requesting PAUSE to Source, Waiting","");
#endif
            /*Wait till PAUSE_ACK is received from source*/
            while(sinkState.load() != SINK_PAUSED);
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending PAUSE request : " + std::to_string(mqStatus.retCode.errCode), "");
#endif
            std::cerr<<"Error in sending PAUSE request: "<<mqStatus.retCode.errCode;
            goto exit;
        }
    }
    else
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;

}

/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/

/**
 * Resume the paused messaging session
 *
 * This is supposed to be called by sink endpoint only
*/

NvPSSComErr NvPSSCom::NvPSSComResume()
{
    NvPSSComErr err = NVPSSCOM_SUCCESS;
    NvPSSComPacket send_pkt;
    NvPSFMsgQueStatus mqStatus;

    /*Prepare the msg*/
    memset(&send_pkt, 0, sizeof(NvPSSComPacket));
    send_pkt.cmd = RESUME;
    send_pkt.size = 0;
    memset(send_pkt.data, 0, MAX_DATA_SIZE);
    calculateChecksum(&send_pkt);

    if(backend == POSIX_MSG_QUE)
    {
        mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&send_pkt), sizeof(NvPSSComPacket), MSG_PRIO_DEFAULT+10);
        if(mqStatus.err == NvPSFMSGQ_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_INFO,"Requesting RESUME, Waiting for RESUME_ACK","");
#endif
            /*TODO: Should we make sinkState (& srcState also) vars atomic ?*/
            while(sinkState.load() != SINK_ACTIVE);
        }
        else
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Error in sending RESUME request : " + std::to_string(mqStatus.retCode.errCode),"");
#endif
            std::cerr<<"Error in sending RESUME request:"<<mqStatus.retCode.errCode<<"\n";
            goto exit;
        }
    }
    else
    {
        err = NVPSSCOM_FAIL;
        goto exit;
    }

exit:
    return err;

}
/*------------------------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------------------------*/
}
