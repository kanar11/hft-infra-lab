/*
 * Bollinger %B (expansion #246).
 *
 * %B = (price - lower_band) / (upper_band - lower_band), where the bands =
 * SMA +/- k * standard deviation (population) over a window of N. Normalizes
 * the position of price RELATIVE to the Bollinger bands:
 *   0.0 = on the lower band, 0.5 = on the average, 1.0 = on the upper;
 *   > 1.0 = above the upper (overbought), < 0.0 = below the lower (oversold).
 *
 * Unlike the Bollinger strategy (entry signals) %B is a continuous position
 * INDICATOR — handy for filters and ensembles. Flat window (sd=0) -> 0.5. Header-only.
 */
#pragma once

#include <deque>
#include <cmath>


class BollingerPercentB {
    int    period_;
    double k_;
    std::deque<double> window_;

public:
    explicit BollingerPercentB(int period = 20, double k = 2.0) noexcept
        : period_(period < 1 ? 1 : period), k_(k) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double value() const noexcept {
        if (window_.size() < 2) return 0.5;
        const double n = static_cast<double>(window_.size());
        double sum = 0.0;
        for (double p : window_) sum += p;
        const double mean = sum / n;
        double var = 0.0;
        for (double p : window_) { const double d = p - mean; var += d * d; }
        var /= n;                                   // population variance
        const double sd = std::sqrt(var);
        if (sd == 0.0) return 0.5;                  // flat window -> middle
        const double lower = mean - k_ * sd;
        const double width = 2.0 * k_ * sd;         // upper - lower
        return (window_.back() - lower) / width;
    }

    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
