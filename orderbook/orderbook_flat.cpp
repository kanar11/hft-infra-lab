// orderbook_flat: smoke test + head-to-head benchmark against the
// std::map variant in orderbook.cpp.
//
// 1M random orders generated deterministically with an LCG, prices in
// a 200-tick band (10000..10200 = $100.00..$102.00) so both books
// hit the matching path frequently. Reports ns/op and the speedup
// FlatOrderBook achieves over the std::map baseline.

#include "orderbook_flat.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>


// --- std::map reference, copied from benchmark_orderbook.cpp ---------------

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


// --- Deterministic order stream --------------------------------------------

struct Op { std::int32_t price; std::int32_t qty; bool is_buy; };

static std::vector<Op> make_ops(int n) {
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(n));
    std::uint32_t rng = 0xC0FFEEu;
    auto roll = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
    for (int i = 0; i < n; ++i) {
        const std::int32_t price = 10000 + static_cast<std::int32_t>(roll() % 200);  // $100.00..$101.99
        const std::int32_t qty   = 1 + static_cast<std::int32_t>(roll() % 100);
        const bool is_buy        = (roll() & 1u) != 0;
        ops.push_back({price, qty, is_buy});
    }
    return ops;
}


// --- Benchmark drivers -----------------------------------------------------

template <typename Book>
static int64_t run(Book& book, const std::vector<Op>& ops) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (const Op& o : ops) {
        if (o.is_buy) book.add_buy(o.price, o.qty);
        else          book.add_sell(o.price, o.qty);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}


// --- Sanity test (small) ---------------------------------------------------

static int sanity() {
    // LEVELS=16384 fits the 10000..10200 test band with headroom.
    orderbook::FlatOrderBook<16384> b;
    b.add_buy(10050, 10);
    b.add_buy(10030,  5);
    b.add_sell(10100, 8);
    b.add_sell(10080, 12);
    // best_bid = 10050, best_ask = 10080, no crossing yet
    if (b.best_bid() != 10050) return 1;
    if (b.best_ask() != 10080) return 2;
    if (b.trades()   != 0)     return 3;

    // Aggressive buy crosses: 15 buy @ 10080 vs 12 ask @ 10080 → 1 trade, 3 buy left
    b.add_buy(10080, 15);
    if (b.trades() != 1)             return 4;
    if (b.bid_qty_at(10080) != 3)    return 5;
    if (b.ask_qty_at(10080) != 0)    return 6;
    if (b.best_ask() != 10100)       return 7;  // advanced past empty 10080
    if (b.best_bid() != 10080)       return 8;
    return 0;
}


// --- Main ------------------------------------------------------------------

int main(int argc, char* argv[]) {
    int n = (argc > 1) ? std::atoi(argv[1]) : 1'000'000;
    if (n <= 0) n = 1'000'000;

    if (int rc = sanity(); rc != 0) {
        std::fprintf(stderr, "sanity FAILED (code %d)\n", rc);
        return 1;
    }
    std::printf("sanity: OK\n");

    const auto ops = make_ops(n);

    // Warm caches once on each book before timing.
    orderbook::FlatOrderBook<65536> flat;
    const int64_t flat_ns = run(flat, ops);

    ref::MapOrderBook map_book;
    const int64_t map_ns = run(map_book, ops);

    const double flat_per_op = static_cast<double>(flat_ns) / n;
    const double map_per_op  = static_cast<double>(map_ns)  / n;
    const double speedup     = static_cast<double>(map_ns) / static_cast<double>(flat_ns);

    std::printf("\n=== FlatOrderBook vs std::map ===\n");
    std::printf("  ops:               %d (random, price 10000..10199)\n", n);
    std::printf("  FlatOrderBook:     %.1f ns/op   trades=%lu\n",
                flat_per_op, (unsigned long)flat.trades());
    std::printf("  std::map (ref):    %.1f ns/op   trades=%lu\n",
                map_per_op, (unsigned long)map_book.trades());
    std::printf("  speedup:           %.2fx\n", speedup);
    return 0;
}
