/*
 * ForceIndex — Elder's Force Index (expansion #373).
 *
 *   raw_t   = volume_t * (price_t - price_{t-1})
 *   value   = EMA(raw, period)            (Elder smooths with a 13-period EMA)
 *
 * A volume-weighted momentum: it combines the DIRECTION of the move, its
 * ABSOLUTE size, and the volume behind it into one number. Positive and
 * rising = buyers in control on real volume; a spike then fade often marks
 * exhaustion. It rounds out the lab's volume family:
 *   - OBV (#341)             — direction only (±full volume)
 *   - PVT (#357)             — % price change × volume, cumulative
 *   - VolumeOscillator (#349)— trend of volume itself
 *   - ForceIndex (this)      — absolute Δprice × volume, EMA-smoothed
 *
 * Same on_trade(price, volume) convention as OBV/PVT/VWAPTracker. Built on
 * EMA (#173). Header-only, O(1) per print.
 */
#pragma once

#include "ema.hpp"

#include <cstdint>

class ForceIndex {
    EMA     ema_;
    double  raw_        = 0.0;
    double  last_price_ = 0.0;
    bool    has_last_   = false;

public:
    explicit ForceIndex(int period = 13) noexcept : ema_(EMA::from_period(period)) {}

    // on_trade: add a market print. The first print only seeds last_price_ —
    // there is no prior price to difference against, so nothing feeds the EMA.
    void on_trade(double price, std::int64_t volume) noexcept {
        if (!(price > 0.0) || volume <= 0) return;
        if (has_last_) {
            raw_ = static_cast<double>(volume) * (price - last_price_);
            ema_.update(raw_);
        }
        last_price_ = price;
        has_last_   = true;
    }

    // raw: the latest single-print force (volume * Δprice), unsmoothed.
    double raw()   const noexcept { return raw_; }
    // value: the EMA-smoothed force index (Elder's default 13-period).
    double value() const noexcept { return ema_.value(); }
    bool   bullish() const noexcept { return ema_.value() > 0.0; }
    bool   ready() const noexcept { return ema_.ready(); }
    void   reset() noexcept { ema_.reset(); raw_ = 0.0; last_price_ = 0.0; has_last_ = false; }
};
