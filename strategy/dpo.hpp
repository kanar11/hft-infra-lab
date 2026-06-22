/*
 * DPO — Detrended Price Oscillator (expansion #292).
 *
 * DPO = price[(N/2)+1 periods ago] - SMA(N).
 *
 * By comparing a PAST price to the current N-period simple moving average (and
 * shifting back by half the window) DPO removes the underlying trend, leaving the
 * short-term CYCLE component around zero. Unlike momentum oscillators (ROC/CMO/TSI)
 * it's not about direction-of-change but about where price sits relative to its
 * detrended mean — handy for spotting cycle peaks/troughs. 0 on a flat series.
 * Header-only, holds a window of N prices.
 */
#pragma once

#include <deque>


class DPO {
    int period_;
    std::deque<double> window_;

public:
    explicit DPO(int period = 20) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double value() const noexcept {
        const int n = static_cast<int>(window_.size());
        if (n < period_) return 0.0;
        double sum = 0.0;
        for (double p : window_) sum += p;
        const double sma = sum / static_cast<double>(n);
        const int shift = period_ / 2 + 1;
        const int idx = n - 1 - shift;          // index of the price `shift` periods ago
        if (idx < 0) return 0.0;
        return window_[static_cast<std::size_t>(idx)] - sma;
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
