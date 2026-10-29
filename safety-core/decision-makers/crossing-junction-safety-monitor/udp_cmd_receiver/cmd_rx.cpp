/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Crossing-Junction-Safety-Monitor UDP Command Receiver Application
 *
 * Simulates the junction's siren controller.  Incoming SDM commands
 * are buffered and every EVAL_WINDOW_MS (100 ms) the window is evaluated
 * using a "most conservative wins" policy:
 *
 *   - Any CMD_SW_ERROR                            →  FAULT SAFE STATE / ALARM
 *   - Any CMD_STOP / CMD_HW_ERROR                 →  ESTOP
 *   - Any CMD_JUNCTION_ALARM (nothing more severe) →  JUNCTION SIREN ON
 *   - All CMD_NORMAL                              →  NORMAL OPERATION
 *   - Empty window                                →  hold previous action
 *
 * Holding the previous action on an empty window matters here: the SDM sends
 * only on change plus a slow periodic re-assert, so a quiet window is the normal
 * case for a junction that is not changing state, not a signal to release.
 *
 * Key Components:
 * - UDPReceiver Class: Manages the UDP socket and handles data reception.
 * - Windowed evaluator thread: samples buffered commands every 100 ms.
 * - poll()-based event loop: Non-blocking message reception.
 * - 64-byte packet format with acknowledgment system.
 */

#include <iostream>
#include <fstream>
#include <string>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/select.h>
#include <cstdio>

#include "crossing_junction_safety_cmd_pkt.h"
#include "../../common/include/metropolis_cmd_identifiers.h"

#define POLL_TIMEOUT_MS         50
#define EVAL_WINDOW_MS          100

static constexpr int64_t kUpstreamHbExpectedMs = 5000;
static_assert(METROPOLIS_CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER == CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER,
              "Crossing-Junction-Safety-Monitor packet identifier mismatch");

class UDPReceiver;
void upstreamHeartbeatWatchdog(UDPReceiver& receiver);
void windowedEvaluator(UDPReceiver& receiver);
void safeReleaseInputLoop(UDPReceiver& receiver);
void startupSafeReleaseLoop(UDPReceiver& receiver);

/* Grace period after first peer contact before the startup latch-clear release
 * is sent, giving the SDM/agent a moment to be ready to process it. */
static constexpr int kStartupReleaseSettleMs = 500;

static const char* actionLabel(unsigned char cmd)
{
    switch (cmd) {
    case CMD_STOP:       return "ESTOP";
    case CMD_HW_ERROR:   return "ESTOP (HW_ERROR)";
    case CMD_SW_ERROR:   return "FAULT SAFE STATE / ALARM";
    case CMD_JUNCTION_ALARM: return "JUNCTION SIREN ON";
    case CMD_NORMAL:     return "NORMAL OPERATION";
    default:             return "UNKNOWN";
    }
}

/*
 * Global pointer so the signal handler can reach the receiver instance.
 *
 * Published before the handlers are installed, and cleared only once every
 * thread has joined, so the handler never reaches a receiver that has been
 * destroyed. That clear is still concurrent with the handler by construction --
 * the handlers stay installed until the process returns from main(), so a
 * signal can land while the store is in flight -- and a signal handler may only
 * touch a lock-free atomic or a volatile sig_atomic_t, whatever the generated
 * load happens to look like on a given target. Hence the atomic rather than a
 * plain pointer; the assert below keeps it in the handler-safe class.
 */
static std::atomic<UDPReceiver*> g_receiver{nullptr};
static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
              "signalHandler() requires a lock-free atomic pointer");

static std::atomic<uint32_t> g_maxHbFailures{10U};
static std::atomic<uint32_t> g_warnThreshold{5U};

static uint32_t missCountFromElapsedMs(int64_t elapsedMs)
{
    const int64_t kStaleStartMs = kUpstreamHbExpectedMs;
    const int64_t kPeriodMs     = 5500;
    if (elapsedMs <= kStaleStartMs)
        return 0U;
    const uint64_t m = 1U + static_cast<uint64_t>((elapsedMs - kStaleStartMs) / kPeriodMs);
    const uint32_t maxF = g_maxHbFailures.load();
    if (m > static_cast<uint64_t>(maxF))
        return maxF;
    return static_cast<uint32_t>(m);
}

static bool isVstDecisionCommand(unsigned char command)
{
    return command == CMD_STOP || command == CMD_JUNCTION_ALARM ||
           command == CMD_NORMAL || command == CMD_HW_ERROR ||
           command == CMD_SW_ERROR;
}

/* Short wall-clock HH:MM:SS stamp for the human-facing console notifications. */
static std::string nowTimeStr()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

class UDPReceiver
{
private:
    int            sock_;
    int            vst_sock_;
    unsigned int   listen_port_;
    std::atomic<bool> running_;
    std::mutex     sock_mtx_;
    bool           vst_relay_enabled_ = false;
    bool           vst_send_failure_logged_ = false;
    struct sockaddr_in vst_addr_ = {};

    static constexpr std::size_t max_length = 1024;
    char data_[max_length];

    std::mutex                    hb_mtx_;
    std::chrono::steady_clock::time_point last_hb_time_;
    std::atomic<bool>             hb_fault_latched_{false};

    std::mutex                    window_mtx_;
    std::vector<unsigned char>    window_cmds_;
    unsigned char                 last_action_{CMD_NORMAL};
    std::mutex                    sender_mtx_;
    struct sockaddr_in            last_sender_addr_ = {};
    socklen_t                     last_sender_len_ = 0;
    bool                          have_last_sender_ = false;
    std::atomic<bool>             safe_release_prompt_ready_{false};
    std::atomic<uint32_t>         safe_release_seq_{0U};
    struct in_addr                expected_sender_ip_ = {};
    uint16_t                      expected_sender_port_ = 0U;

    /* Verbose per-packet log sink. When open, the high-volume received-command /
     * object / ack lines go here instead of the console, so the interactive
     * terminal stays free for the 'release' command. Written only from the
     * single receiver thread (handleReceive), so no extra locking is needed. */
    std::ofstream                 log_ofs_;
    /* Tracks whether the console currently reflects a latched safe-state, so we
     * emit exactly one "LATCHED" / "CLEARED" edge notification (not per packet). */
    bool                          console_latched_ = false;

public:
    UDPReceiver(unsigned int listen_port,
                const struct in_addr& expected_sender_ip,
                uint16_t expected_sender_port,
                bool vst_relay_enabled,
                const struct sockaddr_in& vst_addr)
        : sock_(-1),
          vst_sock_(-1),
          listen_port_(listen_port),
          running_(true),
          vst_relay_enabled_(vst_relay_enabled),
          vst_addr_(vst_addr),
          expected_sender_ip_(expected_sender_ip),
          expected_sender_port_(expected_sender_port)
    {
        last_hb_time_ = std::chrono::steady_clock::now();
        window_cmds_.reserve(64);
        initSocket();
        if (vst_relay_enabled_)
            initVstSocket();
    }

    ~UDPReceiver()
    {
        stop();
    }

    /*
     * requestStop() — async-signal-safe shutdown request.
     * Only touches a single std::atomic<bool>.  Safe to call from
     * a signal handler.  Does NOT close the socket or join threads;
     * the full cleanup happens via stop() in main() after run() returns.
     */
    void requestStop()
    {
        running_.store(false);
    }

    void stop()
    {
        running_.store(false);
        std::lock_guard<std::mutex> lk(sock_mtx_);
        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
        if (vst_sock_ >= 0) {
            close(vst_sock_);
            vst_sock_ = -1;
        }
    }

    bool isRunning() const
    {
        return running_.load();
    }

    void markHeartbeatReceived()
    {
        std::lock_guard<std::mutex> lk(hb_mtx_);
        last_hb_time_ = std::chrono::steady_clock::now();
    }

    void bufferCommand(unsigned char cmd)
    {
        std::lock_guard<std::mutex> lk(window_mtx_);
        window_cmds_.push_back(cmd);
    }

    bool shouldPromptSafeRelease()
    {
        std::lock_guard<std::mutex> lk(sender_mtx_);
        return have_last_sender_ &&
               safe_release_prompt_ready_.load(std::memory_order_acquire);
    }

    /* True once a valid packet from the expected peer has been seen, i.e. the
     * SDM/agent "connection" is established and its reply address is known. */
    bool haveSender()
    {
        std::lock_guard<std::mutex> lk(sender_mtx_);
        return have_last_sender_;
    }

    /* Open (append) the verbose log file. Returns false if it cannot be opened,
     * in which case vlog() falls back to std::cout. */
    bool openLogFile(const std::string& path)
    {
        log_ofs_.open(path, std::ios::out | std::ios::app);
        return log_ofs_.is_open();
    }

    /* Verbose sink: the log file when configured, otherwise the console. */
    std::ostream& vlog()
    {
        return log_ofs_.is_open() ? static_cast<std::ostream&>(log_ofs_)
                                  : std::cout;
    }

    /*
     * Send a CMD_SAFE_RELEASE_REQUEST to the learned peer.
     *   force == false : interactive/normal path — only sent once the SDM has
     *                     reported a release-ready safe state (SW_ERROR/DENIED).
     *   force == true  : startup path — sent unconditionally on first contact to
     *                     clear a stale latch; the SDM still validates and denies
     *                     it if the underlying cause is still active.
     */
    bool sendSafeReleaseRequest(bool force = false)
    {
        struct sockaddr_in target_addr = {};
        socklen_t target_len = 0;
        {
            std::lock_guard<std::mutex> lk(sender_mtx_);
            if (!have_last_sender_)
            {
                std::cerr << "No SDM sender known yet; wait for a command before sending safe release\n";
                return false;
            }
            if (!force && !safe_release_prompt_ready_.load(std::memory_order_acquire))
            {
                std::cerr << "Safe-release request ignored; SDM has not reported a release-ready safe state\n";
                return false;
            }
            target_addr = last_sender_addr_;
            target_len = last_sender_len_;
        }

        CmdPacket pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        const uint64_t sec = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
        const uint64_t usec = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count()
                              - (sec * 1000000ULL);

        pkt.identifier = CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER;
        pkt.seq = static_cast<uint16_t>(safe_release_seq_.fetch_add(1U));
        pkt.command = CMD_SAFE_RELEASE_REQUEST;
        pkt.ts_seconds = sec;
        pkt.ts_microseconds = usec;
        pkt.crc32 = cmdPacketCRC32(&pkt);

        ssize_t sent = -1;
        {
            std::lock_guard<std::mutex> lk(sock_mtx_);
            if (sock_ >= 0) {
                sent = sendto(sock_, &pkt, sizeof(pkt), 0,
                              reinterpret_cast<const struct sockaddr*>(&target_addr),
                              target_len);
            }
        }
        if (sent == static_cast<ssize_t>(sizeof(pkt)))
        {
            std::cout << "Sent SAFE RELEASE REQUEST SeqNo: " << pkt.seq
                      << (force ? " (startup latch-clear)" : "") << std::endl;
            return true;
        }
        std::cerr << "Failed to send SAFE RELEASE REQUEST: " << strerror(errno) << std::endl;
        return false;
    }

    /* Drain the window and return the collected commands. */
    std::vector<unsigned char> drainWindow()
    {
        std::lock_guard<std::mutex> lk(window_mtx_);
        std::vector<unsigned char> out;
        out.swap(window_cmds_);
        window_cmds_.reserve(64);
        return out;
    }

    void run()
    {
        struct pollfd pfd;

        while (running_.load())
        {
            {
                std::lock_guard<std::mutex> lk(sock_mtx_);
                if (sock_ < 0) break;
                pfd.fd = sock_;
            }
            pfd.events  = POLLIN;
            pfd.revents = 0;

            int ready = poll(&pfd, 1, POLL_TIMEOUT_MS);
            if (ready <= 0)
                continue;

            struct sockaddr_in sender_addr;
            socklen_t sender_len = sizeof(sender_addr);
            ssize_t n = -1;

            {
                std::lock_guard<std::mutex> lk(sock_mtx_);
                if (sock_ < 0) break;
                n = recvfrom(sock_, data_, max_length, MSG_DONTWAIT,
                             reinterpret_cast<struct sockaddr*>(&sender_addr),
                             &sender_len);
            }

            if (n > 0)
                handleReceive(static_cast<std::size_t>(n),
                              sender_addr, sender_len);
        }
    }

private:
    bool isExpectedSender(const struct sockaddr_in& sender_addr,
                          socklen_t sender_len) const
    {
        if (sender_len < sizeof(struct sockaddr_in) ||
            sender_addr.sin_family != AF_INET ||
            sender_addr.sin_addr.s_addr != expected_sender_ip_.s_addr) {
            return false;
        }
        return expected_sender_port_ == 0U ||
               sender_addr.sin_port == htons(expected_sender_port_);
    }

    void initSocket()
    {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            std::cerr << "Error creating socket: "
                      << strerror(errno) << std::endl;
            return;
        }

        /* SO_REUSEADDR */
        int optval = 1;
        if (setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                       &optval, sizeof(optval)) < 0) {
            std::cerr << "Error setting SO_REUSEADDR: "
                      << strerror(errno) << std::endl;
            close(sock_); sock_ = -1;
            return;
        }

        /* Bind */
        struct sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(listen_port_);

        if (bind(sock_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            std::cerr << "Error binding socket: "
                      << strerror(errno) << std::endl;
            close(sock_); sock_ = -1;
            return;
        }

        /* Non-blocking */
        int flags = fcntl(sock_, F_GETFL, 0);
        fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
    }

    void initVstSocket()
    {
        vst_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (vst_sock_ < 0) {
            vst_relay_enabled_ = false;
            std::cerr << "VST relay disabled: failed to create UDP socket\n";
            return;
        }

        const int flags = fcntl(vst_sock_, F_GETFL, 0);
        if (flags < 0 || fcntl(vst_sock_, F_SETFL, flags | O_NONBLOCK) < 0) {
            std::cerr << "VST relay disabled: failed to configure UDP socket\n";
            close(vst_sock_);
            vst_sock_ = -1;
            vst_relay_enabled_ = false;
        }
    }

    void relayToVst()
    {
        std::lock_guard<std::mutex> lk(sock_mtx_);
        if (!vst_relay_enabled_ || vst_sock_ < 0)
            return;

        const ssize_t sent = sendto(vst_sock_, data_, COMMAND_PACKET_SIZE,
                                    MSG_DONTWAIT,
                                    reinterpret_cast<const struct sockaddr*>(&vst_addr_),
                                    sizeof(vst_addr_));
        if (sent == static_cast<ssize_t>(COMMAND_PACKET_SIZE)) {
            vst_send_failure_logged_ = false;
            return;
        }

        if (!vst_send_failure_logged_) {
            std::cerr << "VST relay send failed; continuing without display delivery\n";
            vst_send_failure_logged_ = true;
        }
    }

    void handleReceive(std::size_t bytes_recvd,
                       const struct sockaddr_in& sender_addr,
                       socklen_t sender_len)
    {
        if (bytes_recvd != COMMAND_PACKET_SIZE)
            return;

        if (!isExpectedSender(sender_addr, sender_len)) {
            std::cerr << "Dropped packet from unexpected SDM sender" << std::endl;
            return;
        }

        const CmdPacket* pkt = reinterpret_cast<const CmdPacket*>(data_);

        /* Validate identifier */
        if (pkt->identifier != CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER) {
            std::cerr << "Invalid packet identifier: 0x"
                      << std::hex << (int)pkt->identifier << std::endl;
            return;
        }

        if (!cmdPacketValidateCRC(pkt)) {
            std::cerr << "WARNING: CRC mismatch for SeqNo " << pkt->seq << std::endl;
            return;
        }

        uint16_t       seqNo       = pkt->seq;
        unsigned char  receivedCmd = pkt->command;

        {
            std::lock_guard<std::mutex> lk(sender_mtx_);
            last_sender_addr_ = sender_addr;
            last_sender_len_ = sender_len;
            have_last_sender_ = true;
        }

        if (receivedCmd == CMD_HEARTBEAT)
        {
            if (!hb_fault_latched_.load())
                markHeartbeatReceived();
            return;
        }

        if (!(receivedCmd == CMD_STOP || receivedCmd == CMD_JUNCTION_ALARM ||
              receivedCmd == CMD_NORMAL || receivedCmd == CMD_HW_ERROR ||
              receivedCmd == CMD_SW_ERROR ||
              receivedCmd == CMD_SAFE_RELEASE_REQUEST ||
              receivedCmd == CMD_SAFE_RELEASE_ACK ||
              receivedCmd == CMD_SAFE_RELEASE_DENIED))
            return;

        if (receivedCmd == CMD_SW_ERROR ||
            receivedCmd == CMD_SAFE_RELEASE_DENIED)
        {
            safe_release_prompt_ready_.store(true, std::memory_order_release);
            /* Console: announce the latch edge exactly once, so the operator
             * knows a latch has taken place without watching the packet flood
             * (which now goes to the log file). */
            if (!console_latched_)
            {
                console_latched_ = true;
                std::cout << "\n[" << nowTimeStr() << "]  *** SAFE-STATE LATCHED ***  ("
                          << commandName(receivedCmd) << ", SeqNo " << seqNo << ")\n"
                          << "        Enter 'release' to clear once the area is confirmed safe."
                          << "  (per-packet detail -> log file)\n" << std::flush;
            }
            else if (receivedCmd == CMD_SAFE_RELEASE_DENIED)
            {
                std::cout << "[" << nowTimeStr() << "]  safe-release DENIED (SeqNo " << seqNo
                          << ") — latch cause still active.\n" << std::flush;
            }
        }
        else if (receivedCmd == CMD_SAFE_RELEASE_ACK ||
                 receivedCmd == CMD_NORMAL ||
                 receivedCmd == CMD_JUNCTION_ALARM)
        {
            safe_release_prompt_ready_.store(false, std::memory_order_release);
            /* Console: announce the clear edge exactly once. */
            if (console_latched_)
            {
                console_latched_ = false;
                std::cout << "[" << nowTimeStr() << "]  >>> safe-state CLEARED <<<  ("
                          << commandName(receivedCmd) << ", SeqNo " << seqNo
                          << ") — normal operation resumed.\n" << std::flush;
            }
        }

        /* Verbose per-packet detail -> log file (keeps the console readable). */
        vlog() << "Received Crossing-Junction-Safety-Monitor command: 0x"
               << std::hex << std::setfill('0') << std::setw(2)
               << (int)receivedCmd << std::dec
               << " - " << commandName(receivedCmd)
               << ", SeqNo: " << seqNo
               << ", UTC epoch: " << pkt->ts_seconds
               << "." << std::setfill('0') << std::setw(6)
               << pkt->ts_microseconds << std::endl;

        /* Object records -> log file */
        const char* objectLabels[COMMAND_NUM_OBJECTS] = {
            "Object 1 (configured primary/center role)",
            "Object 2 (configured secondary/surrounding role)"
        };
        for (int i = 0; i < COMMAND_NUM_OBJECTS; i++) {
            const ObjectRecord& obj = pkt->objects[i];
            vlog() << "  " << objectLabels[i]
                   << ": ID=" << obj.object_id
                   << ", X=" << obj.x
                   << ", Y=" << obj.y
                   << ", Z=" << obj.z
                   << ", Type=" << obj.metadata << std::endl;
        }

        /* Build 64-byte ACK packet */
        CmdPacket ackPkt;
        std::memset(&ackPkt, 0, sizeof(ackPkt));

        auto     now   = std::chrono::system_clock::now();
        auto     epoch = now.time_since_epoch();
        uint64_t ackSec  = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
        uint64_t ackUsec = std::chrono::duration_cast<std::chrono::microseconds>(epoch).count()
                           - (ackSec * 1000000ULL);

        ackPkt.identifier      = CROSSING_JUNCTION_SAFETY_PACKET_IDENTIFIER;
        ackPkt.seq             = seqNo;
        ackPkt.command         = receivedCmd;
        ackPkt.ts_seconds      = ackSec;
        ackPkt.ts_microseconds = ackUsec;
        std::memcpy(ackPkt.objects, pkt->objects,
                    sizeof(ObjectRecord) * COMMAND_NUM_OBJECTS);
        ackPkt.crc32 = cmdPacketCRC32(&ackPkt);

        /* Send ACK */
        ssize_t sent = -1;
        {
            std::lock_guard<std::mutex> lk(sock_mtx_);
            if (sock_ >= 0) {
                sent = sendto(sock_, &ackPkt, sizeof(ackPkt), 0,
                              reinterpret_cast<const struct sockaddr*>(&sender_addr),
                              sender_len);
            }
        }
        if (sent == static_cast<ssize_t>(sizeof(ackPkt)))
            vlog() << "Sent acknowledgment for SeqNo: " << seqNo << std::endl;
        else
            std::cerr << "Failed to send acknowledgment: "
                      << strerror(errno) << std::endl;

        if (isVstDecisionCommand(receivedCmd))
            relayToVst();

        if (receivedCmd == CMD_STOP || receivedCmd == CMD_JUNCTION_ALARM ||
            receivedCmd == CMD_NORMAL || receivedCmd == CMD_HW_ERROR ||
            receivedCmd == CMD_SW_ERROR)
        {
            bufferCommand(receivedCmd);
        }
    }

    friend void upstreamHeartbeatWatchdog(UDPReceiver&);
    friend void windowedEvaluator(UDPReceiver&);
    friend void safeReleaseInputLoop(UDPReceiver&);
    friend void startupSafeReleaseLoop(UDPReceiver&);
};

void upstreamHeartbeatWatchdog(UDPReceiver& receiver)
{
    uint32_t prevMiss = 0U;
    while (receiver.isRunning())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (receiver.hb_fault_latched_.load())
            continue;

        int64_t elapsed = 0;
        {
            std::lock_guard<std::mutex> lk(receiver.hb_mtx_);
            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - receiver.last_hb_time_).count();
        }

        const uint32_t m = missCountFromElapsedMs(elapsed);
        if (m == 0U)
            prevMiss = 0U;

        const uint32_t maxF = g_maxHbFailures.load();
        const uint32_t warnW = g_warnThreshold.load();

        if (m > prevMiss && m >= 1U && m <= warnW)
        {
            std::cerr << "cmd_rx: upstream HB warn tier miss=" << m << "/" << maxF
                      << " elapsed_ms=" << elapsed << std::endl;
        }
        if (m > warnW && m < maxF && prevMiss <= warnW)
        {
            std::cerr << "cmd_rx: upstream HB active fault (tier 2) miss=" << m << std::endl;
        }

        prevMiss = m;

        if (m >= maxF && !receiver.hb_fault_latched_.exchange(true))
        {
            std::cerr << "cmd_rx: upstream CMD_HEARTBEAT fault latched (tier 3) — local fail-safe, no PSS — stopping\n";
            receiver.requestStop();
        }
    }
}

/*
 * Windowed command evaluator — runs every EVAL_WINDOW_MS.
 * Drains all buffered commands, applies "most conservative wins", and
 * prints the decided action.  On state transitions the output is highlighted.
 */
void windowedEvaluator(UDPReceiver& receiver)
{
    while (receiver.isRunning())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(EVAL_WINDOW_MS));

        auto cmds = receiver.drainWindow();
        if (cmds.empty())
            continue;

        unsigned int nStop = 0, nAlarm = 0, nNormal = 0, nHwError = 0, nSwError = 0;
        for (unsigned char c : cmds) {
            switch (c) {
            case CMD_STOP:           ++nStop;  break;
            case CMD_JUNCTION_ALARM: ++nAlarm; break;
            case CMD_NORMAL:     ++nNormal; break;
            case CMD_HW_ERROR:   ++nHwError; break;
            case CMD_SW_ERROR:   ++nSwError; break;
            default: break;
            }
        }

        unsigned char action;
        if (nSwError > 0)
            action = CMD_SW_ERROR;
        else if (nStop > 0 || nHwError > 0)
            action = CMD_STOP;
        else if (nAlarm > 0)
            action = CMD_JUNCTION_ALARM;
        else
            action = CMD_NORMAL;

        bool changed = (action != receiver.last_action_);
        receiver.last_action_ = action;

        std::cout << "------------------------------------------------------------------------\n"
                  << (changed ? ">>> " : "    ")
                  << "[EVAL] Action: " << actionLabel(action)
                  << "  |  window: " << cmds.size() << " cmd(s)"
                  << " [STOP=" << nStop
                  << " ALARM=" << nAlarm
                  << " NORMAL=" << nNormal;
        if (nHwError > 0)
            std::cout << " HW_ERROR=" << nHwError;
        if (nSwError > 0)
            std::cout << " SW_ERROR=" << nSwError;
        std::cout << "]"
                  << (changed ? "  *** STATE CHANGE ***" : "")
                  << "\n------------------------------------------------------------------------"
                  << std::endl;
    }
}

void safeReleaseInputLoop(UDPReceiver& receiver)
{
    bool promptShown = false;
    while (receiver.isRunning())
    {
        const bool releaseReady = receiver.shouldPromptSafeRelease();
        if (!releaseReady)
        {
            promptShown = false;
        }
        else if (!promptShown)
        {
            std::cout << "Is it safe to return to normal mode? "
                      << "Enter 'release' to exit safe-state, or 'no' to stay safe: "
                      << std::flush;
            promptShown = true;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        const int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready == 0)
            continue;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "Safe-release stdin disabled: select failed: "
                      << strerror(errno) << std::endl;
            break;
        }
        if (!FD_ISSET(STDIN_FILENO, &readfds))
            continue;

        std::string line;
        if (!std::getline(std::cin, line))
            return;
        if (!releaseReady) {
            if (!line.empty())
                std::cout << "Ignoring safe-release input until SDM reports a release-ready safe state.\n";
            continue;
        }
        if (line == "r" || line == "release" || line == "safe-release" ||
            line == "y" || line == "yes")
            (void)receiver.sendSafeReleaseRequest();
        promptShown = false;
    }
}

/*
 * startupSafeReleaseLoop
 * ----------------------
 * Fires exactly one unconditional (forced) safe-release the moment cmd_rx makes
 * first contact with its peer (crossing_junction_safety_monitor_sdm on CCplex). This
 * clears any safety latch left set by a previous run without needing an
 * operator prompt. The SDM validates the request and denies it if the
 * underlying cause is still active, so an active latch is never force-cleared.
 * Runs once, then exits.
 */
void startupSafeReleaseLoop(UDPReceiver& receiver)
{
    /* Wait until the peer reply address is known ("connected"), or shutdown. */
    while (receiver.isRunning() && !receiver.haveSender())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!receiver.isRunning())
        return;

    /* Brief settle so the peer/SDM is ready to accept the request. */
    for (int slept = 0; slept < kStartupReleaseSettleMs && receiver.isRunning();
         slept += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!receiver.isRunning())
        return;

    std::cout << "Startup: peer connected — sending safe-release to clear any "
                 "pre-existing latch\n";
    (void)receiver.sendSafeReleaseRequest(true /*force*/);
}

/*
 * Signal handler
 * --------------
 * Calls requestStop() which only sets std::atomic<bool> running_ = false.
 * This is async-signal-safe in practice.  The run() loop sees the flag
 * within POLL_TIMEOUT_MS and exits.  Full socket cleanup
 * happens in main() via stop() after run() returns.
 */
static void signalHandler(int /*sig*/)
{
    UDPReceiver* receiver = g_receiver.load(std::memory_order_acquire);
    if (receiver != nullptr)
        receiver->requestStop();
}

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog << " [-p <LISTEN_PORT>] [--sdm_ip <IPv4>] [--sdm_port <SENDER_PORT>] [--vst_ip <IPv4> --vst_port <PORT>] [--max_hb_failures <N>] [--log-file <PATH>] [--no-log-file] [-h|--help]\n\n"
              << "Crossing-Junction-Safety-Monitor UDP Command Receiver — listens for\n"
              << "JUNCTION ALARM / NORMAL decisions plus the shared fail-safe commands.\n\n"
              << "Ports (these are different):\n"
              << "  -p <LISTEN_PORT>       LOCAL bind/listen port for this receiver, 1-65535\n"
              << "                         (default: 12345). crossing_junction_safety_monitor_sdm must\n"
              << "                         send CmdPackets TO this port (--ip/-p of the peer).\n"
              << "  --sdm_port <SENDER_PORT>\n"
              << "                         Optional filter on the SOURCE port of the upstream\n"
              << "                         SDM peer, 1-65535. Omit (default) to\n"
              << "                         accept any source port from --sdm_ip. This is NOT\n"
              << "                         the listen port.\n\n"
              << "Options:\n"
              << "  --sdm_ip <IPv4>        Expected SDM sender IP (default: 127.0.0.1).\n"
              << "  --vst_ip <IPv4>        Optional VST display destination IP; requires --vst_port.\n"
              << "  --vst_port <PORT>      Optional VST display destination port, 1-65535; requires --vst_ip.\n"
              << "  --max_hb_failures <N>  Upstream heartbeat miss limit, 1-255 (default: 10).\n"
              << "  --log-file <PATH>      Write verbose per-packet logs to PATH; console shows only\n"
              << "                         latch/clear/release events (default: /tmp/cmdrx.log).\n"
              << "  --no-log-file          Keep verbose per-packet logs on the console (legacy behavior).\n"
              << "  -h, --help             Show this help message.\n\n"
              << "Safe-release: always enabled. Type 'release' on stdin, or 'echo release > <fifo>'.\n";
}

int main(int argc, char *argv[])
{
    const char* prog = (argc > 0 && argv[0] != nullptr) ? argv[0] : "crossing_junction_safety_monitor_cmd_rx";
    unsigned int port = 12345;
    uint32_t maxHb = 10U;
    const char* logFilePath = "/tmp/cmdrx.log";
    const char* expectedSdmIp = "127.0.0.1";
    uint16_t expectedSdmPort = 0U;
    const char* vstIp = nullptr;
    uint16_t vstPort = 0U;
    bool vstIpProvided = false;
    bool vstPortProvided = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printUsage(prog);
            return 0;
        }
        else if (strcmp(argv[i], "-p") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: -p requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            unsigned long p = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || end == argv[i] || *end != '\0' || p < 1UL || p > 65535UL)
            {
                std::cerr << "error: -p: invalid listen port (use 1..65535)\n";
                printUsage(prog);
                return 1;
            }
            port = static_cast<unsigned int>(p);
        }
        else if (strcmp(argv[i], "--sdm_ip") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --sdm_ip requires a value\n";
                printUsage(prog);
                return 1;
            }
            expectedSdmIp = argv[++i];
        }
        else if (strcmp(argv[i], "--sdm_port") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --sdm_port requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            unsigned long p = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || end == argv[i] || *end != '\0' || p < 1UL || p > 65535UL)
            {
                std::cerr << "error: --sdm_port: invalid sender source port (use 1..65535)\n";
                printUsage(prog);
                return 1;
            }
            expectedSdmPort = static_cast<uint16_t>(p);
        }
        else if (strcmp(argv[i], "--vst_ip") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --vst_ip requires a value\n";
                printUsage(prog);
                return 1;
            }
            vstIp = argv[++i];
            vstIpProvided = true;
        }
        else if (strcmp(argv[i], "--vst_port") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --vst_port requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            unsigned long p = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || end == argv[i] || *end != '\0' ||
                p < 1UL || p > 65535UL)
            {
                std::cerr << "error: --vst_port: invalid port (use 1..65535)\n";
                printUsage(prog);
                return 1;
            }
            vstPort = static_cast<uint16_t>(p);
            vstPortProvided = true;
        }
        else if (strcmp(argv[i], "--max_hb_failures") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --max_hb_failures requires a value\n";
                printUsage(prog);
                return 1;
            }
            char* end = nullptr;
            errno = 0;
            unsigned long v = std::strtoul(argv[++i], &end, 10);
            if (errno == ERANGE || end == argv[i] || *end != '\0' || v < 1UL || v > 255UL)
            {
                std::cerr << "error: --max_hb_failures: use 1..255\n";
                printUsage(prog);
                return 1;
            }
            maxHb = static_cast<uint32_t>(v);
        }
        else if (strcmp(argv[i], "--log-file") == 0)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "error: --log-file requires a value\n";
                printUsage(prog);
                return 1;
            }
            logFilePath = argv[++i];
        }
        else if (strcmp(argv[i], "--no-log-file") == 0)
        {
            logFilePath = "";
        }
        else if (argv[i][0] == '-')
        {
            std::cerr << "error: unknown option (see --help)\n";
            printUsage(prog);
            return 1;
        }
        else
        {
            std::cerr << "error: unexpected positional argument (see --help)\n";
            printUsage(prog);
            return 1;
        }
    }

    g_maxHbFailures.store(maxHb);
    g_warnThreshold.store(g_maxHbFailures.load() / 2U);

    struct in_addr expectedSdmAddr = {};
    if (inet_pton(AF_INET, expectedSdmIp, &expectedSdmAddr) != 1)
    {
        std::cerr << "error: --sdm_ip: invalid IPv4 address\n";
        printUsage(prog);
        return 1;
    }

    if (vstIpProvided != vstPortProvided)
    {
        std::cerr << "error: --vst_ip and --vst_port must be used together\n";
        printUsage(prog);
        return 1;
    }

    struct sockaddr_in vstAddr = {};
    const bool vstRelayEnabled = vstIpProvided;
    if (vstRelayEnabled)
    {
        vstAddr.sin_family = AF_INET;
        vstAddr.sin_port = htons(vstPort);
        if (inet_pton(AF_INET, vstIp, &vstAddr.sin_addr) != 1)
        {
            std::cerr << "error: --vst_ip: invalid IPv4 address\n";
            printUsage(prog);
            return 1;
        }
    }

    UDPReceiver receiver(port, expectedSdmAddr, expectedSdmPort,
                         vstRelayEnabled, vstAddr);
    g_receiver.store(&receiver, std::memory_order_release);

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "Crossing-Junction-Safety-Monitor UDP Command Receiver listening on port " << port
              << "  (evaluation window: " << EVAL_WINDOW_MS << " ms)" << std::endl;
    std::cout << "Accepting packets from SDM " << expectedSdmIp;
    if (expectedSdmPort != 0U)
        std::cout << " source port " << expectedSdmPort;
    else
        std::cout << " (any source port)";
    std::cout << std::endl;

    if (logFilePath != nullptr && logFilePath[0] != '\0')
    {
        if (receiver.openLogFile(logFilePath))
            std::cout << "Verbose per-packet logs -> " << logFilePath
                      << " (console shows latch / clear / release events only)"
                      << std::endl;
        else
            std::cerr << "warning: could not open log file '" << logFilePath
                      << "'; verbose logs will remain on the console\n";
    }

    std::thread hbWatch(upstreamHeartbeatWatchdog, std::ref(receiver));
    std::thread evalThread(windowedEvaluator, std::ref(receiver));
    /* Safe-release via stdin is always enabled: an operator can type 'release'
     * in the (now uncluttered) console, or a script can do 'echo release > …'
     * into the process's stdin/FIFO. */
    std::cout << "Safe-release stdin enabled ('release' on stdin, or echo into the FIFO)" << std::endl;
    std::thread releaseInput(safeReleaseInputLoop, std::ref(receiver));
    std::cout << "Startup safe-release enabled (clears stale latch on first peer contact)" << std::endl;
    std::thread startupRelease_thr(startupSafeReleaseLoop, std::ref(receiver));

    /* Run the event loop (blocks until running_ becomes false) */
    receiver.run();

    /* Clean up — close socket, join threads */
    std::cerr << "Shutting down..." << std::endl;
    receiver.stop();

    evalThread.join();
    hbWatch.join();
    if (releaseInput.joinable())
        releaseInput.join();
    if (startupRelease_thr.joinable())
        startupRelease_thr.join();

    g_receiver.store(nullptr, std::memory_order_release);
    return 0;
}
