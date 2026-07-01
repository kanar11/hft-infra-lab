/*
 * PVT — Price Volume Trend (expansion #357).
 *
 * PVT_t = PVT_{t-1} + volume_t * (price_t - price_{t-1}) / price_{t-1}
 *
 * A cumulative volume-flow indicator like OBV (#341), but where OBV adds or
 * subtracts the FULL volume based only on the DIRECTION of the tick, PVT
 * weights each volume by the MAGNITUDE of the price move (as a % change) —
 * a 0.1% uptick and a 5% uptick on the same volume move PVT very
 * differently, whereas they move OBV identically. Together with OBV
 * (direction-only) and VolumeOscillator (#349, the rate-of-change of
 * volume itself), this completes the lab's volume-based trio.
 *
 * Same on_trade(price, volume) convention as OBV/VWAPTracker. Header-only,
 * O(1) per print.
 */
#pragma once

#include <cstdint>

class PVT {
    double pvt_        = 0.0;
    double last_price_ = 0.0;
    bool   has_last_   = false;

public:
    // on_trade: add a market print. The first print only seeds last_price_ —
    // there is no prior price to compute a % change against yet, so pvt_
    // does not move on it.
    void on_trade(double price, std::int64_t volume) noexcept {
        if (!(price > 0.0) || volume <= 0) return;
        if (has_last_ && last_price_ > 0.0) {
            pvt_ += static_cast<double>(volume) * (price - last_price_) / last_price_;
        }
        last_price_ = price;
        has_last_   = true;
    }

    double value() const noexcept { return pvt_; }
    bool   ready() const noexcept { return has_last_; }
    void   reset() noexcept { pvt_ = 0.0; last_price_ = 0.0; has_last_ = false; }
};
