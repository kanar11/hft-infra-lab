/*
 * FIX 4.2 Protocol Parser Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o fix_demo fix_demo.cpp
 * Run:     ./fix_demo [number_of_parses]
 */

#include "fix_parser.hpp"
#include <vector>
#include <algorithm>
#include <cstdlib>

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


void test_parse_new_order() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(std::strcmp(msg.get_msg_type(), "D") == 0, "test_new_order_type");
    ASSERT(std::strcmp(msg.get_symbol(), "AAPL") == 0, "test_new_order_symbol");
    ASSERT(std::strcmp(msg.get_side(), "BUY") == 0, "test_new_order_side");
    ASSERT(msg.get_price() == 150.25, "test_new_order_price");
    ASSERT(msg.get_quantity() == 100, "test_new_order_qty");
}

void test_parse_sell() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|55=MSFT|54=2|44=380.50|38=50");
    ASSERT(std::strcmp(msg.get_side(), "SELL") == 0, "test_sell_side");
    ASSERT(std::strcmp(msg.get_symbol(), "MSFT") == 0, "test_sell_symbol");
    ASSERT(msg.get_price() == 380.50, "test_sell_price");
    ASSERT(msg.get_quantity() == 50, "test_sell_qty");
}

void test_parse_cancel() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=F|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(std::strcmp(msg.get_msg_type(), "F") == 0, "test_cancel_type");
}

void test_parse_execution() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=8|49=EXCHANGE|56=TRADER1|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(std::strcmp(msg.get_msg_type(), "8") == 0, "test_execution_type");
    ASSERT(std::strcmp(msg.get_symbol(), "AAPL") == 0, "test_execution_symbol");
}

void test_malformed_tags() {
    // Malformed tags should not crash — parser skips invalid entries
    // Nieprawidłowe tagi nie powinny crashować — parser pomija je
    FIXMessage msg;
    msg.parse("abc=xyz|35=D|=empty|55=AAPL|bad|44=100.0");
    ASSERT(std::strcmp(msg.get_msg_type(), "D") == 0, "test_malformed_type");
    ASSERT(std::strcmp(msg.get_symbol(), "AAPL") == 0, "test_malformed_symbol");
    ASSERT(msg.get_price() == 100.0, "test_malformed_price");
}

void test_empty_message() {
    FIXMessage msg;
    msg.parse("");
    ASSERT(std::strcmp(msg.get_msg_type(), "UNKNOWN") == 0, "test_empty_type");
    ASSERT(msg.field_count() == 0, "test_empty_fields");
}

void test_null_message() {
    FIXMessage msg;
    msg.parse(nullptr);
    ASSERT(std::strcmp(msg.get_msg_type(), "UNKNOWN") == 0, "test_null_type");
}

void test_field_count() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    ASSERT(msg.field_count() == 8, "test_field_count");
}

void test_generic_field_lookup() {
    FIXMessage msg;
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL");
    // Tag 49 = SenderCompID
    const char* sender = msg.get_field(49);
    ASSERT(sender != nullptr, "test_generic_field_exists");
    ASSERT(std::strcmp(sender, "TRADER1") == 0, "test_generic_field_value");
    // Tag 999 doesn't exist
    ASSERT(msg.get_field(999) == nullptr, "test_generic_field_missing");
}

void test_multiple_messages() {
    // Parse two different messages into separate objects
    // Parsuj dwie różne wiadomości do oddzielnych obiektów
    FIXMessage buy, sell;
    buy.parse("8=FIX.4.2|35=D|55=AAPL|54=1|44=150.25|38=100");
    sell.parse("8=FIX.4.2|35=D|55=TSLA|54=2|44=250.00|38=200");

    ASSERT(std::strcmp(buy.get_symbol(), "AAPL") == 0, "test_multi_buy_symbol");
    ASSERT(std::strcmp(sell.get_symbol(), "TSLA") == 0, "test_multi_sell_symbol");
    ASSERT(std::strcmp(buy.get_side(), "BUY") == 0, "test_multi_buy_side");
    ASSERT(std::strcmp(sell.get_side(), "SELL") == 0, "test_multi_sell_side");
}

void test_parse_speed() {
    FIXMessage msg;
    auto start = std::chrono::high_resolution_clock::now();
    msg.parse("8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100");
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_parse_speed");
    printf("    (latency: %ld ns)\n", ns);
}


void benchmark(int num_parses) {
    printf("\n=== FIX Parser Throughput Benchmark ===\n");
    printf("Parses: %d\n\n", num_parses);

    // 4 sample messages (cycle through them for realistic mix)
    // 4 przykładowe wiadomości (cyklicznie dla realistycznego miksu)
    const char* messages[] = {
        "8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100",
        "8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=MSFT|54=2|44=380.50|38=50",
        "8=FIX.4.2|35=F|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100",
        "8=FIX.4.2|35=8|49=EXCHANGE|56=TRADER1|55=TSLA|54=1|44=250.00|38=200",
    };

    std::vector<int64_t> latencies;
    latencies.reserve(num_parses);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_parses; ++i) {
        FIXMessage msg;
        auto start = std::chrono::high_resolution_clock::now();
        msg.parse(messages[i % 4]);
        auto end = std::chrono::high_resolution_clock::now();

        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        total_end - total_start).count();

    std::sort(latencies.begin(), latencies.end());
    int n = static_cast<int>(latencies.size());
    double avg = static_cast<double>(total_ns) / n;
    double throughput = n / (total_ns / 1e9) / 1e6;

    printf("--- Results ---\n");
    printf("  Total time:  %.2f ms\n", total_ns / 1e6);
    printf("  Avg:         %.0f ns/parse\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M msg/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    printf("=== FIX 4.2 Parser C++ Unit Tests ===\n\n");

    test_parse_new_order();
    test_parse_sell();
    test_parse_cancel();
    test_parse_execution();
    test_malformed_tags();
    test_empty_message();
    test_null_message();
    test_field_count();
    test_generic_field_lookup();
    test_multiple_messages();
    test_parse_speed();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);

    int num_parses = 1'000'000;
    if (argc > 1) {
        num_parses = std::atoi(argv[1]);
        if (num_parses <= 0) num_parses = 1'000'000;
    }

    benchmark(num_parses);

    return (tests_passed == tests_total) ? 0 : 1;
}
