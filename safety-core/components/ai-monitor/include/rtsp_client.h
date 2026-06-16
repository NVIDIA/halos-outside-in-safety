/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTSP_CLIENT_H
#define RTSP_CLIENT_H

#include <vector>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>

#include "sai_common.h"

constexpr size_t RTSP_BUFFER_SIZE      = 65536;  // Max bytes per TCP/UDP recv call.
constexpr int    RTSP_RECV_TIMEOUT_SEC = 60;     // Seconds of silence before the RTP receiver gives up.
constexpr size_t MAX_RTSP_HEADER = 32u * 1024;   // 32 KB MAX header size for RTSP 
constexpr size_t MAX_RTSP_BODY = 64u * 1024;            // 64 KB MAX Body size for RTSP
constexpr int MAX_RTSP_CONNECT_RETRIES = 5; // Maximum number of retries for RTSP/RTP connection.

// H.264 Annex-B 4-byte start code prepended to every NAL unit.
extern const unsigned char NAL_START_CODE[4];

// A single H.264 NAL unit with its capture timestamp (microseconds).
struct NalUnit {
    std::vector<unsigned char> data;  // Includes the 4-byte Annex-B start code prefix.
    int64_t timestamp = 0;
    uint32_t rtpTimestamp = 0;        // 90kHz RTP media clock; 0 for SDP-sourced NALs.
};

// Thread-safe bounded producer/consumer queue for NAL units.
// Blocks the producer when full and the consumer when empty.
class NalQueue {
public:
    explicit NalQueue(size_t max_size = 120);

    void push(NalUnit unit);
    bool pop(NalUnit& out);
    void markFinished();

private:
    std::queue<NalUnit> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    size_t max_size_;
    bool finished_ = false;
};

/*
 * Minimal RTSP/RTP client that negotiates an H.264 unicast UDP session,
 * de-packetizes RTP into H.264 NAL units, and pushes them into a NalQueue
 * for the decoder thread to consume.
 */
class RTSPClient {
public:
    RTSPClient(const std::string& url, NalQueue* queue, std::atomic<bool>* stopFlag);
    ~RTSPClient();

    bool connectToServer();           // Opens TCP connection to the RTSP server.
    bool setupRTSPSession();          // Runs DESCRIBE, SETUP, PLAY; binds UDP sockets.
    void receiveLoop();               // Blocking RTP receive loop; runs until stop or timeout.
    void requestStop();               // Signals receiveLoop to exit (thread-safe, idempotent).

    void setFuaDropAlertCallback(std::function<void(uint32_t)> cb, uint32_t threshold = 5);

    /* Human-readable label used in per-stream diagnostic logs (e.g. the
     * periodic FU-A drop-cause histogram). Defaults to "rtsp" if not set;
     * callers typically pass the sensor name. Safe to set before receiveLoop. */
    void setStreamLabel(const std::string& label) { streamLabel_ = label; }

private:
    int sockfd_ = -1;                 // TCP socket for RTSP signaling.
    NalQueue* nalQueue_;
    std::atomic<bool>* stopFlag_;
    std::atomic<bool> localStop_{false};
    std::string rtspUrl_;
    std::string serverIp_;
    int serverPort_ = 554;            // Default RTSP port.
    std::string sessionId_;
    int cseq_ = 0;                    // RTSP CSeq counter, incremented per request.

    int rtpSockfd_  = -1;             // UDP socket for incoming RTP media packets.
    int rtcpSockfd_ = -1;             // UDP socket for RTCP (control).
    int clientRtpPort_  = 0;
    int clientRtcpPort_ = 0;
    int serverRtpPort_  = 0;
    int serverRtcpPort_ = 0;

    std::vector<unsigned char> streamBuf_;  // Leftover bytes from TCP recv that belong to the next RTSP response.

    std::mutex fuaMutex_;                   // Guards all FU-A reassembly state below.
    std::vector<unsigned char> fuaBuf_;     // Reassembly buffer for FU-A fragmented NAL units.
    bool fuaInProgress_ = false;
    uint16_t fuaNextSeq_ = 0;              // Expected RTP sequence number for next FU-A fragment.
    static constexpr size_t MAX_FUA_SIZE = 4u << 20;

    uint32_t fuaDropCount_ = 0;
    uint32_t fuaDropThreshold_ = 5;
    std::function<void(uint32_t)> onFuaDropAlert_;
    void handleFuaDrop(const std::string& reason);

    /* Per-cause FU-A drop counters for operator-facing diagnostics.
     * Updated from the RTSP receive thread only (same thread as
     * handleFuaDrop callers), so plain uint32_t is sufficient. The periodic
     * histogram in receiveLoop uses these to attribute drops to the actual
     * failure mode (lost start vs. lost end vs. seq gap vs. size limit). */
    uint32_t fuaDropInterrupted_ = 0;
    uint32_t fuaDropContinuation_ = 0;
    uint32_t fuaDropSeqDiscontinuity_ = 0;
    uint32_t fuaDropSizeLimit_ = 0;
    uint32_t fuaDropSsrcChange_ = 0;
    uint32_t fuaDropShutdown_ = 0;
    uint32_t fuaDropOther_ = 0;
    std::string streamLabel_ = "rtsp";
    std::chrono::steady_clock::time_point lastFuaHistogramLog_{};
    uint32_t fuaDropSnapshotAtLastLog_ = 0;

    int sessionTimeoutSec_ = 60;     // Server-advertised session timeout; keep-alive is sent at 1/3 of this.
    std::chrono::steady_clock::time_point lastKeepAlive_;
    std::chrono::steady_clock::time_point lastDataReceived_;

    // Loop counter used by receiveLoop() to run housekeeping every ~256
    // packets instead of every packet. Receive-thread only; uint32_t wrap
    // is harmless (the & 0xFF gate is unaffected).
    uint32_t rxIterSinceHk_ = 0;

    uint32_t expectedSSRC_ = 0;      // SSRC of the RTP source, locked on first valid packet.
    bool ssrcLocked_ = false;
    uint32_t ssrcMismatchCount_ = 0; // Consecutive packets with wrong SSRC.
    static constexpr uint32_t SSRC_MISMATCH_LIMIT = 200;
    int expectedPayloadType_ = -1;   // Dynamic PT from SDP a=rtpmap; -1 = not yet known.
    uint32_t currentRtpTimestamp_ = 0;
    bool rtcpBye_ = false;
    uint32_t unknownNalCount_ = 0;

    void parseRTSPUrl(const std::string& url);
    bool sendRequest(const std::string& request);
    std::string receiveRtspResponse();
    int getResponseCode(const std::string& response);
    std::string extractSessionId(const std::string& response);
    bool createUdpSockets();
    void extractServerPorts(const std::string& response);
    std::string extractTrackUrl(const std::string& sdp, size_t videoPos, size_t videoEnd);
    void extractSpropParams(const std::string& sdp);
    void extractPayloadType(const std::string& sdp, size_t videoPos, size_t videoEnd);
    void sendHolePunch();
    void pushNalUnit(const unsigned char* data, size_t len);
    void processRtpPayload(const unsigned char* payload, size_t len, uint16_t seq);
    void maybeSendKeepAlive();
    void checkRtcpSocket();
    void drainSignalingSocket();
    void sendTeardown();
};

#endif
