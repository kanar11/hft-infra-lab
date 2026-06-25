/*
 * wss_demo — connects to a REAL exchange (Binance public WS stream),
 *            reads N trades in JSON format and prints the price.
 *
 * Requires libssl-dev and the HFT_USE_OPENSSL flag — see feed/README.md
 * section "Connecting to a real exchange".
 *
 * Default endpoint:
 *   wss://stream.binance.com:9443/ws/btcusdt@trade
 *
 * Format Binance trade event (uproszczone):
 *   {"e":"trade","E":1234,"s":"BTCUSDT","t":12,"p":"42000.50","q":"0.5",...}
 *
 * Build:
 *   make feed/wss_demo HFT_USE_OPENSSL=1
 *   # or directly:
 *   g++ -O2 -std=c++17 -DHFT_USE_OPENSSL -Wall -Wextra -pthread \
 *       -o feed/wss_demo feed/wss_demo.cpp -lssl -lcrypto
 *
 * Run:
 *   ./feed/wss_demo                                                # Binance BTCUSDT
 *   ./feed/wss_demo stream.binance.com 9443 /ws/ethusdt@trade      # ETH/USDT
 *   ./feed/wss_demo stream.binance.com 9443 /ws/btcusdt@trade 10   # only 10 trades
 */
#include "wss_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>


int main(int argc, char* argv[]) {
    const char* host  = (argc > 1) ? argv[1] : "stream.binance.com";
    const int   port  = (argc > 2) ? std::atoi(argv[2]) : 9443;
    const char* path  = (argc > 3) ? argv[3] : "/ws/btcusdt@trade";
    const int   limit = (argc > 4) ? std::atoi(argv[4]) : 20;

    std::printf("=== wss_demo — Binance WebSocket live trades ===\n");
    std::printf("Endpoint: wss://%s:%d%s\n", host, port, path);
    std::printf("Limit:    %d trade events\n\n", limit);

    feed::WssClient cli;
    const auto t0 = std::chrono::steady_clock::now();
    if (!cli.connect(host, port, path)) {
        std::fprintf(stderr, "ERROR: failed to connect (TCP/TLS/upgrade). "
                             "Check internet + certificates (apt install ca-certificates).\n");
        return 1;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double connect_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("[connected] TCP + TLS + upgrade: %.1f ms\n\n", connect_ms);

    char buf[2048];
    int  received = 0;
    while (received < limit) {
        const int n = cli.recv_text(buf, sizeof(buf) - 1);
        if (n <= 0) {
            std::fprintf(stderr, "stream closed (received=%d)\n", received);
            break;
        }
        buf[n] = '\0';

        // Extract price ("p") and quantity ("q") from the payload.
        const char* p = std::strstr(buf, "\"p\":\"");
        const char* q = std::strstr(buf, "\"q\":\"");
        char price[32] = {}, qty[32] = {};
        if (p) {
            const char* end = std::strchr(p + 5, '"');
            if (end) {
                const std::size_t l = std::min<std::size_t>(end - (p + 5), sizeof(price) - 1);
                std::memcpy(price, p + 5, l);
                price[l] = '\0';
            }
        }
        if (q) {
            const char* end = std::strchr(q + 5, '"');
            if (end) {
                const std::size_t l = std::min<std::size_t>(end - (q + 5), sizeof(qty) - 1);
                std::memcpy(qty, q + 5, l);
                qty[l] = '\0';
            }
        }
        std::printf("  [%3d]  price=%-10s  qty=%-12s\n", ++received, price, qty);
    }

    cli.close();
    std::printf("\nReceived %d trades. Exit.\n", received);
    return received == limit ? 0 : 1;
}
