/*
 * Aroon — trend-strength oscillator (expansion #260).
 *
 *   Aroon Up   = (period - periods_since_highest) / period * 100
 *   Aroon Down = (period - periods_since_lowest)  / period * 100
 *
 * Measures HOW RECENTLY the window's high/low occurred rather than price level.
 * Up near 100 = a new high just printed (strong uptrend); Down near 100 = a new
 * low (strong downtrend). When Up > Down the trend is up, and vice versa; both
 * low = consolidation. Distinct from price oscillators (MACD/CCI/%B) — it's about
 * the timing of extremes. Header-only, holds a window of period+1 prices.
 */
#pragma once

#include <deque>


class Aroon {
    int period_;
    std::deque<double> window_;   // keeps period_+1 observations

public:
    explicit Aroon(int period = 14) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_ + 1) window_.pop_front();
    }

    double up() const noexcept {
        if (static_cast<int>(window_.size()) < period_ + 1) return 0.0;
        const int n = static_cast<int>(window_.size());
        int hi_idx = 0; double hi = window_[0];
        for (int i = 0; i < n; ++i) if (window_[i] >= hi) { hi = window_[i]; hi_idx = i; }
        const int since = (n - 1) - hi_idx;   // periods since the most recent high
        return static_cast<double>(period_ - since) / static_cast<double>(period_) * 100.0;
    }

    double down() const noexcept {
        if (static_cast<int>(window_.size()) < period_ + 1) return 0.0;
        const int n = static_cast<int>(window_.size());
        int lo_idx = 0; double lo = window_[0];
        for (int i = 0; i < n; ++i) if (window_[i] <= lo) { lo = window_[i]; lo_idx = i; }
        const int since = (n - 1) - lo_idx;   // periods since the most recent low
        return static_cast<double>(period_ - since) / static_cast<double>(period_) * 100.0;
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_ + 1; }
    void reset() noexcept { window_.clear(); }
};
