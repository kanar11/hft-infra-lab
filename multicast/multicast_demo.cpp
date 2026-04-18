/**
 * Multicast Market Data Feed — unit tests + throughput benchmark.
 * Kanał danych rynkowych Multicast — testy jednostkowe + benchmark przepustowości.
 *
 * Compile / Kompilacja:
 *   g++ -O2 -std=c++17 -pthread -o multicast/multicast_demo multicast/multicast_demo.cpp
 *
 * Run / Uruchomienie:
 *   ./multicast/multicast_demo [iterations]
 */

#include "multicast.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <thread>
#include <atomic>

// ============================================================
// Test helpers / Helpery testowe
// ============================================================

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, name) do { \
    if (cond) { printf("  PASS: %s\n", name); ++g_pass; } \
    else      { printf("  FAIL: %s (line %d)\n", name, __LINE__); ++g_fail; } \
} while(0)

// ============================================================
// Unit tests / Testy jednostkowe
// ============================================================

void test_make_message() {
    auto msg = multicast::make_message(42, "AAPL", 1502500, 100, 'B', MsgType::ADD);
    ASSERT(msg.sequence == 42, "make_message_sequence");
    ASSERT(msg.price == 1502500, "make_message_price");
    ASSERT(msg.quantity == 100, "make_message_quantity");
    ASSERT(msg.side == 'B', "make_message_side");
    ASSERT(msg.msg_type == 'A', "make_message_type");
    ASSERT(std::memcmp(msg.symbol, "AAPL    ", 8) == 0, "make_message_symbol_padded");
    ASSERT(msg.timestamp_ns > 0, "make_message_timestamp_set");
}

void test_serialize_deserialize() {
    auto msg = multicast::make_message(1001, "MSFT", 4101900, 500, 'S', MsgType::TRADE);
    uint8_t buf[MC_MSG_SIZE];
    size_t len = multicast::serialize(msg, buf);
    ASSERT(len == MC_MSG_SIZE, "serialize_returns_40");

    MarketDataMessage decoded{};
    bool ok = multicast::deserialize(buf, len, decoded);
    ASSERT(ok, "deserialize_success");
    ASSERT(decoded.sequence == 1001, "roundtrip_sequence");
    ASSERT(decoded.timestamp_ns == msg.timestamp_ns, "roundtrip_timestamp");
    ASSERT(std::memcmp(decoded.symbol, "MSFT    ", 8) == 0, "roundtrip_symbol");
    ASSERT(decoded.price == 4101900, "roundtrip_price");
    ASSERT(decoded.quantity == 500, "roundtrip_quantity");
    ASSERT(decoded.side == 'S', "roundtrip_side");
    ASSERT(decoded.msg_type == 'T', "roundtrip_msg_type");
}

void test_negative_price() {
    // Negative price for short position / Ujemna cena dla krótkiej pozycji
    auto msg = multicast::make_message(0, "TEST", -1500000, 10, 'S', MsgType::QUOTE);
    uint8_t buf[MC_MSG_SIZE];
    multicast::serialize(msg, buf);
    MarketDataMessage decoded{};
    multicast::deserialize(buf, MC_MSG_SIZE, decoded);
    ASSERT(decoded.price == -1500000, "negative_price_roundtrip");
}

void test_deserialize_too_short() {
    uint8_t buf[10] = {};
    MarketDataMessage msg{};
    bool ok = multicast::deserialize(buf, 10, msg);
    ASSERT(!ok, "deserialize_too_short_fails");
}

void test_deserialize_empty() {
    MarketDataMessage msg{};
    bool ok = multicast::deserialize(nullptr, 0, msg);
    ASSERT(!ok, "deserialize_empty_fails");
}

void test_big_endian_encoding() {
    // Verify specific byte ordering / Sprawdź kolejność bajtów
    MarketDataMessage msg{};
    msg.sequence     = 0x0102030405060708ULL;
    msg.timestamp_ns = 0x1112131415161718ULL;
    std::memcpy(msg.symbol, "ABCDEFGH", 8);
    msg.price    = 0x2122232425262728LL;
    msg.quantity = 0x31323334;
    msg.side     = 'B';
    msg.msg_type = 'A';

    uint8_t buf[MC_MSG_SIZE];
    multicast::serialize(msg, buf);

    // Check sequence bytes are big-endian / Sprawdź że bajty sekwencji są big-endian
    ASSERT(buf[0] == 0x01 && buf[7] == 0x08, "sequence_big_endian");
    ASSERT(buf[8] == 0x11 && buf[15] == 0x18, "timestamp_big_endian");
    ASSERT(buf[16] == 'A' && buf[23] == 'H', "symbol_copied");
    ASSERT(buf[24] == 0x21 && buf[31] == 0x28, "price_big_endian");
    ASSERT(buf[32] == 0x31 && buf[35] == 0x34, "quantity_big_endian");
    ASSERT(buf[36] == 'B', "side_byte");
    ASSERT(buf[37] == 'A', "msg_type_byte");
}

void test_all_msg_types() {
    // Test each MsgType serializes correctly / Testuj każdy MsgType
    MsgType types[] = {MsgType::ADD, MsgType::DELETE, MsgType::TRADE,
                       MsgType::UPDATE, MsgType::QUOTE, MsgType::SYSTEM};
    char expected[] = {'A', 'D', 'T', 'U', 'Q', 'S'};
    bool all_ok = true;
    for (int i = 0; i < 6; ++i) {
        auto msg = multicast::make_message(0, "X", 0, 0, 'B', types[i]);
        uint8_t buf[MC_MSG_SIZE];
        multicast::serialize(msg, buf);
        MarketDataMessage decoded{};
        multicast::deserialize(buf, MC_MSG_SIZE, decoded);
        if (decoded.msg_type != expected[i]) all_ok = false;
    }
    ASSERT(all_ok, "all_msg_types_roundtrip");
}

void test_symbol_short() {
    // Short symbol gets space-padded / Krótki symbol dopełniony spacjami
    auto msg = multicast::make_message(0, "FB", 0, 0, 'B', MsgType::ADD);
    ASSERT(std::memcmp(msg.symbol, "FB      ", 8) == 0, "short_symbol_padded");
}

void test_symbol_exact_8() {
    // Exactly 8-char symbol / Dokładnie 8-znakowy symbol
    auto msg = multicast::make_message(0, "ABCDEFGH", 0, 0, 'B', MsgType::ADD);
    ASSERT(std::memcmp(msg.symbol, "ABCDEFGH", 8) == 0, "exact_8_symbol");
}

void test_large_sequence() {
    auto msg = multicast::make_message(UINT64_MAX, "X", 0, 0, 'B', MsgType::ADD);
    uint8_t buf[MC_MSG_SIZE];
    multicast::serialize(msg, buf);
    MarketDataMessage decoded{};
    multicast::deserialize(buf, MC_MSG_SIZE, decoded);
    ASSERT(decoded.sequence == UINT64_MAX, "max_sequence_roundtrip");
}

void test_large_quantity() {
    auto msg = multicast::make_message(0, "X", 0, UINT32_MAX, 'B', MsgType::ADD);
    uint8_t buf[MC_MSG_SIZE];
    multicast::serialize(msg, buf);
    MarketDataMessage decoded{};
    multicast::deserialize(buf, MC_MSG_SIZE, decoded);
    ASSERT(decoded.quantity == UINT32_MAX, "max_quantity_roundtrip");
}

void test_latency_stats() {
    multicast::LatencyStats stats;
    stats.record(100);
    stats.record(200);
    stats.record(300);
    ASSERT(stats.count == 3, "latency_stats_count");
    ASSERT(stats.avg_ns() == 200, "latency_stats_avg");
    ASSERT(stats.min_ns == 100, "latency_stats_min");
    ASSERT(stats.max_ns == 300, "latency_stats_max");
}

void test_latency_stats_empty() {
    multicast::LatencyStats stats;
    ASSERT(stats.count == 0, "latency_stats_empty_count");
    ASSERT(stats.avg_ns() == 0, "latency_stats_empty_avg");
}

void test_udp_loopback() {
    // Send and receive via UDP localhost (not multicast, just basic socket test)
    // Wyślij i odbierz przez UDP localhost (nie multicast, podstawowy test gniazda)
    // NOTE: Soft test — may not work on restricted CI environments
    // UWAGA: Miękki test — może nie działać na ograniczonych środowiskach CI
    int send_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    int recv_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd < 0 || recv_fd < 0) {
        printf("  WARN: udp_loopback_socket_create (skipped — no socket access)\n");
        if (send_fd >= 0) ::close(send_fd);
        if (recv_fd >= 0) ::close(recv_fd);
        return;
    }

    int reuse = 1;
    ::setsockopt(recv_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(19876);  // Ephemeral test port / Efemeryczny port testowy
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(recv_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        printf("  WARN: udp_loopback_bind (skipped — port unavailable)\n");
        ::close(send_fd); ::close(recv_fd);
        return;
    }

    // Set receive timeout / Ustaw timeout odbioru
    struct timeval tv{0, 100000};  // 100ms
    ::setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send a message / Wyślij wiadomość
    auto msg = multicast::make_message(777, "NVDA", 9500000, 250, 'B', MsgType::ADD);
    uint8_t buf[MC_MSG_SIZE];
    multicast::serialize(msg, buf);
    ssize_t sent = ::sendto(send_fd, buf, MC_MSG_SIZE, 0,
                            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    // Receive it / Odbierz ją
    uint8_t recv_buf[MC_MSG_SIZE + 16];
    ssize_t recvd = ::recv(recv_fd, recv_buf, sizeof(recv_buf), 0);

    bool roundtrip_ok = false;
    if (sent == MC_MSG_SIZE && recvd >= static_cast<ssize_t>(MC_MSG_SIZE)) {
        MarketDataMessage decoded{};
        if (multicast::deserialize(recv_buf, static_cast<size_t>(recvd), decoded)) {
            roundtrip_ok = (decoded.sequence == 777 &&
                            decoded.price == 9500000 &&
                            decoded.quantity == 250 &&
                            decoded.side == 'B' &&
                            std::memcmp(decoded.symbol, "NVDA    ", 8) == 0);
        }
    }
    ASSERT(roundtrip_ok, "udp_loopback_roundtrip");

    ::close(send_fd);
    ::close(recv_fd);
}

// ============================================================
// Throughput benchmark / Benchmark przepustowości
// ============================================================

void benchmark_serialize(int iterations) {
    printf("\n=== Multicast Serialization Benchmark ===\n");
    printf("=== Benchmark serializacji Multicast ===\n\n");
    printf("  Messages: %d\n", iterations);

    // Pre-build messages / Przygotuj wiadomości
    auto msg = multicast::make_message(0, "AAPL", 1502500, 100, 'B', MsgType::ADD);
    uint8_t buf[MC_MSG_SIZE];

    // Warmup / Rozgrzewka
    for (int i = 0; i < 1000; ++i) {
        msg.sequence = static_cast<uint64_t>(i);
        multicast::serialize(msg, buf);
    }

    // Benchmark serialize / Benchmark serializacji
    std::vector<uint64_t> latencies(static_cast<size_t>(iterations));
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        msg.sequence = static_cast<uint64_t>(i);
        multicast::serialize(msg, buf);
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies[static_cast<size_t>(i)] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    printf("\n--- Serialize / Serializacja ---\n");
    printf("  Elapsed:   %.2f ms\n", elapsed_ms);
    printf("  p50:       %lu ns\n", latencies[n * 50 / 100]);
    printf("  p90:       %lu ns\n", latencies[n * 90 / 100]);
    printf("  p99:       %lu ns\n", latencies[n * 99 / 100]);
    printf("  Max:       %lu ns\n", latencies[n - 1]);
    printf("  Throughput: %.1f M msg/sec\n",
           iterations / elapsed_ms / 1000.0);

    // Benchmark deserialize / Benchmark deserializacji
    MarketDataMessage decoded{};
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        multicast::deserialize(buf, MC_MSG_SIZE, decoded);
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies[static_cast<size_t>(i)] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    end = std::chrono::high_resolution_clock::now();
    elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    std::sort(latencies.begin(), latencies.end());
    printf("\n--- Deserialize / Deserializacja ---\n");
    printf("  Elapsed:   %.2f ms\n", elapsed_ms);
    printf("  p50:       %lu ns\n", latencies[n * 50 / 100]);
    printf("  p90:       %lu ns\n", latencies[n * 90 / 100]);
    printf("  p99:       %lu ns\n", latencies[n * 99 / 100]);
    printf("  Max:       %lu ns\n", latencies[n - 1]);
    printf("  Throughput: %.1f M msg/sec\n",
           iterations / elapsed_ms / 1000.0);

    // Benchmark full roundtrip (serialize + deserialize)
    // Benchmark pełnego roundtripu (serializacja + deserializacja)
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        msg.sequence = static_cast<uint64_t>(i);
        multicast::serialize(msg, buf);
        multicast::deserialize(buf, MC_MSG_SIZE, decoded);
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies[static_cast<size_t>(i)] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    end = std::chrono::high_resolution_clock::now();
    elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

    std::sort(latencies.begin(), latencies.end());
    printf("\n--- Full Roundtrip (serialize+deserialize) ---\n");
    printf("--- Pełny roundtrip (serializacja+deserializacja) ---\n");
    printf("  Elapsed:   %.2f ms\n", elapsed_ms);
    printf("  p50:       %lu ns\n", latencies[n * 50 / 100]);
    printf("  p90:       %lu ns\n", latencies[n * 90 / 100]);
    printf("  p99:       %lu ns\n", latencies[n * 99 / 100]);
    printf("  Max:       %lu ns\n", latencies[n - 1]);
    printf("  Throughput: %.1f M msg/sec\n",
           iterations / elapsed_ms / 1000.0);

    // Prevent dead-code optimization / Zapobiegaj optymalizacji martwego kodu
    volatile uint64_t sink = decoded.sequence + decoded.price;
    (void)sink;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    int iterations = (argc > 1) ? std::atoi(argv[1]) : 100000;

    printf("=== Multicast Market Data Unit Tests ===\n");
    printf("=== Testy jednostkowe danych rynkowych Multicast ===\n\n");

    test_make_message();
    test_serialize_deserialize();
    test_negative_price();
    test_deserialize_too_short();
    test_deserialize_empty();
    test_big_endian_encoding();
    test_all_msg_types();
    test_symbol_short();
    test_symbol_exact_8();
    test_large_sequence();
    test_large_quantity();
    test_latency_stats();
    test_latency_stats_empty();
    test_udp_loopback();

    printf("\n%d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0) return 1;

    benchmark_serialize(iterations);
    return 0;
}
