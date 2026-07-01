/*
 * VolumeOscillator — trend of trading ACTIVITY, built on two EMAs of volume
 * (expansion #349).
 *
 * value() = (EMA(volume, fast) - EMA(volume, slow)) / EMA(volume, slow) * 100
 *
 * Every indicator in this lab before OBV (#341) read the PRICE stream. OBV
 * was the first to use volume, but as a cumulative running total — a LEVEL,
 * not a rate of change. VolumeOscillator instead measures whether volume
 * ITSELF is trending up or down: positive = recent activity is picking up
 * relative to its longer-run average (often precedes or confirms a price
 * breakout); negative = activity is drying up.
 *
 * Same on_trade(price, volume) convention as VWAPTracker/OBV, though price is
 * unused here — kept for a uniform call signature across the market-data
 * feed. Built on EMA (#173), the same building block as MACD. Header-only,
 * O(1) state (two EMAs).
 */
#pragma once

#include "ema.hpp"

#include <cstdint>

class VolumeOscillator {
    EMA  fast_;
    EMA  slow_;
    bool ready_ = false;

public:
    explicit VolumeOscillator(int fast_period = 5, int slow_period = 20) noexcept
        : fast_(EMA::from_period(fast_period)),
          slow_(EMA::from_period(slow_period)) {}

    // on_trade: add a market print. Non-positive volume is ignored (no print).
    void on_trade(double /*price*/, std::int64_t volume) noexcept {
        if (volume <= 0) return;
        fast_.update(static_cast<double>(volume));
        slow_.update(static_cast<double>(volume));
        ready_ = true;
    }

    // value: percent difference between the fast and slow volume EMAs.
    // 0 when the slow EMA has no positive volume yet.
    double value() const noexcept {
        const double s = slow_.value();
        return s > 0.0 ? (fast_.value() - s) / s * 100.0 : 0.0;
    }
    bool rising() const noexcept { return value() > 0.0; }   // activity trending up
    bool ready()  const noexcept { return ready_; }
    void reset() noexcept { fast_.reset(); slow_.reset(); ready_ = false; }
};
