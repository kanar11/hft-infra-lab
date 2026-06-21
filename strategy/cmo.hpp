/*
 * CMO — Chande Momentum Oscillator (expansion #268).
 *
 * CMO = (sum_up - sum_down) / (sum_up + sum_down) * 100, over a window of N price
 * CHANGES, where sum_up is the total of positive moves and sum_down the total of
 * |negative moves|. Ranges [-100, +100]: +100 = every move up, -100 = every move
 * down, 0 = balanced. Unlike RSI (which smooths gains/losses with a moving
 * average) CMO uses the RAW sums, so it reacts faster and swings wider. Common
 * thresholds: overbought > +50, oversold < -50. Header-only, holds period+1 prices.
 */
#pragma once

#include <deque>


class CMO {
    int period_;
    std::deque<double> window_;   // keeps period_+1 prices

public:
    explicit CMO(int period = 9) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_ + 1) window_.pop_front();
    }

    double value() const noexcept {
        if (window_.size() < 2) return 0.0;
        double up = 0.0, down = 0.0;
        for (std::size_t i = 1; i < window_.size(); ++i) {
            const double d = window_[i] - window_[i - 1];
            if (d > 0.0) up += d; else down += -d;
        }
        const double tot = up + down;
        return tot > 0.0 ? (up - down) / tot * 100.0 : 0.0;
    }

    bool ready()      const noexcept { return static_cast<int>(window_.size()) >= period_ + 1; }
    bool overbought() const noexcept { return value() > 50.0; }
    bool oversold()   const noexcept { return value() < -50.0; }
    void reset()      noexcept { window_.clear(); }
};
