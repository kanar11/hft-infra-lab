/*
 * ALMA — Arnaud Legoux Moving Average (expansion #550, MILESTONE 550).
 *
 * A Gaussian-kernel weighted average over the last N prices:
 *   m   = offset * (N - 1)              (where the kernel peak sits)
 *   s   = N / sigma                     (kernel width)
 *   w_i = exp( -(i - m)^2 / (2 s^2) )   (i = 0 oldest .. N-1 newest)
 *   ALMA = Σ w_i P_i / Σ w_i
 *
 * The two knobs make it a tunable lag/smoothness trade-off, which is what made
 * it popular: offset -> 1 shifts the Gaussian peak onto the NEWEST bars (low
 * lag, tracks like a fast MA), offset 0.5 centers it (zero phase shift, like a
 * symmetric smoother), and sigma controls how wide the kernel is (small sigma
 * -> nearly one bar; large -> nearly a plain SMA). Classic params: window 9,
 * offset 0.85, sigma 6.
 *
 * Distinct from every MA in the lab: the EMA family is recursive, WMA is a
 * fixed linear ramp, KAMA (#300) / VIDYA (#518) / McGinley (#542) adapt to the
 * DATA — ALMA is a fixed GAUSSIAN kernel over the window whose shape the user
 * chooses. With offset 0.5 the kernel is symmetric, so on ANY window it
 * returns a phase-neutral center estimate (pinned by test). Header-only,
 * update(price) convention.
 */
#pragma once

#include <cmath>
#include <deque>


class ALMA {
    int    period_;
    double offset_;
    double sigma_;
    std::deque<double> w_;   // oldest at front, newest at back

public:
    explicit ALMA(int period = 9, double offset = 0.85, double sigma = 6.0) noexcept
        : period_(period < 1 ? 1 : period),
          offset_(offset < 0.0 ? 0.0 : (offset > 1.0 ? 1.0 : offset)),
          sigma_(sigma > 0.0 ? sigma : 6.0) {}

    void update(double price) {
        w_.push_back(price);
        while (static_cast<int>(w_.size()) > period_) w_.pop_front();
    }

    double value() const noexcept {
        const int n = static_cast<int>(w_.size());
        if (n < period_) return 0.0;
        const double m = offset_ * static_cast<double>(n - 1);
        const double s = static_cast<double>(n) / sigma_;
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; ++i) {
            const double d = static_cast<double>(i) - m;
            const double wgt = std::exp(-(d * d) / (2.0 * s * s));
            num += wgt * w_[static_cast<std::size_t>(i)];
            den += wgt;
        }
        return den > 0.0 ? num / den : 0.0;
    }

    bool ready() const noexcept { return static_cast<int>(w_.size()) >= period_; }
    void reset() noexcept { w_.clear(); }
};
