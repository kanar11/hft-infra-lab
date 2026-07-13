/*
 * SuperSmoother — John Ehlers' 2-pole Butterworth smoother (expansion #566).
 *
 *   a1 = exp(-sqrt(2)*pi / N)         b1 = 2*a1*cos(sqrt(2)*pi / N)
 *   c2 = b1   c3 = -a1^2   c1 = 1 - c2 - c3
 *   SS = c1*(P + P_prev)/2 + c2*SS_prev + c3*SS_prev2
 *
 * A 2-pole Butterworth low-pass with a 2-bar averager in front: Ehlers'
 * replacement for the EMA/SMA as the smoothing element inside indicators. The
 * Butterworth pole placement gives a maximally flat passband — price swings
 * slower than the N-bar cutoff pass nearly untouched, faster ones are crushed
 * — with roughly HALF the lag of an SMA of the same smoothness (the classic
 * single-pole EMA trades those off linearly; two poles beat that curve).
 * Because c1+c2+c3 == 1 by construction, a flat tape pins EXACTLY at the
 * level (asserted in tests).
 *
 * Distinct from every smoother in the lab: EMA/DEMA/TEMA are single-pole
 * cascades, ALMA (#550) is a windowed kernel, the adaptive family (KAMA #300 /
 * VIDYA #518 / McGinley #542) varies its speed with the data — this is a
 * FIXED recursive 2-pole IIR filter with an engineered frequency response.
 * Classic param: period 10. update(price) convention. Header-only.
 */
#pragma once

#include <cmath>


class SuperSmoother {
    double c1_, c2_, c3_;
    double ss_ = 0.0, ss1_ = 0.0, ss2_ = 0.0;
    double prev_price_ = 0.0;
    bool   seeded_ = false;

public:
    explicit SuperSmoother(int period = 10) noexcept {
        const double n  = static_cast<double>(period < 2 ? 2 : period);
        const double a1 = std::exp(-std::sqrt(2.0) * M_PI / n);
        c2_ = 2.0 * a1 * std::cos(std::sqrt(2.0) * M_PI / n);
        c3_ = -a1 * a1;
        c1_ = 1.0 - c2_ - c3_;
    }

    void update(double price) noexcept {
        if (!(price > 0.0)) return;              // invalid prints ignored
        if (!seeded_) {
            ss_ = ss1_ = ss2_ = prev_price_ = price;
            seeded_ = true;
            return;
        }
        const double out = c1_ * (price + prev_price_) / 2.0 + c2_ * ss1_ + c3_ * ss2_;
        ss2_ = ss1_; ss1_ = out; ss_ = out;
        prev_price_ = price;
    }

    double value() const noexcept { return ss_; }
    bool   ready() const noexcept { return seeded_; }
    void   reset() noexcept { ss_ = ss1_ = ss2_ = prev_price_ = 0.0; seeded_ = false; }
};
