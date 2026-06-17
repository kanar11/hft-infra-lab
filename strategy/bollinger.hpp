/*
 * BollingerStrategy — mean-reversion adaptacyjny do zmiennosci (expansion #93).
 *
 * MeanReversionStrategy uzywa STALEGO progu % od SMA — ten sam prog na spokojnym
 * i rozchwianym rynku, wiec albo za duzo sygnalow na zmiennym, albo za malo na
 * spokojnym. Bollinger skaluje prog ZMIENNOSCIA: pasma = SMA +/- k*sigma
 * (odchylenie standardowe okna). Sygnal dopiero gdy cena wybije poza pasmo.
 *
 *   cena > SMA + k*sigma -> SELL (przewartosciowane wzgledem zmiennosci)
 *   cena < SMA - k*sigma -> BUY
 *   inaczej             -> HOLD
 *
 * Reuzywa Signal/StrategyStats/limity z mean_reversion.hpp. Wlasne okno bo
 * PriceWindow nie trzyma sumy kwadratow potrzebnej do sigma O(1).
 */
#pragma once

#include "mean_reversion.hpp"   // Signal, StrategyStats, MAX_STOCKS/WINDOW, mono_ns

#include <algorithm>
#include <cmath>
#include <cstring>


class BollingerStrategy {
    struct VarWindow {
        char   symbol[9];
        double prices[MAX_WINDOW];
        double sum;        // Sigma cen
        double sum_sq;     // Sigma cen^2 — do sigma w O(1)
        int    count, head, window_size;
        bool   active;
    };

    VarWindow     windows_[MAX_STOCKS];
    int           stock_count_;
    int           window_size_;
    double        num_std_;       // k — ile sigma od SMA (np. 2.0)
    int32_t       order_size_;
    StrategyStats stats_;

    VarWindow* find_or_create(const char* stock) noexcept {
        for (int i = 0; i < stock_count_; ++i)
            if (std::strcmp(windows_[i].symbol, stock) == 0) return &windows_[i];
        if (stock_count_ >= MAX_STOCKS) return nullptr;
        VarWindow& w = windows_[stock_count_++];
        w.sum = 0.0; w.sum_sq = 0.0; w.count = 0; w.head = 0;
        w.window_size = window_size_; w.active = true;
        std::strncpy(w.symbol, stock, 8); w.symbol[8] = '\0';
        return &w;
    }

    static void add(VarWindow& w, double price) noexcept {
        if (w.count == w.window_size) {            // eviction najstarszej
            const double old = w.prices[w.head];
            w.sum -= old; w.sum_sq -= old * old;
        }
        w.prices[w.head] = price;
        w.sum += price; w.sum_sq += price * price;
        w.head = (w.head + 1) % w.window_size;
        if (w.count < w.window_size) ++w.count;
    }

    void emit(Signal& sig, const char* stock, Side side, double price,
              double sma, double dev, int64_t ts) const noexcept {
        sig.valid = true; sig.timestamp_ns = ts; sig.side = side; sig.price = price;
        sig.quantity = order_size_; sig.sma = sma; sig.deviation_pct = dev * 100.0;
        std::strncpy(sig.stock, stock, 8); sig.stock[8] = '\0';
    }

public:
    BollingerStrategy(int window = 20, double num_std = 2.0, int32_t order_size = 100) noexcept
        : windows_{},                                              // value-init slotow (active=false)
          stock_count_(0),
          window_size_(std::max(2, std::min(window, MAX_WINDOW))),  // >=2 do wariancji
          num_std_(num_std > 0.0 ? num_std : 2.0),
          order_size_(order_size) {}

    Signal on_market_data(const char* stock, double price, int64_t timestamp_ns = 0) noexcept {
        const int64_t t0 = mono_ns();
        Signal sig;
        VarWindow* w = (std::isfinite(price) && price > 0.0) ? find_or_create(stock) : nullptr;
        if (!w) { ++stats_.holds; stats_.total_latency_ns += (mono_ns() - t0); return sig; }
        add(*w, price);
        if (w->count < w->window_size) { ++stats_.holds; stats_.total_latency_ns += (mono_ns()-t0); return sig; }

        const double n    = static_cast<double>(w->count);
        const double mean = w->sum / n;
        const double var  = std::max(0.0, w->sum_sq / n - mean * mean);
        const double sd   = std::sqrt(var);
        const double upper = mean + num_std_ * sd;
        const double lower = mean - num_std_ * sd;
        const double dev   = (mean > 0.0) ? (price - mean) / mean : 0.0;

        if (sd > 0.0 && price > upper) {
            emit(sig, stock, Side::SELL, price, mean, dev, timestamp_ns);
            ++stats_.sells; ++stats_.signals_generated;
        } else if (sd > 0.0 && price < lower) {
            emit(sig, stock, Side::BUY, price, mean, dev, timestamp_ns);
            ++stats_.buys; ++stats_.signals_generated;
        } else {
            ++stats_.holds;
        }
        stats_.total_latency_ns += (mono_ns() - t0);
        return sig;
    }

    const StrategyStats& stats() const noexcept { return stats_; }
    int stock_count() const noexcept { return stock_count_; }
};
