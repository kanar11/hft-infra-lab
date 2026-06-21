/*
 * ROC — Rate of Change momentum indicator (expansion #253).
 *
 * ROC = (price - price_n_periods_ago) / price_n_periods_ago * 100.
 * A normalized percentage momentum: positive = price rising vs N periods ago,
 * negative = falling. Unlike MACD/TRIX (built on EMA cascades) ROC is a direct
 * point-to-point percentage change — fast, no smoothing, no lag. Zero-line cross
 * flags a momentum reversal. Header-only, holds a window of period+1 prices.
 */
#pragma once

#include <deque>


class ROC {
    int period_;
    std::deque<double> window_;   // keeps period_+1 prices (oldest + current)

public:
    explicit ROC(int period = 12) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_ + 1) window_.pop_front();
    }

    double value() const noexcept {
        if (static_cast<int>(window_.size()) < period_ + 1) return 0.0;
        const double old = window_.front();
        return old != 0.0 ? (window_.back() - old) / old * 100.0 : 0.0;
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_ + 1; }
    void reset() noexcept { window_.clear(); }
};
