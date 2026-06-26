/*
 * RSIStrategy — Relative Strength Index (expansion #135).
 *
 * RSI is a momentum oscillator in [0,100] from averaged gains/losses over N periods:
 *   RSI = 100 - 100/(1 + avg_gain/avg_loss)
 * Classic mean-reversion interpretation:
 *   RSI > overbought (70) -> SELL (overvalued, expected drop)
 *   RSI < oversold  (30) -> BUY  (undervalued)
 *   otherwise           -> HOLD
 *
 * Differs from Bollinger (sigma bands) and mean-reversion (% from SMA): RSI looks
 * at the RATIO of up-strength to down-strength, not the price level. Reuses Signal.
 */
#pragma once

#include "mean_reversion.hpp"   // Signal, StrategyStats, MAX_STOCKS/WINDOW, mono_ns

#include <algorithm>
#include <cmath>
#include <cstring>


class RSIStrategy {
    struct Win {
        char   symbol[9];
        double prev_price;
        double gains[MAX_WINDOW];
        double losses[MAX_WINDOW];
        int    count, head, size;
        bool   has_prev;
    };

    Win           windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    double        overbought_, oversold_;
    int32_t       order_size_;
    StrategyStats stats_;

    Win* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i)
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        if (stock_count_ >= MAX_STOCKS) return nullptr;
        Win& w = windows_[stock_count_++];
        w.prev_price = 0.0; w.count = 0; w.head = 0; w.size = window_size_; w.has_prev = false;
        std::strncpy(w.symbol, stock, 8); w.symbol[8] = '\0';
        return &w;
    }

    void emit(Signal& sig, const char* stock, Side side, double price,
              double rsi, int64_t ts) const noexcept {
        sig.valid = true; sig.timestamp_ns = ts; sig.side = side; sig.price = price;
        sig.quantity = order_size_; sig.sma = rsi; sig.deviation_pct = rsi;
        std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
    }

public:
    RSIStrategy(int window = 14, double overbought = 70.0, double oversold = 30.0,
                int32_t order_size = 100) noexcept
        : windows_{}, stock_count_(0),
          window_size_(std::max(1, std::min(window, MAX_WINDOW))),
          overbought_(overbought), oversold_(oversold), order_size_(order_size) {}

    Signal on_market_data(const char* stock, double price, int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;
        Win* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }

        if (!w->has_prev) {                          // first price = baseline
            w->prev_price = price; w->has_prev = true;
            ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig;
        }
        const double delta = price - w->prev_price;
        w->prev_price = price;
        w->gains[w->head]  = (delta > 0.0) ?  delta : 0.0;
        w->losses[w->head] = (delta < 0.0) ? -delta : 0.0;
        if (++w->head == w->size) w->head = 0;
        if (w->count < w->size) ++w->count;

        if (w->count >= w->size) {
            double sum_g = 0.0, sum_l = 0.0;
            for (int i = 0; i < w->count; ++i) { sum_g += w->gains[i]; sum_l += w->losses[i]; }
            const double avg_g = sum_g / w->count;
            const double avg_l = sum_l / w->count;
            const double rsi = (avg_l <= 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + avg_g / avg_l);
            if (rsi > overbought_)      { emit(sig, stock, Side::SELL, price, rsi, timestamp_ns); ++stats_.sells; ++stats_.signals_generated; }
            else if (rsi < oversold_)   { emit(sig, stock, Side::BUY,  price, rsi, timestamp_ns); ++stats_.buys;  ++stats_.signals_generated; }
            else                        { ++stats_.holds; }
        } else {
            ++stats_.holds;
        }
        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }
};
