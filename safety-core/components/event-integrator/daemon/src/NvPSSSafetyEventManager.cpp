/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <vector>
#include <iostream>
#include <cstring>
#include <ctime>

#include "NvPSSSafetyEventManager.hpp"
#include "NvPSSDRPC.hpp"
#include "NvPSB.h"
#include "pss_protocol.h"
#include "pss_message_validate.h"

extern std::atomic<uint32_t> g_pssMaxHbFailures;
extern std::atomic<uint32_t> g_pssWarnThreshold;

namespace nvpss
{

SafetyEventManager::SafetyEventManager(uint64_t criticalPrioQuePeriod, uint64_t highPrioQuePeriod,
                                       uint64_t mediumPrioQuePeriod, uint64_t lowPrioQuePeriod,
                                       uint64_t inputSafetyEventQuePeriod, uint64_t fusionEventPeriod, NvPSDChannelBackend PSSDToPSDComBackend)
    :criticalPrioQue(MAX_EVENTS_PER_QUE, std::make_pair(-1, FusedSafetyEvent())),
    highPrioQue(MAX_EVENTS_PER_QUE, std::make_pair(-1,FusedSafetyEvent())),
    mediumPrioQue(MAX_EVENTS_PER_QUE, std::make_pair(-1, FusedSafetyEvent())),
    lowPrioQue(MAX_EVENTS_PER_QUE, std::make_pair(-1,FusedSafetyEvent())),
    inputSafetyEventQue(MAX_EVENTS_PER_QUE, std::make_pair(-1, SafetyEvent())),
    criticalPrioQuePeriod(criticalPrioQuePeriod), highPrioQuePeriod(highPrioQuePeriod),
    mediumPrioQuePeriod(mediumPrioQuePeriod), lowPrioQuePeriod(lowPrioQuePeriod),
    inputSafetyEventQuePeriod(inputSafetyEventQuePeriod), fusionEventPeriod(fusionEventPeriod), PSSDToPSDComBackend(PSSDToPSDComBackend),
    queMonitorsRunning(false), maxPipelinesSupported(2), registeredPipelines(),
    psdRequestId(0)
{
    psdCtx = nullptr;

    // Clear all the deques before start
    criticalPrioQue.clear();
    highPrioQue.clear();
    mediumPrioQue.clear();
    lowPrioQue.clear();
    inputSafetyEventQue.clear();

    NvPSBWriteData(NVPSB_LOG_INFO, "Instance of NvPSSDaemon-Event Manager is created", "");
}

SafetyEventManager::~SafetyEventManager()
{
    StopSafetyEventManager();
}

void SafetyEventManager::SetRpcForOperationalMode(NvPSSDRPC* rpc)
{
    std::lock_guard<std::mutex> lock(rpcOperationalModeMutex_);
    rpcForOperationalMode_ = rpc;
}

OperationalMode SafetyEventManager::decisionRequestOperationalMode() const
{
    std::lock_guard<std::mutex> lock(rpcOperationalModeMutex_);
    NvPSSDRPC* const rpc = rpcForOperationalMode_;
    if (!rpc)
        return NORMAL;
    return rpc->getSafetyMonitorOperationalMode(
        g_pssMaxHbFailures.load(std::memory_order_relaxed),
        g_pssWarnThreshold.load(std::memory_order_relaxed));
}

SystemStatus SafetyEventManager::makePssStatusForDecisionRequest() const
{
    return {false, false, decisionRequestOperationalMode()};
}

std::deque<std::pair<int, FusedSafetyEvent>>& SafetyEventManager::getCriticalPrioQueRef()
{
    return criticalPrioQue;
}

std::deque<std::pair<int, FusedSafetyEvent>>& SafetyEventManager::getHighPrioQueRef()
{
    return highPrioQue;
}

std::deque<std::pair<int, FusedSafetyEvent>>& SafetyEventManager::getMediumPrioQueRef()
{
    return mediumPrioQue;
}

std::deque<std::pair<int, FusedSafetyEvent>>& SafetyEventManager::getLowPrioQueRef()
{
    return lowPrioQue;
}

std::deque<std::pair<int, SafetyEvent>>& SafetyEventManager::getInputSafetyEventQueRef()
{
    return inputSafetyEventQue;
}

std::mutex& SafetyEventManager::getInputSafetyEventQueMutexRef()
{
    return inputSafetyEventQueMutex;
}

NvPSSDErr SafetyEventManager::StartSafetyEventManager()
{
    NvPSSDErr err = NVPSSD_SUCCESS;

    if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
    {
        /*Initialize the communication channel with PSD here*/
        psdCtx = NvPSDCreateContext();
        if (!psdCtx)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to create NvPSD context", "");
            err = NVPSSD_FAIL;
            goto exit;
        }

        if(NvPSDInitialize(psdCtx, "/pss_to_client", "/client_to_pss", "/pss_to_client_critical",
                           "/client_to_pss_critical", NVPSD_PSS) != NVPSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to initialize NvPSD", "");
            NvPSDDestroyContext(psdCtx);
            err = NVPSSD_FAIL;
            goto exit;
        }

        if(NvPSDStart(psdCtx) != NVPSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to start NvPSD", "");
            NvPSDExit(psdCtx);
            NvPSDDestroyContext(psdCtx);
            err = NVPSSD_FAIL;
            goto exit;
        }
    }
    else if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
    {
        // Initialize and start PSS Daemon Socket
        err = initializePSSDServer();
        if(err != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to initialize PSD server", "");
            goto exit;
        }

        err = startPSSDServer();
        if(err != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR, "Failed to start PSD server", "");
            goto exit;
        }
    }

    queMonitorsRunning = true;
    criticalPrioQueMonitor = std::thread(&SafetyEventManager::manageCriticalPrioQue, this);
    highPrioQueMonitor = std::thread(&SafetyEventManager::manageHighPrioQue, this);
    mediumPrioQueMonitor = std::thread(&SafetyEventManager::manageMediumPrioQue, this);
    lowPrioQueMonitor = std::thread(&SafetyEventManager::manageLowPrioQue, this);
    inputSafetyEventQueMonitor = std::thread(&SafetyEventManager::manageInputSafetyEventQue, this);

exit:
    return err;
}

NvPSSDErr SafetyEventManager::StopSafetyEventManager()
{
    queMonitorsRunning = false;

    if(PSSDToPSDComBackend == POSIX_SOCKET)
    {
        stopPSSDServer();
    }

    if (criticalPrioQueMonitor.joinable()) criticalPrioQueMonitor.join();
    if (highPrioQueMonitor.joinable()) highPrioQueMonitor.join();
    if (mediumPrioQueMonitor.joinable()) mediumPrioQueMonitor.join();
    if (lowPrioQueMonitor.joinable()) lowPrioQueMonitor.join();
    if (inputSafetyEventQueMonitor.joinable()) inputSafetyEventQueMonitor.join();

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::manageCriticalPrioQue()
{
    DecisionRequest psdDecisionRequest{};
    DecisionResponse psdDecisionResponse{};

    if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
    {
        while(queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(criticalPrioQuePeriod));
            std::unique_lock<std::mutex> lock(criticalPrioQueMutex);
            if(criticalPrioQue.empty())
            {
                lock.unlock();
                continue;
            }
            else
            {
                // Log data exit point - sending to smartdoor_psd
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending CRITICAL priority events to PSD Gateway",
                            "Event Type: " + std::to_string(std::get<1>(criticalPrioQue.front()).type) +
                            ", Severity: " + std::to_string(std::get<1>(criticalPrioQue.front()).severity));

                psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                {
                    size_t n = criticalPrioQue.size();
                    if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                        n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                    psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                }
                for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                {
                    const FusedSafetyEvent& ev = std::get<1>(criticalPrioQue.front());
                    psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                    const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                    ev.fusionMetadata.clientID);
                    psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                    psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                    psdDecisionRequest.sensorDataSummary[i].event = ev;
                    criticalPrioQue.pop_front();
                }
                lock.unlock();
                pssDecisionRequestSetCRC(&psdDecisionRequest);
                if(NvPSDProcessDecisionRequest(psdCtx,&psdDecisionRequest,&psdDecisionResponse)
                    != NVPSD_SUCCESS)
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "Failed to report events to PSD", "");
                    /*TODO : This is a serious failure. Devise a strategy to handle this kind
                        of failure*/
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_INFO, "Reported CRITICAL priority event to PSD", "");
                }
            }
        }
    }
    else if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
    {
        /*Busy wait for critical que*/
        while (queMonitorsRunning)
        {
            std::unique_lock<std::mutex> lock(criticalPrioQueMutex);
            if(criticalPrioQue.empty())
            {
                lock.unlock();
                // Sleep instead of busy-wait to reduce CPU usage to near-zero
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            else
            {
                // Get event data BEFORE popping from queue; clientID = semantic AI pipeline id.
                FusedSafetyEvent fusedEvent = std::get<1>(criticalPrioQue.front());
                const uint32_t semanticClientId = static_cast<uint32_t>(fusedEvent.fusionMetadata.clientID);

                // Remove event from queue after extracting data
                criticalPrioQue.pop_front();

                // Release lock before processing/logging
                lock.unlock();

                // Log data exit point - sending to PSD clients via socket
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending CRITICAL priority event to PSD clients via socket",
                              "Event Type: " + std::to_string(fusedEvent.type) +
                              ", Severity: " + std::to_string(fusedEvent.severity) +
                              ", Client ID: " + std::to_string(semanticClientId));

                // Send DecisionRequest via socket (replacing message queue logic)
                if(pssdServer)
                {
                    DecisionRequest psdDecisionRequest{};
                    DecisionResponse psdDecisionResponse{};

                    psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                    psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                    psdDecisionRequest.sensorDataSummarySize = 1;
                    psdDecisionRequest.sensorDataSummary[0].clientID = semanticClientId;
                    psdDecisionRequest.sensorDataSummary[0].event = fusedEvent;
                    const auto ts0 = QueryTrustState(fusedEvent.fusionMetadata.pipelineID,
                                                     fusedEvent.fusionMetadata.clientID);
                    psdDecisionRequest.sensorDataSummary[0].isHealthy = !ts0.sensorInvalid;
                    psdDecisionRequest.sensorDataSummary[0].isTrustedSource = !ts0.aiPipelineInvalid;
                    pssDecisionRequestSetCRC(&psdDecisionRequest);

                    if(pssdServer->sendDecisionRequestToPSD(psdDecisionRequest, &psdDecisionResponse) == NVPSSD_SUCCESS)
                    {
                        NvPSBWriteData(NVPSB_LOG_INFO, "Successfully sent CRITICAL DecisionRequest and received response via socket", "");
                    }
                    else
                    {
                        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to send CRITICAL DecisionRequest via socket", "");
                        /*TODO : This is a serious failure. Devise a strategy to handle this kind of failure*/
                    }
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "PSD server not available for DecisionRequest processing", "");
                }
            }
        }
    }
    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::manageHighPrioQue()
{
    DecisionRequest psdDecisionRequest{};
    DecisionResponse psdDecisionResponse{};

    if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
    {
        while (queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(highPrioQuePeriod));
            std::unique_lock<std::mutex> lock(highPrioQueMutex);
            if(highPrioQue.empty())
            {
                lock.unlock();
                continue;
            }
            else
            {
                // Log data exit point - sending to smartdoor_psd
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending HIGH priority events to PSD Gateway",
                            "Event count: " + std::to_string(highPrioQue.size()));
                psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                {
                    size_t n = highPrioQue.size();
                    if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                        n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                    psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                }
                for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                {
                    const FusedSafetyEvent& ev = std::get<1>(highPrioQue.front());
                    psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                    const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                    ev.fusionMetadata.clientID);
                    psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                    psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                    psdDecisionRequest.sensorDataSummary[i].event = ev;
                    highPrioQue.pop_front();
                }
                lock.unlock();
                pssDecisionRequestSetCRC(&psdDecisionRequest);
                /*Now pass this bundle to PSD*/
                if(NvPSDProcessDecisionRequest(psdCtx,&psdDecisionRequest,&psdDecisionResponse)
                    != NVPSD_SUCCESS)
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "Failed to report events to PSD", "");
                    /*TODO : This is a serious failure. Devise a strategy to handle this kind
                    of failure*/
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_INFO,"Reported HIGH priority events to PSD", "");
                }
            }
        }
    }

    else if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
    {
        while (queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(highPrioQuePeriod));
            std::unique_lock<std::mutex> lock(highPrioQueMutex);
            if(highPrioQue.empty())
            {
                lock.unlock();
                // Sleep instead of busy-wait to reduce CPU usage to near-zero
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            else
            {
                // Log data exit point - sending to PSD clients via socket
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending HIGH priority events to PSD clients via socket",
                              "Event count: " + std::to_string(highPrioQue.size()));

                // Send DecisionRequest with bundled HIGH priority events via socket
                if(pssdServer)
                {
                    DecisionRequest psdDecisionRequest{};
                    DecisionResponse psdDecisionResponse{};

                    psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                    psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                    {
                        size_t n = highPrioQue.size();
                        if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                            n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                        psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                    }
                    for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                    {
                        const FusedSafetyEvent& ev = std::get<1>(highPrioQue.front());
                        psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                        const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                        ev.fusionMetadata.clientID);
                        psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                        psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                        psdDecisionRequest.sensorDataSummary[i].event = ev;
                        highPrioQue.pop_front();
                    }

                    // Release lock before processing/logging
                    lock.unlock();
                    pssDecisionRequestSetCRC(&psdDecisionRequest);

                    if(pssdServer->sendDecisionRequestToPSD(psdDecisionRequest, &psdDecisionResponse) == NVPSSD_SUCCESS)
                    {
                        NvPSBWriteData(NVPSB_LOG_INFO, "Successfully sent HIGH priority DecisionRequest bundle and received response via socket",
                                      "Event count: " + std::to_string(psdDecisionRequest.sensorDataSummarySize));
                    }
                    else
                    {
                        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to send HIGH priority DecisionRequest bundle via socket", "");
                    }
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "PSD server not available for HIGH priority DecisionRequest processing", "");
                    highPrioQue.clear();
                    lock.unlock();
                }

            }
        }
    }
    else
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Backend other than message queue and posix socket is not supported for high priority events", "");
        return NVPSSD_FAIL;
    }

    return NVPSSD_SUCCESS;
}



NvPSSDErr SafetyEventManager::manageMediumPrioQue()
{
    DecisionRequest psdDecisionRequest{};
    DecisionResponse psdDecisionResponse{};

    if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
    {
        while (queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(mediumPrioQuePeriod));
            std::unique_lock<std::mutex> lock(mediumPrioQueMutex);
            if(mediumPrioQue.empty())
            {
                lock.unlock();
                continue;
            }
            else
            {
                // Log data exit point - sending to psd
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending MEDIUM priority events to PSD clients",
                            "Event count: " + std::to_string(mediumPrioQue.size()));
                psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                {
                    size_t n = mediumPrioQue.size();
                    if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                        n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                    psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                }
                for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                {
                    const FusedSafetyEvent& ev = std::get<1>(mediumPrioQue.front());
                    psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                    const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                    ev.fusionMetadata.clientID);
                    psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                    psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                    psdDecisionRequest.sensorDataSummary[i].event = ev;
                    mediumPrioQue.pop_front();
                }

                lock.unlock();
                pssDecisionRequestSetCRC(&psdDecisionRequest);
                /*Now pass this bundle to PSD*/
                if(NvPSDProcessDecisionRequest(psdCtx,&psdDecisionRequest,&psdDecisionResponse)
                    != NVPSD_SUCCESS)
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "Failed to report events to PSD", "");
                    /*TODO : This is a serious failure. Devise a strategy to handle this kind
                    of failure*/
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_INFO,"Reported MEDIUM priority events to PSD", "");
                }
            }
        }
    }

    else if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
    {
        while (queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(mediumPrioQuePeriod));
            std::unique_lock<std::mutex> lock(mediumPrioQueMutex);
            if(mediumPrioQue.empty())
            {
                lock.unlock();
                // Sleep instead of busy-wait to reduce CPU usage to near-zero
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            else
            {
                // Log data exit point - sending to PSD clients via socket
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending MEDIUM priority events to PSD clients via socket",
                              "Event count: " + std::to_string(mediumPrioQue.size()));

                // Send DecisionRequest with bundled MEDIUM priority events via socket
                if(pssdServer)
                {
                    DecisionRequest psdDecisionRequest{};
                    DecisionResponse psdDecisionResponse{};

                    psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                    psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                    {
                        size_t n = mediumPrioQue.size();
                        if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                            n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                        psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                    }
                    for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                    {
                        const FusedSafetyEvent& ev = std::get<1>(mediumPrioQue.front());
                        psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                        const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                        ev.fusionMetadata.clientID);
                        psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                        psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                        psdDecisionRequest.sensorDataSummary[i].event = ev;
                        mediumPrioQue.pop_front();
                    }

                    lock.unlock();
                    pssDecisionRequestSetCRC(&psdDecisionRequest);
                    if(pssdServer->sendDecisionRequestToPSD(psdDecisionRequest, &psdDecisionResponse) == NVPSSD_SUCCESS)
                    {
                        NvPSBWriteData(NVPSB_LOG_INFO, "Successfully sent MEDIUM priority DecisionRequest bundle and received response via socket",
                                      "Event count: " + std::to_string(psdDecisionRequest.sensorDataSummarySize));
                    }
                    else
                    {
                        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to send MEDIUM priority DecisionRequest bundle via socket", "");
                    }
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "PSD server not available for MEDIUM priority DecisionRequest processing", "");
                    mediumPrioQue.clear();
                    lock.unlock();
                }

            }
        }
    }
    else
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Backend other than message queue and posix socket is not supported for medium priority events", "");
        return NVPSSD_FAIL;
    }

    return NVPSSD_SUCCESS;
}


NvPSSDErr SafetyEventManager::manageLowPrioQue()
{
    DecisionRequest psdDecisionRequest{};
    DecisionResponse psdDecisionResponse{};

    if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_MSG_QUE)
    {
        while (queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(lowPrioQuePeriod));
            std::unique_lock<std::mutex> lock(lowPrioQueMutex);
            if(lowPrioQue.empty())
            {
                lock.unlock();
                continue;
            }
            else
            {
                // Log data exit point - sending to PSD Gateway
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending LOW priority events to PSD Gateway",
                            "Event count: " + std::to_string(lowPrioQue.size()));
                psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                {
                    size_t n = lowPrioQue.size();
                    if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                        n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                    psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                }
                for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                {
                    const FusedSafetyEvent& ev = std::get<1>(lowPrioQue.front());
                    psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                    const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                    ev.fusionMetadata.clientID);
                    psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                    psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                    psdDecisionRequest.sensorDataSummary[i].event = ev;
                    lowPrioQue.pop_front();
                }
                lock.unlock();
                pssDecisionRequestSetCRC(&psdDecisionRequest);
                /*Now pass this bundle to PSD*/
                if(NvPSDProcessDecisionRequest(psdCtx,&psdDecisionRequest,&psdDecisionResponse)
                    != NVPSD_SUCCESS)
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "Failed to report events to PSD", "");
                    /*TODO : This is a serious failure. Devise a strategy to handle this kind
                    of failure*/
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_INFO,"Reported LOW priority events to PSD", "");
                }
            }
        }
    }

    else if(PSSDToPSDComBackend == NvPSDChannelBackend::POSIX_SOCKET)
    {
        while (queMonitorsRunning)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(lowPrioQuePeriod));
            std::unique_lock<std::mutex> lock(lowPrioQueMutex);
            if(lowPrioQue.empty())
            {
                lock.unlock();
                // Sleep instead of busy-wait to reduce CPU usage to near-zero
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            else
            {
                // Log data exit point - sending to PSD clients via socket
                NvPSBWriteData(NVPSB_LOG_INFO, "EXIT POINT: Sending LOW priority events to PSD clients via socket",
                              "Event count: " + std::to_string(lowPrioQue.size()));

                // Send DecisionRequest with bundled LOW priority events via socket
                if(pssdServer)
                {
                    DecisionRequest psdDecisionRequest{};
                    DecisionResponse psdDecisionResponse{};

                    psdDecisionRequest.requestId = psdRequestId.fetch_add(1, std::memory_order_relaxed);
                    psdDecisionRequest.pssStatus = makePssStatusForDecisionRequest();
                    {
                        size_t n = lowPrioQue.size();
                        if (n > MAX_SENSORS_DATA_SUMMARY_SIZE)
                            n = MAX_SENSORS_DATA_SUMMARY_SIZE;
                        psdDecisionRequest.sensorDataSummarySize = static_cast<uint8_t>(n);
                    }
                    for (uint8_t i = 0; i < psdDecisionRequest.sensorDataSummarySize; i++)
                    {
                        const FusedSafetyEvent& ev = std::get<1>(lowPrioQue.front());
                        psdDecisionRequest.sensorDataSummary[i].clientID = static_cast<uint32_t>(ev.fusionMetadata.clientID);
                        const auto ts = QueryTrustState(ev.fusionMetadata.pipelineID,
                                                        ev.fusionMetadata.clientID);
                        psdDecisionRequest.sensorDataSummary[i].isHealthy = !ts.sensorInvalid;
                        psdDecisionRequest.sensorDataSummary[i].isTrustedSource = !ts.aiPipelineInvalid;
                        psdDecisionRequest.sensorDataSummary[i].event = ev;
                        lowPrioQue.pop_front();
                    }
                    lock.unlock();
                    pssDecisionRequestSetCRC(&psdDecisionRequest);
                    if(pssdServer->sendDecisionRequestToPSD(psdDecisionRequest, &psdDecisionResponse) == NVPSSD_SUCCESS)
                    {
                        NvPSBWriteData(NVPSB_LOG_INFO, "Successfully sent LOW priority DecisionRequest bundle and received response via socket",
                                      "Event count: " + std::to_string(psdDecisionRequest.sensorDataSummarySize));
                    }
                    else
                    {
                        NvPSBWriteData(NVPSB_LOG_ERR, "Failed to send LOW priority DecisionRequest bundle via socket", "");
                    }
                }
                else
                {
                    NvPSBWriteData(NVPSB_LOG_ERR, "PSD server not available for LOW priority DecisionRequest processing", "");
                    lowPrioQue.clear();
                    lock.unlock();
                }

            }
        }
    }
    else
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Backend other than message queue and posix socket is not supported for low priority events", "");
        return NVPSSD_FAIL;
    }

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::manageInputSafetyEventQue()
{
#ifdef NVPSF_DBG
    struct timespec ts = {};
    uint64_t timestamp_ns;
#endif
    /*Busy wait for input que*/
    while (queMonitorsRunning)
    {

        std::this_thread::sleep_for(std::chrono::microseconds(inputSafetyEventQuePeriod));
        //Lock before checking and accessing queue
        std::unique_lock<std::mutex> lock(inputSafetyEventQueMutex);

        if(inputSafetyEventQue.empty())
        {
            lock.unlock();
            continue;
        }
        else
        {
            if(fusionEnabled)
            {
#ifdef NVPSF_DBG
                clock_gettime(CLOCK_MONOTONIC, &ts);
                timestamp_ns = (ts.tv_nsec + ts.tv_sec*SEC_TO_NANO_SEC);
                std::cout << "Input event timestamp in ns: " << timestamp_ns << std::endl;
#endif
                SafetyEvent eventToProcess = std::get<1>(inputSafetyEventQue.front());
                inputSafetyEventQue.pop_front();

                // Release lock before expensive fusion processing
                lock.unlock();

                /* Events from invalid sensors (pipelineID) or invalid AI pipelines (clientID) are not fused; send to PSD as UNKNOWN with reported severity. */
                const auto trust = QueryTrustState(eventToProcess.fusionMetadata.pipelineID,
                                                   eventToProcess.fusionMetadata.clientID);
                if (trust.sensorInvalid || trust.aiPipelineInvalid)
                {
                    FusedSafetyEvent invalidEvent = CreateInvalidSourceEvent(eventToProcess);
                    /* Use semantic AI pipeline id for PSD attribution, not RPC slot. */
                    const int semanticClientId = static_cast<int>(invalidEvent.fusionMetadata.clientID);
                    switch (invalidEvent.severity)
                    {
                        case CRITICAL:
                            {
                                std::lock_guard<std::mutex> qLock(criticalPrioQueMutex);
                                if (criticalPrioQue.size() < MAX_EVENTS_PER_QUE)
                                    criticalPrioQue.push_back(std::make_pair(semanticClientId, invalidEvent));
                            }
                            break;
                        case HIGH:
                            {
                                std::lock_guard<std::mutex> qLock(highPrioQueMutex);
                                if (highPrioQue.size() < MAX_EVENTS_PER_QUE)
                                    highPrioQue.push_back(std::make_pair(semanticClientId, invalidEvent));
                            }
                            break;
                        case MEDIUM:
                            {
                                std::lock_guard<std::mutex> qLock(mediumPrioQueMutex);
                                if (mediumPrioQue.size() < MAX_EVENTS_PER_QUE)
                                    mediumPrioQue.push_back(std::make_pair(semanticClientId, invalidEvent));
                            }
                            break;
                        case LOW:
                            {
                                std::lock_guard<std::mutex> qLock(lowPrioQueMutex);
                                if (lowPrioQue.size() < MAX_EVENTS_PER_QUE)
                                    lowPrioQue.push_back(std::make_pair(semanticClientId, invalidEvent));
                            }
                            break;
                        default:
                            NvPSBWriteData(NVPSB_LOG_ERR, "Invalid severity level for invalid source event",
                                          "Severity: " + std::to_string(invalidEvent.severity));
                            break;
                    }
                }
                else if(ProcessSafetyEventForFusion(eventToProcess) != NVPSSD_SUCCESS)
                {
                    std::cerr<<"Failed to process reported SafetyEvent\n" << std::endl;
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_ERR,"Failed to process reported SafetyEvent","");
#endif
                    /*TODO : This is a serious failure. Devise a strategy to handle this kind of failure*/
                }
                else
                {
#ifdef NVPSF_DBG
                    NvPSBWriteData(NVPSB_LOG_INFO, "processed reported SafetyEvent","");
#endif
                }
            } else
            {
                lock.unlock();
                // No-op. Severity threads will be filled by input safety events.
            }
        }
    }
    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::EnableFusion(bool enable)
{
    fusionEnabled = enable;

    return NVPSSD_SUCCESS;
}

bool SafetyEventManager::IsFusionEnabled() const
{
    return fusionEnabled;
}

NvPSSDErr SafetyEventManager::ConfigureMultiCameraFusion(uint8_t maxPipelines,
                                                        std::chrono::milliseconds timeWindowSize,
                                                        float fusionThreshold,
                                                        float temporalW,
                                                        float spatialW,
                                                        float attributeW,
                                                        std::chrono::milliseconds temporalT,
                                                        uint8_t trajectoryCount,
                                                        float earlyTermThreshold,
                                                        bool enableEarlyTerm)
{
    NvPSSDErr result = NVPSSD_SUCCESS;
    float weightSum = temporalW + spatialW + attributeW;

    if (!fusionEnabled) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Fusion has not been enabled", "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }

    // Validate maximum pipelines
    if (maxPipelines == 0 || maxPipelines > MAX_SUPPORTED_PIPELINES) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid maxPipelines value: " + std::to_string(maxPipelines), "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }
    maxPipelinesSupported = maxPipelines;
    {
        const auto ms = timeWindowSize.count();
        if (ms < 0) {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "Invalid negative timeWindowSize: " + std::to_string(ms) + " ms", "");
            result = NVPSSD_FAIL;
            goto done;
        }
        stalenessThresholdMs_ = static_cast<uint64_t>(ms);
    }

    // Validate fusion parameters
    if (fusionThreshold < 0.0f || fusionThreshold > 1.0f) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Invalid fusionThreshold: " + std::to_string(fusionThreshold), "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }


    if (std::abs(weightSum - 1.0f) > 0.01f) {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_ERR, "Weight sum should be close to 1.0. Current sum: " + std::to_string(weightSum), "");
#endif
        result = NVPSSD_FAIL;
        goto done;
    }

    if (!eventFusion)
    {
        eventFusion = std::make_unique<SafetyEventFusion>(
            std::chrono::milliseconds(timeWindowSize).count(),
            fusionThreshold,
            temporalW,
            spatialW,
            attributeW,
            std::chrono::milliseconds(temporalT).count(),
            trajectoryCount,
            maxPipelinesSupported,
            earlyTermThreshold,
            enableEarlyTerm);

        NvPSBWriteData(NVPSB_LOG_INFO, "Multi-camera fusion configured with " + std::to_string(maxPipelines) + " pipelines", "");
    }
    else
    {
#ifdef NVPSF_DBG
        NvPSBWriteData(NVPSB_LOG_INFO, "Fusion module already configured. Updating parameters for multi-camera support.", "");
#endif

        eventFusion->SetMaxPipelines(maxPipelines);
        eventFusion->SetTimeWindowSize(std::chrono::milliseconds(timeWindowSize).count());
        eventFusion->SetFusionThreshold(fusionThreshold);
        eventFusion->SetTemporalTolerance(std::chrono::milliseconds(temporalT).count());
        eventFusion->SetFusionWeights(temporalW, spatialW, attributeW);
        eventFusion->SetTrajectoryCount(trajectoryCount);
        eventFusion->EnableEarlyTermination(enableEarlyTerm);
        eventFusion->SetEarlyTerminationThreshold(earlyTermThreshold);
    }

done:
    return result;
}

NvPSSDErr SafetyEventManager::ProcessSafetyEventForFusion(const SafetyEvent& event)
{
    NvPSSDErr result = NVPSSD_SUCCESS;
    uint8_t pipelineId = 0;

    if (!fusionEnabled || !eventFusion)
    {
        result = NVPSSD_FAIL;
        goto done;
    }

    pipelineId = event.fusionMetadata.pipelineID;
    if (pipelineId > maxPipelinesSupported)
    {
        NvPSBWriteData(NVPSB_LOG_ERR, "Event from unregistered sensor: " + std::to_string(pipelineId), "");
        result = NVPSSD_FAIL;
        goto done;
    }

    if (sensorConfigLoaded_ && pipelineId != 0)
    {
        auto cfgIt = sensorConfigIdToName_.find(pipelineId);
        if (cfgIt == sensorConfigIdToName_.end())
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: pipelineID " + std::to_string(pipelineId) + " not in sensor_config", "");
            result = NVPSSD_FAIL;
            goto done;
        }
        /* Treat inbound buffer as untrusted: bound the length to the fixed-size
         * field so we never read past event.sensorIdentifier even if the sender
         * omits the NUL terminator. */
        const size_t sensorIdLen = strnlen(event.sensorIdentifier, MAX_INDENTIFIER_LENGTH);
        std::string sensorId(event.sensorIdentifier, sensorIdLen);
        if (sensorId.empty())
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: sensorIdentifier is empty for pipelineID " +
                std::to_string(pipelineId) + " (expected '" + cfgIt->second + "')", "");
            result = NVPSSD_FAIL;
            goto done;
        }
        if (sensorId != cfgIt->second)
        {
            NvPSBWriteData(NVPSB_LOG_WARNING,
                "Rejecting event: sensorIdentifier '" + sensorId +
                "' does not match sensor_config entry '" + cfgIt->second +
                "' for pipelineID " + std::to_string(pipelineId), "");
            result = NVPSSD_FAIL;
            goto done;
        }
    }

    /* pipelineId == 0 is the well-known "unknown sensor" passthrough channel
     * (MDX emits it for events that cannot be attributed to a configured pipeline).
     * SafetyEventFusion::RegisterPipeline rejects 0, and
     * SafetyEventFusion::ProcessNewSafetyEvent has explicit passthrough handling
     * for 0 — so skip auto-registration here and let the downstream path run. */
    if (pipelineId != 0 &&
        registeredPipelines.find(pipelineId) == registeredPipelines.end())
    {
        NvPSSDErr regRes = eventFusion->RegisterPipeline(pipelineId);
        if (regRes != NVPSSD_SUCCESS)
        {
            NvPSBWriteData(NVPSB_LOG_ERR,
                "Failed to auto-register pipeline: " + std::to_string(pipelineId), "");
            result = NVPSSD_FAIL;
            goto done;
        }
        registeredPipelines[pipelineId] = true;
        NvPSBWriteData(NVPSB_LOG_INFO,
            "Auto-registered pipeline: " + std::to_string(pipelineId), "");
    }

    if (bypassFusionEvents.count(event.type))
    {
        // Directly create a FusedSafetyEvent of status PASSTHROUGH and route to decision
        FusedSafetyEvent fusedEvent = CreateBypassEvent(event);

        // Add fusedEvent to appropriate queue based on severity
        switch (fusedEvent.severity)
        {
            case CRITICAL:
                {
                    std::lock_guard<std::mutex> lock(criticalPrioQueMutex);
                    // Add to critical priority queue
                    if (criticalPrioQue.size() < MAX_EVENTS_PER_QUE)
                    {
                        // Add to critical queue
                        criticalPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                }
                break;

            case HIGH:
                {
                    std::lock_guard<std::mutex> lock(highPrioQueMutex);
                    // Add to high priority queue
                    if (highPrioQue.size() < MAX_EVENTS_PER_QUE)
                    {
                        highPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                }
                break;

            case MEDIUM:
                {
                    std::lock_guard<std::mutex> lock(mediumPrioQueMutex);
                    // Add to medium priority queue
                    if (mediumPrioQue.size() < MAX_EVENTS_PER_QUE)
                    {
                        mediumPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                }
                break;

            case LOW:
                {
                    std::lock_guard<std::mutex> lock(lowPrioQueMutex);
                    // Add to low priority queue
                    if (lowPrioQue.size() < MAX_EVENTS_PER_QUE)
                    {
                        lowPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                }
                break;
        }

        return NVPSSD_SUCCESS;
    }

    result = eventFusion->ProcessNewSafetyEvent(event);

done:
    return result;
}

NvPSSDErr SafetyEventManager::StartFusionProcessing()
{
    fusionProcessorRunning = true;
    fusionProcessorThread = std::thread(&SafetyEventManager::fusionProcessingLoop, this);

    return NVPSSD_SUCCESS;
}

NvPSSDErr SafetyEventManager::StopFusionProcessing()
{
    fusionProcessorRunning = false;

    if (fusionProcessorThread.joinable())
    {
        fusionProcessorThread.join();
    }

    return NVPSSD_SUCCESS;
}

std::vector<FusedSafetyEvent> SafetyEventManager::GetFusedEvents() const
{
    if (!fusionEnabled || !eventFusion)
    {
        return {};
    }

    return eventFusion->GetFusedEvents();
}

NvPSSDErr SafetyEventManager::HandleFusedEvents()
{
#ifdef NVPSF_DBG
    struct timespec ts = {};
    uint64_t timestamp_ns;
#endif
    uint64_t batchNowMs = 0;
    if (!fusionEnabled)
    {
        while (true)
        {
            SafetyEvent event = {};
            {
                std::lock_guard<std::mutex> lock(inputSafetyEventQueMutex);
                if (inputSafetyEventQue.empty())
                    break;
                event = std::get<1>(inputSafetyEventQue.front());
                inputSafetyEventQue.pop_front();
            }

            /* Apply trust enforcement when fusion is disabled: events from invalid sources get UNKNOWN status. */
            const auto trustBypass = QueryTrustState(event.fusionMetadata.pipelineID,
                                                     event.fusionMetadata.clientID);
            FusedSafetyEvent fusedEvent = (trustBypass.sensorInvalid || trustBypass.aiPipelineInvalid)
                ? CreateInvalidSourceEvent(event)
                : CreateBypassEvent(event);

            /* Use semantic client ID (fusionMetadata.clientID) for queue key, consistent with invalid-source and fusion-enabled paths. */
            const int semanticClientId = static_cast<int>(fusedEvent.fusionMetadata.clientID);

            switch (fusedEvent.severity)
            {
                case CRITICAL:
                    {
                        std::lock_guard<std::mutex> lock(criticalPrioQueMutex);
                        if (criticalPrioQue.size() < MAX_EVENTS_PER_QUE)
                            criticalPrioQue.push_back(std::make_pair(semanticClientId, fusedEvent));
                    }
                    break;

                case HIGH:
                    {
                        std::lock_guard<std::mutex> lock(highPrioQueMutex);
                        if (highPrioQue.size() < MAX_EVENTS_PER_QUE)
                            highPrioQue.push_back(std::make_pair(semanticClientId, fusedEvent));
                    }
                    break;

                case MEDIUM:
                    {
                        std::lock_guard<std::mutex> lock(mediumPrioQueMutex);
                        if (mediumPrioQue.size() < MAX_EVENTS_PER_QUE)
                            mediumPrioQue.push_back(std::make_pair(semanticClientId, fusedEvent));
                    }
                    break;

                case LOW:
                    {
                        std::lock_guard<std::mutex> lock(lowPrioQueMutex);
                        if (lowPrioQue.size() < MAX_EVENTS_PER_QUE)
                            lowPrioQue.push_back(std::make_pair(semanticClientId, fusedEvent));
                    }
                    break;
            }
        }


    } else
    {
        // Get fused events
        auto fusedEvents = eventFusion->GetFusedEvents();

        /* Snapshot monotonic time once for the whole batch so we avoid
         * a clock_gettime syscall per event in the loop below. */
        {
            struct timespec tsNow;
            clock_gettime(CLOCK_MONOTONIC, &tsNow);
            batchNowMs = static_cast<uint64_t>(tsNow.tv_sec) * 1000ULL
                       + static_cast<uint64_t>(tsNow.tv_nsec) / 1000000ULL;
        }

        // Process each fused event and add to appropriate priority queue
        for (auto fusedEvent : fusedEvents)
        {
#ifdef NVPSF_DBG
            std::cout << "Number of Event Fused/Pssthrough: " << fusedEvents.size() << std::endl;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            timestamp_ns = (ts.tv_nsec + ts.tv_sec*SEC_TO_NANO_SEC);
            std::cout << "Event Severity queue timestamp in ns: " << timestamp_ns << std::endl;
#endif
            if (isEventStale(fusedEvent.timestamp, batchNowMs))
                fusedEvent.status = STALE;

            switch (fusedEvent.severity)
            {
                case CRITICAL:
                    {
                        std::lock_guard<std::mutex> lock(criticalPrioQueMutex);
                        if (criticalPrioQue.size() < MAX_EVENTS_PER_QUE)
                            criticalPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                    break;

                case HIGH:
                    {
                        std::lock_guard<std::mutex> lock(highPrioQueMutex);
                        if (highPrioQue.size() < MAX_EVENTS_PER_QUE)
                            highPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                    break;

                case MEDIUM:
                    {
                        std::lock_guard<std::mutex> lock(mediumPrioQueMutex);
                        if (mediumPrioQue.size() < MAX_EVENTS_PER_QUE)
                            mediumPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                    break;

                case LOW:
                    {
                        std::lock_guard<std::mutex> lock(lowPrioQueMutex);
                        if (lowPrioQue.size() < MAX_EVENTS_PER_QUE)
                            lowPrioQue.push_back(std::make_pair(fusedEvent.fusionMetadata.clientID, fusedEvent));
                    }
                    break;
            }
        }

        eventFusion->ClearFusedEvents(fusedEvents.size());
    }

    return NVPSSD_SUCCESS;
}

void SafetyEventManager::fusionProcessingLoop()
{
    while(fusionProcessorRunning)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(fusionEventPeriod));

        if(eventFusion->PerformSafetyEventFusion() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to perform safety event fusion","");
#endif
        }

        if(eventFusion->ProcessUnmatchedEvents() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to process unmatched safety events","");
#endif
        }

        if(eventFusion->CleanProcessedEvents() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to clean processed safety events","");
#endif
        }

        if(HandleFusedEvents() != NVPSSD_SUCCESS)
        {
#ifdef NVPSF_DBG
            NvPSBWriteData(NVPSB_LOG_ERR,"Failed to process fused safety events","");
#endif
        }
    }
}

bool SafetyEventManager::isEventStale(uint64_t eventTimestampNs,
                                      uint64_t nowMs) const
{
    const uint64_t eventMs = eventTimestampNs / 1000000ULL;

    /* Reject timestamps that are unreasonably far in the future.
     * A corrupted or malicious value could sit in the queue
     * indefinitely; treat it as stale so it gets discarded. */
    if (eventMs > nowMs && (eventMs - nowMs > stalenessThresholdMs_))
        return true;

    return (nowMs > eventMs) &&
           (nowMs - eventMs > stalenessThresholdMs_);
}

bool SafetyEventManager::isEventStale(uint64_t eventTimestampNs) const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t nowMs = static_cast<uint64_t>(ts.tv_sec) * 1000ULL
                         + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    return isEventStale(eventTimestampNs, nowMs);
}

FusedSafetyEvent SafetyEventManager::CreateBypassEvent(const SafetyEvent& event) const
{
    //Pass through SafetyEvent to Fused SafetyEvent
    FusedSafetyEvent fusedEvent;

    // Generate a unique ID
    fusedEvent.id = static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count());

    // Copy properties directly from the source event
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    strncpy(fusedEvent.ruleIdentifier, event.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    fusedEvent.type = event.type;
    fusedEvent.timestamp = event.timestamp;
    fusedEvent.confidenceLevel = event.confidenceLevel;
    fusedEvent.severity = event.severity;
    std::memcpy(&fusedEvent.fusionMetadata, &event.fusionMetadata, sizeof(EventFusionMetadata));

    fusedEvent.status = isEventStale(event.timestamp) ? STALE : UNKNOWN;

    return fusedEvent;
}

void SafetyEventManager::SetBypassFusionEvents(const std::unordered_set<EventType>& set)
{
    bypassFusionEvents = set;
}

void SafetyEventManager::SetSensorConfig(const std::unordered_map<uint8_t, std::string>& pipelineIdToName)
{
    sensorConfigIdToName_ = pipelineIdToName;
    sensorConfigLoaded_ = !sensorConfigIdToName_.empty();
}

bool SafetyEventManager::OnTrustReport(uint32_t rpcClientId, uint8_t reporterClientType, const SafetyEvent& event)
{
    /* Only Safety Monitor may send SENSOR_* / AI_PIPELINE_* VALID/INVALID events (enforced at RPC; reject here as defense in depth). */
    if (reporterClientType != CLIENT_SAFETY_MONITOR)
    {
        NvPSBWriteData(NVPSB_LOG_WARNING, "Trust report rejected: only Safety Monitor may send VALID/INVALID events", "");
        return false;
    }

    (void)rpcClientId;

    std::lock_guard<std::mutex> lock(trustStateMutex);
    switch (event.type)
    {
        case SENSOR_INVALID:
            invalidSensors.insert(event.fusionMetadata.pipelineID);  /* pipelineID = sensor */
            break;
        case SENSOR_VALID:
            invalidSensors.erase(event.fusionMetadata.pipelineID);
            break;
        case AI_PIPELINE_INVALID:
            invalidAIPipelines.insert(event.fusionMetadata.clientID);  /* clientID = target AI pipeline */
            break;
        case AI_PIPELINE_VALID:
            invalidAIPipelines.erase(event.fusionMetadata.clientID);
            break;
        default:
            break;
    }
    return true;
}

SafetyEventManager::TrustState SafetyEventManager::QueryTrustState(
    uint8_t pipelineId, uint8_t clientId) const
{
    std::lock_guard<std::mutex> lock(trustStateMutex);
    return { invalidSensors.count(pipelineId) != 0,
             invalidAIPipelines.count(clientId) != 0 };
}

FusedSafetyEvent SafetyEventManager::CreateInvalidSourceEvent(const SafetyEvent& event) const
{
    FusedSafetyEvent fusedEvent;
    fusedEvent.id = static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count());
    strncpy(fusedEvent.sensorIdentifier, event.sensorIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.sensorIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    strncpy(fusedEvent.ruleIdentifier, event.ruleIdentifier, MAX_INDENTIFIER_LENGTH - 1);
    fusedEvent.ruleIdentifier[MAX_INDENTIFIER_LENGTH - 1] = '\0';
    fusedEvent.type = event.type;
    fusedEvent.timestamp = event.timestamp;
    fusedEvent.confidenceLevel = event.confidenceLevel;
    fusedEvent.severity = event.severity;  /* Use reported severity, not forced CRITICAL */
    fusedEvent.status = isEventStale(event.timestamp) ? STALE : UNKNOWN;
    std::memcpy(&fusedEvent.fusionMetadata, &event.fusionMetadata, sizeof(EventFusionMetadata));
    return fusedEvent;
}
NvPSSDErr SafetyEventManager::initializePSSDServer()
{
    pssdServer = std::make_unique<NvPSSDToPSDClient>();
    return pssdServer->initializePSSDServer();
}

NvPSSDErr SafetyEventManager::startPSSDServer()
{
    if(pssdServer)
    {
        return pssdServer->startPSSDServer();
    }
    return NVPSSD_FAIL;
}

NvPSSDErr SafetyEventManager::stopPSSDServer()
{
    if(pssdServer)
    {
        return pssdServer->stopPSSDServer();
    }
    return NVPSSD_SUCCESS;
}

}
