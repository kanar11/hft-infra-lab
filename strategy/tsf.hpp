/*
 * TSF — Time Series Forecast (expansion #534).
 *
 * The least-squares regression line projected ONE bar into the future:
 *   TSF = LSMA_endpoint + slope = (a + b*(N-1)) + b = a + b*N
 * where a + b*x is the OLS fit over the last N prices. LinReg (#308) already
 * gives the endpoint (value(), the line at the CURRENT bar) and the slope;
 * TSF advances that one step, forecasting where the fitted trend points next.
 *
 * Distinct from the LSMA endpoint it is built on: LSMA reports the trend's
 * value NOW, TSF the value one bar AHEAD, so in an uptrend TSF leads the LSMA
 * (and price) by exactly one slope step, in a downtrend it trails below. That
 * one-bar lead is what a forecast/crossover strategy wants over the coincident
 * LSMA. On a perfectly linear series the projection is exact. Built on the
 * existing LinReg primitive — the same idiom as Coppock-on-ROC and VIDYA-on-CMO.
 *
 * Classic param: period 14. update(price) convention. Header-only.
 */
#pragma once

#include "linreg.hpp"


class TSF {
    LinReg lr_;

public:
    explicit TSF(int period = 14) noexcept : lr_(period) {}

    void update(double price) { lr_.update(price); }

    // value: the regression line projected to the NEXT bar (x = N) = LSMA + slope.
    double value() const noexcept { return lr_.value() + lr_.slope(); }
    // slope: the per-bar trend of the fit (shared with the LSMA it forecasts).
    double slope() const noexcept { return lr_.slope(); }
    bool   ready() const noexcept { return lr_.ready(); }
    void   reset() noexcept { lr_.reset(); }
};
