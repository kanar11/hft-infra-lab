/*
 * OBV — On-Balance Volume (expansion #341).
 *
 * A cumulative volume-flow indicator (Granville, 1963): a running total that
 * ADDS the print's volume when price ticks UP from the prior print, SUBTRACTS
 * it when price ticks DOWN, and leaves the total UNCHANGED on a flat print.
 *
 * Every other indicator in this lab (EMA/RSI/CCI/Stochastic/...) is a pure
 * PRICE oscillator; OBV is volume-based — it tries to catch accumulation or
 * distribution before it shows up in price. A rising OBV while price is flat
 * or falling is the classic bullish-divergence signal (and vice versa for a
 * falling OBV against a rising price).
 *
 * Header-only, O(1) per print. Same on_trade(price, volume) convention as
 * VWAPTracker (strategy/vwap_tracker.hpp).
 */
#pragma once

#include <cstdint>

class OBV {
    double obv_        = 0.0;
    double last_price_ = 0.0;
    bool   has_last_   = false;

public:
    // on_trade: add a market print (price, volume). The very first print seeds
    // last_price_ only — there is no prior price to compare against yet, so obv_
    // does not move on it.
    void on_trade(double price, std::int64_t volume) noexcept {
        if (!(price > 0.0) || volume <= 0) return;
        if (has_last_) {
            if (price > last_price_)      obv_ += static_cast<double>(volume);
            else if (price < last_price_) obv_ -= static_cast<double>(volume);
            // flat print (price == last_price_): obv_ unchanged
        }
        last_price_ = price;
        has_last_   = true;
    }

    double obv() const noexcept { return obv_; }
    bool   ready() const noexcept { return has_last_; }
    void   reset() noexcept { obv_ = 0.0; last_price_ = 0.0; has_last_ = false; }
};
