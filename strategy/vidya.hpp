/*
 * VIDYA — Variable Index Dynamic Average (Tushar Chande, expansion #518).
 *
 * An adaptive moving average whose smoothing speeds up in a strong move and
 * slows to a crawl in chop:
 *   k       = |CMO(period)| / 100          (the volatility index, in [0,1])
 *   alpha   = 2 / (period + 1)             (the base EMA constant)
 *   VIDYA_t = alpha*k * price_t + (1 - alpha*k) * VIDYA_{t-1}
 * When momentum is strong (|CMO| near 100) the effective smoothing alpha*k is
 * large and VIDYA tracks price like a fast EMA; when the tape is choppy
 * (|CMO| near 0) alpha*k collapses toward 0 and VIDYA flattens, filtering the
 * noise instead of whipsawing through it.
 *
 * Distinct from KAMA (#300), the lab's other adaptive MA: KAMA scales its
 * smoothing by Kaufman's Efficiency Ratio (net change / summed absolute change),
 * VIDYA by Chande's Momentum Oscillator (net move / summed absolute move as a
 * signed oscillator) — related ideas, different adaptivity gauges. Built on the
 * existing CMO primitive (#268), the same idiom as Coppock-on-ROC and KST-on-ROC.
 *
 * Classic param: period 9. update(price) convention. Header-only.
 */
#pragma once

#include "cmo.hpp"
#include <cmath>


class VIDYA {
    CMO    cmo_;
    double alpha_;          // base EMA smoothing 2/(period+1)
    double value_ = 0.0;

public:
    explicit VIDYA(int period = 9) noexcept
        : cmo_(period), alpha_(2.0 / (static_cast<double>(period < 1 ? 1 : period) + 1.0)) {}

    void update(double price) {
        cmo_.update(price);
        if (!cmo_.ready()) {
            // Warmup: hold the current price so the first ready value seeds the
            // recursion from the live level, not from 0.
            value_ = price;
            return;
        }
        const double k = std::fabs(cmo_.value()) / 100.0;   // [0,1] volatility index
        const double a = alpha_ * k;                         // effective smoothing
        value_ = a * price + (1.0 - a) * value_;
    }

    double value() const noexcept { return value_; }
    bool   ready() const noexcept { return cmo_.ready(); }
    void   reset() noexcept { cmo_.reset(); value_ = 0.0; }
};
