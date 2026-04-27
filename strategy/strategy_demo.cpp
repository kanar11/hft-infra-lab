/*
 * Mean Reversion Strategy Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o strategy_demo strategy_demo.cpp
 * Run:     ./strategy_demo [number_of_ticks]
 */

#include "mean_reversion.hpp"
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


// Helper: feed N identical prices to fill the window, then return strategy
// Pomocnik: podaj N identycznych cen aby wypełnić okno
void fill_window(MeanReversionStrategy& strat, const char* stock,
                 double price, int count) {
    for (int i = 0; i < count; ++i) {
        strat.on_market_data(stock, price);
    }
}


void test_hold_during_warmup() {
    // During warmup (window not full), all signals should be HOLD (invalid)
    // Podczas rozgrzewki (okno niepełne), wszystkie sygnały powinny być HOLD
    MeanReversionStrategy strat(20, 0.1, 100);
    for (int i = 0; i < 19; ++i) {
        auto sig = strat.on_market_data("AAPL", 150.0);
        ASSERT(!sig.valid, "test_hold_during_warmup");
    }
}

void test_hold_at_mean() {
    // Price exactly at SMA → HOLD (no deviation)
    // Cena dokładnie na SMA → HOLD (brak odchylenia)
    MeanReversionStrategy strat(5, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 5);
    auto sig = strat.on_market_data("AAPL", 150.0);
    ASSERT(!sig.valid, "test_hold_at_mean");
}

void test_buy_signal() {
    // Price drops well below SMA → BUY
    // Cena spada znacznie poniżej SMA → KUP
    MeanReversionStrategy strat(5, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 5);
    // SMA = 150.0, price = 149.0 → deviation = -0.67% (below -0.1% threshold)
    auto sig = strat.on_market_data("AAPL", 149.0);
    ASSERT(sig.valid, "test_buy_signal_valid");
    ASSERT(std::strcmp(sig.side, "BUY") == 0, "test_buy_signal_side");
    ASSERT(sig.price == 149.0, "test_buy_signal_price");
    ASSERT(sig.quantity == 100, "test_buy_signal_qty");
}

void test_sell_signal() {
    // Price rises well above SMA → SELL
    // Cena rośnie znacznie powyżej SMA → SPRZEDAJ
    MeanReversionStrategy strat(5, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 5);
    // SMA = 150.0, price = 151.0 → deviation = +0.67% (above +0.1% threshold)
    auto sig = strat.on_market_data("AAPL", 151.0);
    ASSERT(sig.valid, "test_sell_signal_valid");
    ASSERT(std::strcmp(sig.side, "SELL") == 0, "test_sell_signal_side");
    ASSERT(sig.price == 151.0, "test_sell_signal_price");
}

void test_sma_updates() {
    // SMA should shift as new prices arrive (circular buffer works)
    // SMA powinno się zmieniać gdy przychodzą nowe ceny (bufor kołowy działa)
    MeanReversionStrategy strat(3, 0.1, 100);
    // Prices: 100, 100, 100 → SMA = 100
    fill_window(strat, "X", 100.0, 3);
    // Add 103 → window is now [100, 100, 103] → SMA ≈ 101.0
    // 103 vs SMA 101 → deviation ≈ +1.98% → SELL
    auto sig = strat.on_market_data("X", 103.0);
    ASSERT(sig.valid, "test_sma_updates_signal");
    ASSERT(std::strcmp(sig.side, "SELL") == 0, "test_sma_updates_sell");
}

void test_multiple_stocks() {
    // Strategy should track multiple stocks independently
    // Strategia powinna śledzić wiele akcji niezależnie
    MeanReversionStrategy strat(5, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 5);
    fill_window(strat, "TSLA", 250.0, 5);

    // AAPL drops → BUY AAPL
    auto sig1 = strat.on_market_data("AAPL", 148.0);
    ASSERT(sig1.valid && std::strcmp(sig1.side, "BUY") == 0, "test_multi_aapl_buy");
    ASSERT(std::strcmp(sig1.stock, "AAPL") == 0, "test_multi_aapl_symbol");

    // TSLA rises → SELL TSLA
    auto sig2 = strat.on_market_data("TSLA", 253.0);
    ASSERT(sig2.valid && std::strcmp(sig2.side, "SELL") == 0, "test_multi_tsla_sell");
    ASSERT(std::strcmp(sig2.stock, "TSLA") == 0, "test_multi_tsla_symbol");

    ASSERT(strat.stock_count() == 2, "test_multi_stock_count");
}

void test_stats_tracking() {
    MeanReversionStrategy strat(5, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 5);  // 5 holds (warmup)
    strat.on_market_data("AAPL", 150.0);    // hold at mean
    strat.on_market_data("AAPL", 149.0);    // buy
    strat.on_market_data("AAPL", 151.0);    // sell

    auto& s = strat.stats();
    ASSERT(s.buys >= 1, "test_stats_buys");
    ASSERT(s.sells >= 1, "test_stats_sells");
    ASSERT(s.signals_generated >= 2, "test_stats_signals");
}

void test_custom_order_size() {
    MeanReversionStrategy strat(5, 0.1, 500);
    fill_window(strat, "AAPL", 150.0, 5);
    auto sig = strat.on_market_data("AAPL", 149.0);
    ASSERT(sig.valid, "test_custom_size_valid");
    ASSERT(sig.quantity == 500, "test_custom_order_size");
}

void test_custom_threshold() {
    // High threshold (1%) — small move should NOT trigger
    // Wysoki próg (1%) — mały ruch NIE powinien wyzwolić sygnału
    MeanReversionStrategy strat(5, 1.0, 100);  // 1.0% threshold
    fill_window(strat, "AAPL", 150.0, 5);
    // 0.5% move — below 1% threshold
    auto sig = strat.on_market_data("AAPL", 150.75);
    ASSERT(!sig.valid, "test_custom_threshold_hold");

    // 2% move — above 1% threshold → SELL
    auto sig2 = strat.on_market_data("AAPL", 153.5);
    ASSERT(sig2.valid, "test_custom_threshold_sell");
}

void test_deviation_sign() {
    // Buy signal should have negative deviation, sell should have positive
    MeanReversionStrategy strat(5, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 5);

    auto buy = strat.on_market_data("AAPL", 149.0);
    ASSERT(buy.valid && buy.deviation_pct < 0, "test_deviation_negative_buy");

    // Reset with fresh strategy for sell test
    MeanReversionStrategy strat2(5, 0.1, 100);
    fill_window(strat2, "AAPL", 150.0, 5);
    auto sell = strat2.on_market_data("AAPL", 151.0);
    ASSERT(sell.valid && sell.deviation_pct > 0, "test_deviation_positive_sell");
}

void test_decision_speed() {
    MeanReversionStrategy strat(20, 0.1, 100);
    fill_window(strat, "AAPL", 150.0, 20);

    auto start = std::chrono::high_resolution_clock::now();
    auto sig [[maybe_unused]] = strat.on_market_data("AAPL", 149.0);
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_decision_speed");
    printf("    (latency: %ld ns)\n", ns);
}


void benchmark(int num_ticks) {
    printf("\n=== Mean Reversion Throughput Benchmark ===\n");
    printf("Ticks: %d\n\n", num_ticks);

    MeanReversionStrategy strat(20, 0.1, 100);
    std::vector<int64_t> latencies;
    latencies.reserve(num_ticks);

    // Simple pseudo-random price generator (LCG — linear congruential generator)
    // Like /dev/urandom but deterministic — same seed always gives same sequence
    // Prosty pseudo-losowy generator cen — deterministyczny, zawsze ta sama sekwencja
    uint64_t seed = 42;
    double price = 150.0;

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_ticks; ++i) {
        // LCG random step: price walks ±0.15
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        double rnd = (static_cast<double>(seed >> 33) / static_cast<double>(1ULL << 31)) - 1.0;
        price += rnd * 0.15;

        auto start = std::chrono::high_resolution_clock::now();
        strat.on_market_data("AAPL", price, i * 1000);
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
    printf("  Avg:         %.0f ns/tick\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M ticks/sec\n", throughput);

    strat.print_stats();
}


int main(int argc, char* argv[]) {
    printf("=== Mean Reversion Strategy C++ Unit Tests ===\n\n");

    test_hold_during_warmup();
    test_hold_at_mean();
    test_buy_signal();
    test_sell_signal();
    test_sma_updates();
    test_multiple_stocks();
    test_stats_tracking();
    test_custom_order_size();
    test_custom_threshold();
    test_deviation_sign();
    test_decision_speed();

    printf("\n%d/%d tests passed\n", tests_passed, tests_total);

    int num_ticks = 1'000'000;
    if (argc > 1) {
        num_ticks = std::atoi(argv[1]);
        if (num_ticks <= 0) num_ticks = 1'000'000;
    }

    benchmark(num_ticks);

    return (tests_passed == tests_total) ? 0 : 1;
}
