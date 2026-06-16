/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rtsp_client.h"

#include <iostream>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

const unsigned char NAL_START_CODE[] = {0x00, 0x00, 0x00, 0x01};

NalQueue::NalQueue(size_t max_size) : max_size_(max_size) {}

void NalQueue::push(NalUnit unit) {
    NVTX_RANGE("NalQueue::push", 0xFFAA00AA);
    std::unique_lock<std::mutex> lock(mtx_);
    cv_not_full_.wait(lock, [&] { return queue_.size() < max_size_ || finished_; });
    if (finished_) return;
    queue_.push(std::move(unit));
    lock.unlock();
    cv_not_empty_.notify_one();
}

bool NalQueue::pop(NalUnit& out) {
    NVTX_RANGE("NalQueue::pop", 0xFFAA44AA);
    std::unique_lock<std::mutex> lock(mtx_);
    cv_not_empty_.wait(lock, [&] { return !queue_.empty() || finished_; });
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    cv_not_full_.notify_one();
    return true;
}

void NalQueue::markFinished() {
    std::lock_guard<std::mutex> lock(mtx_);
    finished_ = true;
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
}

RTSPClient::RTSPClient(const std::string& url, NalQueue* queue,
                       std::atomic<bool>* stopFlag)
    : nalQueue_(queue), stopFlag_(stopFlag)
{
    if (!nalQueue_)
        throw std::runtime_error("RTSPClient: nalQueue must not be null");
    if (!stopFlag_)
        throw std::runtime_error("RTSPClient: stopFlag must not be null");
    rtspUrl_ = url;
    parseRTSPUrl(url);
}

RTSPClient::~RTSPClient() {
    try { sendTeardown(); } catch (...) {}
    if (sockfd_ >= 0) ::close(sockfd_);
    if (rtpSockfd_ >= 0) ::close(rtpSockfd_);
    if (rtcpSockfd_ >= 0) ::close(rtcpSockfd_);
}

// Extracts serverIp and serverPort from an "rtsp://host[:port]/..." URL.
void RTSPClient::parseRTSPUrl(const std::string& url) {
    size_t start = url.find("://");
    if (start == std::string::npos)
        throw std::runtime_error("Invalid RTSP URL: missing ://");
    start += 3;

    size_t slashPos = url.find('/', start);
    if (slashPos == std::string::npos) slashPos = url.size();

    std::string hostPort = url.substr(start, slashPos - start);
    size_t colonPos = hostPort.find(':');
    if (colonPos != std::string::npos) {
        serverIp_ = hostPort.substr(0, colonPos);
        std::string portStr = hostPort.substr(colonPos + 1);
        int port;
        if (!safe_stoi(portStr, port))
            throw std::runtime_error("Invalid RTSP URL: bad port '" + portStr + "'");
        if (port < 1 || port > 65535)
            throw std::runtime_error("Invalid RTSP URL: port " + portStr + " out of range 1-65535");
        serverPort_ = port;
    } else {
        serverIp_ = hostPort;
        serverPort_ = 554;
    }
}

bool RTSPClient::connectToServer() {
    NVTX_RANGE("RTSP::Connect", 0xFF2288FF);
    if (sockfd_ >= 0) { ::close(sockfd_); sockfd_ = -1; }
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) { std::cerr << "Failed to create socket\n"; return false; }

    struct timeval tv;
    tv.tv_sec  = 10;
    tv.tv_usec = 0;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        std::cerr << "Failed to set RTSP socket receive timeout\n";
        ::close(sockfd_); sockfd_ = -1; return false;
    }
    if (setsockopt(sockfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        std::cerr << "Failed to set RTSP socket send timeout\n";
        ::close(sockfd_); sockfd_ = -1; return false;
    }

    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string portStr = std::to_string(serverPort_);
    int gai_err = getaddrinfo(serverIp_.c_str(), portStr.c_str(), &hints, &result);
    if (gai_err != 0) {
        std::cerr << "Failed to resolve '" << serverIp_ << "': "
                  << gai_strerror(gai_err) << "\n";
        ::close(sockfd_); sockfd_ = -1; return false;
    }

    bool connected = false;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        if (::connect(sockfd_, rp->ai_addr, rp->ai_addrlen) == 0) {
            connected = true;
            break;
        }
        // A failed connect() leaves the TCP socket in an error state on Linux.
        // Recreate it so the next address can be tried cleanly.
        ::close(sockfd_);
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) break;
        if (setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
            setsockopt(sockfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
            ::close(sockfd_); sockfd_ = -1;
            break;
        }
    }
    freeaddrinfo(result);

    if (!connected) {
        std::cerr << "Connection to " << serverIp_ << ":" << serverPort_ << " failed\n";
        if (sockfd_ >= 0) { ::close(sockfd_); sockfd_ = -1; }
        return false;
    }

#ifdef DEBUG
    std::cout << "Connected to " << serverIp_ << ":" << serverPort_ << "\n";
#endif
    return true;
}

bool RTSPClient::sendRequest(const std::string& request) {
    size_t totalSent = 0;
    while (totalSent < request.size()) {
        ssize_t sent = send(sockfd_, request.c_str() + totalSent,
                            request.size() - totalSent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (sent == 0) return false;
        totalSent += sent;
    }
    return true;
}

// Reads a complete RTSP response (headers + Content-Length body) from the TCP socket.
// Any excess bytes beyond the response are saved in streamBuf for the next call.
std::string RTSPClient::receiveRtspResponse() {
    std::string response;

    if (!streamBuf_.empty()) {
        response.assign(streamBuf_.begin(), streamBuf_.end());
        streamBuf_.clear();
    }

    char buf[RTSP_BUFFER_SIZE];
    while (true) {
        // Strip interleaved binary frames (RFC 2326: $ + channel + 2-byte len + data).
        // Some servers embed RTP/RTCP on the TCP channel even with UDP transport.
        size_t skip = 0;
        while (skip + 4 <= response.size() && response[skip] == '$') {
            uint16_t flen = ((unsigned char)response[skip + 2] << 8) |
                            (unsigned char)response[skip + 3];
            size_t total = 4 + (size_t)flen;
            if (skip + total > response.size()) break;
            skip += total;
        }
        if (skip > 0) response.erase(0, skip);
        if (!response.empty() && response[0] != '$' &&
            response.find("\r\n\r\n") != std::string::npos)
            break;
        if (response.size() > MAX_RTSP_HEADER) {
            std::cerr << "RTSP header exceeds " << MAX_RTSP_HEADER
                      << " bytes, aborting\n";
            return "";
        }
        ssize_t n = recv(sockfd_, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            std::cerr << "RTSP connection lost before complete response headers\n";
            return "";
        }
        response.append(buf, n);
    }

    size_t headerEnd = response.find("\r\n\r\n") + 4;
    size_t contentLength = 0;

    size_t clPos = ci_find(response, "content-length:");
    if (clPos != std::string::npos && clPos < headerEnd) {
        size_t valStart = response.find_first_not_of(" \t", clPos + 15);
        if (valStart != std::string::npos && valStart < headerEnd) {
            size_t valEnd = response.find_first_of("\r\n", valStart);
            if (valEnd == std::string::npos) valEnd = response.size();
            if (!safe_stoul(response.substr(valStart, valEnd - valStart), contentLength))
                contentLength = 0;
        }
        if (contentLength > MAX_RTSP_BODY) {
            std::cerr << "RTSP Content-Length " << contentLength
                      << " exceeds " << MAX_RTSP_BODY
                      << " byte limit, rejecting response\n";
            return "";
        }
    }

    size_t totalNeeded = headerEnd + contentLength;

    while (response.size() < totalNeeded) {
        ssize_t n = recv(sockfd_, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        response.append(buf, n);
    }

    if (contentLength > 0 && response.size() < totalNeeded) {
        std::cerr << "RTSP connection lost before complete response body ("
                  << response.size() - headerEnd << "/" << contentLength << " bytes)\n";
        return "";
    }

    // Save any bytes beyond this response for the next receiveRtspResponse() call.
    if (response.size() > totalNeeded) {
        const char* extra = response.data() + totalNeeded;
        size_t extraLen = response.size() - totalNeeded;
        streamBuf_.assign(reinterpret_cast<const unsigned char*>(extra),
                          reinterpret_cast<const unsigned char*>(extra) + extraLen);
        response.resize(totalNeeded);
    }

    return response;
}

int RTSPClient::getResponseCode(const std::string& response) {
    size_t spacePos = response.find(' ');
    if (spacePos == std::string::npos) return -1;
    size_t codeEnd = response.find(' ', spacePos + 1);
    if (codeEnd == std::string::npos) return -1;
    int code;
    if (!safe_stoi(response.substr(spacePos + 1, codeEnd - spacePos - 1), code))
        return -1;
    return code;
}

std::string RTSPClient::extractSessionId(const std::string& response) {
    size_t pos = ci_find(response, "session:");
    if (pos == std::string::npos) return "";
    size_t start = response.find_first_not_of(" \t", pos + 8);
    if (start == std::string::npos) return "";
    size_t lineEnd = response.find_first_of("\r\n", start);
    std::string sessionLine = response.substr(start, lineEnd - start);

    size_t semicolon = sessionLine.find(';');
    if (semicolon != std::string::npos) {
        std::string params = sessionLine.substr(semicolon + 1);
        size_t tPos = ci_find(params, "timeout=");
        if (tPos != std::string::npos) {
            if(safe_stoi(params.substr(tPos + 8), sessionTimeoutSec_) == false) {
                std::cerr << "Failed to parse session timeout from RTSP response\n";
                sessionTimeoutSec_ = 60;
            }
            else {
                if(sessionTimeoutSec_ < 5)
                    sessionTimeoutSec_ = 5;
            }
        }
        return sessionLine.substr(0, semicolon);
    }
    return sessionLine;
}

// Raise the RTP socket's kernel receive buffer. A large rcvbuf is the single
// most important mitigation for FU-A fragment loss in bursty 4K H.264 streams:
// while the decode thread is busy, the kernel has to hold queued UDP datagrams,
// and the default rmem (typically 208 KB) overflows in milliseconds at
// multi-megabit rates. Try increasingly modest targets, and if an unprivileged
// SO_RCVBUF is silently clipped by net.core.rmem_max, fall back to
// SO_RCVBUFFORCE (requires CAP_NET_ADMIN) before giving up. The applied value
// is read back via getsockopt so operators can see whether they actually got
// what they asked for. Called from createUdpSockets() so the buffer is in place
// before SETUP/PLAY and any packets start flowing.
static void tuneRtpRecvBuf(int fd)
{
    static const int kTargets[] = {
        16 * 1024 * 1024,
        8 * 1024 * 1024,
        4 * 1024 * 1024,
        2 * 1024 * 1024,
    };
    int applied = 0;
    for (int target : kTargets) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &target, sizeof(target)) == 0) {
            applied = target;
            break;
        }
    }
#ifdef SO_RCVBUFFORCE
    if (applied < kTargets[0]) {
        int force = kTargets[0];
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &force, sizeof(force)) == 0)
            applied = force;
    }
#endif
    int actual = 0;
    socklen_t slen = sizeof(actual);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &slen) == 0) {
        // Linux reports 2 * the value the kernel actually allocated.
        if (actual < (2 * 1024 * 1024)) {
            std::cerr << "Warning: RTP SO_RCVBUF kernel-allocated=" << actual
                      << " bytes (requested=" << applied
                      << "). net.core.rmem_max may be too low — FU-A fragment loss"
                         " under bursty input is likely. Consider: sysctl -w"
                         " net.core.rmem_max=16777216\n";
        }
    } else if (applied == 0) {
        std::cerr << "Warning: failed to tune RTP SO_RCVBUF; FU-A drops may increase"
                     " under load\n";
    }
}

// Finds and binds a consecutive even/odd UDP port pair for RTP/RTCP reception.
// Scans from port 6970 upward until a free pair is found.
bool RTSPClient::createUdpSockets() {
    if (rtpSockfd_ >= 0)  { ::close(rtpSockfd_);  rtpSockfd_  = -1; }
    if (rtcpSockfd_ >= 0) { ::close(rtcpSockfd_); rtcpSockfd_ = -1; }
    for (int port = 6970; port < 65534; port += 2) {
        rtpSockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtpSockfd_ < 0) return false;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(rtpSockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(rtpSockfd_); rtpSockfd_ = -1; continue;
        }

        rtcpSockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtcpSockfd_ < 0) { ::close(rtpSockfd_); rtpSockfd_ = -1; return false; }
        addr.sin_port = htons(port + 1);

        if (bind(rtcpSockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(rtpSockfd_); ::close(rtcpSockfd_);
            rtpSockfd_ = -1; rtcpSockfd_ = -1; continue;
        }

        clientRtpPort_ = port;
        clientRtcpPort_ = port + 1;

        /* Apply the large rcvbuf here, before PLAY, so we never lose the
         * very first RTP burst while the decode thread is still warming up. */
        tuneRtpRecvBuf(rtpSockfd_);

#ifdef DEBUG
        std::cout << "UDP sockets bound to ports "
                  << clientRtpPort_ << "-" << clientRtcpPort_ << "\n";
#endif
        return true;
    }
    std::cerr << "Failed to find available UDP port pair\n";
    return false;
}

void RTSPClient::extractServerPorts(const std::string& response) {
    size_t pos = ci_find(response, "server_port=");
    if (pos == std::string::npos) return;
    size_t valStart = pos + 12;
    size_t valEnd = response.find_first_of("; \r\n", valStart);
    std::string ports = response.substr(valStart, valEnd - valStart);
    size_t dash = ports.find('-');
    if (dash != std::string::npos) {
        if (!safe_stoi(ports.substr(0, dash), serverRtpPort_) ||
            !safe_stoi(ports.substr(dash + 1), serverRtcpPort_)) {
            std::cerr << "Failed to parse server_port from RTSP response\n";
            serverRtpPort_  = 0;
            serverRtcpPort_ = 0;
        }
    } else {
        if (!safe_stoi(ports, serverRtpPort_)) {
            std::cerr << "Failed to parse server_port from RTSP response\n";
            serverRtpPort_  = 0;
            serverRtcpPort_ = 0;
        } else {
            serverRtcpPort_ = serverRtpPort_ + 1;
        }
    }
    if (serverRtpPort_ < 1 || serverRtpPort_ > 65535 ||
        serverRtcpPort_ < 1 || serverRtcpPort_ > 65535) {
        std::cerr << "server_port out of valid range (1-65535): RTP="
                  << serverRtpPort_ << " RTCP=" << serverRtcpPort_ << "\n";
        serverRtpPort_  = 0;
        serverRtcpPort_ = 0;
    }
#ifdef DEBUG
    std::cout << "Server RTP port: " << serverRtpPort_
              << ", RTCP port: " << serverRtcpPort_ << "\n";
#endif
}

// Parses the SDP body to find the video track's "a=control:" URL for SETUP.
// videoPos/videoEnd are the pre-computed boundaries of the "m=video" section;
// videoPos == npos means no video section was found (fallback: any non-"*" control).
std::string RTSPClient::extractTrackUrl(const std::string& sdp,
                                        size_t videoPos, size_t videoEnd) {
    if (videoPos == std::string::npos) {
        size_t ctrlPos = sdp.find("a=control:");
        while (ctrlPos != std::string::npos) {
            size_t valStart = ctrlPos + 10;
            size_t valEnd = sdp.find_first_of("\r\n", valStart);
            std::string ctrl = sdp.substr(valStart, valEnd - valStart);
            if (ctrl != "*") return ctrl;
            ctrlPos = sdp.find("a=control:", valEnd);
        }
        return "";
    }
    size_t ctrlPos = sdp.find("a=control:", videoPos);
    if (ctrlPos == std::string::npos || ctrlPos >= videoEnd) return "";
    size_t valStart = ctrlPos + 10;
    size_t valEnd = sdp.find_first_of("\r\n", valStart);
    return sdp.substr(valStart, valEnd - valStart);
}

// Extracts base64-encoded SPS/PPS NAL units from the SDP "sprop-parameter-sets"
// attribute and pushes them into the NAL queue so the decoder is initialized
// before the first IDR frame arrives.
void RTSPClient::extractSpropParams(const std::string& sdp) {
    size_t pos = sdp.find("sprop-parameter-sets=");
    if (pos == std::string::npos) return;

    size_t valStart = pos + 21;
    size_t valEnd = sdp.find_first_of("; \r\n", valStart);
    std::string sprop = sdp.substr(valStart, valEnd - valStart);

    size_t start = 0;
    int count = 0;
    while (start < sprop.size()) {
        size_t comma = sprop.find(',', start);
        std::string b64 = (comma == std::string::npos)
            ? sprop.substr(start)
            : sprop.substr(start, comma - start);
        if (!b64.empty()) {
            auto nalData = base64Decode(b64);
            if (!nalData.empty()) {
                pushNalUnit(nalData.data(), nalData.size());
                count++;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
#ifdef DEBUG
    if (count > 0)
        std::cout << "Extracted " << count << " SPS/PPS NAL units from SDP\n";
#endif
}

// Parses "a=rtpmap:" lines within the video media section of the SDP to find
// the dynamic payload type number for H.264. Logs a warning if no H264 mapping
// is found; sets expectedPayloadType_ on success.
void RTSPClient::extractPayloadType(const std::string& sdp,
                                    size_t videoPos, size_t videoEnd) {
    if (videoPos == std::string::npos) return;

    size_t pos = videoPos;
    while (pos < videoEnd) {
        size_t rtpmapPos = sdp.find("a=rtpmap:", pos);
        if (rtpmapPos == std::string::npos || rtpmapPos >= videoEnd) break;

        size_t ptStart = rtpmapPos + 9;
        size_t spacePos = sdp.find(' ', ptStart);
        if (spacePos == std::string::npos || spacePos >= videoEnd) break;

        size_t lineEnd = sdp.find_first_of("\r\n", spacePos);
        if (lineEnd == std::string::npos) lineEnd = sdp.size();
        std::string codec = sdp.substr(spacePos + 1, lineEnd - spacePos - 1);

        if (ci_find(codec, "h264") != std::string::npos) {
            int pt;
            if (safe_stoi(sdp.substr(ptStart, spacePos - ptStart), pt) &&
                pt >= 0 && pt <= 127) {
                expectedPayloadType_ = pt;
#ifdef DEBUG
                std::cout << "SDP: H264 payload type = " << pt << "\n";
#endif
                return;
            }
            else {
                std::cerr << "Failed to parse payload type from SDP video section\n";
                expectedPayloadType_ = -1;
                return;
            }
        }
        pos = lineEnd;
    }

    std::cerr << "Warning: no H264 rtpmap found in SDP video section\n";
}

// Performs the full RTSP handshake: DESCRIBE -> SETUP -> PLAY, then sends
// a UDP hole-punch so the server can reach the client's RTP port through NAT.
// Returns true if the session is ready for receiveLoop().
bool RTSPClient::setupRTSPSession() {
    NVTX_RANGE("RTSP::SetupSession", 0xFF4488FF);
    std::string descReq = "DESCRIBE " + rtspUrl_ + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(++cseq_) + "\r\n"
        "User-Agent: RTSPClient\r\n"
        "Accept: application/sdp\r\n\r\n";

    std::string descResp;
    {
        NVTX_RANGE("RTSP::DESCRIBE", 0xFF1166FF);
        if (!sendRequest(descReq)) return false;
        descResp = receiveRtspResponse();
    }
#ifdef DEBUG
    std::cout << "DESCRIBE Response:\n" << descResp << "\n";
#endif

    int code = getResponseCode(descResp);
    if (code != 200) {
        std::cerr << "DESCRIBE failed with code " << code << "\n";
        return false;
    }

    size_t sdpStart = descResp.find("\r\n\r\n");
    std::string sdpBody = (sdpStart != std::string::npos) ? descResp.substr(sdpStart + 4) : "";

    // Content-Base overrides the request URL for resolving
    // relative a=control: URLs in the SDP (common with proxies/load balancers).
    std::string baseUrl = rtspUrl_;
    size_t cbPos = ci_find(descResp, "content-base:");
    if (cbPos != std::string::npos && sdpStart != std::string::npos && cbPos < sdpStart) {
        size_t valStart = descResp.find_first_not_of(" \t", cbPos + 13);
        if (valStart != std::string::npos && valStart < sdpStart) {
            size_t valEnd = descResp.find_first_of("\r\n", valStart);
            if (valEnd != std::string::npos)
                baseUrl = descResp.substr(valStart, valEnd - valStart);
        }
    }

    extractSpropParams(sdpBody);

    // Bound SDP parsing to the first m=video section so we don't accidentally
    // pick up attributes (a=control:, a=rtpmap:) from m=audio or other sections.
    size_t videoPos = sdpBody.find("m=video");
    size_t videoEnd = sdpBody.size();
    if (videoPos != std::string::npos) {
        size_t nextMedia = sdpBody.find("\nm=", videoPos + 1);
        if (nextMedia != std::string::npos) videoEnd = nextMedia;
    }
    extractPayloadType(sdpBody, videoPos, videoEnd);
    if(expectedPayloadType_ == -1) {
        std::cerr << "No H264 payload type found in SDP video section\n";
        return false;
    }

    // Resolve the SDP a=control: URL against baseUrl (RFC 3986 reference resolution):
    //   empty       → use baseUrl as-is (no track-level control)
    //   rtsp://...  → absolute URL, use directly
    //   /path       → absolute path, combine with baseUrl's scheme://authority
    //   trackID=1   → relative, append to baseUrl
    std::string trackUrl = extractTrackUrl(sdpBody, videoPos, videoEnd);
    std::string setupUrl;
    if (trackUrl.empty()) {
        setupUrl = baseUrl;
    } else if (ci_find(trackUrl, "rtsp://") == 0) {
        setupUrl = trackUrl;
    } else if (trackUrl[0] == '/') {
        size_t authEnd = baseUrl.find('/', baseUrl.find("://") + 3);
        setupUrl = (authEnd != std::string::npos ? baseUrl.substr(0, authEnd) : baseUrl)
                   + trackUrl;
    } else {
        setupUrl = baseUrl;
        if (!setupUrl.empty() && setupUrl.back() != '/')
            setupUrl += '/';
        setupUrl += trackUrl;
    }

    if (!createUdpSockets()) { std::cerr << "Failed to create UDP sockets\n"; return false; }
    std::string setupReq = "SETUP " + setupUrl + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(++cseq_) + "\r\n"
        "User-Agent: RTSPClient\r\n"
        "Transport: RTP/AVP;unicast;client_port=" +
        std::to_string(clientRtpPort_) + "-" +
        std::to_string(clientRtcpPort_) + "\r\n\r\n";

    std::string setupResp;
    {
        NVTX_RANGE("RTSP::SETUP", 0xFF3388FF);
        if (!sendRequest(setupReq)) return false;
        setupResp = receiveRtspResponse();
    }
#ifdef DEBUG
    std::cout << "SETUP Response:\n" << setupResp << "\n";
#endif

    code = getResponseCode(setupResp);
    if (code != 200) { std::cerr << "SETUP failed with code " << code << "\n"; return false; }

    extractServerPorts(setupResp);

    sessionId_ = extractSessionId(setupResp);
    if (sessionId_.empty()) { std::cerr << "No session ID in SETUP response\n"; return false; }

    std::string playReq = "PLAY " + rtspUrl_ + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(++cseq_) + "\r\n"
        "User-Agent: RTSPClient\r\n"
        "Session: " + sessionId_ + "\r\n"
        "Range: npt=0.000-\r\n\r\n";

    std::string playResp;
    {
        NVTX_RANGE("RTSP::PLAY", 0xFF55AAFF);
        if (!sendRequest(playReq)) return false;
        playResp = receiveRtspResponse();
    }
#ifdef DEBUG
    std::cout << "PLAY Response:\n" << playResp << "\n";
#endif

    code = getResponseCode(playResp);
    if (code != 200) { std::cerr << "PLAY failed with code " << code << "\n"; return false; }

    // connect() on UDP sockets makes the kernel reject packets from any source
    // other than the server's IP:port — prevents spoofed/injected RTP and RTCP.
    if (serverRtpPort_ > 0) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        std::string rtpPortStr = std::to_string(serverRtpPort_);
        int gai_err = getaddrinfo(serverIp_.c_str(), rtpPortStr.c_str(),
                                  &hints, &result);
        if (gai_err == 0 && result != nullptr) {
            if (::connect(rtpSockfd_, result->ai_addr, result->ai_addrlen) < 0)
                std::cerr << "Warning: failed to connect RTP socket to server"
                             " (source filtering disabled)\n";
            if (serverRtcpPort_ > 0 && rtcpSockfd_ >= 0) {
                struct sockaddr_in rtcpAddr;
                memcpy(&rtcpAddr, result->ai_addr, sizeof(rtcpAddr));
                rtcpAddr.sin_port = htons(serverRtcpPort_);
                if (::connect(rtcpSockfd_, reinterpret_cast<struct sockaddr*>(&rtcpAddr),
                              sizeof(rtcpAddr)) < 0)
                    std::cerr << "Warning: failed to connect RTCP socket to server\n";
            }
            freeaddrinfo(result);
        }
    }

    sendHolePunch();

    return true;
}

// Sends a minimal RTP-like packet to the server's RTP/RTCP ports so that
// NAT/firewall mappings are created for the return media traffic.
// Called after connect() on the UDP sockets, so send() (not sendto()) is used.
void RTSPClient::sendHolePunch() {
    NVTX_RANGE("RTSP::HolePunch", 0xFF77BBFF);
    unsigned char punch[4] = {0x80, 0x00, 0x00, 0x00};

    if (rtpSockfd_ >= 0) {
        if (send(rtpSockfd_, punch, sizeof(punch), 0) < 0)
            std::cerr << "Hole-punch send RTP failed: " << strerror(errno) << "\n";
    }
    if (rtcpSockfd_ >= 0) {
        if (send(rtcpSockfd_, punch, sizeof(punch), 0) < 0)
            std::cerr << "Hole-punch send RTCP failed: " << strerror(errno) << "\n";
    }
#ifdef DEBUG
    std::cout << "Sent UDP hole-punch to server ports "
              << serverRtpPort_ << "/" << serverRtcpPort_ << "\n";
#endif
}

// Wraps raw NAL bytes with a 4-byte Annex-B start code and enqueues them.
void RTSPClient::pushNalUnit(const unsigned char* data, size_t len) {
    NVTX_RANGE("RTSP::PushNAL", 0xFF6644CC);
    if (len == 0) return;
    NalUnit nal;
    nal.data.reserve(4 + len);
    nal.data.insert(nal.data.end(), NAL_START_CODE, NAL_START_CODE + 4);
    nal.data.insert(nal.data.end(), data, data + len);
    nal.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    nal.rtpTimestamp = currentRtpTimestamp_;
    nalQueue_->push(std::move(nal));
}

void RTSPClient::setFuaDropAlertCallback(std::function<void(uint32_t)> cb,
                                         uint32_t threshold) {
    onFuaDropAlert_ = std::move(cb);
    fuaDropThreshold_ = (threshold > 0) ? threshold : 5;
}

void RTSPClient::handleFuaDrop(const std::string& reason) {
    fuaDropCount_++;

    /* Attribute each drop to one of the known causes so the periodic
     * histogram can point at the actual failure mode. Matching is done on a
     * leading substring because some reasons are parameterized (e.g. the
     * "seq discontinuity (expected X, got Y)" variant). Unknown reasons fall
     * into fuaDropOther_ rather than being silently aggregated. */
    if (reason.rfind("interrupted by new start fragment", 0) == 0) {
        fuaDropInterrupted_++;
    } else if (reason.rfind("continuation without start fragment", 0) == 0) {
        fuaDropContinuation_++;
    } else if (reason.rfind("seq discontinuity", 0) == 0) {
        fuaDropSeqDiscontinuity_++;
    } else if (reason.rfind("start fragment exceeds size limit", 0) == 0 ||
               reason.rfind("size limit exceeded", 0) == 0) {
        fuaDropSizeLimit_++;
    } else if (reason.rfind("SSRC change during reassembly", 0) == 0) {
        fuaDropSsrcChange_++;
    } else if (reason.rfind("shutdown with incomplete reassembly", 0) == 0) {
        fuaDropShutdown_++;
    } else {
        fuaDropOther_++;
    }

    std::cerr << "[" << streamLabel_ << "] FU-A: " << reason
              << " (total drops: " << fuaDropCount_ << ")\n";
    fuaBuf_.clear();
    fuaInProgress_ = false;
    if (onFuaDropAlert_ && (fuaDropCount_ % fuaDropThreshold_ == 0))
        onFuaDropAlert_(fuaDropCount_);
}

/*
 * Handles the three RTP payload formats for H.264 (RFC 6184):
 *   - Single NAL unit (type 1-23): payload is one complete NAL.
 *   - STAP-A (type 24): multiple NALs aggregated in one RTP packet.
 *   - FU-A  (type 28): a single NAL fragmented across multiple RTP packets.
 */
void RTSPClient::processRtpPayload(const unsigned char* payload, size_t len,
                                   uint16_t seq) {
    NVTX_RANGE("RTSP::ProcessRTP", 0xFF8866DD);
    if (len < 1) return;
    std::lock_guard<std::mutex> fuaLock(fuaMutex_);

    unsigned char nalType = payload[0] & 0x1F;

    if (nalType >= 1 && nalType <= 23) {
        pushNalUnit(payload, len);
    }
    else if (nalType == 24) {
        size_t offset = 1;

        while (offset + 2 <= len) {
            uint16_t nalSize = (payload[offset] << 8) | payload[offset + 1];
            offset += 2;
            if (nalSize == 0 || offset + nalSize > len) break;
            pushNalUnit(payload + offset, nalSize);
            offset += nalSize;
        }
    } else if (nalType == 28) {
        if (len < 2) return;
        unsigned char fuIndicator = payload[0];
        unsigned char fuHeader = payload[1];
        bool startBit = (fuHeader & 0x80) != 0;
        bool endBit   = (fuHeader & 0x40) != 0;
        unsigned char originalType = fuHeader & 0x1F;

        if (startBit) {
            if (fuaInProgress_)
                handleFuaDrop("interrupted by new start fragment");

            // Reconstruct the original NAL header: NRI bits from the FU indicator,
            // NAL type from the FU header. Then append the fragment data (skip 2-byte FU header).
            fuaBuf_.clear();
            fuaBuf_.push_back((fuIndicator & 0xE0) | originalType);
            if (len - 2 > MAX_FUA_SIZE - fuaBuf_.size()) {
                handleFuaDrop("start fragment exceeds size limit");
                return;
            }
            fuaBuf_.insert(fuaBuf_.end(), payload + 2, payload + len);

            if (endBit) {
                pushNalUnit(fuaBuf_.data(), fuaBuf_.size());
                fuaBuf_.clear();
                fuaInProgress_ = false;
            } else {
                fuaInProgress_ = true;
                fuaNextSeq_ = seq + 1;
            }
        } else {
            if (!fuaInProgress_) {
                handleFuaDrop("continuation without start fragment");
                return;
            }

            if (seq != fuaNextSeq_) {
                handleFuaDrop("seq discontinuity (expected "
                    + std::to_string(fuaNextSeq_) + ", got "
                    + std::to_string(seq) + ")");
                return;
            }
            fuaNextSeq_ = seq + 1;

            if (len - 2 > MAX_FUA_SIZE - fuaBuf_.size()) {
                handleFuaDrop("size limit exceeded");
                return;
            }

            fuaBuf_.insert(fuaBuf_.end(), payload + 2, payload + len);

            if (endBit) {
                pushNalUnit(fuaBuf_.data(), fuaBuf_.size());
                fuaBuf_.clear();
                fuaInProgress_ = false;
            }
        }
    } else {
        if (++unknownNalCount_ <= 5 || (unknownNalCount_ % 1000 == 0))
            std::cerr << "RTP: unsupported NAL type " << (int)nalType
                      << " (total: " << unknownNalCount_ << ")\n";
    }
}

// Non-blocking drain of any pending data on the TCP signaling socket
// (e.g., stale keep-alive responses) before sending the next request.
void RTSPClient::drainSignalingSocket() {
    char drain[1024];
    while (true) {
        ssize_t n = recv(sockfd_, drain, sizeof(drain), MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
    }
}

// Non-blocking read of the RTCP socket. Walks RTCP compound packets
// (multiple sub-packets per UDP datagram) looking for BYE (PT=203),
// which indicates the server has stopped the stream.
void RTSPClient::checkRtcpSocket() {
    if (rtcpSockfd_ < 0) return;
    unsigned char buf[512];
    for (;;) {
        ssize_t n = recv(rtcpSockfd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        size_t offset = 0;
        // Each RTCP sub-packet: 4-byte header (V/P/RC, PT, length in 32-bit words - 1).
        while (offset + 4 <= (size_t)n) {
            if ((buf[offset] >> 6) != 2) break;
            uint8_t pt = buf[offset + 1];
            uint16_t len = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
            size_t remaining = (size_t)n - offset;
            size_t pktLen = ((size_t)len + 1) * 4;
            if (pktLen > remaining) break;
            if (pt == 203) {
                std::cerr << "RTCP BYE received from server\n";
                rtcpBye_ = true;
                return;
            }
            offset += pktLen;
        }
    }
}

void RTSPClient::maybeSendKeepAlive() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastKeepAlive_).count();

    // Send keep-alive at 1/3 of the session timeout to stay well within the window.
    if (elapsed >= (sessionTimeoutSec_ >= 3 ? sessionTimeoutSec_ / 3 : sessionTimeoutSec_)) {
        drainSignalingSocket();
        NVTX_MARK("RTSP::KeepAlive");
        std::string req = "GET_PARAMETER " + rtspUrl_ + " RTSP/1.0\r\n"
            "CSeq: " + std::to_string(++cseq_) + "\r\n"
            "Session: " + sessionId_ + "\r\n\r\n";
        if (!sendRequest(req))
            std::cerr << "RTSP keep-alive send failed, control channel may be broken\n";
        lastKeepAlive_ = now;
    }
}

// Main RTP receive loop. Reads UDP packets, strips the RTP header (including
// CSRC, extension, and padding), and dispatches the payload to processRtpPayload().
// Periodically sends RTSP keep-alives. Exits on stopFlag, timeout, or socket error.
void RTSPClient::receiveLoop() {
    NVTX_RANGE("RTSP::ReceiveLoop", 0xFF0066FF);
#ifdef DEBUG
    int packetCount = 0;
    size_t totalBytes = 0;
#endif

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(rtpSockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        std::cerr << "Failed to set RTP socket receive timeout\n";
        return;
    }

    /* SO_RCVBUF is tuned earlier in createUdpSockets() to be in place before
     * the server starts pushing packets; setting it here would be too late to
     * absorb the initial burst. Intentionally not re-applied here. */

    auto now = std::chrono::steady_clock::now();
    lastKeepAlive_ = now;
    lastDataReceived_ = now;
    lastFuaHistogramLog_ = now;
    fuaDropSnapshotAtLastLog_ = 0;

#ifdef DEBUG
    std::cout << "Receiving H264 stream (UDP, keep-alive every "
              << (sessionTimeoutSec_ >= 3 ? sessionTimeoutSec_ / 3 : sessionTimeoutSec_) << "s)...\n";
#endif
    unsigned char buf[RTSP_BUFFER_SIZE];

    /* Emit a per-stream FU-A drop histogram every 30s when there has been
     * recent activity. This lets operators see *which* stream is degraded and
     * *why* (lost-start vs lost-end vs seq-gap), without having to grep every
     * drop line. Ticked from the RTSP receive thread so there's no extra
     * thread or lock; counters are only updated from the same thread. */
    constexpr std::chrono::seconds kFuaHistogramInterval{30};
    auto maybeLogFuaHistogram = [&]() {
        auto tnow = std::chrono::steady_clock::now();
        if (tnow - lastFuaHistogramLog_ < kFuaHistogramInterval)
            return;
        lastFuaHistogramLog_ = tnow;
        if (fuaDropCount_ == fuaDropSnapshotAtLastLog_)
            return;  // silent period — nothing worth printing
        const uint32_t deltaDrops = fuaDropCount_ - fuaDropSnapshotAtLastLog_;
        fuaDropSnapshotAtLastLog_ = fuaDropCount_;
        std::cerr << "[" << streamLabel_ << "] FU-A histogram"
                  << " window=30s deltaDrops=" << deltaDrops
                  << " total=" << fuaDropCount_
                  << " | interrupted=" << fuaDropInterrupted_
                  << " continuation=" << fuaDropContinuation_
                  << " seqDisc=" << fuaDropSeqDiscontinuity_
                  << " sizeLimit=" << fuaDropSizeLimit_
                  << " ssrcChange=" << fuaDropSsrcChange_
                  << " shutdown=" << fuaDropShutdown_
                  << " other=" << fuaDropOther_
                  << "\n";
    };

    // All per-iteration housekeeping. Each call uses syscalls/clock reads,
    // and each callee is internally rate-limited to seconds, so calling
    // them per packet is pure overhead (~9 kHz at 3 kpps).
    auto runHousekeeping = [&]() {
        maybeSendKeepAlive();
        checkRtcpSocket();
        maybeLogFuaHistogram();
    };

    rxIterSinceHk_ = 0;  // First iteration always runs housekeeping.

    while (!stopFlag_->load() && !localStop_.load()) {
        // Run housekeeping every 256 packets (~85 ms at 3 kpps). Well
        // below keep-alive deadline (~1.7 s minimum) and histogram window
        // (30 s); BYE detection slows from per-packet to ~85 ms.
        if ((rxIterSinceHk_++ & 0xFF) == 0) {
            runHousekeeping();
            if (rtcpBye_) break;
        }

        ssize_t n = recv(rtpSockfd_, buf, sizeof(buf), 0);
        if (n > 0) {
            // Same 256-iteration cadence as housekeeping. Idle timeout is
            // 60 s, so ~85 ms staleness is irrelevant.
            if ((rxIterSinceHk_ & 0xFF) == 1)
                lastDataReceived_ = std::chrono::steady_clock::now();
        } else if (n == 0) {
            continue;  // 0-byte UDP datagram, not a connection close.
        } else {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                // SO_RCVTIMEO fires every 2 s of silence. The packet-rate
                // counter cannot advance during silence, so tick housekeeping
                // here so keep-alives still get sent.
                runHousekeeping();
                if (rtcpBye_) break;
                auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - lastDataReceived_).count();
                if (idle > RTSP_RECV_TIMEOUT_SEC) break;
                continue;
            }
            break;
        }

        if (n < 12) continue;  // Minimum RTP header is 12 bytes.

        const unsigned char* rtp = buf;
        size_t rtpLen = (size_t)n;

        if ((rtp[0] >> 6) != 2) continue;  // RFC 3550: version must be 2.

        uint8_t pt = rtp[1] & 0x7F;
        if (expectedPayloadType_ >= 0 && pt != expectedPayloadType_) continue;

        uint32_t ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) |
                        ((uint32_t)rtp[10] << 8) | rtp[11];
        if (!ssrcLocked_) {
            expectedSSRC_ = ssrc;
            ssrcLocked_ = true;
        } else if (ssrc != expectedSSRC_) {
            // Tolerate brief SSRC glitches but accept a new source after 200
            // consecutive mismatches (likely server restart, not a rogue sender).
            if (++ssrcMismatchCount_ >= SSRC_MISMATCH_LIMIT) {
                std::cerr << "RTP SSRC changed from 0x" << std::hex << expectedSSRC_
                          << " to 0x" << ssrc << std::dec
                          << " (server restart?), resetting stream state\n";
                expectedSSRC_ = ssrc;
                ssrcMismatchCount_ = 0;
                { std::lock_guard<std::mutex> fuaLock(fuaMutex_);
                  if (fuaInProgress_)
                      handleFuaDrop("SSRC change during reassembly");
                }
            } else {
                continue;
            }
        } else {
            ssrcMismatchCount_ = 0;
        }

        size_t rtpHeaderLen = 12;  // Fixed part: V/P/X/CC, M/PT, SeqNo, Timestamp, SSRC.

        unsigned char cc = rtp[0] & 0x0F;  // 4-bit field, max 15 → max 60 bytes.
        size_t csrcLen = (size_t)cc * 4;
        if (csrcLen > rtpLen - 12) continue;
        rtpHeaderLen += csrcLen;

        bool hasExtension = (rtp[0] & 0x10) != 0;
        if (hasExtension) {
            if (rtpHeaderLen + 4 > rtpLen) continue;  // Truncated extension header.
            uint16_t extLen = (rtp[rtpHeaderLen + 2] << 8) | rtp[rtpHeaderLen + 3];
            rtpHeaderLen += 4 + extLen * 4;
            if (rtpHeaderLen > rtpLen) continue;
        }

        // If padding bit is set, the last byte of the packet
        // contains the count of padding bytes (including itself) to strip.
        bool hasPadding = (rtp[0] & 0x20) != 0;
        size_t payloadEnd = rtpLen;
        if (hasPadding && rtpLen > 0) {
            unsigned char paddingLen = rtp[rtpLen - 1];
            if (paddingLen <= rtpLen && paddingLen > 0)
                payloadEnd = rtpLen - paddingLen;
        }

        if (rtpHeaderLen < payloadEnd) {
            uint16_t seq = (rtp[2] << 8) | rtp[3];
            currentRtpTimestamp_ = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) |
                                   ((uint32_t)rtp[6] << 8) | rtp[7];
            const unsigned char* payload = rtp + rtpHeaderLen;
            size_t payloadLen = payloadEnd - rtpHeaderLen;
            processRtpPayload(payload, payloadLen, seq);
#ifdef DEBUG
            totalBytes += payloadLen;
#endif
        }

#ifdef DEBUG
        packetCount++;
        if (packetCount % 500 == 0) {
            std::cout << "Received " << packetCount << " RTP packets ("
                      << (totalBytes / 1024) << " KB)\n";
                    }
#endif
    }

    { std::lock_guard<std::mutex> fuaLock(fuaMutex_);
      if (fuaInProgress_)
          handleFuaDrop("shutdown with incomplete reassembly");
    }

#ifdef DEBUG
    std::cout << "RTSP receiver stopped. Total RTP packets: " << packetCount
              << ", Total H264 data: " << (totalBytes / 1024) << " KB\n";
#endif
}

void RTSPClient::requestStop() {
    localStop_.store(true);
}

void RTSPClient::sendTeardown() {
    if (sockfd_ < 0 || sessionId_.empty()) return;
    std::string req = "TEARDOWN " + rtspUrl_ + " RTSP/1.0\r\n"
        "CSeq: " + std::to_string(++cseq_) + "\r\n"
        "Session: " + sessionId_ + "\r\n\r\n";
    if (!sendRequest(req))
        std::cerr << "RTSP TEARDOWN send failed\n";
    sessionId_.clear();
}
