/*
 * Keltner Channel — EMA midline with ATR bands (expansion #414).
 *
 *   mid   = EMA(price, ema_period)
 *   upper = mid + mult * ATR      lower = mid - mult * ATR
 *
 * The ATR-based band family member: Bollinger (#93/#246) widens with the
 * standard deviation of CLOSES (spikes hard on a single outlier print),
 * Keltner widens with the average true range (CloseATR #406) — a slower,
 * sturdier envelope that trend-followers prefer for breakout filters
 * (a close beyond a Keltner band means the move beat typical volatility,
 * not just one noisy print; the classic "squeeze" compares the two:
 * Bollinger inside Keltner = compression). Composed from the lab's own
 * primitives the way MACD (#182) is built on EMA (#173).
 *
 * percent_b(price, mult) maps a price into band coordinates like
 * Bollinger %B (#246): 0.5 at the midline, 1.0 at the upper band,
 * 0.0 at the lower.
 *
 * update(price) convention. Header-only, O(1) per print.
 */
#pragma once

#include "ema.hpp"
#include "close_atr.hpp"

class Keltner {
    EMA      mid_;
    CloseATR atr_;

public:
    explicit Keltner(int ema_period = 20, int atr_period = 14) noexcept
        : mid_(EMA::from_period(ema_period)), atr_(atr_period) {}

    // update: feed the next price into both the midline and the range.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        mid_.update(price);
        atr_.update(price);
    }

    double mid() const noexcept { return mid_.value(); }
    double atr() const noexcept { return atr_.value(); }
    double upper(double mult = 2.0) const noexcept { return mid_.value() + mult * atr_.value(); }
    double lower(double mult = 2.0) const noexcept { return mid_.value() - mult * atr_.value(); }

    // percent_b: the price's position in band coordinates (Bollinger %B
    // convention, #246): 0.5 = at the midline, 1.0 = at the upper band,
    // 0.0 = at the lower, outside [0,1] = beyond the envelope. 0.5 while
    // the channel has no width yet (no range absorbed).
    double percent_b(double price, double mult = 2.0) const noexcept {
        const double up = upper(mult), lo = lower(mult);
        const double width = up - lo;
        return width > 0.0 ? (price - lo) / width : 0.5;
    }

    // ready: the ATR leg has absorbed a full period (the EMA seeds on the
    // first print, so the range is always the later of the two).
    bool ready() const noexcept { return atr_.ready(); }

    void reset() noexcept { mid_.reset(); atr_.reset(); }
};
