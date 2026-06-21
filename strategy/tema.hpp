/*
 * TEMA — Triple Exponential Moving Average (expansion #222).
 *
 * Rozszerzenie DEMA (#214) o trzeci stopien wygladzenia:
 *   EMA1 = EMA(price), EMA2 = EMA(EMA1), EMA3 = EMA(EMA2)
 *   TEMA = 3*EMA1 - 3*EMA2 + EMA3
 *
 * Kombinacja trzech EMA niemal calkowicie kasuje zwloke, dajac jeszcze szybszy
 * sygnal niz DEMA przy zachowaniu gladkosci. Kosztem wiekszej wrazliwosci na szum.
 * Header-only, trzy EMA, O(1) pamieci.
 */
#pragma once

#include "ema.hpp"


class TEMA {
    EMA e1_;   // EMA(price)
    EMA e2_;   // EMA(EMA1)
    EMA e3_;   // EMA(EMA2)

public:
    explicit TEMA(int period = 16) noexcept
        : e1_(EMA::from_period(period)),
          e2_(EMA::from_period(period)),
          e3_(EMA::from_period(period)) {}

    void update(double price) noexcept {
        e1_.update(price);
        e2_.update(e1_.value());
        e3_.update(e2_.value());
    }

    double value() const noexcept {
        return 3.0 * e1_.value() - 3.0 * e2_.value() + e3_.value();
    }
    bool   ready() const noexcept { return e1_.ready(); }
    void   reset() noexcept { e1_.reset(); e2_.reset(); e3_.reset(); }
};
