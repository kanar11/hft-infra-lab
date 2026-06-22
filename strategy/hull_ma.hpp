/*
 * Hull Moving Average (HMA) — expansion #206.
 *
 * Alan Hull's construction, built on the WMA (#198):
 *   raw = 2 * WMA(price, n/2) - WMA(price, n)     (subtracts lag)
 *   HMA = WMA(raw, sqrt(n))                        (smooths)
 *
 * Effect: near-zero lag with smoothness comparable to an SMA — catches trend
 * turns faster than an EMA/WMA, with less noise than a short SMA. Header-only,
 * three WMAs inside.
 */
#pragma once

#include "wma.hpp"
#include <cmath>


class HullMA {
    WMA half_;     // WMA(n/2)
    WMA full_;     // WMA(n)
    WMA smooth_;   // WMA(sqrt(n)) over the raw series

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
        smooth_.update(2.0 * half_.value() - full_.value());   // raw -> smoothing
    }

    double value() const noexcept { return smooth_.value(); }
    // ready when the full WMA(n) window has a complete set of data.
    bool   ready() const noexcept { return full_.ready(); }
    void   reset() noexcept { half_.reset(); full_.reset(); smooth_.reset(); }
};
