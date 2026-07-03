/*
 * ZLEMA — Zero-Lag Exponential Moving Average (expansion #422).
 *
 *   lag    = (period - 1) / 2
 *   ZLEMA  = EMA( 2 * price_t - price_{t-lag} )
 *
 * Ehlers/Way's lag compensation: an EMA's output trails its input by
 * roughly `lag` bars, so feed it a price EXTRAPOLATED by that same lag
 * (2p_t - p_{t-lag} linearly projects the price one lag forward) and the
 * smoothing cancels most of the delay. In a steady trend ZLEMA hugs the
 * price where a plain EMA (#173) trails behind it; on a flat tape the
 * de-lagged input equals the price and ZLEMA degenerates to the EMA.
 * The cost is overshoot on sharp turns — the extrapolation keeps pointing
 * the old way for `lag` prints.
 *
 * MA family: EMA/WMA/Hull/DEMA/TEMA/KAMA/VWMA + ZLEMA. HullMA (#206) and
 * DEMA/TEMA (#214/#222) also fight lag but by RE-SMOOTHING smoothed
 * series; ZLEMA instead corrects the INPUT — one EMA, one small price
 * ring. update(price) convention. O(1) per print.
 */
#pragma once

#include "ema.hpp"

class ZLEMA {
public:
    static constexpr int MAX_LAG = 64;

private:
    EMA    ema_;
    double ring_[MAX_LAG] = {};   // last `lag_` prices (value-init)
    int    lag_;
    int    head_  = 0;
    int    count_ = 0;

public:
    explicit ZLEMA(int period = 20) noexcept
        : ema_(EMA::from_period(period < 1 ? 1 : period)),
          lag_((period < 1 ? 1 : (period > 2 * MAX_LAG ? 2 * MAX_LAG : period) - 1) / 2) {}

    // update: feed the next price. Until `lag` prices are buffered the raw
    // price feeds the EMA (no history to de-lag against). Invalid prices
    // (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        double input = price;
        if (lag_ > 0) {
            if (count_ >= lag_) {
                // head_ points at the oldest of the last `lag_` prices.
                input = 2.0 * price - ring_[head_];
            }
            ring_[head_] = price;
            if (++head_ == lag_) head_ = 0;
            if (count_ < lag_) ++count_;
        }
        ema_.update(input);
    }

    double value() const noexcept { return ema_.value(); }
    // ready: the EMA is seeded AND the de-lag buffer is full (lag 0 needs
    // only the seed).
    bool   ready() const noexcept { return ema_.ready() && count_ >= lag_; }
    int    lag()   const noexcept { return lag_; }

    void reset() noexcept {
        ema_.reset();
        for (int i = 0; i < MAX_LAG; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
    }
};
