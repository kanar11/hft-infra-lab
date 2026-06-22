/*
 * TSI — True Strength Index (expansion #284).
 *
 * Double-smoothed momentum oscillator (Blau):
 *   momentum = price - prev_price
 *   TSI = 100 * EMA_s(EMA_r(momentum)) / EMA_s(EMA_r(|momentum|))
 *
 * The two-stage EMA smoothing (default r=25 long, s=13 short) strips most of the
 * noise that whips ROC/CMO around, so TSI gives a clean trend-strength reading in
 * roughly [-100, +100]: near +100 = strong, persistent up-moves, near -100 = down.
 * A zero-line cross or a signal-line cross is the trade trigger. Built on the EMA
 * primitive (#173). Header-only, four EMAs + the previous price.
 */
#pragma once

#include "ema.hpp"
#include <cmath>


class TSI {
    EMA    pc1_;    // EMA_r(momentum)
    EMA    pc2_;    // EMA_s(EMA_r(momentum))  — double-smoothed momentum
    EMA    apc1_;   // EMA_r(|momentum|)
    EMA    apc2_;   // EMA_s(EMA_r(|momentum|)) — double-smoothed absolute momentum
    double prev_ = 0.0;
    bool   have_prev_ = false;

public:
    explicit TSI(int long_period = 25, int short_period = 13) noexcept
        : pc1_(EMA::from_period(long_period)), pc2_(EMA::from_period(short_period)),
          apc1_(EMA::from_period(long_period)), apc2_(EMA::from_period(short_period)) {}

    void update(double price) noexcept {
        if (!have_prev_) { prev_ = price; have_prev_ = true; return; }
        const double mom = price - prev_;
        prev_ = price;
        pc1_.update(mom);  pc2_.update(pc1_.value());
        apc1_.update(std::fabs(mom)); apc2_.update(apc1_.value());
    }

    double value() const noexcept {
        const double denom = apc2_.value();
        return denom != 0.0 ? 100.0 * pc2_.value() / denom : 0.0;
    }
    bool ready() const noexcept { return have_prev_ && pc1_.ready(); }
    void reset() noexcept {
        pc1_.reset(); pc2_.reset(); apc1_.reset(); apc2_.reset();
        prev_ = 0.0; have_prev_ = false;
    }
};
