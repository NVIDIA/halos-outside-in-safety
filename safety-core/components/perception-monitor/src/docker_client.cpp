/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "docker_client.h"

#include "json_extract.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

static constexpr const char* kDockerSocketPath = "/var/run/docker.sock";

static constexpr int kConnectTimeoutMs = 2000;
static constexpr int kReadTimeoutMs    = 5000;

/* Cap response body size to bound memory against a misbehaving daemon. */
static constexpr std::size_t kMaxResponseBytes = 8U * 1024U * 1024U;

static constexpr std::size_t kReadChunk = 4096U;

/* readSome: >0 bytes, 0 EOF, -1 error, -2 timeout. The timeout case lets
 * streaming consumers service a stop flag between idle reads. */
static constexpr ssize_t kReadTimeout = -2;

/* Safe charset for splicing a container name/id into the HTTP request line. */
static bool isSafeNameChar(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '-' || c == '_' || c == '.') return true;
    return false;
}

static bool isSafeName(const std::string& name) {
    if (name.empty() || name.size() > 256U) return false;
    for (const char c : name) {
        if (!isSafeNameChar(c)) return false;
    }
    return true;
}

/* poll() one fd with timeout. Returns 1 ready, 0 timeout, -1 error. */
static int waitFd(int fd, short events, int timeoutMs) {
    while (true) {
        struct pollfd pfd;
        pfd.fd      = fd;
        pfd.events  = events;
        pfd.revents = 0;
        const int rc = poll(&pfd, 1, timeoutMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return rc;
    }
}

/* Connect a non-blocking AF_UNIX socket to dockerd within @p timeoutMs.
 * Returns the fd, or -1 with errno set. */
static int openDockerSocket(int timeoutMs) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const std::size_t plen = std::strlen(kDockerSocketPath);
    if (plen >= sizeof(addr.sun_path)) {
        ::close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    std::memcpy(addr.sun_path, kDockerSocketPath, plen + 1U);

    const int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) return fd;
    if (errno != EINPROGRESS) {
        const int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }

    const int ready = waitFd(fd, POLLOUT, timeoutMs);
    if (ready <= 0) {
        ::close(fd);
        errno = (ready == 0) ? ETIMEDOUT : EIO;
        return -1;
    }
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
        ::close(fd);
        errno = (err != 0) ? err : EIO;
        return -1;
    }
    return fd;
}

/* Write all @p len bytes, handling short writes and EAGAIN. */
static bool writeAll(int fd, const char* data, std::size_t len, int timeoutMs) {
    std::size_t off = 0U;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n > 0) { off += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int rc = waitFd(fd, POLLOUT, timeoutMs);
            if (rc <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

static ssize_t readSome(int fd, char* buf, std::size_t cap, int timeoutMs) {
    while (true) {
        const ssize_t n = ::read(fd, buf, cap);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const int rc = waitFd(fd, POLLIN, timeoutMs);
            if (rc < 0) return -1;
            if (rc == 0) return kReadTimeout;
            continue;
        }
        return -1;
    }
}

/* Send a minimal HTTP/1.0 GET. HTTP/1.0 + Connection: close avoids
 * keep-alive and keeps the per-call lifecycle simple. */
static bool sendHttpGet(int fd, const std::string& path, int timeoutMs) {
    std::string req;
    req.reserve(64U + path.size());
    req.append("GET ");
    req.append(path);
    req.append(" HTTP/1.0\r\nHost: localhost\r\nAccept: application/json\r\nConnection: close\r\n\r\n");
    return writeAll(fd, req.data(), req.size(), timeoutMs);
}

struct HttpResponse {
    int status = 0;
    bool chunked = false;
    long long contentLength = -1;
    /* Body bytes already read while parsing headers (they share a read). */
    std::string carryover;
};

/* Read up to the "\r\n\r\n" terminator and parse status/headers. Leftover
 * body bytes land in out.carryover. */
static bool readHeaders(int fd, HttpResponse& out, int timeoutMs) {
    std::string buf;
    buf.reserve(512U);
    char tmp[kReadChunk];

    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const ssize_t n = readSome(fd, tmp, sizeof(tmp), timeoutMs);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<std::size_t>(n));
        if (buf.size() > 1U * 1024U * 1024U) return false;  /* runaway */
        headerEnd = buf.find("\r\n\r\n");
    }

    /* Parse status line: "HTTP/1.x <code> <reason>" */
    const std::size_t firstEol = buf.find("\r\n");
    if (firstEol == std::string::npos || firstEol < 12U) return false;
    const std::size_t sp1 = buf.find(' ');
    if (sp1 == std::string::npos || sp1 + 4U > firstEol) return false;
    int code = 0;
    for (std::size_t i = sp1 + 1U; i < sp1 + 4U; ++i) {
        const char c = buf[i];
        if (c < '0' || c > '9') return false;
        code = (code * 10) + (c - '0');
    }
    out.status = code;

    /* Walk header lines for Transfer-Encoding and Content-Length. */
    std::size_t pos = firstEol + 2U;
    while (pos < headerEnd) {
        const std::size_t eol = buf.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd) break;
        if (eol == pos) break;  /* empty line == headers end */

        const std::size_t colon = buf.find(':', pos);
        if (colon != std::string::npos && colon < eol) {
            auto headerNameIs = [&](const char* expected) -> bool {
                const std::size_t nlen = std::strlen(expected);
                if (colon - pos != nlen) return false;
                for (std::size_t i = 0U; i < nlen; ++i) {
                    char c = buf[pos + i];
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    char e = expected[i];
                    if (e >= 'A' && e <= 'Z') e = static_cast<char>(e - 'A' + 'a');
                    if (c != e) return false;
                }
                return true;
            };

            std::size_t vbeg = colon + 1U;
            while (vbeg < eol && (buf[vbeg] == ' ' || buf[vbeg] == '\t')) ++vbeg;

            if (headerNameIs("Transfer-Encoding")) {
                /* Match "chunked" anywhere in the value. */
                for (std::size_t i = vbeg; i + 7U <= eol; ++i) {
                    bool m = true;
                    static const char* k = "chunked";
                    for (std::size_t j = 0U; j < 7U; ++j) {
                        char c = buf[i + j];
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        if (c != k[j]) { m = false; break; }
                    }
                    if (m) { out.chunked = true; break; }
                }
            } else if (headerNameIs("Content-Length")) {
                long long cl = 0;
                bool any = false;
                for (std::size_t i = vbeg; i < eol; ++i) {
                    const char c = buf[i];
                    if (c < '0' || c > '9') break;
                    cl = (cl * 10) + (c - '0');
                    any = true;
                    /* Cap early: a body this large is rejected downstream by
                     * kMaxResponseBytes anyway, so stop before cl can grow
                     * unbounded / signed-overflow. */
                    if (cl > static_cast<long long>(kMaxResponseBytes)) break;
                }
                if (any) out.contentLength = cl;
            }
        }
        pos = eol + 2U;
    }

    out.carryover.assign(buf, headerEnd + 4U, buf.size() - (headerEnd + 4U));
    return true;
}

/* Drain a one-shot (non-streaming) response body into @p body. */
static bool readBody(int fd, HttpResponse& meta, std::string& body, int timeoutMs) {
    body = std::move(meta.carryover);
    meta.carryover.clear();

    if (meta.chunked) {
        std::string raw = std::move(body);
        body.clear();

        std::size_t pos = 0U;
        while (true) {
            while (raw.find("\r\n", pos) == std::string::npos) {
                char tmp[kReadChunk];
                const ssize_t n = readSome(fd, tmp, sizeof(tmp), timeoutMs);
                if (n <= 0) return false;
                raw.append(tmp, static_cast<std::size_t>(n));
                if (raw.size() > kMaxResponseBytes) return false;
            }
            const std::size_t eol = raw.find("\r\n", pos);
            std::size_t sz = 0U;
            bool any = false;
            for (std::size_t i = pos; i < eol; ++i) {
                const char c = raw[i];
                std::size_t d = 16U;
                if (c >= '0' && c <= '9')      d = static_cast<std::size_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<std::size_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<std::size_t>(c - 'A' + 10);
                else break;  /* chunk-extension separator ';' or end of size */
                sz = (sz * 16U) + d;
                any = true;
                /* Reject a chunk larger than the body cap before sz can wrap. */
                if (sz > kMaxResponseBytes) return false;
            }
            if (!any) return false;
            pos = eol + 2U;

            if (sz == 0U) return true;  /* terminating chunk */

            /* Guard the pos + sz + 2U arithmetic against std::size_t overflow. */
            if (pos > kMaxResponseBytes || sz > kMaxResponseBytes - 2U) return false;

            while (raw.size() < pos + sz + 2U) {
                char tmp[kReadChunk];
                const ssize_t n = readSome(fd, tmp, sizeof(tmp), timeoutMs);
                if (n <= 0) return false;
                raw.append(tmp, static_cast<std::size_t>(n));
                if (raw.size() > kMaxResponseBytes) return false;
            }
            body.append(raw, pos, sz);
            pos += sz + 2U;  /* skip payload + "\r\n" */

            if (body.size() > kMaxResponseBytes) return false;
        }
    }

    /* Non-chunked: read until Content-Length or EOF. */
    if (meta.contentLength >= 0) {
        while (body.size() < static_cast<std::size_t>(meta.contentLength)) {
            char tmp[kReadChunk];
            const ssize_t n = readSome(fd, tmp, sizeof(tmp), timeoutMs);
            if (n <= 0) return false;
            body.append(tmp, static_cast<std::size_t>(n));
            if (body.size() > kMaxResponseBytes) return false;
        }
        return true;
    }

    /* Length unknown: read to EOF. */
    while (true) {
        char tmp[kReadChunk];
        const ssize_t n = readSome(fd, tmp, sizeof(tmp), timeoutMs);
        if (n == 0) return true;
        if (n < 0) return false;
        body.append(tmp, static_cast<std::size_t>(n));
        if (body.size() > kMaxResponseBytes) return false;
    }
}

/* Tri-state httpGet outcome so callers can distinguish a failed connect to
 * docker.sock (Result::SocketUnreachable) from an I/O or parse failure on an
 * established connection (Result::CommandError). */
enum class HttpOutcome {
    Ok,             /* request completed; outStatus is set */
    ConnectFailed,  /* could not open/connect docker.sock */
    IoOrParseError, /* send/read/parse failed on an open connection */
};

/* One-shot GET: fills @p body and @p outStatus on success. */
static HttpOutcome httpGet(const std::string& path, std::string& body, int& outStatus) {
    const int fd = openDockerSocket(kConnectTimeoutMs);
    if (fd < 0) return HttpOutcome::ConnectFailed;

    bool ok = sendHttpGet(fd, path, kReadTimeoutMs);
    HttpResponse resp;
    if (ok) ok = readHeaders(fd, resp, kReadTimeoutMs);
    if (ok) ok = readBody(fd, resp, body, kReadTimeoutMs);

    ::close(fd);
    if (!ok) return HttpOutcome::IoOrParseError;
    outStatus = resp.status;
    return HttpOutcome::Ok;
}

/* The Engine API returns names with a leading '/'; PCM uses unprefixed. */
static std::string stripLeadingSlash(const std::string& s) {
    if (!s.empty() && s.front() == '/') return s.substr(1U);
    return s;
}

DockerClient::DockerClient() = default;

bool DockerClient::isValidContainerName(const std::string& name) {
    return isSafeName(name);
}

DockerClient::Result DockerClient::listContainers(std::vector<ContainerInfo>& out) {
    std::string body;
    int status = 0;
    const HttpOutcome outcome = httpGet("/containers/json?all=1", body, status);
    if (outcome == HttpOutcome::ConnectFailed) return Result::SocketUnreachable;
    if (outcome == HttpOutcome::IoOrParseError) return Result::CommandError;
    if (status == 404) return Result::NotFound;
    if (status != 200) return Result::CommandError;

    std::vector<ContainerInfo> parsed;
    parsed.reserve(32U);

    const bool ok = pcm_json::forEachArrayObject(body, [&](std::size_t s, std::size_t e) {
        ContainerInfo ci;
        std::string nameWithSlash;
        std::string state;
        (void)pcm_json::extractString          (body, "Id",    ci.id,         s, e);
        (void)pcm_json::extractStringArrayFirst(body, "Names", nameWithSlash, s, e);
        (void)pcm_json::extractString          (body, "Image", ci.image,      s, e);
        (void)pcm_json::extractString          (body, "State", state,         s, e);
        ci.name  = stripLeadingSlash(nameWithSlash);
        ci.state = std::move(state);
        if (ci.id.empty() || ci.name.empty()) return;
        parsed.push_back(std::move(ci));
    });

    if (!ok) return Result::CommandError;
    out = std::move(parsed);
    return Result::Ok;
}

/* The targeted extractors assume complete JSON containers. Verify the inspect
 * response framing first so a missing close cannot publish an early Running
 * field as a valid verdict. Quoted braces do not change nesting. */
static bool isCompleteInspectObject(const std::string& body) {
    std::size_t pos = pcm_json::skipWs(body, 0U, body.size());
    if (pos == body.size() || body[pos] != '{') return false;
    std::vector<char> closes;
    while (pos < body.size()) {
        const char value = body[pos];
        if (value == '"') {
            pos = pcm_json::skipJsonString(body, pos, body.size());
            if (pos == std::string::npos) return false;
            continue;
        }
        if (value == '{' || value == '[') {
            closes.push_back(value == '{' ? '}' : ']');
        } else if (value == '}' || value == ']') {
            if (closes.empty() || closes.back() != value) return false;
            closes.pop_back();
            if (closes.empty()) {
                return pcm_json::skipWs(body, pos + 1U, body.size()) == body.size();
            }
        }
        ++pos;
    }
    return false;
}

DockerClient::Result DockerClient::inspectContainer(const std::string& name, ContainerHealth& out) {
    if (!isSafeName(name)) return Result::CommandError;

    std::string path;
    path.reserve(32U + name.size());
    path.append("/containers/");
    path.append(name);
    path.append("/json");

    std::string body;
    int status = 0;
    const HttpOutcome outcome = httpGet(path, body, status);
    if (outcome == HttpOutcome::ConnectFailed) return Result::SocketUnreachable;
    if (outcome == HttpOutcome::IoOrParseError) return Result::CommandError;
    if (status == 404) return Result::NotFound;
    if (status != 200) return Result::CommandError;
    if (!isCompleteInspectObject(body)) return Result::CommandError;

    ContainerHealth h;

    /* .State.Running / .State.Status / .State.ExitCode / .State.Health.Status */
    std::size_t stateStart = 0U, stateEnd = 0U;
    /* An incomplete inspect reply is an API failure, not evidence that a
     * container stopped. Do not publish ContainerHealth's default false flags. */
    if (!pcm_json::findObject(body, "State", 0U, body.size(), stateStart, stateEnd) ||
        !pcm_json::extractBool(body, "Running", h.running, stateStart, stateEnd)) {
        return Result::CommandError;
    }
    const auto optionalBool = [&](const char* key, bool& value) {
        return pcm_json::findKey(body, key, stateStart, stateEnd) == std::string::npos ||
               pcm_json::extractBool(body, key, value, stateStart, stateEnd);
    };
    if (!optionalBool("Paused", h.paused) ||
        !optionalBool("Restarting", h.restarting) ||
        !optionalBool("Dead", h.dead)) {
        return Result::CommandError;
    }
    if (pcm_json::findKey(body, "Status", stateStart, stateEnd) != std::string::npos) {
        if (!pcm_json::extractString(body, "Status", h.status, stateStart, stateEnd) ||
            (h.status != "created" && h.status != "running" && h.status != "paused" &&
             h.status != "restarting" && h.status != "removing" && h.status != "exited" &&
             h.status != "dead")) {
            return Result::CommandError;
        }
    }
    h.hasExitCode = pcm_json::extractInt(body, "ExitCode", h.exitCode, stateStart, stateEnd);
    /* Health is legitimately absent when the image has no HEALTHCHECK.
     * When present, a malformed health result must not look healthy. */
    if (pcm_json::findKey(body, "Health", stateStart, stateEnd) != std::string::npos) {
        std::size_t healthStart = 0U, healthEnd = 0U;
        if (!pcm_json::findObject(body, "Health", stateStart, stateEnd, healthStart, healthEnd) ||
            !pcm_json::extractString(body, "Status", h.health, healthStart, healthEnd) ||
            (h.health != "healthy" && h.health != "unhealthy" && h.health != "starting")) {
            return Result::CommandError;
        }
    }

    /* .Config.Image is the human tag; top-level .Image is a sha256. */
    std::size_t configStart = 0U, configEnd = 0U;
    if (pcm_json::findObject(body, "Config", 0U, body.size(), configStart, configEnd)) {
        (void)pcm_json::extractString(body, "Image", h.image, configStart, configEnd);
        std::size_t labelsStart = 0U, labelsEnd = 0U;
        if (pcm_json::findObject(body, "Labels", configStart, configEnd, labelsStart, labelsEnd)) {
            (void)pcm_json::extractString(body,
                                          "com.docker.compose.service",
                                          h.composeService,
                                          labelsStart,
                                          labelsEnd);
        }
    }

    if (h.image.empty()) return Result::CommandError;

    out = std::move(h);
    return Result::Ok;
}
