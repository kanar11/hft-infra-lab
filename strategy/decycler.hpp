/*
 * Decycler — John Ehlers' Simple Decycler (expansion #582).
 *
 * Subtracts a 2-pole HIGH-PASS of period N from the price, leaving everything
 * slower than N bars — the trend — untouched:
 *   w  = sqrt(2)*pi / N
 *   a  = (cos w + sin w - 1) / cos w
 *   HP = (1-a/2)^2 * (P - 2P' + P'') + 2(1-a)HP' - (1-a)^2 HP''
 *   Decycler = P - HP
 * The high-pass sees only CURVATURE (P - 2P' + P''), so on a perfectly linear
 * trend it outputs 0 and the decycler tracks price EXACTLY — zero lag on
 * trends is the whole design (a smoother of the same period would trail a
 * ramp forever), while a one-bar cycle spike is largely absorbed by the HP
 * and mostly removed from the output. Both properties are asserted.
 *
 * Distinct from the lab's smoothers: SuperSmoother (#566) is the matching
 * LOW-pass (keeps the fast detail out), the decycler is its complement built
 * the other way — subtract the fast detail, keep the rest. Ehlers pairs them
 * as the two halves of his filter toolkit. Classic period: 60 (trend), small
 * N for tests. update(price) convention. Header-only.
 */
#pragma once

#include <cmath>


class Decycler {
    double a_, c_;
    double p1_ = 0.0, p2_ = 0.0, hp1_ = 0.0, hp2_ = 0.0;
    double dec_ = 0.0;
    bool   seeded_ = false;

public:
    explicit Decycler(int period = 60) noexcept {
        const double n = static_cast<double>(period < 3 ? 3 : period);
        const double w = std::sqrt(2.0) * M_PI / n;
        a_ = (std::cos(w) + std::sin(w) - 1.0) / std::cos(w);
        c_ = (1.0 - a_ / 2.0) * (1.0 - a_ / 2.0);
    }

    void update(double price) noexcept {
        if (!(price > 0.0)) return;              // invalid prints ignored
        if (!seeded_) {
            p1_ = p2_ = price; hp1_ = hp2_ = 0.0; dec_ = price;
            seeded_ = true;
            return;
        }
        const double hp = c_ * (price - 2.0 * p1_ + p2_)
                        + 2.0 * (1.0 - a_) * hp1_
                        - (1.0 - a_) * (1.0 - a_) * hp2_;
        hp2_ = hp1_; hp1_ = hp;
        p2_ = p1_; p1_ = price;
        dec_ = price - hp;
    }

    double value() const noexcept { return dec_; }
    bool   ready() const noexcept { return seeded_; }
    void   reset() noexcept {
        p1_ = p2_ = hp1_ = hp2_ = dec_ = 0.0;
        seeded_ = false;
    }
};
