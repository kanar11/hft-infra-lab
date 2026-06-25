/*
 * feed_demo — runs a mini WebSocket server on one thread and a WsClient
 * on another. The server accepts 1 connection, replies to the HTTP upgrade
 * (without verifying Sec-WebSocket-Accept — this is a lab), then sends N
 * "tickers" in JSON format like Binance: {"e":"trade","s":"BTCUSDT",...}.
 *
 * The client connects via WsClient, reads text frames, parses minimally
 * (sscanf on the price) and counts the received events. The test passes when the client
 * got all N messages.
 *
 * Why? It shows the lab's WS stack works end-to-end — without an external
 * exchange / proxy / curl. In real life, instead of mock_server there would be
 * stream://stream.binance.com:9443/ws/btcusdt@trade or the Coinbase WS API.
 *
 * Self-contained = CI doesn't need a second process or a network.
 */
#include "ws_client.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>


static constexpr int  DEMO_PORT = 19998;   // unprivileged, doesn't collide with fix_server_demo
static constexpr int  N_TRADES  = 50;
static constexpr auto WAIT      = std::chrono::milliseconds(2000);


// Send the whole buffer or return false. ::send may return short under congestion.
static bool send_all(int fd, const void* buf, std::size_t n) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t  sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}


// Build a WS text frame (FIN=1, opcode=1, no mask — server→client usually none).
// For N_TRADES all payloads are < 126 bytes, so a 2-byte header is enough.
static bool send_ws_text(int fd, const char* payload, std::size_t len) noexcept {
    std::uint8_t hdr[4];
    std::size_t  hdr_len;
    hdr[0] = 0x81;  // FIN + TEXT
    if (len < 126) {
        hdr[1]  = static_cast<std::uint8_t>(len);
        hdr_len = 2;
    } else {
        hdr[1]  = 126;
        hdr[2]  = static_cast<std::uint8_t>((len >> 8) & 0xFF);
        hdr[3]  = static_cast<std::uint8_t>(len & 0xFF);
        hdr_len = 4;
    }
    return send_all(fd, hdr, hdr_len) && send_all(fd, payload, len);
}


// Mock server: 1 accept, HTTP 101, N trades, close. Runs on a separate thread.
static void run_server(std::atomic<bool>& ready, std::atomic<int>& sent_count) {
    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;

    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(DEMO_PORT);

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "server: bind failed errno=%d\n", errno);
        ::close(srv);
        return;
    }
    if (::listen(srv, 1) != 0) { ::close(srv); return; }

    ready.store(true, std::memory_order_release);

    sockaddr_in cli{};
    socklen_t   clilen = sizeof(cli);
    const int   c = ::accept(srv, reinterpret_cast<sockaddr*>(&cli), &clilen);
    if (c < 0) { ::close(srv); return; }

    // Read the HTTP request until CRLF CRLF. We don't verify — we'll ignore it anyway.
    char req[2048];
    std::size_t got = 0;
    while (got < sizeof(req) - 1) {
        const ssize_t r = ::recv(c, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0) { ::close(c); ::close(srv); return; }
        got += static_cast<std::size_t>(r);
        req[got] = '\0';
        if (std::strstr(req, "\r\n\r\n")) break;
    }

    // The 101 response — Sec-WebSocket-Accept should be SHA1+base64 of the client's
    // key + the magic GUID. WsClient doesn't verify, so a constant value is OK.
    const char* resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    if (!send_all(c, resp, std::strlen(resp))) { ::close(c); ::close(srv); return; }

    // Send N "trades" — the format from the Binance trade stream.
    for (int i = 0; i < N_TRADES; ++i) {
        char json[160];
        const int n = std::snprintf(json, sizeof(json),
            "{\"e\":\"trade\",\"s\":\"BTCUSDT\",\"p\":\"%d.50\",\"q\":\"0.5\",\"t\":%d}",
            42000 + i, 1000 + i);
        if (n < 0 || !send_ws_text(c, json, static_cast<std::size_t>(n))) break;
        sent_count.fetch_add(1, std::memory_order_relaxed);
    }

    ::close(c);
    ::close(srv);
}


int main() {
    std::atomic<bool> ready{false};
    std::atomic<int>  sent{0};
    std::thread       srv_thread(run_server, std::ref(ready), std::ref(sent));

    // Spin until the server binds the port. A tight loop, but a few ms at most.
    while (!ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    feed::WsClient cli;
    if (!cli.connect("127.0.0.1", DEMO_PORT, "/ws/btcusdt@trade")) {
        std::fprintf(stderr, "client: connect/upgrade failed\n");
        srv_thread.join();
        return 1;
    }

    char buf[512];
    int  received = 0;
    int  last_price = 0;
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
    while (received < N_TRADES && std::chrono::steady_clock::now() < deadline) {
        const int n = cli.recv_text(buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';

        // A minimal JSON "parser" — enough to test we got a valid stream.
        int price = 0;
        const char* p = std::strstr(buf, "\"p\":\"");
        if (p) {
            std::sscanf(p + 5, "%d", &price);
            if (price > 0) last_price = price;
        }
        ++received;
    }
    cli.close();
    srv_thread.join();

    std::printf("=== feed_demo (WebSocket) ===\n");
    std::printf("  sent by mock : %d\n", sent.load());
    std::printf("  received     : %d\n", received);
    std::printf("  last price   : %d.50\n", last_price);

    return (received == N_TRADES) ? 0 : 1;
}
