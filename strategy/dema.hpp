/*
 * DEMA — Double Exponential Moving Average (expansion #214).
 *
 * Mulloy's construction, built on the EMA (#173):
 *   DEMA = 2 * EMA(price) - EMA(EMA(price))
 *
 * The second term (EMA of EMA) estimates the lag of the first EMA; by subtracting it
 * twice DEMA almost cancels the lag while staying smooth. Unlike HullMA (based on the
 * WMA) it does this exponentially and windowless — O(1) memory, two EMAs. Header-only.
 */
#pragma once

#include "ema.hpp"


class DEMA {
    EMA ema1_;   // EMA(price)
    EMA ema2_;   // EMA(EMA(price))

public:
    explicit DEMA(int period = 16) noexcept
        : ema1_(EMA::from_period(period)), ema2_(EMA::from_period(period)) {}

    void update(double price) noexcept {
        ema1_.update(price);
        ema2_.update(ema1_.value());   // EMA of the current EMA
    }

    double value() const noexcept { return 2.0 * ema1_.value() - ema2_.value(); }
    bool   ready() const noexcept { return ema1_.ready(); }
    void   reset() noexcept { ema1_.reset(); ema2_.reset(); }
};
