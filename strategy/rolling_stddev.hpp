/*
 * RollingStdDev — rolling sample standard deviation over a price window (#316).
 *
 * A pure VOLATILITY primitive: the dispersion of the last N prices around their
 * mean. Unlike the oscillators (which normalise or smooth), this hands back the raw
 * sample standard deviation (N-1 denominator, the unbiased estimator) plus its
 * variance and mean. It's the building block for position sizing (risk per unit
 * vol), volatility bands, and regime detection — feed it prices (or returns) and
 * read value(). 0 until at least two samples. Header-only, holds a window of N.
 */
#pragma once

#include <cmath>
#include <deque>


class RollingStdDev {
    int period_;
    std::deque<double> window_;

public:
    explicit RollingStdDev(int period = 20) noexcept : period_(period < 2 ? 2 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double mean() const noexcept {
        if (window_.empty()) return 0.0;
        double s = 0.0;
        for (double p : window_) s += p;
        return s / static_cast<double>(window_.size());
    }
    // variance: sample variance (N-1 denominator). 0 with fewer than 2 samples.
    double variance() const noexcept {
        const int n = static_cast<int>(window_.size());
        if (n < 2) return 0.0;
        const double m = mean();
        double ss = 0.0;
        for (double p : window_) { const double d = p - m; ss += d * d; }
        return ss / static_cast<double>(n - 1);
    }
    double value() const noexcept { return std::sqrt(variance()); }
    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
