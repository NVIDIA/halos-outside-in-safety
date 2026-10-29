/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcm_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <time.h>

namespace {

/* Function-local so it is constructed before first use from any thread. */
std::mutex& logMutex()
{
    static std::mutex mutex;
    return mutex;
}

const char* levelName(PcmLogLevel level)
{
    switch (level) {
        case PcmLogLevel::Info:    return "INFO";
        case PcmLogLevel::Warning: return "WARNING";
        case PcmLogLevel::Err:     return "ERR";
    }
    return "ERR";
}

} // namespace

void pcmLog(PcmLogLevel level, const std::string& message)
{
    /* Serialize timestamp capture with the write so threads cannot reorder it. */
    std::lock_guard<std::mutex> lock(logMutex());
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch()).count() % 1000;
    if (millis < 0) millis = 0;

    /* Local time, like the SDM logs, so PCM and SDM lines can be compared directly. */
    char stamp[48];
    struct tm local;
    if (localtime_r(&seconds, &local) != nullptr) {
        (void)std::snprintf(stamp, sizeof(stamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
                            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                            local.tm_hour, local.tm_min, local.tm_sec,
                            static_cast<int>(millis));
    } else {
        (void)std::snprintf(stamp, sizeof(stamp), "[1970-01-01 00:00:00.000]");
    }

    FILE* const out = (level == PcmLogLevel::Info) ? stdout : stderr;
    /* A failed log write has nowhere to be reported; it must not stop monitoring. */
    (void)std::fprintf(out, "%s [PCM][%s] %s\n", stamp, levelName(level), message.c_str());
    (void)std::fflush(out);
}

PcmLogLine::~PcmLogLine()
{
    try {
        pcmLog(level_, stream_.str());
    } catch (...) {
        /* Logging must never terminate PCM from a destructor. */
    }
}
