/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <syslog.h>
#include <ctime>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <iomanip>
#include <sstream>

#include "NvPSB.h"

NvPSBEndpoint mPSBEndpoint;
bool mInitialized = false;

const std::string getEndpointName(NvPSBEndpoint endpoint);

const std::string getEndpointName(NvPSBEndpoint endpoint)
{
        std::string endpointName = "INVALID";

        switch (endpoint)
        {
        case NVPSB_PSS_SOURCE:
                endpointName = "NVPSB_PSS_SOURCE"; break;
        case NVPSB_PSS_SINK:
                endpointName = "NVPSB_PSS_SINK"; break;
        case NVPSB_PSS_DAEMON:
                endpointName = "NVPSB_PSS_DAEMON"; break;
        case NVPSB_PSD_CLIENT:
                endpointName = "NVPSB_PSD_CLIENT "; break;
        case NVPSB_SDM_CLIENT:
                endpointName = "NVPSB_SDM_CLIENT"; break;
        default:
                break;
        };

        return endpointName;
}

NvPSBErr NvPSBInitialize(const char* ident, NvPSBEndpoint endpoint)
{

    NvPSBErr err = NVPSB_SUCCESS;
    if(ident == NULL)
    {
       err = NVPSB_FAIL;
       goto exit;
    }
    /**
      * LOG_PID: Include PID with each message.
      * LOG_CONS: Write directly to system console if there is an error while sending to system logger.
      * LOG_NDELAY: Open the connection immediately
      * LOG_USER: generic user-level messages
    */
    openlog (ident, LOG_PID | LOG_CONS | LOG_NDELAY, LOG_USER);

    mPSBEndpoint = endpoint;
    mInitialized = true;
exit:
    return err;
}

NvPSBErr NvPSBWriteData(NvPSBLogLevel level, const std::string data, const std::string additionalInfo)
{
        /* Highest prioorty loggings level sare reserved for PSS Daemon.
         * Lower the priority level for other endpoints */
        if(level == NVPSB_LOG_EMERG || level == NVPSB_LOG_ALERT) {
                if(mPSBEndpoint != NVPSB_PSS_DAEMON) {
                        std::cerr<<"NVPSB_LOG_EMERG and NVPSB_LOG_ALERT are reserved for NVPSB_PSS_DAEMON only\n";
                        std::cerr<<"Lowering Log level to NVPSB_LOG_CRIT\n";
                        level = NVPSB_LOG_CRIT;
                }
        }

        // Get current time with microseconds precision using the same method as SmartDoorCtrlAlgo
        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        auto timeMicro = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count() % 1000000;
        std::tm* timeinfo = std::localtime(&timeT);

        // Format timestamp with full date and time plus microseconds
        std::ostringstream timestampStream;
        if (timeinfo != nullptr)
        {
            timestampStream << std::setfill('0')
                           << (timeinfo->tm_year + 1900) << "-"
                           << std::setw(2) << (timeinfo->tm_mon + 1) << "-"
                           << std::setw(2) << timeinfo->tm_mday << " "
                           << std::setw(2) << timeinfo->tm_hour << ":"
                           << std::setw(2) << timeinfo->tm_min << ":"
                           << std::setw(2) << timeinfo->tm_sec << ":"
                           << std::setw(6) << timeMicro;
        }
        else
        {
            timestampStream << "0000-00-00 00:00:00:000000";
        }

        std::string timestamp = timestampStream.str();

        std::string dataLog = " Timestamp: " + timestamp + " Endpoint: " +  getEndpointName(mPSBEndpoint) +
                       " Data: " +  std::string(data) + std::string(additionalInfo);

        syslog(level, "%s", dataLog.c_str());
#ifdef NVPSF_DBG
        printf("%s\n", dataLog.c_str());
#endif
        if (fflush(stdout) != 0)
        {
            // This is to address CERT POS54_C violation
        }

        return NVPSB_SUCCESS;
}

NvPSBErr NvPSBExit()
{
        closelog();
        return NVPSB_SUCCESS;
}