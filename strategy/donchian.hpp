/*
 * DonchianBreakout — channel-breakout strategy (expansion #124).
 *
 * A Donchian channel = the highest and lowest price of the last N observations.
 * A signal appears when the current price BREAKS a channel built from the N PREVIOUS:
 *   price > prior_high -> BUY  (upside breakout — a new uptrend)
 *   price < prior_low  -> SELL (downside breakout)
 *   otherwise         -> HOLD
 *
 * Differs from momentum (a threshold relative to the SMA) and Bollinger (sigma bands):
 * here only a clean break of the window's extreme matters — classic trend-following/turtle.
 * Reuses Signal/StrategyStats from mean_reversion.hpp.
 */
#pragma once

#include "mean_reversion.hpp"   // Signal, StrategyStats, MAX_STOCKS/WINDOW, mono_ns

#include <algorithm>
#include <cmath>
#include <cstring>


class DonchianBreakout {
    struct Win {
        char   symbol[9];
        double prices[MAX_WINDOW];
        int    count, head, size;
        bool   active;
    };

    Win           windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    int32_t       order_size_;
    StrategyStats stats_;

    Win* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i)
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        if (stock_count_ >= MAX_STOCKS) return nullptr;
        Win& w = windows_[stock_count_++];
        w.count = 0; w.head = 0; w.size = window_size_; w.active = true;
        std::strncpy(w.symbol, stock, 8); w.symbol[8] = '\0';
        return &w;
    }

    static void push(Win& w, double price) noexcept {
        w.prices[w.head] = price;
        if (++w.head == w.size) w.head = 0;
        if (w.count < w.size) ++w.count;
    }

    void emit(Signal& sig, const char* stock, Side side, double price,
              double ref, int64_t ts) const noexcept {
        sig.valid = true; sig.timestamp_ns = ts; sig.side = side; sig.price = price;
        sig.quantity = order_size_; sig.sma = ref;
        sig.deviation_pct = (ref > 0.0) ? (price - ref) / ref * 100.0 : 0.0;
        std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
    }

public:
    DonchianBreakout(int window = 20, int32_t order_size = 100) noexcept
        : windows_{},
          stock_count_(0),
          window_size_(std::max(1, std::min(window, MAX_WINDOW))),
          order_size_(order_size) {}

    Signal on_market_data(const char* stock, double price, int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;
        Win* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }

        // Channel computed from the N PREVIOUS prices; signal when the current one breaks it.
        if (w->count >= w->size) {
            double hi = -1e300, lo = 1e300;
            for (int i = 0; i < w->count; ++i) {
                const double p = w->prices[i];
                if (p > hi) hi = p;
                if (p < lo) lo = p;
            }
            if (price > hi)      { emit(sig, stock, Side::BUY,  price, hi, timestamp_ns); ++stats_.buys;  ++stats_.signals_generated; }
            else if (price < lo) { emit(sig, stock, Side::SELL, price, lo, timestamp_ns); ++stats_.sells; ++stats_.signals_generated; }
            else                 { ++stats_.holds; }
        } else {
            ++stats_.holds;
        }
        push(*w, price);
        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }
};
