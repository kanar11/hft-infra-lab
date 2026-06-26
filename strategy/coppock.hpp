/*
 * Coppock Curve — long-horizon momentum oscillator (expansion #333).
 *
 * Coppock = WMA_w( ROC_long + ROC_short ): the weighted moving average of the
 * SUM of two rate-of-change momenta. Designed by Edwin Coppock (1962) for
 * monthly index data — a turn UP through the zero line from below is the
 * classic long-horizon BUY signal. It is deliberately slow, filtering
 * short-term noise to catch major trend changes.
 *
 * Distinct from MACD/TRIX (EMA cascades) and from a lone ROC: it blends two
 * ROC horizons and then linearly weights them (WMA), so recent momentum
 * dominates while the long lookback anchors the trend. Built on the existing
 * ROC and WMA primitives — same idiom as MACD-on-EMA and Hull-on-WMA.
 *
 * Classic params: WMA=10, ROC_long=14, ROC_short=11. Header-only.
 */
#pragma once

#include "roc.hpp"
#include "wma.hpp"


class Coppock {
    ROC roc_long_;
    ROC roc_short_;
    WMA wma_;

public:
    explicit Coppock(int wma_period = 10, int roc_long = 14,
                     int roc_short = 11) noexcept
        : roc_long_(roc_long), roc_short_(roc_short), wma_(wma_period) {}

    void update(double price) {
        roc_long_.update(price);
        roc_short_.update(price);
        // Feed the WMA only once BOTH ROCs are warmed up, so the curve averages
        // fully-formed momentum readings (no half-baked ROC=0 placeholders
        // leaking in during warmup and biasing the early values toward zero).
        if (roc_long_.ready() && roc_short_.ready())
            wma_.update(roc_long_.value() + roc_short_.value());
    }

    double value() const noexcept { return wma_.value(); }
    bool   ready() const noexcept { return wma_.ready(); }
    // The classic Coppock trigger is a zero-line cross; expose the sign so a
    // strategy can detect that cross across consecutive reads.
    bool   bullish() const noexcept { return ready() && value() > 0.0; }
    void   reset() noexcept { roc_long_.reset(); roc_short_.reset(); wma_.reset(); }
};
