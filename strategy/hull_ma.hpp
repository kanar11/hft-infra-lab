/*
 * Hull Moving Average (HMA) — expansion #206.
 *
 * Konstrukcja Alana Hulla, zbudowana na WMA (#198):
 *   raw = 2 * WMA(price, n/2) - WMA(price, n)     (odejmuje opoznienie)
 *   HMA = WMA(raw, sqrt(n))                        (wygladza)
 *
 * Efekt: prawie zerowe opoznienie przy gladkosci porownywalnej z SMA — szybciej
 * lapie zwroty trendu niz EMA/WMA, mniej szumu niz krotka SMA. Header-only,
 * trzy WMA w srodku.
 */
#pragma once

#include "wma.hpp"
#include <cmath>


class HullMA {
    WMA half_;     // WMA(n/2)
    WMA full_;     // WMA(n)
    WMA smooth_;   // WMA(sqrt(n)) nad surowa seria

    static int half_period(int n) noexcept { const int h = n / 2; return h < 1 ? 1 : h; }
    static int sqrt_period(int n) noexcept {
        const int s = static_cast<int>(std::sqrt(static_cast<double>(n)));
        return s < 1 ? 1 : s;
    }

public:
    explicit HullMA(int period = 16) noexcept
        : half_(half_period(period < 1 ? 1 : period)),
          full_(period < 1 ? 1 : period),
          smooth_(sqrt_period(period < 1 ? 1 : period)) {}

    void update(double price) {
        half_.update(price);
        full_.update(price);
        smooth_.update(2.0 * half_.value() - full_.value());   // raw -> wygladzenie
    }

    double value() const noexcept { return smooth_.value(); }
    // gotowa gdy pelne okno WMA(n) ma komplet danych.
    bool   ready() const noexcept { return full_.ready(); }
    void   reset() noexcept { half_.reset(); full_.reset(); smooth_.reset(); }
};
