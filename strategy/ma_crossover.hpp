/*
 * MACrossover — moving-average crossover (expansion #157).
 *
 * Two SMAs: fast and slow. A signal on the CROSSOVER:
 *   fast crosses slow FROM BELOW -> BUY  (golden cross, start of an uptrend)
 *   fast crosses slow FROM ABOVE -> SELL (death cross)
 *   no crossing                 -> HOLD
 *
 * Differs from momentum (a threshold from a single SMA) and Donchian (channel extremes):
 * here the MOMENT the relationship of the two averages changes is what matters. Classic
 * trend-following. Reuses Signal/StrategyStats from mean_reversion.hpp.
 */
#pragma once

#include "mean_reversion.hpp"   // Signal, StrategyStats, MAX_STOCKS/WINDOW, mono_ns

#include <algorithm>
#include <cmath>
#include <cstring>


class MACrossover {
    struct Win {
        char   symbol[9];
        double prices[MAX_WINDOW];   // ring of size slow_
        int    count, head;
        bool   has_prev;
        bool   prev_fast_above;
    };

    Win           windows_[MAX_STOCKS];
    int           stock_count_;
    int           fast_, slow_;
    int32_t       order_size_;
    StrategyStats stats_;

    Win* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i)
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        if (stock_count_ >= MAX_STOCKS) return nullptr;
        Win& w = windows_[stock_count_++];
        w.count = 0; w.head = 0; w.has_prev = false; w.prev_fast_above = false;
        std::strncpy(w.symbol, stock, 8); w.symbol[8] = '\0';
        return &w;
    }

    // mean of the last k prices (k <= count). Ring of size slow_.
    double mean_last(const Win& w, int k) const noexcept {
        double s = 0.0; int idx = w.head;
        for (int i = 0; i < k; ++i) { idx = (idx - 1 + slow_) % slow_; s += w.prices[idx]; }
        return s / k;
    }

    void emit(Signal& sig, const char* stock, Side side, double price,
              double slow_sma, int64_t ts) const noexcept {
        sig.valid = true; sig.timestamp_ns = ts; sig.side = side; sig.price = price;
        sig.quantity = order_size_; sig.sma = slow_sma;
        sig.deviation_pct = (slow_sma > 0.0) ? (price - slow_sma) / slow_sma * 100.0 : 0.0;
        std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
    }

public:
    MACrossover(int fast = 5, int slow = 20, int32_t order_size = 100) noexcept
        : windows_{}, stock_count_(0),
          fast_(std::max(1, std::min(fast, MAX_WINDOW))),
          slow_(std::max(fast_ + 1, std::min(slow, MAX_WINDOW))),
          order_size_(order_size) {}

    Signal on_market_data(const char* stock, double price, int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;
        Win* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }

        w->prices[w->head] = price;
        w->head = (w->head + 1) % slow_;
        if (w->count < slow_) ++w->count;

        if (w->count >= slow_) {
            const double fast_sma = mean_last(*w, fast_);
            const double slow_sma = mean_last(*w, slow_);
            const bool fast_above = fast_sma > slow_sma;
            if (w->has_prev && fast_above != w->prev_fast_above) {
                if (fast_above) { emit(sig, stock, Side::BUY,  price, slow_sma, timestamp_ns); ++stats_.buys;  ++stats_.signals_generated; }
                else            { emit(sig, stock, Side::SELL, price, slow_sma, timestamp_ns); ++stats_.sells; ++stats_.signals_generated; }
            } else {
                ++stats_.holds;
            }
            w->prev_fast_above = fast_above;
            w->has_prev = true;
        } else {
            ++stats_.holds;
        }
        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }
};
