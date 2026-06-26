/*
 * MeanReversionStrategy — a mean-reversion strategy.
 *
 * Logic: when price deviates from a short-term simple moving average (SMA) by more
 * than a threshold, we bet on a return to the average. This is the classic "reactive"
 * strategy — we wait for the market and react to a deviation. market_maker.hpp shows
 * the proactive variant (we always quote, collecting the spread).
 *
 *   price > SMA + threshold → SELL (overvalued, we expect a drop)
 *   price < SMA - threshold → BUY  (undervalued, we expect a rise)
 *   otherwise              → HOLD (no action)
 *
 * Pipeline: ITCH feed → parser → STRATEGY (signals) → router → risk → OMS.
 *
 * Performance (lab): ~8M decisions/sec, p50=100ns, p99=121ns per tick.
 *
 * Design decisions:
 *   - PriceWindow keeps running_sum → sma() is O(1) instead of O(window).
 *   - A fixed MAX_STOCKS×PriceWindow array, zero heap on the hot path.
 *   - find_or_create() is a linear scan, but N≤64 fits in 1 cache line.
 *   - Symbol as char[9] + std::strcmp — faster than std::string for N≤8 chars.
 *   - NaN/Inf guard so a broken feed does not propagate into deviation_pct.
 */
#pragma once

#include "../common/time_utils.hpp"
#include "../common/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>


// Max number of symbols tracked at once (fixed array, no heap).
static constexpr int MAX_STOCKS = 64;

// Max SMA window size (how many recent prices we keep per symbol).
static constexpr int MAX_WINDOW = 128;


// A trading signal — generated when price deviates enough from the average.
struct Signal {
    int64_t timestamp_ns;      // time the signal was generated
    char    stock[9];          // ticker
    Side    side;              // BUY/SELL (meaningful only when valid=true)
    double  price;             // current price
    int32_t quantity;          // order size
    double  sma;               // SMA at the moment of the signal
    double  deviation_pct;     // how far from the average (%)
    bool    valid;             // false = HOLD (no signal)

    Signal() noexcept
        : timestamp_ns(0), side(Side::BUY), price(0), quantity(0),
          sma(0), deviation_pct(0), valid(false) {
        stock[0] = '\0';
    }
};


// Strategy counters (for print_stats / benchmarks).
struct StrategyStats {
    uint64_t signals_generated;
    uint64_t buys;
    uint64_t sells;
    uint64_t holds;
    uint64_t total_latency_ns;

    StrategyStats() noexcept
        : signals_generated(0), buys(0), sells(0), holds(0), total_latency_ns(0) {}

    double avg_latency_ns() const noexcept {
        const uint64_t total = signals_generated + holds;
        return total == 0 ? 0.0 : static_cast<double>(total_latency_ns) / total;
    }
};


// PriceWindow — circular buffer of one symbol's prices with an incremental running_sum
// so that sma() stays O(1) (instead of O(window) summation per tick).
struct PriceWindow {
    char   symbol[9];
    double prices[MAX_WINDOW];
    double running_sum;        // sum of prices in the window — kept incrementally
    int    count;              // how many prices stored (up to window_size)
    int    head;               // write position in the circular buf
    int    window_size;        // target window size (e.g. 20)
    bool   active;             // is the slot in use?

    PriceWindow() noexcept
        : prices{}, running_sum(0.0), count(0), head(0), window_size(20), active(false) {
        symbol[0] = '\0';
    }

    // add: append a price, maintain running_sum (evict the oldest when the window is full).
    void add(double price) noexcept {
        if (count == window_size) running_sum -= prices[head];  // evict
        prices[head] = price;
        running_sum += price;
        if (++head == window_size) head = 0;   // compare+cmov, avoids integer division
        if (count < window_size) ++count;
    }

    double sma() const noexcept { return count == 0 ? 0.0 : running_sum / count; }
    bool   full() const noexcept { return count >= window_size; }
};


class MeanReversionStrategy {
    PriceWindow   windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    double        threshold_;        // deviation threshold (e.g. 0.001 = 0.1%)
    int32_t       order_size_;
    StrategyStats stats_;
    bool          overflow_warned_ = false;  // warn once per run about a full array

    // find_or_create: find or create the price window for a symbol.
    // Linear scan, but N≤64 fits in 1-2 cache lines → fast.
    PriceWindow* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i) {
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        }
        // The slot pool is exhausted — warn once, otherwise we silently drop signals.
        if (stock_count_ >= MAX_STOCKS) {
            if (!overflow_warned_) {
                printf("[Strategy] WARNING: MAX_STOCKS=%d reached; '%.*s' ignored\n",
                       MAX_STOCKS, 8, stock);
                overflow_warned_ = true;
            }
            return nullptr;
        }
        PriceWindow& w = windows_[stock_count_++];
        w.active      = true;
        w.window_size = window_size_;
        w.count       = 0;
        w.head        = 0;
        std::strncpy(w.symbol, stock, 8);
        w.symbol[8] = '\0';
        return &w;
    }

    // emit_signal: shared field population for BUY and SELL (DRY).
    void emit_signal(Signal& sig, const char* stock, Side side, double price,
                     double sma, double deviation, int64_t ts) const noexcept {
        sig.valid         = true;
        sig.timestamp_ns  = ts;
        sig.side          = side;
        sig.price         = price;
        sig.quantity      = order_size_;
        sig.sma           = sma;
        sig.deviation_pct = deviation * 100.0;
        std::strncpy(sig.stock, stock, 8);
        sig.stock[8] = '\0';
    }

public:
    // window: SMA window (clamped to [1, MAX_WINDOW] — 0 / negative caused div-by-zero in add()/sma()).
    // threshold_pct: deviation threshold in percent (0.1 = 0.1%).
    // order_size: default quantity per signal.
    MeanReversionStrategy(int window = 20, double threshold_pct = 0.1,
                           int32_t order_size = 100) noexcept
        : stock_count_(0),
          window_size_(std::max(1, std::min(window, MAX_WINDOW))),
          threshold_(threshold_pct / 100.0),
          order_size_(order_size) {}

    // on_market_data — HOT PATH, called on every tick.
    // Returns a Signal (valid=true for BUY/SELL, valid=false for HOLD).
    Signal on_market_data(const char* stock, double price,
                          int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;

        // NaN/Inf guard — a broken feed should not generate nonsense signals.
        PriceWindow* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) {
            ++stats_.holds;
            stats_.total_latency_ns += (mono_ns() - t0);
            return sig;
        }

        w->add(price);

        // Without a full window we do not generate signals (the SMA would be unreliable).
        if (!w->full()) {
            ++stats_.holds;
            stats_.total_latency_ns += (mono_ns() - t0);
            return sig;
        }

        const double sma = w->sma();
        if (sma <= 0.0) {  // defensive — all prices in the window were ≤0
            ++stats_.holds;
            stats_.total_latency_ns += (mono_ns() - t0);
            return sig;
        }
        const double deviation = (price - sma) / sma;  // e.g. 0.01 = 1% above SMA

        if (deviation > threshold_) {
            emit_signal(sig, stock, Side::SELL, price, sma, deviation, timestamp_ns);
            ++stats_.sells;
            ++stats_.signals_generated;
        } else if (deviation < -threshold_) {
            emit_signal(sig, stock, Side::BUY, price, sma, deviation, timestamp_ns);
            ++stats_.buys;
            ++stats_.signals_generated;
        } else {
            ++stats_.holds;
        }

        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }

    void print_stats() const {
        printf("\n=== Strategy Statistics ===\n");
        printf("  Signals: %lu (%lu buys, %lu sells)\n",
               (unsigned long)stats_.signals_generated,
               (unsigned long)stats_.buys, (unsigned long)stats_.sells);
        printf("  Holds:   %lu\n", (unsigned long)stats_.holds);
        const uint64_t total = stats_.signals_generated + stats_.holds;
        if (total > 0) {
            printf("  Signal rate: %.1f%%\n", stats_.signals_generated * 100.0 / total);
        }
        printf("  Avg decision latency: %.0f ns\n", stats_.avg_latency_ns());
    }
};
