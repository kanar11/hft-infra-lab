/*
 * orderbook_flat — sanity test + head-to-head benchmark FlatOrderBook
 *                   vs baseline std::map.
 *
 * What we test:
 *   1. sanity   — small scenarios (best_bid/ask, matching, cancel/modify
 *                 with ID, book invariants). Always run.
 *   2. uniform  — N random orders, prices uniformly 10000..10199 ticks.
 *                 Realistic for stress-testing the matching engine.
 *   3. realistic — N random orders with a distribution clustered around the mid:
 *                 60% ±10 ticks (at market), 30% ±50, 9% ±100, 1% long tail.
 *                 Closer to what you see in real order flow.
 *
 * Each bench has TWO passes:
 *   a) throughput — clock only once before/after the loop → mean ns/op
 *   b) latency    — clock per op → p50/p95/p99/p99.9 histogram
 *
 * Run:
 *   ./orderbook/orderbook_flat                # default: 1M ops
 *   ./orderbook/orderbook_flat 100000         # 100k ops (CI sanity bench)
 *   ./orderbook/orderbook_flat 5000000        # 5M ops (serious benchmark)
 */
#include "orderbook_flat.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>


// std::map reference (copy from benchmark_orderbook.cpp) — so the comparison
// is apples-to-apples with FlatOrderBook on the same order stream.
namespace ref {
class MapOrderBook {
    std::map<std::int32_t, std::int32_t, std::greater<std::int32_t>> bids;
    std::map<std::int32_t, std::int32_t>                              asks;
    std::uint64_t trades_ = 0;

public:
    void add_buy(std::int32_t price, std::int32_t qty) noexcept {
        bids[price] += qty;
        try_match();
    }
    void add_sell(std::int32_t price, std::int32_t qty) noexcept {
        asks[price] += qty;
        try_match();
    }
    std::uint64_t trades() const noexcept { return trades_; }

private:
    void try_match() noexcept {
        while (!bids.empty() && !asks.empty()) {
            auto bb = bids.begin();
            auto ba = asks.begin();
            if (bb->first < ba->first) break;
            const std::int32_t fill = std::min(bb->second, ba->second);
            bb->second -= fill;
            ba->second -= fill;
            ++trades_;
            if (bb->second == 0) bids.erase(bb);
            if (ba->second == 0) asks.erase(ba);
        }
    }
};
}  // namespace ref


// Deterministic order stream — LCG instead of std::mt19937 (faster,
// does not pollute the latency histogram with needless allocations).
struct Op { std::int32_t price; std::int32_t qty; bool is_buy; };

struct LCG {
    std::uint32_t state;
    explicit LCG(std::uint32_t seed = 0xC0FFEEu) : state(seed) {}
    std::uint32_t next() noexcept { state = state * 1664525u + 1013904223u; return state; }
};


// Uniform: prices uniformly 10000..10199 (200-tick band).
static std::vector<Op> make_ops_uniform(int n) {
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(n));
    LCG rng(0xC0FFEEu);
    for (int i = 0; i < n; ++i) {
        const std::int32_t price = 10000 + static_cast<std::int32_t>(rng.next() % 200);
        const std::int32_t qty   = 1 + static_cast<std::int32_t>(rng.next() % 100);
        // Take the side from a high bit: the LCG's bit 0 has period 2, so a
        // fixed number of draws per iteration makes (rng.next() & 1u) constant.
        const bool is_buy        = ((rng.next() >> 16) & 1u) != 0;
        ops.push_back({price, qty, is_buy});
    }
    return ops;
}


// Realistic: most orders near the mid, long tail.
//   mid = 10100, piecewise distribution:
//     bucket 0 (60%): mid ± 10  ticks        → "at market"
//     bucket 1 (30%): mid ± 50  ticks        → "mid-spread"
//     bucket 2 (9%):  mid ± 100 ticks        → "far quotes"
//     bucket 3 (1%):  10000..10199 uniform   → "outlier / long tail"
static std::vector<Op> make_ops_realistic(int n) {
    constexpr std::int32_t MID = 10100;
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(n));
    LCG rng(0xBADCAFEu);
    for (int i = 0; i < n; ++i) {
        const std::uint32_t bucket_roll = rng.next() % 100u;
        std::int32_t offset;
        if      (bucket_roll < 60u) offset = static_cast<std::int32_t>(rng.next() % 21u) - 10;
        else if (bucket_roll < 90u) offset = static_cast<std::int32_t>(rng.next() % 101u) - 50;
        else if (bucket_roll < 99u) offset = static_cast<std::int32_t>(rng.next() % 201u) - 100;
        else                        offset = static_cast<std::int32_t>(rng.next() % 200u) - 100;
        std::int32_t price = MID + offset;
        if (price < 10000) price = 10000;
        if (price > 10199) price = 10199;
        const std::int32_t qty   = 1 + static_cast<std::int32_t>(rng.next() % 100);
        // High-bit side draw — see make_ops_uniform. With 4 draws per iteration
        // bit 0's parity never changes, so every order landed on one side.
        const bool is_buy        = ((rng.next() >> 16) & 1u) != 0;
        ops.push_back({price, qty, is_buy});
    }
    return ops;
}


// Throughput pass — one clock before/after, a single ns/op number.
template <typename Book>
static std::int64_t run_throughput(Book& book, const std::vector<Op>& ops) noexcept {
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (const Op& o : ops) {
        if (o.is_buy) book.add_buy(o.price, o.qty);
        else          book.add_sell(o.price, o.qty);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}


struct LatencyStats {
    double mean_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
    double p99_9_ns;
    double max_ns;
};


// Latency pass — clock per op, sort, percentiles. Samples every N-th operation
// so the clock cost does not dominate sub-100ns measurements. A 100k-op sample
// gives statistically significant percentiles for any distribution.
template <typename Book>
static LatencyStats run_latency(Book& book, const std::vector<Op>& ops) {
    // Sample at most 100k operations, evenly spaced.
    const int sample_size = std::min(100000, static_cast<int>(ops.size()));
    const int stride      = std::max(1, static_cast<int>(ops.size()) / sample_size);

    std::vector<std::int64_t> samples;
    samples.reserve(static_cast<std::size_t>(sample_size));

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& o = ops[i];
        if (static_cast<int>(i) % stride == 0) {
            const auto t0 = std::chrono::high_resolution_clock::now();
            if (o.is_buy) book.add_buy(o.price, o.qty);
            else          book.add_sell(o.price, o.qty);
            const auto t1 = std::chrono::high_resolution_clock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        } else {
            if (o.is_buy) book.add_buy(o.price, o.qty);
            else          book.add_sell(o.price, o.qty);
        }
    }

    std::sort(samples.begin(), samples.end());
    const auto pct = [&](double p) -> double {
        if (samples.empty()) return 0.0;
        const std::size_t idx = std::min(samples.size() - 1,
            static_cast<std::size_t>(p * samples.size()));
        return static_cast<double>(samples[idx]);
    };

    std::int64_t sum = 0;
    for (const auto v : samples) sum += v;
    const double mean = samples.empty() ? 0.0
        : static_cast<double>(sum) / static_cast<double>(samples.size());

    return {mean, pct(0.50), pct(0.95), pct(0.99), pct(0.999),
            samples.empty() ? 0.0 : static_cast<double>(samples.back())};
}


// Sanity test (small) — a quick check that add/match/cancel/modify work.
// Kept in the identical form as before — this is a regression test.
static int sanity() {
    orderbook::FlatOrderBook<16384> b;
    b.add_buy(10050, 10);
    b.add_buy(10030,  5);
    b.add_sell(10100, 8);
    b.add_sell(10080, 12);
    if (b.best_bid() != 10050) return 1;
    if (b.best_ask() != 10080) return 2;
    if (b.trades()   != 0)     return 3;

    b.add_buy(10080, 15);  // crosses: 15 buy @ 10080 vs 12 ask @ 10080 → 1 trade, 3 buy left
    if (b.trades() != 1)             return 4;
    if (b.bid_qty_at(10080) != 3)    return 5;
    if (b.ask_qty_at(10080) != 0)    return 6;
    if (b.best_ask() != 10100)       return 7;
    if (b.best_bid() != 10080)       return 8;

    // ID-tracked submit / cancel / modify
    orderbook::FlatOrderBook<16384> c;
    if (!c.submit_with_id(101, 10050, 10, /*buy=*/true))            return 10;
    if (!c.submit_with_id(102, 10100,  8, /*buy=*/false))           return 11;
    if (c.best_bid() != 10050 || c.best_ask() != 10100)             return 12;
    if (c.tracked_orders() != 2)                                    return 13;

    if (!c.cancel(101))                                             return 14;
    if (c.bid_qty_at(10050) != 0)                                   return 15;
    if (c.best_bid() != orderbook::FlatOrderBook<16384>::NO_BID)    return 16;
    if (c.tracked_orders() != 1)                                    return 17;

    if (c.cancel(999))                                              return 18;  // unknown ID → no-op

    if (!c.modify(102, 10090, 16))                                  return 19;
    if (c.ask_qty_at(10100) != 0)                                   return 20;
    if (c.ask_qty_at(10090) != 16)                                  return 21;
    if (c.best_ask() != 10090)                                      return 22;
    return 0;
}


static void print_scenario(const char* name, int n,
                            const LatencyStats& flat_lat, std::int64_t flat_thru_ns,
                            std::uint64_t flat_trades,
                            const LatencyStats& map_lat,  std::int64_t map_thru_ns,
                            std::uint64_t map_trades) {
    const double flat_thru = static_cast<double>(flat_thru_ns) / n;
    const double map_thru  = static_cast<double>(map_thru_ns)  / n;
    const double speedup   = static_cast<double>(map_thru_ns) /
                             static_cast<double>(flat_thru_ns);

    std::printf("\n=== Scenario: %s  (n=%d ops) ===\n", name, n);
    std::printf("                       FlatOrderBook    std::map (ref)\n");
    std::printf("  Throughput (ns/op)   %12.1f    %12.1f    speedup %.2fx\n",
                flat_thru, map_thru, speedup);
    std::printf("  Latency  p50         %12.0f    %12.0f\n",  flat_lat.p50_ns,   map_lat.p50_ns);
    std::printf("  Latency  p95         %12.0f    %12.0f\n",  flat_lat.p95_ns,   map_lat.p95_ns);
    std::printf("  Latency  p99         %12.0f    %12.0f\n",  flat_lat.p99_ns,   map_lat.p99_ns);
    std::printf("  Latency  p99.9       %12.0f    %12.0f\n",  flat_lat.p99_9_ns, map_lat.p99_9_ns);
    std::printf("  Latency  max         %12.0f    %12.0f\n",  flat_lat.max_ns,   map_lat.max_ns);
    std::printf("  Latency  mean        %12.1f    %12.1f\n",  flat_lat.mean_ns,  map_lat.mean_ns);
    std::printf("  Trades               %12lu    %12lu\n",
                (unsigned long)flat_trades, (unsigned long)map_trades);
}


// Bench for one scenario: 4 passes (2 books × throughput + latency),
// each on a fresh book so the cache is warmed identically.
static void run_scenario(const char* name, int n, const std::vector<Op>& ops) {
    orderbook::FlatOrderBook<65536> flat_thru_book;
    const std::int64_t flat_thru_ns = run_throughput(flat_thru_book, ops);
    const std::uint64_t flat_trades = flat_thru_book.trades();

    orderbook::FlatOrderBook<65536> flat_lat_book;
    const LatencyStats flat_lat = run_latency(flat_lat_book, ops);

    ref::MapOrderBook map_thru_book;
    const std::int64_t map_thru_ns = run_throughput(map_thru_book, ops);
    const std::uint64_t map_trades = map_thru_book.trades();

    ref::MapOrderBook map_lat_book;
    const LatencyStats map_lat = run_latency(map_lat_book, ops);

    print_scenario(name, n, flat_lat, flat_thru_ns, flat_trades,
                            map_lat,  map_thru_ns,  map_trades);
}


int main(int argc, char* argv[]) {
    int n = (argc > 1) ? std::atoi(argv[1]) : 1'000'000;
    if (n <= 0) n = 1'000'000;

    if (int rc = sanity(); rc != 0) {
        std::fprintf(stderr, "sanity FAILED (code %d)\n", rc);
        return 1;
    }
    std::printf("sanity: OK\n");

    const auto ops_uniform   = make_ops_uniform(n);
    const auto ops_realistic = make_ops_realistic(n);

    run_scenario("UNIFORM    (price ~ U[10000, 10199])",    n, ops_uniform);
    run_scenario("REALISTIC  (60%/30%/9%/1% bucketed)",     n, ops_realistic);

    return 0;
}
