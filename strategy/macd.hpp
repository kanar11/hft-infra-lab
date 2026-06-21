/*
 * MACD — Moving Average Convergence Divergence (expansion #182).
 *
 * Klasyczny wskaznik momentum, zbudowany na trzech EMA (#173):
 *   MACD     = EMA(fast) - EMA(slow)      (zbieznosc/rozbieznosc dwoch srednich)
 *   signal   = EMA(MACD, signal_period)   (wygladzona linia MACD)
 *   histogram= MACD - signal              (przyspieszenie momentum)
 *
 * Interpretacja: histogram > 0 (MACD nad sygnalem) = momentum byczy; przeciecie
 * przez zero (zmiana znaku histogramu) = sygnal kupna/sprzedazy. Domyslne okresy
 * 12/26/9 to konwencja Appela. Header-only, O(1) stanu (trzy EMA).
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

    // update: dolicz cene, przelicz MACD i linie sygnalu.
    void update(double price) noexcept {
        const double f = fast_.update(price);
        const double s = slow_.update(price);
        signal_.update(f - s);
        ready_ = true;
    }

    double macd()      const noexcept { return fast_.value() - slow_.value(); }
    double signal()    const noexcept { return signal_.value(); }
    double histogram() const noexcept { return macd() - signal(); }
    // bullish: MACD nad linia sygnalu (histogram dodatni).
    bool   bullish()   const noexcept { return histogram() > 0.0; }
    bool   ready()     const noexcept { return ready_; }

    void reset() noexcept {
        fast_.reset(); slow_.reset(); signal_.reset(); ready_ = false;
    }
};
