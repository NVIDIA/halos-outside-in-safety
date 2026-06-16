/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <iostream>
 #include <string>
 #include <cstring>
 #include <chrono>
 #include <thread>
 #include <errno.h>
 #include <unistd.h>
 #include <pthread.h>
 #include <sys/socket.h>
 #include <sys/un.h>

 #include "NvPSD.hpp"
 #include "NvPSB.h"
 #include "pss_message_validate.h"

 namespace nvpsd
 {

 /*
  * Constructor of NvPSD.
  * Instantiation of the NvPSD.
  * Each of the end would have four channels associated with it,
  * readChannel, writeChannel criticalReadChannel and criticalWriteChannel.
  * readChannel and criticalReadChannel for listening for
  * messages from the other end and writeChannel and criticalWriteChannel
  * for passing messages to the other end.
  */
 NvPSD::NvPSD(std::string writeChannel, std::string readChannel,
              std::string criticalWriteChannel, std::string criticalReadChannel,
              NvPSDEndpoint endpt) : writeChannel(writeChannel),
                                     readChannel(readChannel),
                                     criticalWriteChannel(criticalWriteChannel),
                                     criticalReadChannel(criticalReadChannel),
                                     endpt(endpt)
 {
     switch (endpt)
     {
     case NVPSD_PSS:
         pssState.store(UNINITIALIZED);
         clientState.store(NA);
         break;
     case NVPSD_CLIENT:
         pssState.store(NA);
         clientState.store(UNINITIALIZED);
     }

     if (endpt == NVPSD_CLIENT)
     {
         if (NvPSBInitialize("NVPSB_PSD_CLIENT", NVPSB_PSD_CLIENT) != NVPSB_SUCCESS)
         {
             std::cerr << "Failed to initialize PSB.\n";
         }
     }

     PSDclientSocket.store(-1);
     /* Must match PSS_DAEMON_SOCKET_PATH in pss/daemon/include/pss_daemon.h. */
     PSSDaemonSocketPath = "/run/nvpsf/nvpssd_to_psd";
     clientId = 0;
     pssClientId = UINT32_MAX;

     writeChannelMqd = -1;
     readChannelMqd = -1;
     criticalWriteChannelMqd = -1;
     criticalReadChannelMqd = -1;
     callbacks = {nullptr, nullptr};
     listenOnMsgChannelBackend.store(false);
     response_ready = false;
     decisionResponseTransmission = false;
 }

/*
Destructor of NvPSD
Safety net: releases any resources that NvPSDChannelClose did not (or was
never called for).  All operations are guarded to tolerate double-close.
*/
NvPSD::~NvPSD()
{
    // 1. Signal every loop / thread to stop.
    listenOnMsgChannelBackend.store(false);
    psdHeartbeatRunning.store(false);

    // Wake the decision-response transmission thread, which may be
    // blocked on cv.wait_for().
    {
        std::lock_guard<std::mutex> lk(mtx);
        response_ready = true;
    }
    cv.notify_one();

    // 2. Close the socket early so any thread blocked in recv/send
    //    returns immediately with an error.
    {
        int fd = PSDclientSocket.exchange(-1);
        if (fd != -1)
        {
            close(fd);
        }
    }

    // 3. Join all threads that were ever started.
    if (msgHandlerThread.joinable())
    {
        msgHandlerThread.join();
    }
    if (criticalMsgHandlerThread.joinable())
    {
        criticalMsgHandlerThread.join();
    }
    if (decisionResponseTrasmitThread.joinable())
    {
        decisionResponseTrasmitThread.join();
    }
    if (psdHeartbeatThread.joinable())
    {
        psdHeartbeatThread.join();
    }

    // 4. Close message-queue descriptors (POSIX_MSG_QUE backend).
    if (writeChannelMqd != -1)
    {
        NvPSFMsgQueClose(writeChannelMqd);
        writeChannelMqd = -1;
    }
    if (readChannelMqd != -1)
    {
        NvPSFMsgQueClose(readChannelMqd);
        readChannelMqd = -1;
    }
    if (criticalWriteChannelMqd != -1)
    {
        NvPSFMsgQueClose(criticalWriteChannelMqd);
        criticalWriteChannelMqd = -1;
    }
    if (criticalReadChannelMqd != -1)
    {
        NvPSFMsgQueClose(criticalReadChannelMqd);
        criticalReadChannelMqd = -1;
    }

    // 5. Terminate PSS client registration if still active.
    if (pssClientId != UINT32_MAX)
    {
        NvPSSTerminatePSSClient(pssClientId);
        pssClientId = UINT32_MAX;
    }
}

#if defined(MSG_NOSIGNAL)
static constexpr int kStreamSendFlags = MSG_NOSIGNAL;
#else
static constexpr int kStreamSendFlags = 0;
#endif

/**
 * Read exactly @p len bytes from a stream socket into @p buf.
 *
 * Handles partial reads and EINTR.  Returns:
 *   len  – full frame received
 *   0    – peer closed the connection (possibly after a partial frame)
 *  -1    – unrecoverable error (errno is set)
 */
static ssize_t recvAll(int fd, void *buf, size_t len)
{
    size_t totalRead = 0;
    auto *dst = static_cast<uint8_t *>(buf);

    while (totalRead < len)
    {
        ssize_t n = recv(fd, dst + totalRead, len - totalRead, 0);
        if (n > 0)
        {
            totalRead += static_cast<size_t>(n);
        }
        else if (n == 0)
        {
            return 0;
        }
        else
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
    }
    return static_cast<ssize_t>(totalRead);
}

/**
 * Write exactly @p len bytes from @p buf to a stream socket.
 *
 * Handles partial sends and EINTR.  Returns:
 *   len  – all bytes sent
 *   0    – peer closed the connection mid-send
 *  -1    – unrecoverable error (errno is set)
 */
static ssize_t sendAll(int fd, const void *buf, size_t len)
{
    size_t totalSent = 0;
    const auto *src = static_cast<const uint8_t *>(buf);

    while (totalSent < len)
    {
        ssize_t n = send(fd, src + totalSent, len - totalSent, kStreamSendFlags);
        if (n > 0)
        {
            totalSent += static_cast<size_t>(n);
        }
        else if (n == 0)
        {
            return 0;
        }
        else
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
    }
    return static_cast<ssize_t>(totalSent);
}

/**
 * Initialize the communication channels.
  *
  * NvPSD supports three IPC backends, POSIX message que, FSICom and NvSciIPC.
  * Currently only POSIX message que is supported.
  *
  * POSIX Message Que:
  * For a given endpoint, two message queues are created.
  * They both are opened in NON_BLOCKING mode.
  * At the user API level, asynchronous messaging is exposed by managing
  * separate threads for send and receive over created queues.
  *
  * NvSciIPC:
  * <TBD>
  * FSICom:
  * <TBD>
  *
  */
  NvPSDErr NvPSD::NvPSDChannelCreate(NvPSDChannelBackend backend)
  {
      NvPSDErr err = NVPSD_SUCCESS;
      NvPSFMsgQueStatus mqStatus = {};
      NvPSFMsgQueEndpointType mqEndptType;
      this->backend = backend;
      if(backend == POSIX_MSG_QUE)
      {
          //For PSD Msg Que endpoint is always bidirectional
          mqEndptType = MSG_QUE_BIDIRECTIONAL;
          //Create the queues
          mqStatus = NvPSFMsgQueCreate(writeChannel.c_str(), mqEndptType, NON_BLOCKING);
          if(mqStatus.err == NvPSFMSGQ_SUCCESS)
          {
              writeChannelMqd = mqStatus.retCode.mqd;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_INFO, "MsgQ is opened for read with fd : " + std::to_string(writeChannelMqd), "");
  #endif
          }
          else
          {
              std::cerr<<"Error in opening message queue : "<<writeChannel<<" Code : "<<
              mqStatus.retCode.errCode;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_ERR, "Error in opening message queue " + writeChannel +
                  "Code: " + std::to_string(mqStatus.retCode.errCode), "");
  #endif
              err = NVPSD_FAIL;
              goto exit;
          }
          mqStatus = NvPSFMsgQueCreate(criticalWriteChannel.c_str(), mqEndptType, NON_BLOCKING);
          if(mqStatus.err == NvPSFMSGQ_SUCCESS)
          {
              criticalWriteChannelMqd = mqStatus.retCode.mqd;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_INFO, "MsgQ is opened for read with fd : " + std::to_string(criticalWriteChannelMqd), "");
  #endif
          }
          else
          {
              std::cerr<<"Error in opening message queue : "<< criticalWriteChannel <<" Code : "<<
              mqStatus.retCode.errCode;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_ERR, "Error in opening message queue " + criticalWriteChannel +
                  "Code: " + std::to_string(mqStatus.retCode.errCode), "");
  #endif
              err = NVPSD_FAIL;
              goto exit;
          }
          mqStatus = NvPSFMsgQueCreate(readChannel.c_str(), mqEndptType, NON_BLOCKING);
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
              err = NVPSD_FAIL;
              goto exit;
          }
          mqStatus = NvPSFMsgQueCreate(criticalReadChannel.c_str(), mqEndptType, NON_BLOCKING);
          if(mqStatus.err == NvPSFMSGQ_SUCCESS)
          {
              criticalReadChannelMqd = mqStatus.retCode.mqd;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_INFO, "MsgQ is opened for read with fd : " + std::to_string(criticalReadChannelMqd), "");
  #endif
          }
          else
          {
              std::cerr<<"Error in opening message queue : "<<criticalReadChannel<<"Code : "<<
              mqStatus.retCode.errCode;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_ERR, "Error in opening message queue " + criticalReadChannel +
                  "Code: " + std::to_string(mqStatus.retCode.errCode), "");
  #endif
              err = NVPSD_FAIL;
              goto exit;
          }
      }
      else if(backend == POSIX_SOCKET)
      {
         struct sockaddr_un addr;

         // Create socket for a PSD Client instance
         int fd = socket(AF_UNIX, SOCK_STREAM, 0);
         if (fd == -1)
         {
             std::cerr << "Failed to create socket" << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Failed to create socket");
 #endif
             err = NVPSD_FAIL;
             goto exit;
         }

         // Connect to PSS Daemon
         memset(&addr, 0, sizeof(addr));
         addr.sun_family = AF_UNIX;
         strncpy(addr.sun_path, PSSDaemonSocketPath.c_str(), sizeof(addr.sun_path) - 1);

         if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
         {
             std::cerr << "Failed to connect to PSD server at " << PSSDaemonSocketPath << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Failed to connect to PSD server at %s", PSSDaemonSocketPath.c_str());
 #endif
             close(fd);
             err = NVPSD_FAIL;
             goto exit;
         }

         // Set socket timeouts
         struct timeval tv;
         tv.tv_sec = 5;   // 5 second timeout for setup operations
         tv.tv_usec = 0;

         if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
             std::cerr << "Warning: Failed to set receive timeout: " << strerror(errno) << std::endl;
         }

         if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
             std::cerr << "Warning: Failed to set send timeout: " << strerror(errno) << std::endl;
         }

         // Receive client ID from PSS Daemon
         struct
         {
             uint32_t clientId;
             uint32_t status;
         } response = {};
         if (recvAll(fd, &response, sizeof(response))
             == static_cast<ssize_t>(sizeof(response)))
         {
             clientId = response.clientId;
             PSDclientSocket.store(fd);
             std::cout << "Connected to PSD server with Client ID: " << clientId << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "Connected to PSD server with Client ID: %d", clientId);
 #endif
         }
         else
         {
             std::cerr << "Failed to receive client ID from server (errno="
                       << errno << ")" << std::endl;
             close(fd);
             err = NVPSD_FAIL;
             goto exit;
         }
      }
      else
      {
          std::cerr<<"IPC backend other than POSIX Message Que and SOCKET is not yet supported. Exiting\n";
  #ifdef NVPSF_DBG
          NvPSBWriteData(NVPSB_LOG_ERR, "Messaging backend other than posix message que is not yet supported", "");
  #endif
          err = NVPSD_FAIL;
          goto exit;
      }
  exit:
      return err;
  }

 /**
  * Register callbacks
  *
  * The client endpoint of NvPSD is required to register two callbacks
  * processDecisionRequest, notifyShutdownRequest.
  * notifyShutdownRequest : When PSS requests to stop the messaging and thereby terminating
  * the ongoing session
  * processDecisionRequest : When PSD is required to generate decision response messages
  * publishDecisionResponse: When PSD is required to send decision response
  *
  * When these callbacks are registered client state shifts from UNINITIALIZED => INITIALIZED
  */

 NvPSDErr NvPSD::NvPSDSetCbs(NvPSDCbInternal callbacks)
 {
     this->callbacks.processDecisionRequest = callbacks.processDecisionRequest;
     if (callbacks.publishDecisionResponse)
     {
         this->callbacks.publishDecisionResponse = callbacks.publishDecisionResponse;
         decisionResponseTransmission = true;
     }
     this->callbacks.notifyShutdownRequest = callbacks.notifyShutdownRequest;

     clientState = INITIALIZED;

     return NVPSD_SUCCESS;
 }

 /**
  * Get the endpoint for given instance of NvPSD
  *
  * This is a helper method to identify whether the given
  * instance of NvPSD is connected to PSS or Client.
  *
  * This is helpful in access controlling certain methods which
  * are restricted to particular endpoint.
  */

 NvPSDEndpoint NvPSD::NvPSDGetChannelEndpt()
 {
     return endpt;
 }

 /**
  * Launcher of listener+handler thread
  *
  * This helper function launches a thread that perfoms the following :
  *
  * It listens on the message que for messages from the other endpoint.
  * Once the message is received, depending on the
  * command, it takes the subsequent action i.e. invoke certain callback or
  * send the acknowledgement etc.
  *
  *
  */

 NvPSDErr NvPSD::setPssHeartbeatExternallyManaged(bool external)
 {
     if (listenOnMsgChannelBackend.load())
         return NVPSD_FAIL;
     pssHeartbeatExternallyManaged.store(external);
     return NVPSD_SUCCESS;
 }

 NvPSDErr NvPSD::NvPSDChannelListenerStart()
 {
 NvPSDErr err = NVPSD_SUCCESS;
     if (backend == POSIX_MSG_QUE)
     {
         listenOnMsgChannelBackend.store(true);
         msgHandlerThread = std::thread(&NvPSD::handleMsgsOnMsgQueClientEndpt, this);
         criticalMsgHandlerThread = std::thread(&NvPSD::handleMsgsOnCriticalMsgQueClientEndpt, this);
         if (decisionResponseTransmission)
         {
             decisionResponseTrasmitThread = std::thread(&NvPSD::handleDecisionResponseTransmission, this);
         }

         if (!pssHeartbeatExternallyManaged.load())
             NvPSDStartHeartbeat();
     }
     else if (backend == POSIX_SOCKET)
     {
         listenOnMsgChannelBackend.store(true);

         // Start single socket-based message handler (replaces both normal and critical message handlers)
         msgHandlerThread = std::thread(&NvPSD::handleMsgsOnMsgQueClientEndpt, this);

         // Don't start critical message handler - single socket handles all messages
         // criticalMsgHandlerThread = std::thread(&NvPSD::handleMsgsOnCriticalMsgQueClientEndpt,this);

         if (decisionResponseTransmission)
         {
             decisionResponseTrasmitThread = std::thread(&NvPSD::handleDecisionResponseTransmission, this);
         }

         if (!pssHeartbeatExternallyManaged.load())
             NvPSDStartHeartbeat();
     }
     else
     {
         err = NVPSD_FAIL;
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que and posix socket is not yet supported", "");
 #endif
     }

     return err;
 }

 /**
  * Send the request to NvPSD about staring the messaging session
  *
  * This method would be only accessed by PSS endpoint.
  * Access to this method is controlled at NvPSD_interface.
  *
  */

NvPSDErr NvPSD::NvPSDRequestStart()
{
    pssState.store(INITIALIZED);
    listenOnMsgChannelBackend.store(true);

    return NVPSD_SUCCESS;
}

 /**
  *  Decision Request to PSD Client
  *
  * This funcion is supposed to be called by NVPSD_PSS endpoint.
  *
  */

  NvPSDErr NvPSD::NvPSDGenerateDecision(const DecisionRequest* request, DecisionResponse* response)
  {
      NvPSDErr err = NVPSD_SUCCESS;
      NvPSFMsgQueStatus mqStatus = {};
      int channelMqdWrite = -1;
      int channelMqdRead = -1;
    if(pssState != INITIALIZED)
    {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"PSS endpoint is not in INITIALIZED state", "");
#endif
            std::cerr<<"PSS endpoint is not in INITIALIZED state";
            err = NVPSD_FAIL;
            goto exit;
    }

     /* Reject zero-size requests that somehow bypassed validation;
      * reading sensorDataSummary[0] with no entries is undefined. */
      if (request->sensorDataSummarySize == 0)
      {
          std::cerr << "NvPSDGenerateDecision: sensorDataSummarySize is 0, cannot route" << std::endl;
          err = NVPSD_FAIL;
          goto exit;
      }

     /* Select Channel based on Msg severity */
      if (request->sensorDataSummary[0].event.severity == CRITICAL)
      {
          channelMqdWrite = criticalWriteChannelMqd;
          channelMqdRead  = criticalReadChannelMqd;
      }
      else {
          channelMqdWrite = writeChannelMqd;
          channelMqdRead  = readChannelMqd;
      }
      /* Send Decision Request to Client */
      if(backend == POSIX_MSG_QUE)
      {
          mqStatus = NvPSFMsgQueSend(channelMqdWrite, (char*)(request), sizeof(DecisionRequest), MSG_PRIO_DEFAULT);
          if(mqStatus.err != NvPSFMSGQ_SUCCESS)
          {
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_ERR,"Error in pushing data to client : " + std::to_string(mqStatus.retCode.errCode), "");
  #endif
              std::cerr<<"Error in pushing data to client: "<<mqStatus.retCode.errCode;
              err = NVPSD_FAIL;
              goto exit;
          }
      }
      else
      {
  #ifdef NVPSF_DBG
          NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que is not yet supported", "");
  #endif
          std::cerr<<"messaging backend other than msg que is not yet supported \n";
          err = NVPSD_FAIL;
          goto exit;
      }
      /* Wait for Decision Response from Client (mq NON_BLOCKING — retry EAGAIN until PSD replies or timeout).
       * The deadline drives the loop; listenOnMsgChannelBackend is checked
       * as an early exit so shutdown is still prompt. */
      if(backend == POSIX_MSG_QUE)
      {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
          bool received = false;
          while (std::chrono::steady_clock::now() < deadline)
          {
              if (!listenOnMsgChannelBackend.load())
              {
                  std::cerr << "Backend stopped while waiting for DecisionResponse\n";
                  err = NVPSD_FAIL;
                  goto exit;
              }
              mqStatus = NvPSFMsgQueReceive(channelMqdRead, (char*)(response), sizeof(DecisionResponse), NULL);
              if (mqStatus.err == NvPSFMSGQ_SUCCESS)
              {
                  if (mqStatus.retCode.recvd_bytes != static_cast<int>(sizeof(DecisionResponse)))
                  {
#ifdef NVPSF_DBG
                      NvPSBWriteData(NVPSB_LOG_ERR,
                                     "Short DecisionResponse: got " +
                                     std::to_string(mqStatus.retCode.recvd_bytes) +
                                     " bytes, expected " +
                                     std::to_string(sizeof(DecisionResponse)),
                                     "");
#endif
                      std::cerr << "Short DecisionResponse (" << mqStatus.retCode.recvd_bytes
                                << "/" << sizeof(DecisionResponse) << " bytes), dropping\n";
                      continue;
                  }
                  received = true;
                  break;
              }
              if (mqStatus.retCode.errCode == EAGAIN || mqStatus.retCode.errCode == EWOULDBLOCK)
              {
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
                  continue;
              }
              err = NVPSD_FAIL;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_ERR,"Error in receiving data frm client : " + std::to_string(mqStatus.retCode.errCode), "");
  #endif
              std::cerr<<"Error in receiving data from client: "<<mqStatus.retCode.errCode;
              goto exit;
          }
          if (!received)
          {
              err = NVPSD_FAIL;
  #ifdef NVPSF_DBG
              NvPSBWriteData(NVPSB_LOG_ERR,
                             "Timeout waiting for DecisionResponse from PSD (non-blocking mq)",
                             "");
  #endif
              std::cerr << "Timeout waiting for DecisionResponse from client\n";
              goto exit;
          }
      }
     else if (backend == POSIX_SOCKET)
     {
         int fd = PSDclientSocket.load();
         if (fd == -1)
         {
 #ifdef NVPSD_DBG
         syslog(LOG_ERR, "Socket not connected");
 #endif
             std::cerr << "Socket not connected";
             err = NVPSD_FAIL;
             goto exit;
         }
        // Send Decision Request to PSD Client via socket called from PSS Daemon
        if (sendAll(fd, request, sizeof(DecisionRequest))
            != static_cast<ssize_t>(sizeof(DecisionRequest)))
        {
            int savedErrno = errno;
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in sending data to client via socket (errno=%d)", savedErrno);
#endif
            std::cerr << "Error in sending data to client via socket (errno="
                      << savedErrno << "), disconnecting" << std::endl;
            /* A partial write leaves the stream out of frame alignment.
             * Disconnect so future calls fail fast instead of silently
             * reading/writing corrupted frames. */
            int closeFd = PSDclientSocket.exchange(-1);
            if (closeFd != -1)
            {
                close(closeFd);
            }
            err = NVPSD_FAIL;
            goto exit;
        }
        // Wait for Decision Response from PSD Client via socket
        if (recvAll(fd, response, sizeof(DecisionResponse))
            != static_cast<ssize_t>(sizeof(DecisionResponse)))
        {
            int savedErrno = errno;
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in receiving data from client via socket (errno=%d)", savedErrno);
#endif
            std::cerr << "Error in receiving data from client via socket (errno="
                      << savedErrno << "), disconnecting" << std::endl;
            /* A partial or failed read desynchronizes the stream.
             * Disconnect to prevent corrupted future exchanges. */
            int closeFd = PSDclientSocket.exchange(-1);
            if (closeFd != -1)
            {
                close(closeFd);
            }
            err = NVPSD_FAIL;
            goto exit;
        }
      }
      else
      {
  #ifdef NVPSF_DBG
          NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que is not yet supported", "");
  #endif
          std::cerr<<"messaging backend other than msg que is not yet supported \n";
          err = NVPSD_FAIL;
          goto exit;
      }
  exit:
      return err;
  }

 /**
  * Terminate the ongoing session
  *
  * Function to terminate the ongoing messaging session
  * This can only be invoked by PSS endpoint.
  *
  */

 NvPSDErr NvPSD::NvPSDStop()
 {
     NvPSDErr err = NVPSD_SUCCESS;
     DecisionRequest request = {};

     if (pssState != INITIALIZED)
     {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"PSS endpoint is not in INITIALIZED stated", "");
 #endif
         std::cerr << "PSS endpoint is not in INITIALIZED stated";
         err = NVPSD_FAIL;
         goto exit;
     }

    request.requestId = stopID;
    request.sensorDataSummarySize = 0;  // STOP request has no sensor data
    request.pssStatus.mode = stopMode;
    pssDecisionRequestSetCRC(&request);

    if (backend == POSIX_MSG_QUE)
     {
         NvPSFMsgQueStatus mqStatus = {};
         mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char *)(&request), sizeof(DecisionRequest), MSG_PRIO_DEFAULT);
         if (mqStatus.err != NvPSFMSGQ_SUCCESS)
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"Error in pushing data to client : " + std::to_string(mqStatus.retCode.errCode), "");
 #endif
             std::cerr << "Error in pushing data to client: " << mqStatus.retCode.errCode;
             goto exit;
         }

         mqStatus = NvPSFMsgQueSend(criticalWriteChannelMqd, (char *)(&request),
                                    sizeof(DecisionRequest), MSG_PRIO_DEFAULT);
         if (mqStatus.err != NvPSFMSGQ_SUCCESS)
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"Error in pushing data to client : " + std::to_string(mqStatus.retCode.errCode), "");
 #endif
             std::cerr << "Error in pushing data to client: " << mqStatus.retCode.errCode;
             goto exit;
         }
     }
     else if (backend == POSIX_SOCKET)
     {
         // Send STOP Request to PSD Client via socket
         int fd = PSDclientSocket.load();
         if (fd == -1)
         {
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Socket not connected");
 #endif
             std::cerr << "Socket not connected";
             err = NVPSD_FAIL;
             goto exit;
         }
         if (sendAll(fd, &request, sizeof(DecisionRequest))
             != static_cast<ssize_t>(sizeof(DecisionRequest)))
         {
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Error in sending STOP request to client via socket (errno=%d)", errno);
 #endif
             std::cerr << "Error in sending STOP request to client via socket (errno="
                       << errno << ")" << std::endl;
             err = NVPSD_FAIL;
             goto exit;
         }

 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "STOP request sent to client via socket");
 #endif
         std::cout << "STOP request sent to client via socket" << std::endl;
     }
     else
     {
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_ERR,"Messaging backend other than posix message que is not yet supported", "");
 #endif
         std::cerr<<"messaging backend other than msg que is not yet supported \n";
         err = NVPSD_FAIL;
         goto exit;
     }

 exit:
     return err;
 }

 /**
  * Close the Message queues
  *
  * Close and delelte/unlink the open message queues
  * This is the complete closure of messaging between PSS
  * and Client endpoints of NvPSD
  *
  */

 NvPSDErr NvPSD::NvPSDChannelClose(NvPSDChannelBackend backend)
 {
     NvPSDErr err = NVPSD_SUCCESS;

     // Signal threads to stop
     listenOnMsgChannelBackend.store(false);
     std::this_thread::sleep_for(std::chrono::milliseconds(200));

     /*Before closing the communication channel, lets ensure all the forked threads are joined
     and no thread remain running while channels are closing*/
     if (endpt == NVPSD_CLIENT)
     {
         if (msgHandlerThread.joinable())
         {
             msgHandlerThread.join();
         }

         if (decisionResponseTrasmitThread.joinable())
         {
             decisionResponseTrasmitThread.join();
         }
     }

     if (backend == POSIX_MSG_QUE)
     {
         // Stop heartbeat thread before closing
         NvPSDStopHeartbeat();

         if (criticalMsgHandlerThread.joinable())
         {
             criticalMsgHandlerThread.join();
         }
         NvPSFMsgQueStatus mqStatus = {};
         // Close and unlink the queues
         mqStatus = NvPSFMsgQueClose(writeChannelMqd);

         if (mqStatus.err == NvPSFMSGQ_SUCCESS)
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_INFO,"CLose MsgQ with fd  " + std::to_string(writeChannelMqd), "");
 #endif
             writeChannelMqd = -1;
             NvPSFMsgQueUnlink(writeChannel.c_str());
         }
         else
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"Error in closing message queue " + std::to_string(writeChannelMqd) +
                     " Code: " + std::to_string(mqStatus.retCode.errCode), "");
 #endif
             std::cerr << "Error in closing message queue : " << writeChannel << ": "
                       << mqStatus.retCode.errCode;
             err = NVPSD_FAIL;
             goto exit;
         }

         mqStatus = NvPSFMsgQueClose(readChannelMqd);

         if (mqStatus.err == NvPSFMSGQ_SUCCESS)
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_INFO,"Close MsgQ with fd " + std::to_string(readChannelMqd), "");
 #endif
             readChannelMqd = -1;
             NvPSFMsgQueUnlink(readChannel.c_str());
         }
         else
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"Error in closing message queue " + std::to_string(readChannelMqd) +
                     "Code: " + std::to_string(mqStatus.retCode.errCode), "");
 #endif
             std::cerr << "Error in closing message queue : " << readChannel << ": "
                       << mqStatus.retCode.errCode;
             err = NVPSD_FAIL;
             goto exit;
         }

         mqStatus = NvPSFMsgQueClose(criticalWriteChannelMqd);

         if (mqStatus.err == NvPSFMSGQ_SUCCESS)
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_INFO,"Closed MsgQ with fd " + std::to_string(criticalWriteChannelMqd), "");
 #endif
             criticalWriteChannelMqd = -1;
             NvPSFMsgQueUnlink(criticalWriteChannel.c_str());
         }
         else
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"Error in closing message queue " + std::to_string(criticalWriteChannelMqd) +
                     "Code: " + std::to_string(mqStatus.retCode.errCode), "");
 #endif
             std::cerr << "Error in closing message queue : " << criticalWriteChannel << ": "
                       << mqStatus.retCode.errCode;
             err = NVPSD_FAIL;
             goto exit;
         }

         mqStatus = NvPSFMsgQueClose(criticalReadChannelMqd);

         if (mqStatus.err == NvPSFMSGQ_SUCCESS)
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_INFO,"Closed MsgQ with fd : " + std::to_string(criticalReadChannelMqd), "");
 #endif
             criticalReadChannelMqd = -1;
             NvPSFMsgQueUnlink(criticalReadChannel.c_str());
         }
         else
         {
 #ifdef NVPSF_DBG
             NvPSBWriteData(NVPSB_LOG_ERR,"Error in closing message queue " + std::to_string(criticalReadChannelMqd) +
                     "Code: " + std::to_string(mqStatus.retCode.errCode), "");
 #endif
             std::cerr << "Error in closing message queue : " << criticalReadChannel << ": "
                       << mqStatus.retCode.errCode;
             err = NVPSD_FAIL;
             goto exit;
         }
     }
     else if (backend == POSIX_SOCKET)
     {
         // Stop heartbeat thread before closing
         NvPSDStopHeartbeat();

         // Close PSD Client socket
         int fd = PSDclientSocket.exchange(-1);
         if (fd != -1)
         {
             // Send unregister message to PSS Daemon before closing
             PSDRegistrationMsg msg = {};
             msg.msgType = UNREGISTER_PSD_CLIENT;
             msg.clientId = clientId;
             msg.eventTypesCount = 0;
             if (sendAll(fd, &msg, sizeof(msg))
                 != static_cast<ssize_t>(sizeof(msg)))
             {
 #ifdef NVPSD_DBG
                 syslog(LOG_ERR, "Failed to send unregister message (errno=%d)", errno);
 #endif
                 std::cerr << "Failed to send unregister message (errno="
                           << errno << ")" << std::endl;
             }

             // Close socket
             close(fd);

 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "Socket connection closed");
 #endif
             std::cout << "Disconnected from PSD server" << std::endl;
         }
     }
     else
     {
         std::cerr << "messaging backend other than socket is not yet supported" << std::endl;
         err = NVPSD_FAIL;
         goto exit;
     }

 exit:
     return err;
 }

/**
 * Listener on the message queue  and Message handler thread for client
 *
 * Entry point of the listener thread.
 *
 * This thread would keep on listening on the message que for messages from the PSS endpoint.
 * This thread also handles the received messages.
 *
 */

 NvPSDErr NvPSD::handleMsgsOnMsgQueClientEndpt()
  {
      NvPSFMsgQueStatus mqStatus = {};
      NvPSDErr err = NVPSD_SUCCESS;
      DecisionRequest request = {};
      DecisionResponse response = {};

     if(backend == POSIX_MSG_QUE)
     {
        while(listenOnMsgChannelBackend.load())
        {
            //Obtain Decision/STOP Request
            mqStatus = NvPSFMsgQueReceive(readChannelMqd, (char*)(&request), sizeof(DecisionRequest), NULL);
            if(mqStatus.err == NvPSFMSGQ_SUCCESS)
            {
                /* Reject short/oversized messages before touching fields. */
                if (mqStatus.retCode.recvd_bytes < 0 ||
                    static_cast<size_t>(mqStatus.retCode.recvd_bytes) != sizeof(DecisionRequest))
                {
                    std::cerr << "handleMsgsOnMsgQueClientEndpt: received "
                              << mqStatus.retCode.recvd_bytes << " bytes, expected "
                              << sizeof(DecisionRequest) << ", dropping" << std::endl;
                    continue;
                }

                /* Verify CRC + fields before acting on the message. */
                {
                    uint32_t vErr = validateDecisionRequest(&request);
                    if (vErr != PSS_VALID)
                    {
                        std::cerr << "handleMsgsOnMsgQueClientEndpt: DecisionRequest validation failed (0x"
                                  << std::hex << vErr << std::dec << "), dropping" << std::endl;
                        continue;
                    }
                }

                //Handle STOP
                if(request.requestId == stopID && request.pssStatus.mode == stopMode)
                {
                     listenOnMsgChannelBackend.store(false);
                     if (callbacks.notifyShutdownRequest)
                     {
                         callbacks.notifyShutdownRequest();
                     }
                     else
                     {
                         std::cerr << "ERROR: notifyShutdownRequest callback not registered" << std::endl;
                     }
                     if(NvPSBExit() != NVPSB_SUCCESS)
                     {
                         std::cerr<<"Failed to exit NvPSB.\n";
                     }
                     break;
                 }

                 //Generate Decision
                 if (callbacks.processDecisionRequest)
                 {
                     callbacks.processDecisionRequest(&request, &response);
                 }
                 else
                 {
                     std::cerr << "ERROR: processDecisionRequest callback not registered" << std::endl;
                     // Fill default response
                     response.decisionId = NVPSD_NO_RSP;
                     response.action = NO_ACTION_REQUIRED;
                     response.confidenceLevel = 0.0f;
                 }

                 //Report Decision
                 if(decisionResponseTransmission && callbacks.publishDecisionResponse)
                 {
                     std::lock_guard<std::mutex> lock(mtx);
                     decisionResponseQueue.push(response);
                     response_ready = true;
                     cv.notify_one();
                 }
                 //Send Response
                 mqStatus = NvPSFMsgQueSend(writeChannelMqd, (char*)(&response), sizeof(DecisionResponse), MSG_PRIO_DEFAULT);
                 if(mqStatus.err != NvPSFMSGQ_SUCCESS)
                 {
     #ifdef NVPSF_DBG
                     NvPSBWriteData(NVPSB_LOG_ERR,"Error in pushing data to PSS : " + std::to_string(mqStatus.retCode.errCode), "");
     #endif
                     std::cerr<<"Error in pushing data to PSS: "<<mqStatus.retCode.errCode;
                 }
             }
             else
             {
                // Check if it's EAGAIN/EWOULDBLOCK (no message available in non-blocking mode)
                if(mqStatus.retCode.errCode == EAGAIN || mqStatus.retCode.errCode == EWOULDBLOCK)
                {
                    // No message available - sleep briefly and retry
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                else
                {
                  // Real error occurred
     #ifdef NVPSF_DBG
                  NvPSBWriteData(NVPSB_LOG_ERR,"Error in receiving on message queue : " + std::to_string(mqStatus.retCode.errCode), "");
     #endif
                 /**TODO:
                 * The error in receiving on message que has been ignored here and next message is
                 * being awaited.
                 * Perhaps it would be better to count number of read failures and exit the messaging
                 * if the read failures cross certain threshold
                 */
                  // Sleep briefly on error to avoid busy loop
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
             }
         }
     }
    else if(backend == POSIX_SOCKET)
    {
        while (listenOnMsgChannelBackend.load())
        {
            int fd = PSDclientSocket.load();
            if (fd == -1)
            {
                break;
            }

            ssize_t bytesReceived = recvAll(fd, &request,
                                            sizeof(DecisionRequest));

           if (bytesReceived == static_cast<ssize_t>(sizeof(DecisionRequest)))
           {
                /* Verify CRC + fields before acting on the message. */
                {
                    uint32_t vErr = validateDecisionRequest(&request);
                    if (vErr != PSS_VALID)
                    {
                        std::cerr << "handleMsgsOnMsgQueClientEndpt(socket): DecisionRequest validation failed (0x"
                                  << std::hex << vErr << std::dec << "), dropping" << std::endl;
                        continue;
                    }
                }

                // Handle STOP request (special case)
                if (request.requestId == stopID && request.pssStatus.mode == stopMode)
                {
                     listenOnMsgChannelBackend.store(false);
                     if (callbacks.notifyShutdownRequest)
                     {
                         callbacks.notifyShutdownRequest();
                     }
                     else
                     {
                         std::cerr << "ERROR: notifyShutdownRequest callback not registered" << std::endl;
                     }
                     if (NvPSBExit() != NVPSB_SUCCESS)
                     {
                         std::cerr << "Failed to exit NvPSB.\n";
                     }
                     break;
                 }

                 // Generate Decision using callback
                 if (callbacks.processDecisionRequest)
                 {
                     callbacks.processDecisionRequest(&request, &response);
                 }
                 else
                 {
                     std::cerr << "ERROR: processDecisionRequest callback not registered" << std::endl;
                     // Fill default response
                     response.decisionId = NVPSD_NO_RSP;
                     response.action = NO_ACTION_REQUIRED;
                     response.confidenceLevel = 0.0f;
                 }

                 // Report Decision
                 if (decisionResponseTransmission && callbacks.publishDecisionResponse)
                 {
                     std::lock_guard<std::mutex> lock(mtx);
                     decisionResponseQueue.push(response);
                     response_ready = true;
                     cv.notify_one();
                 }

                 // Send DecisionResponse to PSS Daemon via socket
                 ssize_t bytesSent = sendAll(fd, &response,
                                             sizeof(DecisionResponse));
                 if (bytesSent != static_cast<ssize_t>(sizeof(DecisionResponse)))
                 {
                     int savedErrno = errno;
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in sending DecisionResponse via socket (errno=%d)",
                   savedErrno);
#endif
                     std::cerr << "Error sending DecisionResponse via socket (errno="
                               << savedErrno << "), disconnecting" << std::endl;
                     int closeFd = PSDclientSocket.exchange(-1);
                     if (closeFd != -1)
                     {
                         close(closeFd);
                     }
                     listenOnMsgChannelBackend.store(false);
                     break;
                 }
            }
            else if (bytesReceived == 0)
            {
                std::cout << "PSD server closed connection" << std::endl;
                int closeFd = PSDclientSocket.exchange(-1);
                if (closeFd != -1)
                {
                    close(closeFd);
                }
                listenOnMsgChannelBackend.store(false);
                break;
            }
            else
            {
                /* recvAll returned -1: unrecoverable recv error or timeout.
                 * Disconnect to avoid a desynchronized stream. */
#ifdef NVPSD_DBG
            syslog(LOG_ERR, "Error in receiving DecisionRequest on socket, disconnecting");
#endif
                std::cerr << "Error receiving DecisionRequest on socket (errno="
                          << errno << "), disconnecting" << std::endl;
                int closeFd = PSDclientSocket.exchange(-1);
                if (closeFd != -1)
                {
                    close(closeFd);
                }
                listenOnMsgChannelBackend.store(false);
                break;
            }
        }
    }
      return err;
  }


 /**
  * Listener on the critical message queue and Message handler thread for client
  *
  * Entry point of the listener thread.
  *
  * This thread would keep on listening on the critical message que for messages from the PSS endpoint.
  * This thread also handles the received messages.
  *
  */
  NvPSDErr NvPSD::handleMsgsOnCriticalMsgQueClientEndpt()
  {
      NvPSFMsgQueStatus mqStatus = {};
      NvPSDErr err = NVPSD_SUCCESS;
      DecisionRequest request = {};
      DecisionResponse response = {};
      if(backend == POSIX_MSG_QUE)
      {
         while(listenOnMsgChannelBackend.load())
         {
             //Obtain Decision/STOP Request
            mqStatus = NvPSFMsgQueReceive(criticalReadChannelMqd, (char*)(&request),
                                        sizeof(DecisionRequest), NULL);
            if(mqStatus.err == NvPSFMSGQ_SUCCESS)
            {
                /* Reject short/oversized messages before touching fields. */
                if (mqStatus.retCode.recvd_bytes < 0 ||
                    static_cast<size_t>(mqStatus.retCode.recvd_bytes) != sizeof(DecisionRequest))
                {
                    std::cerr << "handleMsgsOnCriticalMsgQueClientEndpt: received "
                              << mqStatus.retCode.recvd_bytes << " bytes, expected "
                              << sizeof(DecisionRequest) << ", dropping" << std::endl;
                    continue;
                }

                /* Verify CRC + fields before acting on the message. */
                {
                    uint32_t vErr = validateDecisionRequest(&request);
                    if (vErr != PSS_VALID)
                    {
                        std::cerr << "handleMsgsOnCriticalMsgQueClientEndpt: DecisionRequest validation failed (0x"
                                  << std::hex << vErr << std::dec << "), dropping" << std::endl;
                        continue;
                    }
                }

                //Handle STOP
                if(request.requestId == stopID && request.pssStatus.mode == stopMode)
                {
                     // Only break out of this. notifyShutdownRequest is triggered from the other thread.
                     break;
                 }
                 //Generate Decision
                 if (callbacks.processDecisionRequest)
                 {
                     callbacks.processDecisionRequest(&request, &response);
                 }
                 else
                 {
                     std::cerr << "ERROR: processDecisionRequest callback not registered" << std::endl;
                     // Fill default response
                     response.decisionId = NVPSD_NO_RSP;
                     response.action = NO_ACTION_REQUIRED;
                     response.confidenceLevel = 0.0f;
                 }

                 //Report Decision
                 if(decisionResponseTransmission && callbacks.publishDecisionResponse)
                 {
                     std::lock_guard<std::mutex> lock(mtx);
                     decisionResponseQueue.push(response);
                     response_ready = true;
                     cv.notify_one();
                 }
                 //Send Response
                 mqStatus = NvPSFMsgQueSend(criticalWriteChannelMqd, (char*)(&response),
                                             sizeof(DecisionResponse), MSG_PRIO_DEFAULT);
                 if(mqStatus.err != NvPSFMSGQ_SUCCESS)
                 {
     #ifdef NVPSF_DBG
                     NvPSBWriteData(NVPSB_LOG_ERR,"Error in pushing data to PSS : " + std::to_string(mqStatus.retCode.errCode), "");
     #endif
                     std::cerr<<"Error in pushing data to PSS: "<<mqStatus.retCode.errCode;
                 }
             }
             else
             {
                // Check if it's EAGAIN/EWOULDBLOCK (no message available in non-blocking mode)
                if(mqStatus.retCode.errCode == EAGAIN || mqStatus.retCode.errCode == EWOULDBLOCK)
                {
                    // No message available - sleep briefly and retry
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                else
                {
     #ifdef NVPSF_DBG
                 NvPSBWriteData(NVPSB_LOG_ERR,"Error in receiving on message queue :" + std::to_string(mqStatus.retCode.errCode), "");
     #endif
                 /**TODO:
                 * The error in receiving on message que has been ignored here and next message is
                 * being awaited.
                 * Perhaps it would be better to count number of read failures and exit the messaging
                 * if the read failures cross certain threshold
                 */
                  // Sleep briefly on error to avoid busy loop
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
             }
         }
     }
     else if(backend == POSIX_SOCKET)
     {

         // No separate critical message handling needed with socket-based approach
         // All messages (critical and non-critical) handled by single socket in handleMsgsOnMsgQueClientEndpt
         err = NVPSD_SUCCESS;
     }
      return err;
  }


 /**
  * Listener on the decision response for client
  *
  * Entry point of the listener thread.
  *
  * This thread would keep on listening on the decision response genrated by client.
  * This thread also handles transmission of generated decision response.
  *
  */
 NvPSDErr NvPSD::handleDecisionResponseTransmission()
 {
     NvPSDErr err = NVPSD_SUCCESS;

     while (listenOnMsgChannelBackend.load())
     {
         std::unique_lock<std::mutex> lck(mtx);
         if (cv.wait_for(lck, std::chrono::seconds(timeout),
                         [&]
                         { return response_ready; }))
         {
             // Condition met
             response_ready = false;
             if (!decisionResponseQueue.empty())
             {
                 // Copy response before releasing lock
                 auto response = decisionResponseQueue.front();
                 decisionResponseQueue.pop();
                 // Release lock before calling callback
                 lck.unlock();

                 if (callbacks.publishDecisionResponse) {
                     callbacks.publishDecisionResponse(&response);
                 }
             }
             else
             {
                 lck.unlock();
                 std::cerr << "No Decision Response found to report\n";
             }
         }
         else
         {
             // Timeout
             continue;
         }
     }

     return err;
 }

 NvPSDErr NvPSD::NvPSDRegisterEventTypes(const EventType *eventTypes, uint32_t count)
 {
     NvPSDErr err = NVPSD_SUCCESS;

     int fd = PSDclientSocket.load();
     if (fd == -1)
     {
         std::cerr << "Socket not connected. Call NvPSDChannelCreate first." << std::endl;
         return NVPSD_FAIL;
     }

     // Prepare registration message
     PSDRegistrationMsg msg = {};
     msg.msgType = REGISTER_EVENT_TYPES;
     msg.clientId = clientId;
     msg.eventTypesCount = count;

     for (uint32_t i = 0; i < count && i < 10; i++)
     {
         msg.eventTypes[i] = eventTypes[i];
     }

     // Send registration request to PSS Daemon
     if (sendAll(fd, &msg, sizeof(msg))
         != static_cast<ssize_t>(sizeof(msg)))
     {
         std::cerr << "Failed to send event type registration (errno="
                   << errno << ")" << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_ERR, "Failed to send event type registration (errno=%d)", errno);
 #endif
         return NVPSD_FAIL;
     }

     // Wait for confirmation from PSS Daemon
     struct
     {
         uint32_t clientId;
         uint32_t status;
     } response = {};
     if (recvAll(fd, &response, sizeof(response))
         == static_cast<ssize_t>(sizeof(response)))
     {
         if (response.status == 0)
         {
             std::cout << "Successfully registered " << count << " event types" << std::endl;
 #ifdef NVPSD_DBG
             syslog(LOG_INFO, "Successfully registered %d event types", count);
 #endif
         }
         else
         {
             std::cerr << "Failed to register event types - server response: " << response.status << std::endl;
             err = NVPSD_FAIL;
         }
     }
     else
     {
         std::cerr << "Failed to receive registration response (errno="
                   << errno << ")" << std::endl;
         err = NVPSD_FAIL;
     }

     return err;
 }

 NvPSDErr NvPSD::NvPSDStartHeartbeat()
 {
     NvPSDErr err = NVPSD_SUCCESS;

     if (psdHeartbeatRunning.load())
     {
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_INFO, "Heartbeat already running", "");
 #endif
         return NVPSD_SUCCESS;
     }

     if (NvPSSRegisterPSSClient(&pssClientId, CLIENT_PSD_GATEWAY) != NVPSSD_SUCCESS)
     {
         std::cerr << "Failed to register with PSS daemon for heartbeat" << std::endl;
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_ERR, "Failed to register with PSS daemon for heartbeat", "");
 #endif
         return NVPSD_FAIL;
     }

 #ifdef NVPSF_DBG
     NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD registered with PSS daemon, clientId: " + std::to_string(pssClientId), "");
 #endif

     psdHeartbeatRunning.store(true);
     psdHeartbeatThread = std::thread(&NvPSD::psdHeartbeatLoop, this);

 #ifdef NVPSF_DBG
     NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD heartbeat thread started", "");
 #endif

     return err;
 }

 NvPSDErr NvPSD::NvPSDStopHeartbeat()
 {
     NvPSDErr err = NVPSD_SUCCESS;

     psdHeartbeatRunning.store(false);

     if (psdHeartbeatThread.joinable())
     {
         psdHeartbeatThread.join();
     }

     if (pssClientId != UINT32_MAX)
     {
         NvPSSTerminatePSSClient(pssClientId);
         pssClientId = UINT32_MAX;
 #ifdef NVPSF_DBG
         NvPSBWriteData(NVPSB_LOG_INFO, "NvPSD terminated PSS client registration", "");
 #endif
     }

     return err;
 }

 NvPSDErr NvPSD::psdHeartbeatLoop()
 {
     NvPSDErr err = NVPSD_SUCCESS;
     NvPSSDErr hbStatus = NVPSSD_SUCCESS;
     int consecutiveFailures = 0;
     const int maxConsecutiveFailures = 10;

     while (psdHeartbeatRunning.load() && listenOnMsgChannelBackend.load())
     {
         if (pssClientId != UINT32_MAX)
         {
             hbStatus = NvPSSSendHeartbeat(pssClientId, CLIENT_PSD_GATEWAY);
             if (hbStatus != NVPSSD_SUCCESS)
             {
                 consecutiveFailures++;
                 if (consecutiveFailures >= maxConsecutiveFailures)
                 {
 #ifdef NVPSF_DBG
                     NvPSBWriteData(NVPSB_LOG_ERR, "NvPSD heartbeat failed " +
                         std::to_string(maxConsecutiveFailures) + " consecutive times, exiting", "");
 #endif
                     listenOnMsgChannelBackend.store(false);
                     break;
                 }
             }
             else
             {
                 consecutiveFailures = 0;
             }
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(HB_INTERVAL_MS));
     }

     return err;
 }

 }
