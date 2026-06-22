/*
 * Stochastic Oscillator — %K (expansion #190).
 *
 * %K = (close - lowest_low) / (highest_high - lowest_low) * 100, over a window of N
 * observations. Measures the POSITION of the current price within the window's range:
 * ~100 = near the top (overbought), ~0 = near the bottom (oversold). Unlike RSI (based
 * on average gains/losses) it looks at the min-max range — reacting faster to extremes.
 *
 * Convention: overbought > 80, oversold < 20; on a flat window (hi==lo) it returns 50.
 * Header-only, window in a std::deque.
 */
#pragma once

#include <deque>


class Stochastic {
    int period_;
    std::deque<double> window_;

public:
    explicit Stochastic(int period = 14) noexcept
        : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    // percent_k: position of the last price in the window's [min,max] range, 0..100.
    double percent_k() const noexcept {
        if (window_.empty()) return 50.0;
        double lo = window_.front();
        double hi = window_.front();
        for (double p : window_) { if (p < lo) lo = p; if (p > hi) hi = p; }
        if (hi <= lo) return 50.0;                 // flat window — neutral
        return (window_.back() - lo) / (hi - lo) * 100.0;
    }

    bool   ready()      const noexcept { return static_cast<int>(window_.size()) >= period_; }
    bool   overbought() const noexcept { return percent_k() > 80.0; }
    bool   oversold()   const noexcept { return percent_k() < 20.0; }
    void   reset()      noexcept { window_.clear(); }
};
