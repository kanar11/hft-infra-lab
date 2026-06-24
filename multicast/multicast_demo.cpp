/**
 * Multicast Market Data Feed — unit tests + throughput benchmark.
 *
 * Compile:
 *   g++ -O2 -std=c++17 -pthread -o multicast/multicast_demo multicast/multicast_demo.cpp
 *
 * Run:
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
// Test helpers
// ============================================================

static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("  FAIL: %s (%s)\n", msg, #cond); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

// ============================================================
// Unit tests
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
    // Negative price for short position
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
    // Verify specific byte ordering
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

    // Check sequence bytes are big-endian
    ASSERT(buf[0] == 0x01 && buf[7] == 0x08, "sequence_big_endian");
    ASSERT(buf[8] == 0x11 && buf[15] == 0x18, "timestamp_big_endian");
    ASSERT(buf[16] == 'A' && buf[23] == 'H', "symbol_copied");
    ASSERT(buf[24] == 0x21 && buf[31] == 0x28, "price_big_endian");
    ASSERT(buf[32] == 0x31 && buf[35] == 0x34, "quantity_big_endian");
    ASSERT(buf[36] == 'B', "side_byte");
    ASSERT(buf[37] == 'A', "msg_type_byte");
}

void test_all_msg_types() {
    // Test each MsgType serializes correctly
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
    // Short symbol gets space-padded
    auto msg = multicast::make_message(0, "FB", 0, 0, 'B', MsgType::ADD);
    ASSERT(std::memcmp(msg.symbol, "FB      ", 8) == 0, "short_symbol_padded");
}

void test_symbol_exact_8() {
    // Exactly 8-char symbol
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

// Sequence tracking — packet loss detection. Pure logic, no sockets.

void test_seqtrack_in_order() {
    multicast::SequenceTracker t;
    for (uint64_t s = 100; s < 110; ++s) {
        ASSERT(t.observe(s) == multicast::SequenceTracker::Status::OK, "seqtrack_in_order_ok");
    }
    ASSERT(t.received == 10, "seqtrack_in_order_received");
    ASSERT(t.gaps == 0, "seqtrack_in_order_no_gaps");
    ASSERT(t.lost == 0, "seqtrack_in_order_no_loss");
    ASSERT(t.loss_rate() == 0.0, "seqtrack_in_order_zero_loss_rate");
}

void test_seqtrack_gap() {
    multicast::SequenceTracker t;
    t.observe(1);                      // OK, expected=2
    t.observe(2);                      // OK, expected=3
    auto st = t.observe(7);            // gap: lost 3,4,5,6 (4 packets)
    ASSERT(st == multicast::SequenceTracker::Status::GAP, "seqtrack_gap_status");
    ASSERT(t.gaps == 1, "seqtrack_gap_count");
    ASSERT(t.lost == 4, "seqtrack_gap_lost_4");
    // After a gap we resync: 8,9 OK.
    ASSERT(t.observe(8) == multicast::SequenceTracker::Status::OK, "seqtrack_gap_resync");
    ASSERT(t.lost == 4, "seqtrack_gap_lost_stable");
}

void test_seqtrack_duplicate() {
    multicast::SequenceTracker t;
    t.observe(1);
    t.observe(2);
    t.observe(3);                      // expected=4
    auto st = t.observe(2);            // late/duplicate (2 < 4)
    ASSERT(st == multicast::SequenceTracker::Status::DUPLICATE, "seqtrack_dup_status");
    ASSERT(t.duplicates == 1, "seqtrack_dup_count");
    ASSERT(t.received == 3, "seqtrack_dup_received_unchanged");
}

void test_seqtrack_loss_rate() {
    multicast::SequenceTracker t;
    t.observe(0);                      // received=1
    t.observe(10);                     // gap: lost=9, received=2
    // loss_rate = lost / (received + lost) = 9 / (2 + 9) = 9/11.
    const double expected = 9.0 / 11.0;
    ASSERT(std::fabs(t.loss_rate() - expected) < 1e-9, "seqtrack_loss_rate");
}

// MoldUDP64 framing — the industrial standard (NASDAQ). Many messages/packet.

void test_mold_roundtrip_packet() {
    MarketDataMessage msgs[3];
    msgs[0] = multicast::make_message(100, "AAPL", 1500000, 100, 'B', MsgType::ADD);
    msgs[1] = multicast::make_message(101, "MSFT", 4100000,  50, 'S', MsgType::TRADE);
    msgs[2] = multicast::make_message(102, "NVDA", 9000000,  25, 'B', MsgType::ADD);

    uint8_t buf[512];
    size_t n = multicast::mold_serialize_packet(buf, sizeof(buf), "SESSION001", 100, msgs, 3);
    ASSERT(n == multicast::MOLD_HEADER_SIZE + 3 * (2 + MC_MSG_SIZE), "mold_packet_size");

    multicast::MoldUDP64Header h{};
    multicast::SequenceTracker trk;
    int delivered = 0;
    uint64_t first_seq = 0;
    int rc = multicast::mold_parse_packet(buf, n, h, &trk,
        [&](const MarketDataMessage& m) {
            if (delivered == 0) first_seq = m.sequence;
            ++delivered;
        });
    ASSERT(rc == 3, "mold_parse_count");
    ASSERT(h.sequence == 100, "mold_header_seq");
    ASSERT(std::memcmp(h.session, "SESSION001", 10) == 0, "mold_session");
    ASSERT(delivered == 3, "mold_messages_delivered");
    ASSERT(first_seq == 100, "mold_first_msg_seq");
    ASSERT(trk.expected_seq == 103, "mold_tracker_advanced");
    ASSERT(trk.gaps == 0, "mold_no_gaps");
}

void test_mold_heartbeat() {
    uint8_t buf[64];
    size_t n = multicast::mold_serialize_packet(buf, sizeof(buf), "SESSION001", 50, nullptr, 0);
    ASSERT(n == multicast::MOLD_HEADER_SIZE, "mold_heartbeat_size");

    multicast::MoldUDP64Header h{};
    multicast::SequenceTracker trk;
    int rc = multicast::mold_parse_packet(buf, n, h, &trk,
                                          [](const MarketDataMessage&){});
    ASSERT(rc == 0, "mold_heartbeat_no_messages");
    ASSERT(h.message_count == 0, "mold_heartbeat_count");
}

void test_mold_packet_gap() {
    // Packet-level gap detection: a whole datagram lost (msgs 4..9).
    multicast::SequenceTracker trk;
    trk.observe_packet(1, 3);    // msgs 1,2,3 → expected 4
    trk.observe_packet(10, 2);   // gap 4..9 (6 lost), msgs 10,11 → expected 12
    ASSERT(trk.gaps == 1, "mold_gap_count");
    ASSERT(trk.lost == 6, "mold_gap_lost_6");
    ASSERT(trk.expected_seq == 12, "mold_gap_expected");
}

void test_mold_duplicate_packet() {
    // Retransmission / A-B line: the same packet twice → the second is a duplicate.
    multicast::SequenceTracker trk;
    trk.observe_packet(1, 3);    // expected 4
    auto st = trk.observe_packet(1, 3);  // full duplicate (1..3 < 4)
    ASSERT(st == multicast::SequenceTracker::Status::DUPLICATE, "mold_dup_status");
    ASSERT(trk.duplicates == 1, "mold_dup_count");
    ASSERT(trk.expected_seq == 4, "mold_dup_expected_unchanged");
}

// A/B line arbitration — feed redundancy (industry standard).

void test_arbitration_dedup() {
    // Lines A and B send the same packets; the arbiter takes the first one, drops the dup.
    multicast::ArbitratedReceiver arb;
    using L = multicast::ArbitratedReceiver::Line;
    using R = multicast::ArbitratedReceiver::Result;
    ASSERT(arb.observe(L::A, 1, 3) == R::FRESH,     "arb_a_first_fresh");
    ASSERT(arb.observe(L::B, 1, 3) == R::DUPLICATE, "arb_b_same_dup");
    ASSERT(arb.observe(L::A, 4, 2) == R::FRESH,     "arb_a_next_fresh");
    ASSERT(arb.observe(L::B, 4, 2) == R::DUPLICATE, "arb_b_dup_again");
    ASSERT(arb.fresh_from_a() == 2, "arb_2_from_a");
    ASSERT(arb.deduped() == 2,      "arb_2_deduped");
}

void test_arbitration_failover() {
    // Line A loses packets 4-6, line B delivers them — the arbiter picks FRESH from B.
    multicast::ArbitratedReceiver arb;
    using L = multicast::ArbitratedReceiver::Line;
    using R = multicast::ArbitratedReceiver::Result;
    arb.observe(L::A, 1, 3);   // OK from A
    arb.observe(L::B, 1, 3);   // dup
    // A does not deliver 4-6 — it comes only from B
    ASSERT(arb.observe(L::B, 4, 3) == R::FRESH, "arb_b_failover_fresh");
    ASSERT(arb.fresh_from_b() >= 1, "arb_failover_credit_b");
    ASSERT(arb.failover_ratio() > 0.0, "arb_failover_ratio_positive");
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
    // NOTE: Soft test — may not work on restricted CI environments
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
    addr.sin_port        = htons(19876);  // Ephemeral test port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(recv_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        printf("  WARN: udp_loopback_bind (skipped — port unavailable)\n");
        ::close(send_fd); ::close(recv_fd);
        return;
    }

    // Set receive timeout
    struct timeval tv{0, 100000};  // 100ms
    ::setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send a message
    auto msg = multicast::make_message(777, "NVDA", 9500000, 250, 'B', MsgType::ADD);
    uint8_t buf[MC_MSG_SIZE];
    multicast::serialize(msg, buf);
    ssize_t sent = ::sendto(send_fd, buf, MC_MSG_SIZE, 0,
                            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    // Receive it
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
// Throughput benchmark
// ============================================================

void benchmark_serialize(int iterations) {
    printf("\n=== Multicast Serialization Benchmark ===\n\n");
    printf("  Messages: %d\n", iterations);

    // Pre-build messages
    auto msg = multicast::make_message(0, "AAPL", 1502500, 100, 'B', MsgType::ADD);
    uint8_t buf[MC_MSG_SIZE];

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        msg.sequence = static_cast<uint64_t>(i);
        multicast::serialize(msg, buf);
    }

    // Benchmark serialize
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
    printf("\n--- Serialize ---\n");
    printf("  Elapsed:   %.2f ms\n", elapsed_ms);
    printf("  p50:       %lu ns\n", latencies[n * 50 / 100]);
    printf("  p90:       %lu ns\n", latencies[n * 90 / 100]);
    printf("  p99:       %lu ns\n", latencies[n * 99 / 100]);
    printf("  Max:       %lu ns\n", latencies[n - 1]);
    printf("  Throughput: %.1f M msg/sec\n",
           iterations / elapsed_ms / 1000.0);

    // Benchmark deserialize
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
    printf("\n--- Deserialize ---\n");
    printf("  Elapsed:   %.2f ms\n", elapsed_ms);
    printf("  p50:       %lu ns\n", latencies[n * 50 / 100]);
    printf("  p90:       %lu ns\n", latencies[n * 90 / 100]);
    printf("  p99:       %lu ns\n", latencies[n * 99 / 100]);
    printf("  Max:       %lu ns\n", latencies[n - 1]);
    printf("  Throughput: %.1f M msg/sec\n",
           iterations / elapsed_ms / 1000.0);

    // Benchmark full roundtrip (serialize + deserialize)
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
    printf("  Elapsed:   %.2f ms\n", elapsed_ms);
    printf("  p50:       %lu ns\n", latencies[n * 50 / 100]);
    printf("  p90:       %lu ns\n", latencies[n * 90 / 100]);
    printf("  p99:       %lu ns\n", latencies[n * 99 / 100]);
    printf("  Max:       %lu ns\n", latencies[n - 1]);
    printf("  Throughput: %.1f M msg/sec\n",
           iterations / elapsed_ms / 1000.0);

    // Prevent dead-code optimization
    volatile uint64_t sink = decoded.sequence + decoded.price;
    (void)sink;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    int iterations = (argc > 1) ? std::atoi(argv[1]) : 100000;

    printf("=== Multicast Market Data Unit Tests ===\n\n");

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
    test_seqtrack_in_order();
    test_seqtrack_gap();
    test_seqtrack_duplicate();
    test_seqtrack_loss_rate();
    test_mold_roundtrip_packet();
    test_mold_heartbeat();
    test_mold_packet_gap();
    test_mold_duplicate_packet();
    test_arbitration_dedup();
    test_arbitration_failover();
    test_latency_stats();
    test_latency_stats_empty();
    test_udp_loopback();

    printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) printf("  (%d FAILED)", tests_failed);
    printf("\n");
    if (tests_failed > 0) return 1;

    if (iterations > 0)
        benchmark_serialize(iterations);
    return 0;
}
