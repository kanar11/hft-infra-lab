/*
 * MACD — Moving Average Convergence Divergence (expansion #182).
 *
 * A classic momentum indicator, built on three EMAs (#173):
 *   MACD     = EMA(fast) - EMA(slow)      (convergence/divergence of two averages)
 *   signal   = EMA(MACD, signal_period)   (smoothed MACD line)
 *   histogram= MACD - signal              (momentum acceleration)
 *
 * Interpretation: histogram > 0 (MACD above the signal) = bullish momentum; a zero
 * crossing (the histogram changing sign) = a buy/sell signal. The default periods
 * 12/26/9 are Appel's convention. Header-only, O(1) state (three EMAs).
 */
#pragma once

#include "ema.hpp"


class MACD {
    EMA  fast_;
    EMA  slow_;
    EMA  signal_;
    bool ready_ = false;

public:
    explicit MACD(int fast_period = 12, int slow_period = 26, int signal_period = 9) noexcept
        : fast_(EMA::from_period(fast_period)),
          slow_(EMA::from_period(slow_period)),
          signal_(EMA::from_period(signal_period)) {}

    // update: add a price, recompute MACD and the signal line.
    void update(double price) noexcept {
        const double f = fast_.update(price);
        const double s = slow_.update(price);
        signal_.update(f - s);
        ready_ = true;
    }

    double macd()      const noexcept { return fast_.value() - slow_.value(); }
    double signal()    const noexcept { return signal_.value(); }
    double histogram() const noexcept { return macd() - signal(); }
    // bullish: MACD above the signal line (positive histogram).
    bool   bullish()   const noexcept { return histogram() > 0.0; }
    bool   ready()     const noexcept { return ready_; }

    void reset() noexcept {
        fast_.reset(); slow_.reset(); signal_.reset(); ready_ = false;
    }
};
