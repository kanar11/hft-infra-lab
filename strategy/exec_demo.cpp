/*
 * exec_demo — porównanie execution algos TWAP vs VWAP na mock markecie.
 *
 * Scenariusz: BUY 10000 AAPL w oknie 100 sekund, 20 slotów po 5s.
 *
 * Mock market:
 *   - cena startowa 22381 ticków ($223.81)
 *   - random walk ±5 ticków per sekunda
 *   - wolumen rynku per sekunda: U-shape (więcej rano i pod koniec)
 *   - każdy fill execution algo wykonuje się po średniej cenie tej sekundy
 *
 * Mierzymy:
 *   - realized VWAP TWAP'a   (cena średnia z naszych fills'ów)
 *   - realized VWAP VWAP'a
 *   - market VWAP (benchmark — VWAP wszystkich tradeów w tym oknie)
 *   - slippage w basis points względem benchmark'u
 *
 * Oczekiwanie: VWAP powinien dać niższy (lepszy) slippage od TWAP,
 * BO market też ma U-shape volume profile a VWAP go używa.
 *
 * Build: kompiluje się z resztą labu przez Makefile (strategy/exec_demo).
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


// Deterministyczny LCG — żeby demo było reproducible (CI dostaje ten sam
// scenariusz na każdym runie, slippage liczby się nie zmieniają).
struct LCG {
    uint64_t state;
    explicit LCG(uint64_t seed) noexcept : state(seed ? seed : 1) {}
    uint64_t next() noexcept { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 16; }
    int32_t int_in(int32_t lo, int32_t hi) noexcept {
        return lo + static_cast<int32_t>(next() % static_cast<uint64_t>(hi - lo + 1));
    }
};


// Mock market tick — generuje dla każdej sekundy okna:
//   1. cenę średnią tej sekundy (random walk)
//   2. wolumen który handlował się w tej sekundzie (U-shape)
//   3. fragmenty trade'ów (kilka per sekundę, z noisem ceny)
struct MarketTick {
    int32_t avg_price_ticks;   // średnia cena tej sekundy
    int32_t total_volume;      // ile shares przehandlowano w rynku
    std::vector<std::pair<int32_t, int32_t>> trades;  // (price, qty) pary
};


// Generator strumienia rynkowego.
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
    // U-shape volume per sekundę: 30% rano (pierwsze 20%), 40% środek, 30% pod koniec.
    // Skalowane tak żeby średni wolumen ~= 500 shares/s.
    int32_t volume_at(int sec) const noexcept {
        const double frac = static_cast<double>(sec) / duration_sec_;
        double mult;
        if (frac < 0.20)       mult = 1.5;   // open rush
        else if (frac > 0.80)  mult = 1.5;   // close rush
        else                   mult = 0.83;  // midday lull
        // ~500 shares/s średnio (suma * sek), z mult => 750/417/750.
        return static_cast<int32_t>(500 * mult);
    }

    void generate() noexcept {
        for (int s = 0; s < duration_sec_; ++s) {
            // Random walk ceny: ±5 ticków per sekunda.
            current_price_ += rng_.int_in(-5, 5);
            if (current_price_ < 1) current_price_ = 1;

            MarketTick t;
            t.avg_price_ticks = current_price_;
            t.total_volume    = volume_at(s);

            // Rozbij wolumen na 3-5 trade'ów, każdy z ceną w okolicy mid'a.
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


// Symulator: przepuszcza ParentOrder przez executor, dla każdego child order
// natychmiast wykonuje go po średniej cenie aktualnego ticka. Zwraca filled_qty.
template <typename Executor>
static void simulate(Executor& exec, const MockMarket& market,
                     exec::MarketVWAPTracker& bench) noexcept {
    for (int sec = 0; sec < market.duration_sec(); ++sec) {
        const exec::ChildOrder o = exec.on_tick(sec);
        const auto& tick = market.tick(sec);

        // Karmimy benchmark tracker WSZYSTKIMI tradesami rynku tej sekundy.
        // Robimy to ZAWSZE, żeby benchmark VWAP odzwierciedlał wolumen rynku
        // niezależnie od naszej egzekucji.
        for (const auto& tr : tick.trades) bench.on_trade(tr.first, tr.second);

        if (!o.valid) continue;

        // Fill po średniej cenie sekundy. W produkcji byłby random slippage,
        // częściowe fills, market impact (większe zlecenie → gorsza cena).
        // Demo: zakładamy płynność wystarczającą żeby cały child zrealizować.
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
    if (slip_bps < 0)      std::printf("   (lepiej niż rynek)\n");
    else if (slip_bps > 0) std::printf("   (gorzej niż rynek)\n");
    else                    std::printf("   (równo z rynkiem)\n");
}


int main(int argc, char* argv[]) {
    const uint64_t seed = (argc > 1) ? static_cast<uint64_t>(std::atoll(argv[1])) : 42;

    // Parent order: BUY 10000 AAPL w 100 sekund, 20 slotów po 5s.
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

    // Trzy niezależne markety z tym samym seedem żeby TWAP i VWAP grały na
    // dokładnie tym samym scenariuszu cen (fair comparison).
    MockMarket market_twap(22381, parent.duration_sec, seed);
    MockMarket market_vwap(22381, parent.duration_sec, seed);
    MockMarket market_bench(22381, parent.duration_sec, seed);

    // TWAP — bez profilu.
    exec::TWAPExecutor twap(parent);
    exec::MarketVWAPTracker bench_twap;
    simulate(twap, market_twap, bench_twap);

    // VWAP — z U-shape profile (dopasowany do tego co robi rynek).
    auto profile = exec::u_shape_profile(parent.num_slices);
    exec::VWAPExecutor vwap(parent, profile);
    exec::MarketVWAPTracker bench_vwap;
    simulate(vwap, market_vwap, bench_vwap);

    // Trzeci tracker — niezależny, gromadzi market VWAP osobno. Bench_twap
    // i bench_vwap powinny mieć identyczną wartość (same trades) → wystarczy
    // jeden, ale dla pewności wyświetlamy jedną z nich jako "benchmark rynku".
    const int32_t benchmark = bench_twap.vwap_ticks();

    std::printf("\nMarket benchmark VWAP: $%.4f  (%d ticks, volume=%lld)\n",
                benchmark / 10000.0, benchmark,
                static_cast<long long>(bench_twap.volume()));

    print_results("TWAP (równe sloty)",                     twap.stats(), parent, benchmark);
    print_results("VWAP (U-shape volume profile)",          vwap.stats(), parent, benchmark);

    // Sanity check: oba muszą zrealizować dokładnie total_qty.
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
