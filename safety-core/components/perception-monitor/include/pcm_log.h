/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCM_LOG_H
#define PCM_LOG_H

#include <sstream>
#include <string>

/*
 * PCM log lines use the same format as the ATL/PXC SDM logs:
 *   [YYYY-MM-DD HH:MM:SS.mmm] [PCM][INFO|WARNING|ERR] message
 * INFO goes to stdout, WARNING and ERR to stderr (the launchers collect both
 * into pcm.log). Messages printed by the Kafka and PSS client libraries keep
 * their own format.
 */
enum class PcmLogLevel
{
    Info,
    Warning,
    Err
};

/* Write one whole log line. Thread-safe: lines from different PCM threads never
 * interleave, and every line is flushed so pcm.log stays in time order. */
void pcmLog(PcmLogLevel level, const std::string& message);

/*
 * Collects one message with operator<< and writes it as a single line when the
 * statement ends, keeping iostream number formatting:
 *   PcmLogLine(PcmLogLevel::Warning) << "Docker socket unreachable";
 */
class PcmLogLine
{
public:
    explicit PcmLogLine(PcmLogLevel level) : level_(level) {}
    ~PcmLogLine();

    PcmLogLine(const PcmLogLine&) = delete;
    PcmLogLine& operator=(const PcmLogLine&) = delete;
    PcmLogLine(PcmLogLine&&) = delete;
    PcmLogLine& operator=(PcmLogLine&&) = delete;

    template <typename T>
    PcmLogLine& operator<<(const T& value)
    {
        stream_ << value;
        return *this;
    }

private:
    PcmLogLevel        level_;
    std::ostringstream stream_;
};

#endif /* PCM_LOG_H */
