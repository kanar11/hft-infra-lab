/*
 * MomentumStrategy — strategia podążania za trendem (expansion #85).
 *
 * Przeciwienstwo mean-reversion: zamiast stawiac na powrot do sredniej, gramy
 * KONTYNUACJE ruchu. Gdy cena wybija ponad SMA o prog -> trend w gore -> BUY;
 * ponizej -> SELL. Klasyczna "trend-following" alpha; pokazuje ze pipeline i
 * harness backtestu hostuja wiele rodzin strategii, nie jedna.
 *
 *   cena > SMA + threshold -> BUY  (breakout w gore, jedziemy z trendem)
 *   cena < SMA - threshold -> SELL (breakdown, gramy spadek)
 *   inaczej               -> HOLD
 *
 * Reuzywa Signal + PriceWindow z mean_reversion.hpp (ta sama infrastruktura
 * SMA O(1), te same MAX_STOCKS/MAX_WINDOW), wiec wpina sie w istniejacy
 * pipeline/backtest bez zmian w callerze — rozni sie TYLKO znakiem decyzji.
 */
#pragma once

#include "mean_reversion.hpp"   // Signal, PriceWindow, StrategyStats, MAX_STOCKS/WINDOW

#include <algorithm>
#include <cmath>
#include <cstring>


class MomentumStrategy {
    PriceWindow   windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    double        threshold_;
    int32_t       order_size_;
    StrategyStats stats_;
    bool          overflow_warned_ = false;

    PriceWindow* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i)
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        if (stock_count_ >= MAX_STOCKS) {
            if (!overflow_warned_) {
                printf("[Momentum] WARNING: MAX_STOCKS=%d reached; '%.*s' ignored\n",
                       MAX_STOCKS, 8, stock);
                overflow_warned_ = true;
            }
            return nullptr;
        }
        PriceWindow& w = windows_[stock_count_++];
        w.active = true; w.window_size = window_size_; w.count = 0; w.head = 0;
        std::strncpy(w.symbol, stock, 8); w.symbol[8] = '\0';
        return &w;
    }

    void emit(Signal& sig, const char* stock, Side side, double price,
              double sma, double deviation, int64_t ts) const noexcept {
        sig.valid = true; sig.timestamp_ns = ts; sig.side = side; sig.price = price;
        sig.quantity = order_size_; sig.sma = sma; sig.deviation_pct = deviation * 100.0;
        std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
    }

public:
    MomentumStrategy(int window = 20, double threshold_pct = 0.1,
                     int32_t order_size = 100) noexcept
        : stock_count_(0),
          window_size_(std::max(1, std::min(window, MAX_WINDOW))),
          threshold_(threshold_pct / 100.0),
          order_size_(order_size) {}

    Signal on_market_data(const char* stock, double price, int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;
        PriceWindow* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }
        w->add(price);
        if (!w->full()) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }

        const double sma = w->sma();
        if (sma <= 0.0) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }
        const double deviation = (price - sma) / sma;

        // Trend-following: znak DECYZJI odwrotny do mean-reversion.
        if (deviation > threshold_) {          // breakout w gore -> jedziemy long
            emit(sig, stock, Side::BUY, price, sma, deviation, timestamp_ns);
            ++stats_.buys; ++stats_.signals_generated;
        } else if (deviation < -threshold_) {  // breakdown -> short
            emit(sig, stock, Side::SELL, price, sma, deviation, timestamp_ns);
            ++stats_.sells; ++stats_.signals_generated;
        } else {
            ++stats_.holds;
        }
        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }
};
