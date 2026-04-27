/*
 * Trade Logger Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o logger_demo logger_demo.cpp
 * Run:     ./logger_demo [number_of_events]
 */

#include "trade_logger.hpp"
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


void test_log_and_retrieve() {
    TradeLogger logger;
    logger.log(EventType::ORDER_SUBMIT, 1, "AAPL", "BUY", 100, 150.25);
    ASSERT(logger.total_events() == 1, "test_log_and_retrieve_count");

    const TradeEvent* events;
    int n = logger.get_events(&events);
    ASSERT(n == 1, "test_log_and_retrieve_n");
    ASSERT(events[0].event_type == EventType::ORDER_SUBMIT, "test_log_event_type");
    ASSERT(std::strcmp(events[0].symbol, "AAPL") == 0, "test_log_symbol");
    ASSERT(events[0].price == 150.25, "test_log_price");
}

void test_filter_by_order_id() {
    TradeLogger logger;
    logger.log(EventType::ORDER_SUBMIT, 1, "AAPL");
    logger.log(EventType::ORDER_SUBMIT, 2, "TSLA");
    logger.log(EventType::ORDER_FILL, 1, "AAPL");

    TradeEvent buf[16];
    int n = logger.get_events_filtered(buf, 16, 1);
    ASSERT(n == 2, "test_filter_by_order_id");
    ASSERT(buf[0].order_id == 1 && buf[1].order_id == 1, "test_filter_order_ids_match");
}

void test_filter_by_event_type() {
    TradeLogger logger;
    logger.log(EventType::ORDER_SUBMIT, 1);
    logger.log(EventType::RISK_ACCEPT, 1);
    logger.log(EventType::ORDER_FILL, 1);
    logger.log(EventType::ORDER_SUBMIT, 2);

    TradeEvent buf[16];
    int fills = logger.get_events_filtered(buf, 16, 0,
        static_cast<int>(EventType::ORDER_FILL));
    ASSERT(fills == 1, "test_filter_fills");

    int submits = logger.get_events_filtered(buf, 16, 0,
        static_cast<int>(EventType::ORDER_SUBMIT));
    ASSERT(submits == 2, "test_filter_submits");
}

void test_filter_by_symbol() {
    TradeLogger logger;
    logger.log(EventType::ORDER_SUBMIT, 1, "AAPL");
    logger.log(EventType::ORDER_SUBMIT, 2, "TSLA");
    logger.log(EventType::ORDER_SUBMIT, 3, "AAPL");

    TradeEvent buf[16];
    int n = logger.get_events_filtered(buf, 16, 0, -1, "AAPL");
    ASSERT(n == 2, "test_filter_by_symbol");
}

void test_order_trail() {
    TradeLogger logger;
    logger.log(EventType::ORDER_SUBMIT, 1, "AAPL", "BUY", 100, 150.25);
    logger.log(EventType::RISK_ACCEPT, 1, "AAPL", "", 0, 0.0, "passed");
    logger.log(EventType::ORDER_FILL, 1, "AAPL", "BUY", 100, 150.25, "venue=NASDAQ");

    TradeEvent trail[16];
    int n = logger.get_order_trail(1, trail, 16);
    ASSERT(n == 3, "test_order_trail_count");
    ASSERT(trail[0].event_type == EventType::ORDER_SUBMIT, "test_trail_step1");
    ASSERT(trail[1].event_type == EventType::RISK_ACCEPT, "test_trail_step2");
    ASSERT(trail[2].event_type == EventType::ORDER_FILL, "test_trail_step3");
    ASSERT(std::strcmp(trail[2].details, "venue=NASDAQ") == 0, "test_trail_details");
}

void test_summary_stats() {
    TradeLogger logger;
    logger.log(EventType::SYSTEM_START);
    logger.log(EventType::ORDER_SUBMIT, 1, "AAPL");
    logger.log(EventType::ORDER_SUBMIT, 2, "TSLA");
    logger.log(EventType::RISK_REJECT, 2, "TSLA");
    logger.log(EventType::ORDER_FILL, 1, "AAPL");

    ASSERT(logger.total_events() == 5, "test_summary_total");
    ASSERT(logger.unique_orders() == 2, "test_summary_orders");
    ASSERT(logger.unique_symbols() == 2, "test_summary_symbols");
    ASSERT(logger.get_counter(EventType::ORDER_SUBMIT) == 2, "test_summary_submits");
    ASSERT(logger.get_counter(EventType::RISK_REJECT) == 1, "test_summary_rejects");
}

void test_empty_logger() {
    TradeLogger logger;
    ASSERT(logger.total_events() == 0, "test_empty_total");
    ASSERT(logger.unique_orders() == 0, "test_empty_orders");
    ASSERT(logger.unique_symbols() == 0, "test_empty_symbols");
    ASSERT(logger.time_span_ms() == 0.0, "test_empty_time_span");
}

void test_kill_switch_event() {
    TradeLogger logger;
    logger.log(EventType::KILL_SWITCH, 0, "", "", 0, 0.0, "manual_trigger");

    TradeEvent buf[4];
    int n = logger.get_events_filtered(buf, 4, 0,
        static_cast<int>(EventType::KILL_SWITCH));
    ASSERT(n == 1, "test_kill_switch_count");
    ASSERT(std::strcmp(buf[0].details, "manual_trigger") == 0, "test_kill_switch_details");
}

void test_sequence_counter() {
    TradeLogger logger;
    logger.log(EventType::ORDER_SUBMIT, 1);
    logger.log(EventType::ORDER_FILL, 1);
    logger.log(EventType::ORDER_SUBMIT, 2);
    ASSERT(logger.sequence() == 3, "test_sequence_counter");
}

void test_log_speed() {
    TradeLogger logger;
    auto start = std::chrono::high_resolution_clock::now();
    logger.log(EventType::ORDER_SUBMIT, 0, "BENCH", "BUY", 1, 1.0, "benchmark");
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_log_speed");
    printf("    (latency: %ld ns)\n", ns);
}

void test_event_type_str() {
    ASSERT(std::strcmp(event_type_str(EventType::ORDER_SUBMIT), "ORDER_SUBMIT") == 0,
           "test_event_type_str_submit");
    ASSERT(std::strcmp(event_type_str(EventType::KILL_SWITCH), "KILL_SWITCH") == 0,
           "test_event_type_str_kill");
}


void benchmark(int num_events) {
    printf("\n=== Trade Logger Throughput Benchmark ===\n");
    printf("Events: %d\n\n", num_events);

    TradeLogger logger;
    std::vector<int64_t> latencies;
    latencies.reserve(num_events);

    // Cycle through different event types for realistic mix
    // Cykl przez różne typy zdarzeń dla realistycznego miksu
    EventType types[] = {
        EventType::ORDER_SUBMIT, EventType::RISK_ACCEPT,
        EventType::ORDER_FILL, EventType::ORDER_CANCEL
    };
    const char* symbols[] = {"AAPL", "TSLA", "MSFT", "GOOG"};

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_events; ++i) {
        EventType t = types[i % 4];
        const char* sym = symbols[i % 4];
        const char* side = (i % 2 == 0) ? "BUY" : "SELL";

        auto start = std::chrono::high_resolution_clock::now();
        logger.log(t, i + 1, sym, side, 100 + (i % 10) * 10, 150.25);
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
    printf("  Avg:         %.0f ns/event\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M events/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    printf("=== Trade Logger C++ Unit Tests ===\n\n");

    test_log_and_retrieve();
    test_filter_by_order_id();
    test_filter_by_event_type();
    test_filter_by_symbol();
    test_order_trail();
    test_summary_stats();
    test_empty_logger();
    test_kill_switch_event();
    test_sequence_counter();
    test_log_speed();
    test_event_type_str();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);

    int num_events = 500'000;
    if (argc > 1) {
        num_events = std::atoi(argv[1]);
        if (num_events <= 0) num_events = 500'000;
    }

    benchmark(num_events);

    return (tests_passed == tests_total) ? 0 : 1;
}
