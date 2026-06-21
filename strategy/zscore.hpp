/*
 * ZScore — rolling standard-score of price (expansion #276).
 *
 * z = (price - mean) / stddev, over a window of N observations (population stddev).
 * It expresses how many standard deviations the latest price sits from its recent
 * mean — THE canonical mean-reversion signal: z > +2 = stretched high (fade /
 * expect pull-back), z < -2 = stretched low. Unlike Bollinger %B (bounded inside
 * the k-sigma bands) the z-score is unbounded, so it also flags how EXTREME a move
 * is. Flat window (stddev 0) -> 0. Header-only.
 */
#pragma once

#include <deque>
#include <cmath>


class ZScore {
    int period_;
    std::deque<double> window_;

public:
    explicit ZScore(int period = 20) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double value() const noexcept {
        if (window_.size() < 2) return 0.0;
        const double n = static_cast<double>(window_.size());
        double sum = 0.0;
        for (double p : window_) sum += p;
        const double mean = sum / n;
        double var = 0.0;
        for (double p : window_) { const double d = p - mean; var += d * d; }
        var /= n;                                   // population variance
        const double sd = std::sqrt(var);
        if (sd == 0.0) return 0.0;                  // flat window -> no signal
        return (window_.back() - mean) / sd;
    }

    bool ready()    const noexcept { return static_cast<int>(window_.size()) >= period_; }
    bool stretched_high() const noexcept { return value() > 2.0; }
    bool stretched_low()  const noexcept { return value() < -2.0; }
    void reset()    noexcept { window_.clear(); }
};
