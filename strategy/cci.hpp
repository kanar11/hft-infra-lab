/*
 * CCI — Commodity Channel Index (expansion #238).
 *
 * CCI = (price - SMA) / (0.015 * mean_absolute_deviation), over a window of N.
 * Measures how FAR the current price deviates from its average, in units of the
 * typical deviation. The 0.015 constant makes ~70-80% of values fall within
 * [-100, +100]; |CCI| > 100 = an unusually strong move: > +100 overbought / start
 * of an uptrend, < -100 oversold. Unlike oscillators based on min/max (Stochastic)
 * it is based on the mean and the mean deviation. Header-only.
 */
#pragma once

#include <deque>
#include <cmath>


class CCI {
    int period_;
    std::deque<double> window_;

public:
    explicit CCI(int period = 20) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double value() const noexcept {
        if (window_.empty()) return 0.0;
        const double n = static_cast<double>(window_.size());
        double sum = 0.0;
        for (double p : window_) sum += p;
        const double mean = sum / n;
        double mad = 0.0;
        for (double p : window_) mad += std::fabs(p - mean);
        mad /= n;
        if (mad == 0.0) return 0.0;                 // flat window — no signal
        return (window_.back() - mean) / (0.015 * mad);
    }

    bool ready()      const noexcept { return static_cast<int>(window_.size()) >= period_; }
    bool overbought() const noexcept { return value() > 100.0; }
    bool oversold()   const noexcept { return value() < -100.0; }
    void reset()      noexcept { window_.clear(); }
};
