/*
 * PrettyGoodOsc — Mark Johnson's Pretty Good Oscillator (expansion #502).
 *
 *   PGO = (price - SMA(n)) / ATR(n)
 *
 * How far the price sits from its simple moving average, measured in units
 * of the average true range. Like a z-score (ZScore #276) but normalized
 * by ATR instead of the standard deviation: a reading of +3 means the
 * price is three typical daily ranges above its mean — a strong,
 * volatility-scaled breakout, comparable across instruments of different
 * price and noise. Where CloseATR (#406) drives a channel (Keltner #414),
 * a trailing stop (Chandelier #430) and a regime overlay (SuperTrend
 * #462), PGO uses it as the DENOMINATOR of a momentum oscillator.
 *
 * On a bar feed the range uses the true high/low; on a close/trade stream
 * both collapse to the price (the CloseATR adaptation). update(price)
 * convention. O(period) on read.
 */
#pragma once

#include "close_atr.hpp"

class PrettyGoodOsc {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double   ring_[MAX_PERIOD] = {};   // prices for the SMA
    int      period_;
    int      head_  = 0;
    int      count_ = 0;
    CloseATR atr_;

public:
    explicit PrettyGoodOsc(int period = 14) noexcept
        : period_(period < 1 ? 1 : (period > MAX_PERIOD ? MAX_PERIOD : period)),
          atr_(period) {}

    // update: feed the next price into the SMA window and the range leg.
    // Invalid prices (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        ring_[head_] = price;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
        atr_.update(price);
    }

    // value: the ATR-normalized displacement. 0 until ready, and 0 on a
    // flat tape (zero ATR -> no scale).
    double value() const noexcept {
        if (!ready()) return 0.0;
        const double a = atr_.value();
        if (a <= 0.0) return 0.0;
        double sum = 0.0;
        for (int i = 0; i < count_; ++i) sum += ring_[i];
        const double sma = sum / static_cast<double>(count_);
        // the latest price is the slot before head_
        const int cur = (head_ == 0 ? period_ : head_) - 1;
        return (ring_[cur] - sma) / a;
    }

    // ready: both the SMA window and the ATR leg have filled.
    bool ready() const noexcept { return count_ >= period_ && atr_.ready(); }
    double atr() const noexcept { return atr_.value(); }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
        atr_.reset();
    }
};
