/*
 * OMS Demo & Benchmark — C++ Order Management System
 * Demo i Benchmark OMS — System Zarządzania Zleceniami w C++
 *
 * Demonstrates:
 *   1. Full order lifecycle (submit → risk check → fill → P&L)
 *   2. Pre-trade risk rejection (order value + position limit)
 *   3. Position tracking with average cost basis
 *   4. Realized P&L calculation
 *   5. Throughput benchmark (millions of orders/sec)
 *
 * Compile / Kompilacja:
 *   g++ -O2 -std=c++17 -Wall -Wextra -o oms_demo oms_demo.cpp
 *
 * Run / Uruchomienie:
 *   ./oms_demo [number_of_orders]   # default: 1,000,000
 */

#include "oms.hpp"
#include <vector>
#include <algorithm>
#include <numeric>


// === Functional Tests ===
// === Testy Funkcjonalne ===

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

void test_submit_order() {
    OMS oms(1000, 100000.0);
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    ASSERT(order != nullptr, "test_submit_order");
    ASSERT(order->status == OrderStatus::SENT, "test_submit_status");
}

void test_risk_check_value() {
    OMS oms(1000, 10000.0);
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);  // 15000 > 10000
    ASSERT(order == nullptr, "test_risk_check_value");
}

void test_risk_check_position() {
    OMS oms(50, 100000.0);
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);  // 100 > 50
    ASSERT(order == nullptr, "test_risk_check_position");
}

void test_fill_order() {
    OMS oms;
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    uint64_t id = order->order_id;
    oms.fill_order(id, 100, 150.00);
    auto* filled = oms.get_order(id);
    ASSERT(filled->status == OrderStatus::FILLED, "test_fill_order");
    ASSERT(filled->filled_qty == 100, "test_fill_qty");
}

void test_partial_fill() {
    OMS oms;
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    uint64_t id = order->order_id;
    oms.fill_order(id, 50, 150.00);
    auto* partial = oms.get_order(id);
    ASSERT(partial->status == OrderStatus::PARTIAL, "test_partial_fill");
    ASSERT(partial->filled_qty == 50, "test_partial_qty");
}

void test_cancel_order() {
    OMS oms;
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    uint64_t id = order->order_id;
    oms.cancel_order(id);
    auto* cancelled = oms.get_order(id);
    ASSERT(cancelled->status == OrderStatus::CANCELLED, "test_cancel_order");
}

void test_position_tracking() {
    OMS oms;
    auto* order = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    oms.fill_order(order->order_id, 100, 150.00);
    auto* pos = oms.get_position("AAPL");
    ASSERT(pos != nullptr && pos->net_qty == 100, "test_position_tracking");
}

void test_pnl_basic() {
    // Buy 100 @ 150, sell 50 @ 155 → P&L = 250
    OMS oms(10000, 1000000.0);
    auto* o1 = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    oms.fill_order(o1->order_id, 100, 150.00);
    auto* o2 = oms.submit_order("AAPL", Side::SELL, 155.00, 50);
    oms.fill_order(o2->order_id, 50, 155.00);
    auto* pos = oms.get_position("AAPL");
    ASSERT(pos->net_qty == 50, "test_pnl_basic_qty");
    double pnl = to_float(pos->realized_pnl);
    ASSERT(std::abs(pnl - 250.00) < 0.01, "test_pnl_basic_pnl");
}

void test_pnl_full_close() {
    // Buy 100 @ 150, sell 100 @ 160 → P&L = 1000, flat
    OMS oms(10000, 1000000.0);
    auto* o1 = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    oms.fill_order(o1->order_id, 100, 150.00);
    auto* o2 = oms.submit_order("AAPL", Side::SELL, 160.00, 100);
    oms.fill_order(o2->order_id, 100, 160.00);
    auto* pos = oms.get_position("AAPL");
    ASSERT(pos->net_qty == 0, "test_pnl_close_flat");
    double pnl = to_float(pos->realized_pnl);
    ASSERT(std::abs(pnl - 1000.00) < 0.01, "test_pnl_close_pnl");
}

void test_pnl_multiple_buys() {
    // Buy 100@150 + 100@160, sell 100@170 → avg=155, P&L=1500
    OMS oms(10000, 1000000.0);
    auto* o1 = oms.submit_order("AAPL", Side::BUY, 150.00, 100);
    oms.fill_order(o1->order_id, 100, 150.00);
    auto* o2 = oms.submit_order("AAPL", Side::BUY, 160.00, 100);
    oms.fill_order(o2->order_id, 100, 160.00);
    auto* pos = oms.get_position("AAPL");
    double avg = to_float(pos->avg_price);
    ASSERT(std::abs(avg - 155.00) < 0.01, "test_multiple_avg");
    auto* o3 = oms.submit_order("AAPL", Side::SELL, 170.00, 100);
    oms.fill_order(o3->order_id, 100, 170.00);
    ASSERT(pos->net_qty == 100, "test_multiple_qty");
    double pnl = to_float(pos->realized_pnl);
    ASSERT(std::abs(pnl - 1500.00) < 0.01, "test_multiple_pnl");
}


// === Throughput Benchmark ===
// === Benchmark Przepustowości ===

void benchmark(int num_orders) {
    printf("\n=== OMS Throughput Benchmark ===\n");
    printf("Orders: %d\n\n", num_orders);

    OMS oms(num_orders * 2, static_cast<double>(num_orders) * 200.0);
    std::vector<int64_t> latencies;
    latencies.reserve(num_orders);

    // Benchmark: submit + fill cycle
    // Benchmark: cykl submit + fill
    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_orders; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        auto* order = oms.submit_order("AAPL", Side::BUY, 150.00 + (i % 100) * 0.01, 100);
        if (order) {
            oms.fill_order(order->order_id, 100, 150.00 + (i % 100) * 0.01);
        }

        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        total_end - total_start).count();

    // Calculate percentiles
    std::sort(latencies.begin(), latencies.end());
    int n = static_cast<int>(latencies.size());
    double avg = static_cast<double>(total_ns) / n;
    double throughput = n / (total_ns / 1e9) / 1e6;

    printf("--- Results ---\n");
    printf("  Total time:  %.2f ms\n", total_ns / 1e6);
    printf("  Avg:         %.0f ns/order (submit+fill)\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M orders/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    // === Run Tests ===
    printf("=== OMS C++ Unit Tests ===\n\n");

    test_submit_order();
    test_risk_check_value();
    test_risk_check_position();
    test_fill_order();
    test_partial_fill();
    test_cancel_order();
    test_position_tracking();
    test_pnl_basic();
    test_pnl_full_close();
    test_pnl_multiple_buys();

    printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) printf("  (%d FAILED)", tests_failed);
    printf("\n");

    // === Run Benchmark ===
    int num_orders = 1'000'000;
    if (argc > 1) {
        num_orders = std::atoi(argv[1]);
        if (num_orders <= 0) num_orders = 1'000'000;
    }

    benchmark(num_orders);

    return (tests_failed == 0) ? 0 : 1;
}
