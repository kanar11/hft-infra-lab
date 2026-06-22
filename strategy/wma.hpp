/*
 * WMA — Weighted Moving Average (expansion #198).
 *
 * Linearly weighted average: in a window of N the oldest observation has weight 1,
 * the newest N (WMA = Σ w_i*x_i / Σ w_i). Unlike an SMA (equal weights) and an EMA
 * (exponential, infinite tail) it weights linearly and has a FINITE window — shorter
 * lag than an SMA with less noise than an EMA. The basis for the Hull MA.
 * Header-only, window in a std::deque.
 */
#pragma once

#include <deque>


class WMA {
    int period_;
    std::deque<double> window_;

public:
    explicit WMA(int period) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    // value: Σ (weight_i * x_i) / Σ weight_i, weight grows 1..k from the oldest.
    double value() const noexcept {
        if (window_.empty()) return 0.0;
        double num = 0.0;
        long   wsum = 0;
        long   w = 1;
        for (double x : window_) { num += x * static_cast<double>(w); wsum += w; ++w; }
        return num / static_cast<double>(wsum);
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
