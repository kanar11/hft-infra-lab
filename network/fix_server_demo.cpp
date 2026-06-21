/*
 * fix_server_demo — spin up an EpollServer that ingests FIX-ish messages
 * over TCP, parses them with the lab's existing FIXMessage parser, and
 * acks each one. A client thread inside the same binary connects to
 * localhost, sends N messages, and checks every ack came back.
 *
 * Self-contained so CI doesn't need a separate client process.
 *
 * Framing
 * -------
 * FIX over a real wire is variable-length with a checksum trailer. To
 * keep the demo tight we use newline-terminated FIX-style strings —
 * one message per line. Production FIX engines would parse 9=N|...|10=NNN|.
 */
#include "epoll_server.hpp"
#include "../fix-protocol/fix_parser.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>


static constexpr int   DEMO_PORT = 19999;     // unprivileged, unlikely conflict
static constexpr int   N_MSGS    = 50;
static constexpr auto  WAIT      = std::chrono::milliseconds(500);


// Run client connection + N message send + response read. Returns acks received.
static int run_client() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let server bind

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEMO_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "client: connect failed\n");
        ::close(fd);
        return 0;
    }

    // Send N FIX-style new-order messages, newline-terminated
    for (int i = 0; i < N_MSGS; ++i) {
        char msg[128];
        const int len = std::snprintf(msg, sizeof(msg),
            "8=FIX.4.2|35=D|49=CLIENT|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=%d\n",
            100 + i);
        ::send(fd, msg, static_cast<std::size_t>(len), MSG_NOSIGNAL);
    }

    // Read acks back — server sends "ACK\n" per parsed message
    char   buf[8192];
    int    acks = 0;
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
    while (acks < N_MSGS && std::chrono::steady_clock::now() < deadline) {
        const ssize_t got = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (got > 0) {
            for (ssize_t k = 0; k < got; ++k)
                if (buf[k] == '\n') ++acks;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ::close(fd);
    return acks;
}


int main() {
    net::EpollServer server;
    if (!server.listen(DEMO_PORT)) {
        std::fprintf(stderr, "server: listen on %d failed (errno=%d)\n",
                     DEMO_PORT, errno);
        return 1;
    }
    std::printf("fix_server_demo: listening on 127.0.0.1:%d\n", DEMO_PORT);

    // Per-fd read accumulator: bytes streamed in arbitrary chunks across
    // recv() calls, we cut into lines at '\n' and parse each as FIX.
    std::string accum;
    std::atomic<int> parsed{0};

    // Capture the client's actual ack count (the thread's return value would
    // otherwise be discarded). Written by the client thread, read after join().
    int client_acks = 0;
    std::thread client_thread([&client_acks] { client_acks = run_client(); });

    server.run([&](int fd, const std::uint8_t* data, std::size_t n) {
        accum.append(reinterpret_cast<const char*>(data), n);
        std::size_t nl;
        while ((nl = accum.find('\n')) != std::string::npos) {
            const std::string line = accum.substr(0, nl);
            accum.erase(0, nl + 1);

            FIXMessage msg;
            msg.parse(line.c_str());
            if (msg.field_count() > 0) {
                ++parsed;
                net::EpollServer::send_all(fd, "ACK\n", 4);
            }
        }
        // Stop the loop once all messages were ack'd
        if (parsed.load() >= N_MSGS) {
            server.stop();   // exit the run() loop after this callback returns
            return net::Continuation::CLOSE;
        }
        return net::Continuation::KEEP;
    }, /*timeout_ms=*/50);

    // run() has returned; close FDs and join the client thread.
    server.teardown();
    client_thread.join();

    const int got = parsed.load();
    std::printf("=== fix_server_demo ===\n");
    std::printf("  messages sent : %d\n", N_MSGS);
    std::printf("  parsed by srv : %d\n", got);
    std::printf("  acks received : %d  (client side)\n", client_acks);

    // End-to-end success requires BOTH: the server parsed every message AND the
    // client received an ack for every one (full round-trip verified).
    return (got == N_MSGS && client_acks == N_MSGS) ? 0 : 1;
}
