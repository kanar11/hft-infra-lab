/*
 * OUCH 4.2 Protocol Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o ouch_demo ouch_demo.cpp
 * Run:     ./ouch_demo [number_of_encodes]
 */

#include "ouch_protocol.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cmath>

static int tests_passed = 0;
static int tests_total = 0;

#define ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("  FAIL: %s (%s)\n", msg, #cond); \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)


void test_enter_order_encoding() {
    uint8_t buf[64];
    int len = OUCHMessage::enter_order(buf, "ORD001", 'B', 100, "AAPL", 150.25);
    ASSERT(len == 33, "test_enter_order_len");
    ASSERT(buf[0] == 'O', "test_enter_order_type");
    // Check price encoding: 150.25 * 10000 = 1502500
    uint32_t price_raw = read_u32_be(buf + 28);
    ASSERT(price_raw == 1502500, "test_enter_order_price");
}

void test_cancel_order_encoding() {
    uint8_t buf[64];
    int len = OUCHMessage::cancel_order(buf, "ORD001", 0);
    ASSERT(len == 19, "test_cancel_order_len");
    ASSERT(buf[0] == 'X', "test_cancel_order_type");
}

void test_replace_order_encoding() {
    uint8_t buf[64];
    int len = OUCHMessage::replace_order(buf, "ORD001", "ORD002", 50, 151.00);
    ASSERT(len == 37, "test_replace_order_len");
    ASSERT(buf[0] == 'U', "test_replace_order_type");
}

void test_parse_accepted() {
    // Build an Accepted response manually (41 bytes)
    // Zbuduj odpowiedź Accepted ręcznie (41 bajtów)
    uint8_t data[64] = {0};
    data[0] = 'A';                                  // msg type
    // token "ORD001" padded to 14
    std::memcpy(data + 1, "ORD001        ", 14);
    data[15] = 'B';                                  // side = Buy
    write_u32_be(data + 16, 100);                    // shares
    std::memcpy(data + 20, "AAPL    ", 8);           // stock
    write_u32_be(data + 28, 1502500);                // price = 150.25
    data[32] = 'D';                                  // TIF
    write_u64_be(data + 33, 999);                    // order_ref

    auto resp = OUCHMessage::parse_response(data, 41);
    ASSERT(std::strcmp(resp.type, "ACCEPTED") == 0, "test_accepted_type");
    ASSERT(std::strcmp(resp.token, "ORD001") == 0, "test_accepted_token");
    ASSERT(std::strcmp(resp.side, "BUY") == 0, "test_accepted_side");
    ASSERT(resp.shares == 100, "test_accepted_shares");
    ASSERT(std::strcmp(resp.stock, "AAPL") == 0, "test_accepted_stock");
    ASSERT(std::fabs(resp.price - 150.25) < 0.01, "test_accepted_price");
    ASSERT(resp.order_ref == 999, "test_accepted_ref");
}

void test_parse_cancelled() {
    uint8_t data[64] = {0};
    data[0] = 'C';
    std::memcpy(data + 1, "ORD001        ", 14);
    write_u32_be(data + 15, 50);                     // shares cancelled
    data[19] = 'U';                                  // reason = User

    auto resp = OUCHMessage::parse_response(data, 20);
    ASSERT(std::strcmp(resp.type, "CANCELLED") == 0, "test_cancelled_type");
    ASSERT(std::strcmp(resp.token, "ORD001") == 0, "test_cancelled_token");
    ASSERT(resp.shares == 50, "test_cancelled_shares");
    ASSERT(resp.reason[0] == 'U', "test_cancelled_reason");
}

void test_parse_executed() {
    uint8_t data[64] = {0};
    data[0] = 'E';
    std::memcpy(data + 1, "ORD001        ", 14);
    write_u32_be(data + 15, 100);                    // shares
    write_u32_be(data + 19, 1502500);                // price = 150.25
    write_u64_be(data + 23, 12345);                  // match_number

    auto resp = OUCHMessage::parse_response(data, 31);
    ASSERT(std::strcmp(resp.type, "EXECUTED") == 0, "test_executed_type");
    ASSERT(resp.shares == 100, "test_executed_shares");
    ASSERT(std::fabs(resp.price - 150.25) < 0.01, "test_executed_price");
    ASSERT(resp.match_number == 12345, "test_executed_match");
}

void test_parse_truncated() {
    uint8_t data[8] = {'A', 0, 0, 0, 0, 0};
    auto resp = OUCHMessage::parse_response(data, 6);
    ASSERT(std::strcmp(resp.type, "ERROR") == 0, "test_truncated_type");
}

void test_parse_empty() {
    auto resp = OUCHMessage::parse_response(nullptr, 0);
    ASSERT(std::strcmp(resp.type, "ERROR") == 0, "test_empty_type");
}

void test_parse_unknown() {
    uint8_t data[4] = {'Z', 0, 0, 0};
    auto resp = OUCHMessage::parse_response(data, 4);
    ASSERT(std::strcmp(resp.type, "UNKNOWN") == 0, "test_unknown_type");
}

void test_price_precision() {
    uint8_t buf[64];
    OUCHMessage::enter_order(buf, "ORD001", 'B', 1, "TEST", 99.9999);
    uint32_t price_raw = read_u32_be(buf + 28);
    ASSERT(price_raw == 999999, "test_price_precision");
}

void test_roundtrip_encode_decode() {
    // Encode an enter order, then build a matching Accepted response and decode it
    // Zakoduj zlecenie, zbuduj pasującą odpowiedź Accepted i zdekoduj
    uint8_t order_buf[64];
    OUCHMessage::enter_order(order_buf, "ROUNDTRIP", 'S', 250, "MSFT", 380.50);

    // Simulate exchange accepting it
    uint8_t accept_buf[64] = {0};
    accept_buf[0] = 'A';
    std::memcpy(accept_buf + 1, order_buf + 1, 14);   // copy token
    accept_buf[15] = order_buf[15];                    // copy side
    std::memcpy(accept_buf + 16, order_buf + 16, 4);  // copy shares
    std::memcpy(accept_buf + 20, order_buf + 20, 8);  // copy stock
    std::memcpy(accept_buf + 28, order_buf + 28, 4);  // copy price
    accept_buf[32] = 'D';                              // TIF
    write_u64_be(accept_buf + 33, 42);                 // order ref

    auto resp = OUCHMessage::parse_response(accept_buf, 41);
    ASSERT(std::strcmp(resp.type, "ACCEPTED") == 0, "test_roundtrip_type");
    ASSERT(std::strcmp(resp.token, "ROUNDTRIP") == 0, "test_roundtrip_token");
    ASSERT(std::strcmp(resp.side, "SELL") == 0, "test_roundtrip_side");
    ASSERT(resp.shares == 250, "test_roundtrip_shares");
    ASSERT(std::strcmp(resp.stock, "MSFT") == 0, "test_roundtrip_stock");
    ASSERT(std::fabs(resp.price - 380.50) < 0.01, "test_roundtrip_price");
}

void test_encoding_speed() {
    uint8_t buf[64];
    auto start = std::chrono::high_resolution_clock::now();
    OUCHMessage::enter_order(buf, "ORD001", 'B', 100, "AAPL", 150.25);
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_encoding_speed");
    printf("    (latency: %ld ns)\n", ns);
}


void benchmark(int num_encodes) {
    printf("\n=== OUCH Encoding Throughput Benchmark ===\n");
    printf("Encodes: %d\n\n", num_encodes);

    uint8_t buf[64];
    std::vector<int64_t> latencies;
    latencies.reserve(num_encodes);

    const char* tokens[] = {"ORD0001", "ORD0002", "ORD0003", "ORD0004"};
    const char* stocks[] = {"AAPL", "MSFT", "TSLA", "GOOG"};

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_encodes; ++i) {
        char side = (i % 2 == 0) ? 'B' : 'S';

        auto start = std::chrono::high_resolution_clock::now();
        OUCHMessage::enter_order(buf, tokens[i % 4], side,
                                 100 + (i % 10) * 10, stocks[i % 4],
                                 150.25 + (i % 5) * 0.05);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        total_end - total_start).count();

    std::sort(latencies.begin(), latencies.end());
    int n = latencies.size();
    double avg = static_cast<double>(total_ns) / n;
    double throughput = n / (total_ns / 1e9) / 1e6;

    printf("--- Results ---\n");
    printf("  Total time:  %.2f ms\n", total_ns / 1e6);
    printf("  Avg:         %.0f ns/encode\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M msg/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    printf("=== OUCH 4.2 Protocol C++ Unit Tests ===\n\n");

    test_enter_order_encoding();
    test_cancel_order_encoding();
    test_replace_order_encoding();
    test_parse_accepted();
    test_parse_cancelled();
    test_parse_executed();
    test_parse_truncated();
    test_parse_empty();
    test_parse_unknown();
    test_price_precision();
    test_roundtrip_encode_decode();
    test_encoding_speed();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);

    int num_encodes = 1'000'000;
    if (argc > 1) {
        num_encodes = std::atoi(argv[1]);
        if (num_encodes <= 0) num_encodes = 1'000'000;
    }

    benchmark(num_encodes);

    return (tests_passed == tests_total) ? 0 : 1;
}
