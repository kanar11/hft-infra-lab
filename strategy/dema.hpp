/*
 * DEMA — Double Exponential Moving Average (expansion #214).
 *
 * Konstrukcja Mulloya, zbudowana na EMA (#173):
 *   DEMA = 2 * EMA(price) - EMA(EMA(price))
 *
 * Drugi czlon (EMA z EMA) szacuje opoznienie pierwszej EMA; odejmujac je dwukrotnie
 * DEMA prawie kasuje zwloke, pozostajac gladka. W odroznieniu od HullMA (oparta na
 * WMA) robi to wykladniczo i bez okna — O(1) pamieci, dwie EMA. Header-only.
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
        ema2_.update(ema1_.value());   // EMA z biezacej EMA
    }

    double value() const noexcept { return 2.0 * ema1_.value() - ema2_.value(); }
    bool   ready() const noexcept { return ema1_.ready(); }
    void   reset() noexcept { ema1_.reset(); ema2_.reset(); }
};
