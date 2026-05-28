/*
 * Smart Order Router Demo & Benchmark — C++
 *
 * Compile: g++ -O2 -std=c++17 -Wall -Wextra -o router_demo router_demo.cpp
 * Run:     ./router_demo [number_of_routes]
 */

#include "smart_router.hpp"
#include <vector>
#include <algorithm>
#include <numeric>

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


// Helper: set up router with 3 venues
SmartOrderRouter make_test_router() {
    SmartOrderRouter router;
    router.add_venue(Venue("NYSE",   500, 0.003));
    router.add_venue(Venue("NASDAQ", 200, -0.002));
    router.add_venue(Venue("BATS",   150, -0.001));

    router.update_quote("NYSE",   149.90, 150.10, 500, 500);
    router.update_quote("NASDAQ", 149.95, 150.05, 300, 300);
    router.update_quote("BATS",   149.85, 150.15, 200, 200);
    return router;
}

void test_best_price_buy() {
    auto router = make_test_router();
    auto d = router.route_order("BUY", 100, RoutingStrategy::BEST_PRICE);
    ASSERT(d.valid, "test_best_price_buy_valid");
    // NASDAQ has lowest ask (150.05)
    ASSERT(std::strcmp(d.venue, "NASDAQ") == 0, "test_best_price_buy");
}

void test_best_price_sell() {
    auto router = make_test_router();
    auto d = router.route_order("SELL", 100, RoutingStrategy::BEST_PRICE);
    ASSERT(d.valid, "test_best_price_sell_valid");
    // NASDAQ has highest bid (149.95)
    ASSERT(std::strcmp(d.venue, "NASDAQ") == 0, "test_best_price_sell");
}

void test_lowest_latency() {
    auto router = make_test_router();
    auto d = router.route_order("BUY", 100, RoutingStrategy::LOWEST_LATENCY);
    // BATS has lowest latency (150ns)
    ASSERT(std::strcmp(d.venue, "BATS") == 0, "test_lowest_latency");
}

void test_split_order() {
    auto router = make_test_router();
    // Split threshold default = 500, order 600 shares
    auto d = router.route_order("BUY", 600, RoutingStrategy::SPLIT);
    ASSERT(d.valid, "test_split_valid");
    ASSERT(d.quantity == 600, "test_split_qty");  // enough liquidity across venues
}

void test_no_venues() {
    SmartOrderRouter router;
    auto d = router.route_order("BUY", 100);
    ASSERT(!d.valid, "test_no_venues");
}

void test_inactive_venue() {
    SmartOrderRouter router;
    Venue v("DEAD", 100, 0.0);
    v.is_active = false;
    v.best_ask = 150.0;
    v.ask_size = 1000;
    router.add_venue(v);
    auto d = router.route_order("BUY", 100);
    ASSERT(!d.valid, "test_inactive_venue_skipped");
}

void test_fee_tiebreaker() {
    SmartOrderRouter router;
    router.add_venue(Venue("V1", 100,  0.003));
    router.add_venue(Venue("V2", 100, -0.002));
    router.update_quote("V1", 150.0, 150.10, 500, 500);
    router.update_quote("V2", 150.0, 150.10, 500, 500);  // same ask
    auto d = router.route_order("BUY", 100, RoutingStrategy::BEST_PRICE);
    // Same ask, V2 ma rebate (niższa cena efektywna) — wygrywa.
    ASSERT(std::strcmp(d.venue, "V2") == 0, "test_fee_tiebreaker");
}

// Kluczowy test realizmu: venue z NAJTAŃSZYM quote przegrywa przez wysoką
// opłatę. Raw best price wybrałby CHEAP_RAW (150.00), ale cena efektywna
// (z opłatą) faworyzuje GOOD_NET. Tak działa prawdziwy SOR.
void test_effective_price_flips_decision() {
    SmartOrderRouter router;
    router.add_venue(Venue("CHEAP_RAW", 100,  0.05));   // taniej, ale gruba opłata
    router.add_venue(Venue("GOOD_NET",  100, -0.01));   // drożej, ale rebate
    router.update_quote("CHEAP_RAW", 149.90, 150.00, 500, 500);  // ask 150.00, eff 150.05
    router.update_quote("GOOD_NET",  149.93, 150.03, 500, 500);  // ask 150.03, eff 150.02
    auto d = router.route_order("BUY", 100, RoutingStrategy::BEST_PRICE);
    ASSERT(std::strcmp(d.venue, "GOOD_NET") == 0, "test_effective_price_flips_buy");
    ASSERT(d.effective_price < 150.03, "test_effective_price_value");  // 150.02 all-in
}

// Nowe pola RouteDecision: total_fee = fee_per_share × quantity.
void test_route_reports_fee() {
    auto router = make_test_router();
    auto d = router.route_order("BUY", 100, RoutingStrategy::BEST_PRICE);  // → NASDAQ, fee -0.002
    ASSERT(d.valid, "test_fee_report_valid");
    ASSERT(d.num_venues == 1, "test_fee_report_single_venue");
    // NASDAQ rebate -0.002 × 100 = -0.20 (dostajemy rebate).
    ASSERT(d.total_fee < 0.0, "test_fee_report_rebate_negative");
}

// SPLIT rozbija na >1 venue gdy jedno nie ma dość płynności.
void test_split_reports_multiple_venues() {
    auto router = make_test_router();
    // 600 akcji: NASDAQ ma 300, BATS 200, NYSE 500 — musi rozbić.
    auto d = router.route_order("BUY", 600, RoutingStrategy::SPLIT);
    ASSERT(d.valid, "test_split_multi_valid");
    ASSERT(d.num_venues >= 2, "test_split_multi_venues");
    ASSERT(d.quantity == 600, "test_split_multi_qty");
}

void test_routing_speed() {
    auto router = make_test_router();
    auto start = std::chrono::high_resolution_clock::now();
    auto d [[maybe_unused]] = router.route_order("BUY", 100);
    auto end = std::chrono::high_resolution_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    ASSERT(ns < 1'000'000, "test_routing_speed");
    printf("    (latency: %ld ns)\n", ns);
}

void test_stats_tracking() {
    auto router = make_test_router();
    router.route_order("BUY", 100);
    router.route_order("SELL", 200);
    ASSERT(router.get_total_routes() == 2, "test_stats_tracking");
}


void benchmark(int num_routes) {
    printf("\n=== Smart Router Throughput Benchmark ===\n");
    printf("Routes: %d\n\n", num_routes);

    auto router = make_test_router();
    std::vector<int64_t> latencies;
    latencies.reserve(num_routes);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_routes; ++i) {
        const char* side = (i % 2 == 0) ? "BUY" : "SELL";

        auto start = std::chrono::high_resolution_clock::now();
        router.route_order(side, 100 + (i % 10) * 10);
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
    printf("  Avg:         %.0f ns/route\n", avg);
    printf("  p50:         %ld ns\n", latencies[n / 2]);
    printf("  p90:         %ld ns\n", latencies[(int)(n * 0.90)]);
    printf("  p99:         %ld ns\n", latencies[(int)(n * 0.99)]);
    printf("  Max:         %ld ns\n", latencies.back());
    printf("  Throughput:  %.1f M routes/sec\n", throughput);
}


int main(int argc, char* argv[]) {
    printf("=== Smart Router C++ Unit Tests ===\n\n");

    test_best_price_buy();
    test_best_price_sell();
    test_lowest_latency();
    test_split_order();
    test_no_venues();
    test_inactive_venue();
    test_fee_tiebreaker();
    test_effective_price_flips_decision();
    test_route_reports_fee();
    test_split_reports_multiple_venues();
    test_routing_speed();
    test_stats_tracking();

    printf("\n%d/%d tests passed", tests_passed, tests_total);
    if (tests_failed > 0) printf("  (%d FAILED)", tests_failed);
    printf("\n");

    int num_routes = 1'000'000;
    if (argc > 1) {
        num_routes = std::atoi(argv[1]);
        if (num_routes <= 0) num_routes = 1'000'000;
    }

    benchmark(num_routes);

    return (tests_failed == 0) ? 0 : 1;
}
