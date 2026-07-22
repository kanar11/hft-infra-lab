/*
 * TRIMA — Triangular Moving Average (expansion #597).
 *
 * A double-smoothed SMA: an SMA of an SMA, which collapses to a single
 * TRIANGULAR-weighted average of the last N prices — the middle bar carries
 * the most weight and the endpoints the least (for N=3 the weights are
 * 1-2-1). The two sub-windows follow the TA-Lib convention: for odd N both
 * SMAs have period (N+1)/2; for even N the inner is N/2 and the outer N/2+1.
 *
 * The centered (symmetric) weighting makes it the SMOOTHEST of the fixed
 * linear MAs and the slowest to react — the opposite end of the lag/smoothness
 * trade-off from ALMA (#550, whose offset knob pushes weight onto the newest
 * bars) and the adaptive family (KAMA/VIDYA/McGinley, which speed up in a
 * move). Distinct from every MA in the lab: EMA/DEMA/TEMA are recursive
 * exponential cascades, WMA a linear front-weighted ramp — TRIMA is the
 * triangular-kernel member, the classic choice when noise rejection matters
 * more than responsiveness. Because both SMAs of a constant are that constant,
 * a flat tape pins EXACTLY at the level (asserted). Classic param: 20.
 * update(price) convention. Header-only.
 */
#pragma once

#include <deque>


class TRIMA {
    // Minimal rolling SMA (running sum; the deque only bounds the window).
    struct Sma {
        int n_;
        double sum_ = 0.0;
        std::deque<double> w_;
        explicit Sma(int n = 1) noexcept : n_(n < 1 ? 1 : n) {}
        void update(double x) {
            w_.push_back(x); sum_ += x;
            while (static_cast<int>(w_.size()) > n_) { sum_ -= w_.front(); w_.pop_front(); }
        }
        double value() const noexcept {
            return w_.empty() ? 0.0 : sum_ / static_cast<double>(w_.size());
        }
        bool ready() const noexcept { return static_cast<int>(w_.size()) >= n_; }
        void reset() noexcept { w_.clear(); sum_ = 0.0; }
    };

    static int inner_period(int n) noexcept { n = n < 2 ? 2 : n; return (n % 2) ? (n + 1) / 2 : n / 2; }
    static int outer_period(int n) noexcept { n = n < 2 ? 2 : n; return (n % 2) ? (n + 1) / 2 : n / 2 + 1; }

    Sma in_, out_;

public:
    explicit TRIMA(int period = 20) noexcept
        : in_(inner_period(period)), out_(outer_period(period)) {}

    void update(double price) {
        in_.update(price);
        // Feed the outer SMA only once the inner is fully warmed, so it
        // averages complete inner readings (no partial-window placeholders).
        if (in_.ready()) out_.update(in_.value());
    }

    double value() const noexcept { return out_.value(); }
    bool   ready() const noexcept { return out_.ready(); }
    void   reset() noexcept { in_.reset(); out_.reset(); }
};
