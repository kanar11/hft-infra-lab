/*
 * Risk Manager Demo & Benchmark — C++ Implementation
 * Demo i Benchmark Menedżera Ryzyka — implementacja C++
 *
 * Compile / Kompilacja:
 *   g++ -O2 -std=c++17 -Wall -Wextra -o risk_demo risk_demo.cpp
 *
 * Run / Uruchomienie:
 *   ./risk_demo [number_of_checks]   # default: 1,000,000
 */

#include "risk_manager.hpp"
#include "../config/config_loader.hpp"
#include <vector>
#include <algorithm>
#include <numeric>

static RiskLimits risk_limits_from_config(const HFTConfig& cfg) {
    RiskLimits l;
    l.max_position_per_symbol = cfg.risk.max_position_per_symbol;
    l.max_portfolio_exposure  = cfg.risk.max_portfolio_exposure;
    l.max_daily_loss          = static_cast<int64_t>(cfg.risk.max_daily_loss);
    l.max_orders_per_second   = cfg.risk.max_orders_per_second;
    l.max_order_value         = static_cast<int64_t>(cfg.risk.max_order_value);
    l.max_drawdown_pct        = cfg.risk.max_drawdown_pct;
    return l;
}


// === Test Framework ===
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


// === Unit Tests ===

void test_allow_normal_order() {
    RiskManager rm;
    auto r = rm.check_order("AAPL", "BUY", 150.0, 100);
    ASSERT(r.action == RiskAction::ALLOW, "test_allow_normal_order");
}

void test_reject_order_value() {
    RiskLimits limits;
    limits.max_order_value = 10000;
    RiskManager rm(limits);
    auto r = rm.check_order("AAPL", "BUY", 150.0, 100);  // 15000 > 10000
    ASSERT(r.action == RiskAction::REJECT, "test_reject_order_value");
}

void test_reject_position_limit() {
    RiskLimits limits;
    limits.max_position_per_symbol = 50;
    RiskManager rm(limits);
    auto r = rm.check_order("AAPL", "BUY", 150.0, 100);  // 100 > 50
    ASSERT(r.action == RiskAction::REJECT, "test_reject_position_limit");
}

void test_reject_portfolio_exposure() {
    RiskLimits limits;
    limits.max_portfolio_exposure = 150;
    RiskManager rm(limits);
    // Submit 100 AAPL (pending, not yet filled)
    auto r1 = rm.check_order("AAPL", "BUY", 10.0, 100);
    ASSERT(r1.action == RiskAction::ALLOW, "test_portfolio_first_allow");
    rm.on_order_sent("AAPL", "BUY", 100);
    // Now try 100 TSLA — total exposure would be 200 > 150
    auto r2 = rm.check_order("TSLA", "BUY", 10.0, 100);
    ASSERT(r2.action == RiskAction::REJECT, "test_reject_portfolio_exposure");
}

void test_pending_blocks_position() {
    RiskLimits limits;
    limits.max_position_per_symbol = 100;
    RiskManager rm(limits);
    auto r1 = rm.check_order("AAPL", "BUY", 10.0, 100);
    ASSERT(r1.action == RiskAction::ALLOW, "test_pending_first_allow");
    rm.on_order_sent("AAPL", "BUY", 100);
    // realized=0, pending=100 → next BUY 1 would project to 101 > 100
    auto r2 = rm.check_order("AAPL", "BUY", 10.0, 1);
    ASSERT(r2.action == RiskAction::REJECT, "test_pending_blocks_position");
}

void test_cancel_releases_pending() {
    RiskLimits limits;
    limits.max_position_per_symbol = 100;
    RiskManager rm(limits);
    rm.on_order_sent("AAPL", "BUY", 100);
    rm.on_order_cancelled("AAPL", "BUY", 100);
    // pending back to 0 — same-size order should fit again
    auto r = rm.check_order("AAPL", "BUY", 10.0, 100);
    ASSERT(r.action == RiskAction::ALLOW, "test_cancel_releases_pending");
}

void test_fill_flows_pending_to_realized() {
    RiskManager rm;
    rm.on_order_sent("AAPL", "BUY", 100);
    rm.update_position("AAPL", "BUY", 30);  // partial fill
    ASSERT(rm.get_position("AAPL") == 30, "test_fill_realized");
    ASSERT(rm.get_pending("AAPL") == 70,  "test_fill_pending_remaining");
}

void test_circuit_breaker() {
    RiskLimits limits;
    limits.max_daily_loss = 1000;
    RiskManager rm(limits);
    rm.update_pnl(-1500.0);  // loss > 1000
    auto r = rm.check_order("AAPL", "BUY", 10.0, 1);
    ASSERT(r.action == RiskAction::REJECT, "test_circuit_breaker");
    ASSERT(rm.is_kill_switch_active(), "test_circuit_breaker_kills");
}

void test_kill_switch_manual() {
    RiskManager rm;
    rm.activate_kill_switch();
    auto r = rm.check_order("AAPL", "BUY", 10.0, 1);
    ASSERT(r.action == RiskAction::REJECT, "test_kill_switch_manual");
}

void test_kill_switch_deactivate() {
    RiskManager rm;
    rm.activate_kill_switch();
    rm.deactivate_kill_switch();
    auto r = rm.check_order("AAPL", "BUY", 10.0, 1);
    ASSERT(r.action == RiskAction::ALLOW, "test_kill_switch_deactivate");
}

void test_drawdown_limit() {
    RiskLimits limits;
    limits.max_drawdown_pct = 5.0;
    RiskManager rm(limits);
    rm.update_pnl(10000.0);   // peak = 10000
    rm.update_pnl(-1000.0);   // now 9000, drawdown = 10%
    auto r = rm.check_order("AAPL", "BUY", 10.0, 1);
    ASSERT(r.action == RiskAction::REJECT, "test_drawdown_limit");
    ASSERT(rm.is_kill_switch_active(), "test_drawdown_kills");
}

void test_reset_daily() {
    RiskManager rm;
    rm.update_pnl(-5000.0);
    rm.activate_kill_switch();
    rm.reset_daily();
    ASSERT(!rm.is_kill_switch_active(), "test_reset_daily_kill");
    ASSERT(rm.get_daily_pnl() == 0.0, "test_reset_daily_pnl");
}

void test_check_speed() {
    RiskManager rm;
    auto start = std::chrono::high_resolution_clock::now();
    auto r [[maybe_unused]] = rm.check_order("AAPL", "BUY", 150.0, 100);
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_check_speed");  // under 1ms
    printf("    (latency: %ld ns)\n", ns);
}


// === Throughput Benchmark ===

void benchmark(int num_checks) {
    printf("\n=== Risk Manager Throughput Benchmark ===\n");
    printf("Checks: %d\n\n", num_checks);

    RiskLimits limits;
    limits.max_position_per_symbol = num_checks * 2;
    limits.max_portfolio_exposure = num_checks * 2;
    limits.max_orders_per_second = num_checks * 2;
    RiskManager rm(limits);

    std::vector<int64_t> latencies;
    latencies.reserve(num_checks);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_checks; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        rm.check_order("AAPL", "BUY", 150.0 + (i % 100) * 0.01, 1);

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
    printf("  Avg:         %.0f ns/check\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M checks/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    printf("=== Risk Manager C++ Unit Tests ===\n\n");

    test_allow_normal_order();
    test_reject_order_value();
    test_reject_position_limit();
    test_reject_portfolio_exposure();
    test_pending_blocks_position();
    test_cancel_releases_pending();
    test_fill_flows_pending_to_realized();
    test_circuit_breaker();
    test_kill_switch_manual();
    test_kill_switch_deactivate();
    test_drawdown_limit();
    test_reset_daily();
    test_check_speed();

    printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) printf("  (%d FAILED)", tests_failed);
    printf("\n");

    // Load config and show active limits
    HFTConfig cfg = load_config("config.yaml");
    if (!cfg.loaded) cfg = load_config("../config.yaml");
    RiskLimits limits = risk_limits_from_config(cfg);
    printf("\nActive risk limits (from %s):\n", cfg.loaded ? cfg.source : "defaults");
    printf("  max_order_value:         %.0f\n", (double)limits.max_order_value);
    printf("  max_position_per_symbol: %d\n",   limits.max_position_per_symbol);
    printf("  max_orders_per_second:   %d\n",   limits.max_orders_per_second);
    printf("  max_daily_loss:          %.0f\n", (double)limits.max_daily_loss);

    int num_checks = 1'000'000;
    if (argc > 1) {
        num_checks = std::atoi(argv[1]);
        if (num_checks <= 0) num_checks = 1'000'000;
    }

    benchmark(num_checks);

    return (tests_failed == 0) ? 0 : 1;
}
