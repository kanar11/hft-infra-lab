/*
 * KAMA — Kaufman Adaptive Moving Average (expansion #300, milestone).
 *
 * A moving average that SPEEDS UP in a clean trend and SLOWS DOWN in chop, by
 * adapting its smoothing constant to Kaufman's Efficiency Ratio:
 *
 *   ER = |price - price[N ago]| / sum(|price[i] - price[i-1]|)   (net move / path)
 *   SC = (ER * (fast_sc - slow_sc) + slow_sc)^2                  (smoothing constant)
 *   KAMA = KAMA_prev + SC * (price - KAMA_prev)
 *
 * with fast_sc = 2/(fast+1) and slow_sc = 2/(slow+1) (defaults fast=2, slow=30).
 * ER ~ 1 (price marching one direction) -> SC ~ fast_sc^2 -> KAMA hugs price;
 * ER ~ 0 (price thrashing, net move tiny vs path) -> SC ~ slow_sc^2 -> KAMA barely
 * moves, filtering the noise. This gives a trend follower that doesn't whipsaw in
 * range-bound markets the way a fixed-period MA does. Header-only; keeps a window of
 * N+1 prices for the ER plus the running KAMA value.
 */
#pragma once

#include <cmath>
#include <deque>


class KAMA {
    int    period_;
    double fast_sc_;
    double slow_sc_;
    std::deque<double> window_;   // last period_+1 prices (period_ changes)
    double kama_ = 0.0;
    bool   init_ = false;

public:
    explicit KAMA(int period = 10, int fast = 2, int slow = 30) noexcept
        : period_(period < 1 ? 1 : period),
          fast_sc_(2.0 / (fast + 1)),
          slow_sc_(2.0 / (slow + 1)) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_ + 1) window_.pop_front();
        if (!init_) { kama_ = price; init_ = true; return; }       // seed at first price
        if (static_cast<int>(window_.size()) < period_ + 1) {      // not enough for ER yet
            kama_ = price;                                         // track raw until warmed up
            return;
        }
        const double change = std::fabs(window_.back() - window_.front());
        double volatility = 0.0;
        for (std::size_t i = 1; i < window_.size(); ++i)
            volatility += std::fabs(window_[i] - window_[i - 1]);
        const double er = volatility > 0.0 ? change / volatility : 0.0;
        const double sc = std::pow(er * (fast_sc_ - slow_sc_) + slow_sc_, 2.0);
        kama_ += sc * (price - kama_);
    }

    double value() const noexcept { return kama_; }
    bool   ready() const noexcept { return static_cast<int>(window_.size()) >= period_ + 1; }
    void   reset() noexcept { window_.clear(); kama_ = 0.0; init_ = false; }
};
