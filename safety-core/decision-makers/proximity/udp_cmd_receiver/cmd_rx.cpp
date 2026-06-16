/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Proximity UDP Command Receiver Application
 *
 * Simulates the humanoid robot's command interpreter.  Incoming SDM commands
 * are buffered and every EVAL_WINDOW_MS (100 ms) the window is evaluated
 * using a "most conservative wins" policy:
 *
 *   - Any CMD_STOP / CMD_HW_ERROR / CMD_SW_ERROR  →  ESTOP
 *   - Any CMD_REDUCE (no STOP)                    →  SLOW DOWN
 *   - All CMD_NORMAL                              →  NORMAL OPERATION
 *   - Empty window                                →  hold previous action
 *
 * Key Components:
 * - UDPReceiver Class: Manages the UDP socket and handles data reception.
 * - Windowed evaluator thread: samples buffered commands every 100 ms.
 * - poll()-based event loop: Non-blocking message reception.
 * - 64-byte packet format with acknowledgment system.
 */

#include <iostream>
#include <cerrno>
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

#include "proximity_cmd_pkt.h"

#define POLL_TIMEOUT_MS         50
#define EVAL_WINDOW_MS          100

static constexpr int64_t kUpstreamHbExpectedMs = 5000;

class UDPReceiver;
void upstreamHeartbeatWatchdog(UDPReceiver& receiver);
void windowedEvaluator(UDPReceiver& receiver);

static const char* actionLabel(unsigned char cmd)
{
    switch (cmd) {
    case CMD_STOP:     return "ESTOP";
    case CMD_HW_ERROR: return "ESTOP (HW_ERROR)";
    case CMD_SW_ERROR: return "ESTOP (SW_ERROR)";
    case CMD_REDUCE:   return "SLOW DOWN";
    case CMD_NORMAL:   return "NORMAL OPERATION";
    default:           return "UNKNOWN";
    }
}

/*
 * Global pointer so the signal handler can reach the receiver instance.
 * Set once in main() before signals are registered; read-only thereafter.
 */
static UDPReceiver* g_receiver = nullptr;

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

class UDPReceiver
{
private:
    int            sock_;
    unsigned int   listen_port_;
    std::atomic<bool> running_;
    std::mutex     sock_mtx_;

    static constexpr std::size_t max_length = 1024;
    char data_[max_length];

    std::mutex                    hb_mtx_;
    std::chrono::steady_clock::time_point last_hb_time_;
    std::atomic<bool>             hb_fault_latched_{false};

    std::mutex                    window_mtx_;
    std::vector<unsigned char>    window_cmds_;
    unsigned char                 last_action_{CMD_NORMAL};

public:
    UDPReceiver(unsigned int listen_port)
        : sock_(-1),
          listen_port_(listen_port),
          running_(true)
    {
        last_hb_time_ = std::chrono::steady_clock::now();
        window_cmds_.reserve(64);
        initSocket();
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

    void handleReceive(std::size_t bytes_recvd,
                       const struct sockaddr_in& sender_addr,
                       socklen_t sender_len)
    {
        if (bytes_recvd != COMMAND_PACKET_SIZE)
            return;

        const CmdPacket* pkt = reinterpret_cast<const CmdPacket*>(data_);

        /* Validate identifier */
        if (pkt->identifier != PROXIMITY_PACKET_IDENTIFIER) {
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

        if (receivedCmd == CMD_HEARTBEAT)
        {
            if (!hb_fault_latched_.load())
                markHeartbeatReceived();
            return;
        }

        if (!(receivedCmd == CMD_STOP || receivedCmd == CMD_REDUCE ||
              receivedCmd == CMD_NORMAL || receivedCmd == CMD_HW_ERROR ||
              receivedCmd == CMD_SW_ERROR))
            return;

        /* Print received packet info */
        std::cout << "Received Proximity command: 0x"
                  << std::hex << std::setfill('0') << std::setw(2)
                  << (int)receivedCmd << std::dec
                  << " - " << commandName(receivedCmd)
                  << ", SeqNo: " << seqNo
                  << ", UTC epoch: " << pkt->ts_seconds
                  << "." << std::setfill('0') << std::setw(6)
                  << pkt->ts_microseconds << std::endl;

        /* Print object records */
        for (int i = 0; i < COMMAND_NUM_OBJECTS; i++) {
            const ObjectRecord& obj = pkt->objects[i];
            std::cout << "  Object " << i
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

        ackPkt.identifier      = PROXIMITY_PACKET_IDENTIFIER;
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
            std::cout << "Sent acknowledgment for SeqNo: " << seqNo << std::endl;
        else
            std::cerr << "Failed to send acknowledgment: "
                      << strerror(errno) << std::endl;

        bufferCommand(receivedCmd);
    }

    friend void upstreamHeartbeatWatchdog(UDPReceiver&);
    friend void windowedEvaluator(UDPReceiver&);
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

        unsigned int nStop = 0, nReduce = 0, nNormal = 0, nError = 0;
        for (unsigned char c : cmds) {
            switch (c) {
            case CMD_STOP:     ++nStop;   break;
            case CMD_REDUCE:   ++nReduce; break;
            case CMD_NORMAL:   ++nNormal; break;
            case CMD_HW_ERROR:
            case CMD_SW_ERROR: ++nError;  break;
            default: break;
            }
        }

        unsigned char action;
        if (nStop > 0 || nError > 0)
            action = CMD_STOP;
        else if (nReduce > 0)
            action = CMD_REDUCE;
        else
            action = CMD_NORMAL;

        bool changed = (action != receiver.last_action_);
        receiver.last_action_ = action;

        std::cout << "------------------------------------------------------------------------\n"
                  << (changed ? ">>> " : "    ")
                  << "[EVAL] Action: " << actionLabel(action)
                  << "  |  window: " << cmds.size() << " cmd(s)"
                  << " [STOP=" << nStop
                  << " REDUCE=" << nReduce
                  << " NORMAL=" << nNormal;
        if (nError > 0)
            std::cout << " ERROR=" << nError;
        std::cout << "]"
                  << (changed ? "  *** STATE CHANGE ***" : "")
                  << "\n------------------------------------------------------------------------"
                  << std::endl;
    }
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
    if (g_receiver)
        g_receiver->requestStop();
}

static void printUsage(const char* prog)
{
    std::cerr << "Usage: " << prog << " [-p <PORT>] [--max_hb_failures <N>] [-h|--help]\n\n"
              << "Proximity UDP Command Receiver — listens for STOP/REDUCE/NORMAL commands.\n\n"
              << "Options:\n"
              << "  -p <PORT>              Listen port, 1-65535 (default: 12345).\n"
              << "  --max_hb_failures <N>  Upstream heartbeat miss limit, 1-255 (default: 10).\n"
              << "  -h, --help             Show this help message.\n";
}

int main(int argc, char *argv[])
{
    const char* prog = (argc > 0 && argv[0] != nullptr) ? argv[0] : "proximity_cmd_rx";
    unsigned int port = 12345;
    uint32_t maxHb = 10U;

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
                std::cerr << "error: -p: invalid port (use 1..65535)\n";
                printUsage(prog);
                return 1;
            }
            port = static_cast<unsigned int>(p);
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

    UDPReceiver receiver(port);
    g_receiver = &receiver;

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "Proximity UDP Command Receiver listening on port " << port
              << "  (evaluation window: " << EVAL_WINDOW_MS << " ms)" << std::endl;

    std::thread hbWatch(upstreamHeartbeatWatchdog, std::ref(receiver));
    std::thread evalThread(windowedEvaluator, std::ref(receiver));

    /* Run the event loop (blocks until running_ becomes false) */
    receiver.run();

    /* Clean up — close socket, join threads */
    std::cerr << "Shutting down..." << std::endl;
    receiver.stop();

    evalThread.join();
    hbWatch.join();

    g_receiver = nullptr;
    return 0;
}
