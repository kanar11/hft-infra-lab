/*
 * benchmark_orderbook — reproducible throughput benchmark + threshold gate.
 *
 * Runs the SAME deterministic order stream (mt19937 seed 42, prices
 * 9900..10100 ticks, qty 1..100, coin-flip side) through two engines:
 *
 *   baseline — the didactic std::map book (the engine behind the historic
 *              17.8M orders/sec figure in orderbook/README.md)
 *   flat     — orderbook::FlatOrderBook<65536> (occupancy-bitmap cursors)
 *
 * The flat book is measured over several trials, each on a fresh instance
 * after an untimed warmup pass, and the MINIMUM trial (not the best) is
 * the gated figure — a PASS means "every run beats the bar", not "one
 * lucky run did". The two engines must also agree on the trade count
 * (deterministic cross-check; a mismatch fails regardless of mode).
 *
 * Usage:
 *   ./benchmark_orderbook                              # report only (CI-safe)
 *   ./benchmark_orderbook <orders> <trials>            # report only
 *   ./benchmark_orderbook <orders> <trials> <threshold_mops>
 *       gate mode: exit 1 unless min-trial flat throughput >= threshold.
 *       Beat-the-README gate:  ./benchmark_orderbook 1000000 7 17.8
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "orderbook_flat.hpp"

using Price = std::int32_t;

struct Order {
    int   id;
    Price price;
    int   quantity;
    bool  is_buy;
};

// Baseline: std::map book — allocates a red-black-tree node per price level
// and pays log(N) per lookup. Kept unchanged as the reference implementation
// the README's 17.8M orders/sec throughput figure was measured on.
class MapOrderBook {
    std::map<Price, int, std::greater<Price>> bids;
    std::map<Price, int>                      asks;
    std::uint64_t trades_ = 0;

public:
    void add_order(const Order& o) noexcept {
        if (o.is_buy) bids[o.price] += o.quantity;
        else          asks[o.price] += o.quantity;
        try_match();
    }
    std::uint64_t trades() const noexcept { return trades_; }

private:
    void try_match() noexcept {
        while (!bids.empty() && !asks.empty()) {
            auto bb = bids.begin();
            auto ba = asks.begin();
            if (bb->first < ba->first) break;
            const int fill = std::min(bb->second, ba->second);
            ++trades_;
            bb->second -= fill;
            ba->second -= fill;
            if (bb->second == 0) bids.erase(bb);
            if (ba->second == 0) asks.erase(ba);
        }
    }
};

// Deterministic stream: same generator/seed/ranges as the original
// benchmark, so results stay comparable with the historical figures.
static std::vector<Order> make_orders(int n) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> price_dist(9900, 10100);
    std::uniform_int_distribution<int>   qty_dist(1, 100);
    std::uniform_int_distribution<int>   side_dist(0, 1);

    std::vector<Order> orders;
    orders.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        orders.push_back({i, price_dist(rng), qty_dist(rng), side_dist(rng) == 1});
    }
    return orders;
}

struct TrialResult {
    double        mops;    // millions of orders per second
    std::uint64_t trades;
};

static TrialResult run_flat(const std::vector<Order>& orders) {
    // Fresh heap instance per trial: identical cold-book starting state,
    // and the 528 KB of arrays stay off the stack.
    auto book = std::make_unique<orderbook::FlatOrderBook<65536>>();
    const auto t0 = std::chrono::steady_clock::now();
    for (const Order& o : orders) {
        if (o.is_buy) book->add_buy(o.price, o.quantity);
        else          book->add_sell(o.price, o.quantity);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return {static_cast<double>(orders.size()) / sec / 1e6, book->trades()};
}

static TrialResult run_map(const std::vector<Order>& orders) {
    MapOrderBook book;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Order& o : orders) {
        book.add_order(o);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return {static_cast<double>(orders.size()) / sec / 1e6, book.trades()};
}

int main(int argc, char** argv) {
    int n      = (argc > 1) ? std::atoi(argv[1]) : 1'000'000;
    int trials = (argc > 2) ? std::atoi(argv[2]) : 5;
    const double threshold_mops = (argc > 3) ? std::atof(argv[3]) : 0.0;  // 0 = report only
    if (n <= 0)      n = 1'000'000;
    if (trials <= 0) trials = 5;

    const std::vector<Order> orders = make_orders(n);

    // Untimed warmup: pages in the order vector and heats the branch
    // predictors so trial 1 measures the same steady state as the rest.
    (void)run_flat(orders);

    std::printf("=== Order Book Benchmark (%d orders, %d trials) ===\n", n, trials);

    const TrialResult map_res = run_map(orders);
    std::printf("baseline std::map  : %8.2f M orders/sec  (%llu trades)\n",
                map_res.mops, static_cast<unsigned long long>(map_res.trades));

    std::vector<double> mops;
    mops.reserve(static_cast<std::size_t>(trials));
    std::uint64_t flat_trades = 0;
    for (int t = 0; t < trials; ++t) {
        const TrialResult r = run_flat(orders);
        flat_trades = r.trades;
        mops.push_back(r.mops);
        std::printf("flat trial %-2d      : %8.2f M orders/sec\n", t + 1, r.mops);
    }
    std::sort(mops.begin(), mops.end());
    const double mn = mops.front();
    const double md = mops[mops.size() / 2];
    const double mx = mops.back();
    std::printf("flat min/med/max   : %.2f / %.2f / %.2f M orders/sec  (%llu trades)\n",
                mn, md, mx, static_cast<unsigned long long>(flat_trades));

    // Deterministic cross-check: both engines saw the identical stream, so
    // any trade-count divergence is a matching bug, not noise. Fail hard.
    if (map_res.trades != flat_trades) {
        std::printf("FAIL: trade-count mismatch (map %llu vs flat %llu)\n",
                    static_cast<unsigned long long>(map_res.trades),
                    static_cast<unsigned long long>(flat_trades));
        return 1;
    }

    if (threshold_mops > 0.0) {
        const bool pass = mn >= threshold_mops;
        std::printf("gate %.2fM orders/sec: slowest trial %.2fM -> %s\n",
                    threshold_mops, mn, pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    return 0;
}
