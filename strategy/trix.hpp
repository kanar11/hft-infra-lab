/*
 * TRIX — Triple Exponential momentum oscillator (expansion #230).
 *
 * TRIX = procentowa zmiana POTROJNIE wygladzonej EMA:
 *   s = EMA(EMA(EMA(price)))            (kaskada trzech EMA tego samego okresu)
 *   TRIX_t = (s_t - s_{t-1}) / s_{t-1} * 100
 *
 * Potrojne wygladzenie tlumi szum i krotkie wahniecia, wiec TRIX wychwytuje tylko
 * istotne zmiany trendu. >0 = momentum rosnace, <0 = malejace, przeciecie zera =
 * sygnal. W odroznieniu od TEMA (#222, srednia o niskiej zwloce) TRIX to OSCYLATOR
 * (tempo zmian), nie poziom. Header-only, trzy EMA + poprzednia wartosc.
 */
#pragma once

#include "ema.hpp"


class TRIX {
    EMA    e1_;   // EMA(price)
    EMA    e2_;   // EMA(EMA1)
    EMA    e3_;   // EMA(EMA2) — potrojnie wygladzona
    double prev_ = 0.0;
    bool   have_prev_ = false;
    double trix_ = 0.0;

public:
    explicit TRIX(int period = 15) noexcept
        : e1_(EMA::from_period(period)),
          e2_(EMA::from_period(period)),
          e3_(EMA::from_period(period)) {}

    void update(double price) noexcept {
        e1_.update(price);
        e2_.update(e1_.value());
        e3_.update(e2_.value());
        const double cur = e3_.value();
        if (have_prev_ && prev_ != 0.0) trix_ = (cur - prev_) / prev_ * 100.0;
        prev_ = cur;
        have_prev_ = true;
    }

    double value() const noexcept { return trix_; }   // % tempo zmiany triple-EMA
    bool   ready() const noexcept { return e1_.ready(); }
    void   reset() noexcept {
        e1_.reset(); e2_.reset(); e3_.reset();
        prev_ = 0.0; have_prev_ = false; trix_ = 0.0;
    }
};
