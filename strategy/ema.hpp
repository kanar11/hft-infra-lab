/*
 * EMA — exponential moving average (expansion #173).
 *
 * EMA_t = alpha * x_t + (1 - alpha) * EMA_{t-1}. Unlike an SMA it weights
 * fresh data more heavily (shorter lag), with no in-memory window (O(1), a single
 * state number). A reusable primitive for strategies (trend, EMA-crossover signals,
 * metric smoothing).
 *
 * alpha directly or from a period: alpha = 2/(period+1) (the "N-period EMA"
 * convention). Header-only.
 */
#pragma once

#include <cmath>


class EMA {
    double alpha_;
    double value_;
    bool   init_;

public:
    explicit EMA(double alpha) noexcept
        : alpha_(alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha)),
          value_(0.0), init_(false) {}

    // from_period: N-period EMA -> alpha = 2/(N+1).
    static EMA from_period(int period) noexcept {
        const int p = period < 1 ? 1 : period;
        return EMA(2.0 / (static_cast<double>(p) + 1.0));
    }

    // update: add an observation, return the new EMA value. The first = seed.
    double update(double x) noexcept {
        if (!init_) { value_ = x; init_ = true; }
        else        value_ = alpha_ * x + (1.0 - alpha_) * value_;
        return value_;
    }

    double value() const noexcept { return value_; }
    double alpha() const noexcept { return alpha_; }
    bool   ready() const noexcept { return init_; }
    void   reset() noexcept { value_ = 0.0; init_ = false; }
};
