/*
 * exec_demo — comparison of execution algos TWAP vs VWAP on a mock market.
 *
 * Scenario: BUY 10000 AAPL over a 100-second window, 20 slots of 5s each.
 *
 * Mock market:
 *   - start price 22381 ticks ($223.81)
 *   - random walk ±5 ticks per second
 *   - market volume per second: U-shape (more in the morning and near the close)
 *   - each execution-algo fill executes at the average price of that second
 *
 * We measure:
 *   - realized VWAP of TWAP    (the average price of our fills)
 *   - realized VWAP of VWAP
 *   - market VWAP (benchmark — VWAP of all trades in this window)
 *   - slippage in basis points relative to the benchmark
 *
 * Expectation: VWAP should give a lower (better) slippage than TWAP,
 * BECAUSE the market also has a U-shape volume profile and VWAP uses it.
 *
 * Build: compiles with the rest of the lab via the Makefile (strategy/exec_demo).
 * Run:   ./strategy/exec_demo [seed]
 */
#include "exec_algo.hpp"
#include "../common/types.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>


// Deterministic LCG — so the demo is reproducible (CI gets the same
// scenario on every run, the slippage numbers do not change).
struct LCG {
    uint64_t state;
    explicit LCG(uint64_t seed) noexcept : state(seed ? seed : 1) {}
    uint64_t next() noexcept { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 16; }
    int32_t int_in(int32_t lo, int32_t hi) noexcept {
        return lo + static_cast<int32_t>(next() % static_cast<uint64_t>(hi - lo + 1));
    }
};


// Mock market tick — generates for each second of the window:
//   1. the average price of that second (random walk)
//   2. the volume that traded in that second (U-shape)
//   3. fragments of trades (a few per second, with price noise)
struct MarketTick {
    int32_t avg_price_ticks;   // average price of that second
    int32_t total_volume;      // how many shares traded in the market
    std::vector<std::pair<int32_t, int32_t>> trades;  // (price, qty) pairs
};


// Market stream generator.
class MockMarket {
    LCG                       rng_;
    int32_t                   current_price_;
    int                       duration_sec_;
    std::vector<MarketTick>   ticks_;

public:
    MockMarket(int32_t start_price_ticks, int duration_sec, uint64_t seed) noexcept
        : rng_(seed),
          current_price_(start_price_ticks),
          duration_sec_(duration_sec) {
        ticks_.reserve(static_cast<std::size_t>(duration_sec));
        generate();
    }

    const MarketTick& tick(int sec) const noexcept { return ticks_[static_cast<std::size_t>(sec)]; }
    int duration_sec() const noexcept { return duration_sec_; }

private:
    // U-shape volume per second: 30% in the morning (first 20%), 40% middle, 30% near the close.
    // Scaled so the average volume ~= 500 shares/s.
    int32_t volume_at(int sec) const noexcept {
        const double frac = static_cast<double>(sec) / duration_sec_;
        double mult;
        if (frac < 0.20)       mult = 1.5;   // open rush
        else if (frac > 0.80)  mult = 1.5;   // close rush
        else                   mult = 0.83;  // midday lull
        // ~500 shares/s on average (sum * sec), with mult => 750/417/750.
        return static_cast<int32_t>(500 * mult);
    }

    void generate() noexcept {
        for (int s = 0; s < duration_sec_; ++s) {
            // Price random walk: ±5 ticks per second.
            current_price_ += rng_.int_in(-5, 5);
            if (current_price_ < 1) current_price_ = 1;

            MarketTick t;
            t.avg_price_ticks = current_price_;
            t.total_volume    = volume_at(s);

            // Split the volume into 3-5 trades, each with a price near the mid.
            const int n_trades = rng_.int_in(3, 5);
            int32_t  remaining = t.total_volume;
            t.trades.reserve(static_cast<std::size_t>(n_trades));
            for (int i = 0; i < n_trades; ++i) {
                const int32_t qty = (i == n_trades - 1)
                    ? remaining
                    : remaining / (n_trades - i);
                const int32_t price = current_price_ + rng_.int_in(-2, 2);
                t.trades.push_back({price, qty});
                remaining -= qty;
            }
            ticks_.push_back(std::move(t));
        }
    }
};


// Simulator: runs a ParentOrder through the executor; for each child order it
// immediately executes it at the average price of the current tick. Returns filled_qty.
template <typename Executor>
static void simulate(Executor& exec, const MockMarket& market,
                     exec::MarketVWAPTracker& bench) noexcept {
    for (int sec = 0; sec < market.duration_sec(); ++sec) {
        const exec::ChildOrder o = exec.on_tick(sec);
        const auto& tick = market.tick(sec);

        // Feed the benchmark tracker ALL market trades of that second.
        // We do this ALWAYS, so the benchmark VWAP reflects the market volume
        // independently of our execution.
        for (const auto& tr : tick.trades) bench.on_trade(tr.first, tr.second);

        if (!o.valid) continue;

        // Fill at the second's average price. In production there would be random slippage,
        // partial fills, market impact (a larger order → a worse price).
        // Demo: we assume enough liquidity to execute the whole child.
        exec.apply_fill(o.qty, tick.avg_price_ticks);
    }
}


static void print_results(const char* algo_name, const exec::ExecStats& s,
                           const exec::ParentOrder& parent,
                           int32_t benchmark_vwap_ticks) {
    const int32_t realized = s.realized_vwap_ticks();
    const double  slip_bps = exec::slippage_bps(parent.side, realized, benchmark_vwap_ticks);

    std::printf("\n  --- %s ---\n", algo_name);
    std::printf("    Slices emitted   : %d  (skipped: %d)\n", s.slices_emitted, s.slices_skipped);
    std::printf("    Fills received   : %d\n", s.num_fills);
    std::printf("    Filled / total   : %lld / %d  (%.1f%%)\n",
                static_cast<long long>(s.filled_qty), parent.total_qty,
                parent.total_qty > 0 ? 100.0 * s.filled_qty / parent.total_qty : 0.0);
    std::printf("    Realized VWAP    : $%.4f  (%d ticks)\n",
                realized / 10000.0, realized);
    std::printf("    Slippage vs bench: %+.2f bps", slip_bps);
    if (slip_bps < 0)      std::printf("   (better than market)\n");
    else if (slip_bps > 0) std::printf("   (worse than market)\n");
    else                    std::printf("   (even with market)\n");
}


int main(int argc, char* argv[]) {
    const uint64_t seed = (argc > 1) ? static_cast<uint64_t>(std::atoll(argv[1])) : 42;

    // Parent order: BUY 10000 AAPL over 100 seconds, 20 slots of 5s each.
    exec::ParentOrder parent;
    std::strncpy(parent.symbol, "AAPL", sizeof(parent.symbol) - 1);
    parent.symbol[sizeof(parent.symbol) - 1] = '\0';
    parent.side          = Side::BUY;
    parent.total_qty     = 10000;
    parent.start_ts_sec  = 0;
    parent.duration_sec  = 100;
    parent.num_slices    = 20;

    std::printf("=== Execution Algos Demo (TWAP vs VWAP) ===\n");
    std::printf("Parent: %s %s %d shares over %d sec (%d slices)\n",
                parent.side == Side::BUY ? "BUY" : "SELL",
                parent.symbol, parent.total_qty, parent.duration_sec, parent.num_slices);
    std::printf("Mock market: random walk price + U-shape volume (seed=%llu)\n",
                (unsigned long long)seed);

    // Three independent markets with the same seed so TWAP and VWAP play on
    // exactly the same price scenario (fair comparison).
    MockMarket market_twap(22381, parent.duration_sec, seed);
    MockMarket market_vwap(22381, parent.duration_sec, seed);
    MockMarket market_bench(22381, parent.duration_sec, seed);

    // TWAP — no profile.
    exec::TWAPExecutor twap(parent);
    exec::MarketVWAPTracker bench_twap;
    simulate(twap, market_twap, bench_twap);

    // VWAP — with a U-shape profile (matched to what the market does).
    auto profile = exec::u_shape_profile(parent.num_slices);
    exec::VWAPExecutor vwap(parent, profile);
    exec::MarketVWAPTracker bench_vwap;
    simulate(vwap, market_vwap, bench_vwap);

    // Third tracker — independent, accumulates the market VWAP separately. bench_twap
    // and bench_vwap should have identical values (same trades) → one is enough,
    // but to be safe we display one of them as the "market benchmark".
    const int32_t benchmark = bench_twap.vwap_ticks();

    std::printf("\nMarket benchmark VWAP: $%.4f  (%d ticks, volume=%lld)\n",
                benchmark / 10000.0, benchmark,
                static_cast<long long>(bench_twap.volume()));

    print_results("TWAP (equal slots)",                     twap.stats(), parent, benchmark);
    print_results("VWAP (U-shape volume profile)",          vwap.stats(), parent, benchmark);

    // Sanity check: both must execute exactly total_qty.
    if (twap.stats().filled_qty != parent.total_qty ||
        vwap.stats().filled_qty != parent.total_qty) {
        std::fprintf(stderr, "\nERROR: under/over-fill — total_qty=%d, twap=%lld, vwap=%lld\n",
                     parent.total_qty,
                     static_cast<long long>(twap.stats().filled_qty),
                     static_cast<long long>(vwap.stats().filled_qty));
        return 1;
    }

    std::printf("\n");
    return 0;
}
