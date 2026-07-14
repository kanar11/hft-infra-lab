/*
 * T3 — Tim Tillson's T3 moving average (expansion #574).
 *
 * A cascade of six EMAs combined with binomial weights in the volume factor v:
 *   e1..e6 = EMA(price), EMA(e1), ..., EMA(e5)      (all with alpha = 2/(N+1))
 *   c1 = -v^3          c2 = 3v^2 + 3v^3
 *   c3 = -6v^2 - 3v - 3v^3        c4 = 1 + 3v + v^3 + 3v^2
 *   T3 = c1*e6 + c2*e5 + c3*e4 + c4*e3
 * which is GD(GD(GD(price))) for the "generalized DEMA" GD(x) = (1+v)*EMA(x)
 * - v*EMA(EMA(x)). At v = 0 it collapses to a triple EMA-of-EMA (smooth,
 * laggy); at v = 1 it is a full DEMA cascade (fast, overshooting); Tillson's
 * v = 0.7 sits deliberately between — near-DEMA lag with near-TEMA smoothness
 * and far less overshoot than either extreme.
 *
 * Distinct from DEMA (#214) / TEMA (#222): those are FIXED combinations
 * (2E1-E2 and 3E1-3E2+E3); T3 exposes the blend as the v knob and doubles the
 * cascade depth, which is why its step response is famously smooth. Built on
 * the existing EMA primitive (#173), the composite idiom. c1+c2+c3+c4 == 1
 * for every v, so a flat tape pins EXACTLY at the level (asserted). Classic
 * params: period 5, v 0.7. update(price) convention. Header-only.
 */
#pragma once

#include "ema.hpp"


class T3 {
    EMA e1_, e2_, e3_, e4_, e5_, e6_;
    double c1_, c2_, c3_, c4_;

public:
    explicit T3(int period = 5, double v = 0.7) noexcept
        : e1_(EMA::from_period(period)), e2_(EMA::from_period(period)),
          e3_(EMA::from_period(period)), e4_(EMA::from_period(period)),
          e5_(EMA::from_period(period)), e6_(EMA::from_period(period)) {
        const double vv = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        const double v2 = vv * vv, v3 = v2 * vv;
        c1_ = -v3;
        c2_ = 3.0 * v2 + 3.0 * v3;
        c3_ = -6.0 * v2 - 3.0 * vv - 3.0 * v3;
        c4_ = 1.0 + 3.0 * vv + v3 + 3.0 * v2;
    }

    void update(double price) noexcept {
        e6_.update(e5_.update(e4_.update(e3_.update(e2_.update(e1_.update(price))))));
    }

    double value() const noexcept {
        return c1_ * e6_.value() + c2_ * e5_.value()
             + c3_ * e4_.value() + c4_ * e3_.value();
    }
    bool ready() const noexcept { return e6_.ready(); }
    void reset() noexcept {
        e1_.reset(); e2_.reset(); e3_.reset();
        e4_.reset(); e5_.reset(); e6_.reset();
    }
};
