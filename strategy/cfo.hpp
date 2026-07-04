/*
 * ChandeForecastOsc — Chande Forecast Oscillator (expansion #494).
 *
 *   CFO = 100 * (price - LSMA(n)) / price
 *
 * How far the latest price sits from its own linear-regression forecast,
 * in percent. Where a Disparity Index measures deviation from a moving
 * AVERAGE (a lagging line), CFO measures it from the least-squares
 * REGRESSION endpoint (LinReg #308, the LSMA — a zero-lag projection of
 * the trend), so on a straight trend it reads ~0: the price is exactly
 * on its own regression line, however steep. It only leaves zero when
 * price pulls AWAY from its trend — positive when it overshoots above,
 * negative when it lags below — a mean-reversion-to-trend gauge rather
 * than a trend-follower. Composed on the lab's own LinReg the way
 * Keltner (#414) rides CloseATR and MACD (#182) rides EMA.
 *
 * update(price) convention. O(period) on read (delegates to LinReg).
 */
#pragma once

#include "linreg.hpp"

class ChandeForecastOsc {
    LinReg reg_;
    double last_price_ = 0.0;
    bool   has_last_   = false;

public:
    explicit ChandeForecastOsc(int period = 14) noexcept : reg_(period) {}

    // update: feed the next price into the regression and remember it.
    // Invalid prices (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        reg_.update(price);
        last_price_ = price;
        has_last_   = true;
    }

    // value: the forecast oscillator in percent. 0 until the regression
    // window has filled (no trend line yet) and exactly 0 on a price that
    // sits on its own regression endpoint.
    double value() const noexcept {
        if (!reg_.ready() || last_price_ <= 0.0) return 0.0;
        return 100.0 * (last_price_ - reg_.value()) / last_price_;
    }

    // lsma: the regression endpoint the price is measured against.
    double lsma()  const noexcept { return reg_.value(); }
    bool   ready() const noexcept { return reg_.ready(); }

    void reset() noexcept { reg_.reset(); last_price_ = 0.0; has_last_ = false; }
};
