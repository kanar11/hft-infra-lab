/*
 * PPO — Percentage Price Oscillator (expansion #365).
 *
 *   PPO      = (EMA(fast) - EMA(slow)) / EMA(slow) * 100
 *   signal   = EMA(PPO, signal_period)
 *   histogram= PPO - signal
 *
 * The percentage-normalized sibling of MACD (#182): MACD reports the raw
 * price difference of two EMAs, PPO divides by the slow EMA so the result is
 * a percent — directly comparable across instruments at very different price
 * levels (a $5 MACD means something different on a $20 stock than on a $2000
 * one; a PPO of 2% is the same signal on both). It is also the PRICE
 * counterpart of VolumeOscillator (#349), which applies the identical
 * (fast-slow)/slow formula to VOLUME.
 *
 * Built on EMA (#173), the same building block as MACD. Default periods
 * 12/26/9 (Appel's MACD convention). Header-only, O(1) state (three EMAs).
 */
#pragma once

#include "ema.hpp"


class PPO {
    EMA  fast_;
    EMA  slow_;
    EMA  signal_;
    bool ready_ = false;

public:
    explicit PPO(int fast_period = 12, int slow_period = 26, int signal_period = 9) noexcept
        : fast_(EMA::from_period(fast_period)),
          slow_(EMA::from_period(slow_period)),
          signal_(EMA::from_period(signal_period)) {}

    // update: add a price, recompute the PPO line and its signal.
    void update(double price) noexcept {
        fast_.update(price);
        slow_.update(price);
        signal_.update(ppo());
        ready_ = true;
    }

    // ppo: percentage spread of the fast vs slow EMA. 0 when the slow EMA is
    // not positive yet.
    double ppo() const noexcept {
        const double s = slow_.value();
        return s > 0.0 ? (fast_.value() - s) / s * 100.0 : 0.0;
    }
    double signal()    const noexcept { return signal_.value(); }
    double histogram() const noexcept { return ppo() - signal(); }
    // bullish: PPO above its signal line (positive histogram).
    bool   bullish()   const noexcept { return histogram() > 0.0; }
    bool   ready()     const noexcept { return ready_; }

    void reset() noexcept {
        fast_.reset(); slow_.reset(); signal_.reset(); ready_ = false;
    }
};
