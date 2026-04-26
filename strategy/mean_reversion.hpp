/*
 * Mean reversion strategy.
 *
 * When price deviates from its short-term SMA by more than a threshold,
 * bet on reversion:
 *   price > SMA + threshold  -> SELL
 *   price < SMA - threshold  -> BUY
 *   otherwise                -> HOLD
 *
 * Pipeline position: Parser -> [Strategy] -> Router -> Risk -> OMS
 *
 * Hot-path discipline:
 *   - Per-symbol state lives in a fixed array (MAX_STOCKS), no heap allocation.
 *   - SMA is maintained as a running sum on the ring buffer — O(1) per tick
 *     instead of O(window) full re-sum.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>

static constexpr int MAX_STOCKS = 64;
static constexpr int MAX_WINDOW = 128;


struct Signal {
    int64_t timestamp_ns;
    char    stock[9];
    char    side[5];           // "BUY" / "SELL"
    double  price;
    int32_t quantity;
    double  sma;
    double  deviation_pct;
    bool    valid;             // false = HOLD

    Signal() noexcept
        : timestamp_ns(0), price(0), quantity(0), sma(0),
          deviation_pct(0), valid(false) {
        stock[0] = '\0';
        side[0]  = '\0';
    }
};


struct StrategyStats {
    uint64_t signals_generated = 0;
    uint64_t buys              = 0;
    uint64_t sells             = 0;
    uint64_t holds             = 0;
    uint64_t total_latency_ns  = 0;

    double avg_latency_ns() const noexcept {
        const uint64_t n = signals_generated + holds;
        return n == 0 ? 0.0 : static_cast<double>(total_latency_ns) / n;
    }
};


// PriceWindow: ring buffer of recent prices with O(1) running-sum SMA.
struct PriceWindow {
    char   symbol[9];
    double prices[MAX_WINDOW];
    double sum;
    int    count;
    int    head;
    int    window_size;
    bool   active;

    PriceWindow() noexcept
        : sum(0.0), count(0), head(0), window_size(20), active(false) {
        symbol[0] = '\0';
    }

    void add(double price) noexcept {
        if (count == window_size) {
            sum -= prices[head];   // evict oldest before overwrite
        } else {
            ++count;
        }
        prices[head] = price;
        sum += price;
        head = (head + 1) % window_size;
    }

    double sma() const noexcept {
        return count == 0 ? 0.0 : sum / count;
    }

    bool full() const noexcept { return count >= window_size; }
};


class MeanReversionStrategy {
    PriceWindow   windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    double        threshold_;
    int32_t       order_size_;
    StrategyStats stats_;

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    PriceWindow* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i) {
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        }
        if (stock_count_ >= MAX_STOCKS) return nullptr;
        PriceWindow& w = windows_[stock_count_++];
        w.active      = true;
        w.window_size = window_size_;
        w.count       = 0;
        w.head        = 0;
        w.sum         = 0.0;
        std::strncpy(w.symbol, stock, 8);
        w.symbol[8] = '\0';
        return &w;
    }

public:
    MeanReversionStrategy(int window = 20, double threshold_pct = 0.1,
                          int32_t order_size = 100) noexcept
        : stock_count_(0),
          window_size_(window > MAX_WINDOW ? MAX_WINDOW : window),
          threshold_(threshold_pct / 100.0),
          order_size_(order_size) {}

    // on_market_data: hot path — called on every tick. Returns invalid Signal == HOLD.
    Signal on_market_data(const char* stock, double price,
                          int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = now_ns();
        Signal sig;

        PriceWindow* w = find_or_create(stock);
        if (!w) {
            stats_.holds++;
            stats_.total_latency_ns += (now_ns() - t0);
            return sig;
        }

        w->add(price);

        if (!w->full()) {
            stats_.holds++;
            stats_.total_latency_ns += (now_ns() - t0);
            return sig;
        }

        const double sma       = w->sma();
        const double deviation = (price - sma) / sma;

        if (deviation > threshold_) {
            sig.valid        = true;
            sig.timestamp_ns = timestamp_ns;
            std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
            std::strncpy(sig.side,  "SELL", 4); sig.side[4]  = '\0';
            sig.price         = price;
            sig.quantity      = order_size_;
            sig.sma           = sma;
            sig.deviation_pct = deviation * 100.0;
            stats_.sells++;
            stats_.signals_generated++;
        } else if (deviation < -threshold_) {
            sig.valid        = true;
            sig.timestamp_ns = timestamp_ns;
            std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
            std::strncpy(sig.side,  "BUY", 3); sig.side[3]  = '\0';
            sig.price         = price;
            sig.quantity      = order_size_;
            sig.sma           = sma;
            sig.deviation_pct = deviation * 100.0;
            stats_.buys++;
            stats_.signals_generated++;
        } else {
            stats_.holds++;
        }

        stats_.total_latency_ns += (now_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }

    void print_stats() const {
        printf("\n=== Strategy Statistics ===\n");
        printf("  Signals: %lu (%lu buys, %lu sells)\n",
               (unsigned long)stats_.signals_generated,
               (unsigned long)stats_.buys, (unsigned long)stats_.sells);
        printf("  Holds: %lu\n", (unsigned long)stats_.holds);
        const uint64_t total = stats_.signals_generated + stats_.holds;
        if (total > 0) {
            printf("  Signal rate: %.1f%%\n", stats_.signals_generated * 100.0 / total);
        }
        printf("  Avg decision latency: %.0f ns\n", stats_.avg_latency_ns());
    }
};
