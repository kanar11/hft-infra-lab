/*
 * FisherTransform — Ehlers' Fisher Transform oscillator (expansion #324).
 *
 * Price distributions are far from Gaussian, which blunts threshold-based signals.
 * The Fisher Transform remaps the price's position within its recent [low,high] range
 * through 0.5*ln((1+x)/(1-x)) — a function whose output is sharply peaked, turning
 * gentle reversals into decisive, near-Gaussian extremes. The result is an UNBOUNDED
 * oscillator that crosses zero at the midrange and spikes hard near range extremes,
 * giving earlier, crisper turning-point signals than the bounded Stochastic %K (#190)
 * it is built on.
 *
 * Pipeline per price: normalise the position within the N-window to [-1,1], EMA-smooth
 * it (0.33/0.67), CLAMP just inside (-1,1) so the log stays finite, then transform and
 * smooth the transform (0.5/0.5). The 1-bar-lagged Fisher value is the trigger line —
 * a Fisher/trigger crossover is the classic entry. Header-only, fixed N-price ring
 * (no heap), O(N) per update.
 */
#pragma once

#include <cmath>


class FisherTransform {
    static constexpr int CAP = 256;
    int    period_;
    double prices_[CAP];
    int    count_, head_;
    double value_;        // EMA-smoothed normalised position, recursive
    double fisher_;       // smoothed transform (the Fisher line), recursive
    double prev_fisher_;  // last bar's Fisher = trigger line

public:
    explicit FisherTransform(int period = 10) noexcept
        : period_(period < 1 ? 1 : (period > CAP ? CAP : period)),
          prices_{}, count_(0), head_(0), value_(0.0), fisher_(0.0), prev_fisher_(0.0) {}

    void update(double price) noexcept {
        prices_[head_] = price;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;

        // highest-high / lowest-low over the live window (count_ filled slots)
        double lo = prices_[0], hi = prices_[0];
        for (int i = 1; i < count_; ++i) {
            const double p = prices_[i];
            if (p < lo) lo = p;
            if (p > hi) hi = p;
        }
        const double range = hi - lo;
        // normalised position in [-1, 1]; flat window -> 0 (midrange, no signal)
        const double pos = range > 0.0 ? 2.0 * ((price - lo) / range) - 1.0 : 0.0;

        // EMA-smooth the position, then clamp strictly inside (-1, 1) for the log
        value_ = 0.33 * pos + 0.67 * value_;
        double v = value_;
        if (v >  0.999) v =  0.999;
        if (v < -0.999) v = -0.999;

        prev_fisher_ = fisher_;
        fisher_ = 0.5 * std::log((1.0 + v) / (1.0 - v)) + 0.5 * fisher_;
    }

    double value()   const noexcept { return fisher_; }       // the Fisher line
    double trigger() const noexcept { return prev_fisher_; }  // 1-bar lag = signal line
    bool   ready()   const noexcept { return count_ >= period_; }
    void   reset()   noexcept {
        count_ = 0; head_ = 0; value_ = 0.0; fisher_ = 0.0; prev_fisher_ = 0.0;
    }
};
