/*
 * SuperTrend — ATR-banded trend-following overlay (expansion #462).
 *
 *   basic_upper = price + mult * ATR      basic_lower = price - mult * ATR
 *
 * The bands are LOCKED so they only ratchet in the trend's favour (the
 * lower band can rise but not fall while price holds above it, and vice
 * versa), and the SuperTrend line is whichever locked band the price is
 * currently respecting. A close crossing the active band FLIPS the trend:
 * the line jumps to the opposite band and direction() flips sign. Unlike
 * Chandelier (#406/#430), which only trails a stop for a position you
 * already hold, SuperTrend is a self-contained regime signal — long while
 * the line sits below price, short while it sits above.
 *
 * Same CloseATR (#406) range leg as Keltner (#414) and Chandelier (#430);
 * on a close/trade stream hl2 collapses to the price. update(price)
 * convention. O(1) per print.
 */
#pragma once

#include "close_atr.hpp"

class SuperTrend {
    CloseATR atr_;
    double   mult_;
    double   final_upper_ = 0.0;
    double   final_lower_ = 0.0;
    double   line_        = 0.0;   // the SuperTrend value (active band)
    double   prev_price_  = 0.0;
    int      dir_         = 1;     // +1 uptrend (line = lower), -1 downtrend
    bool     started_     = false; // first computable bar seen

public:
    explicit SuperTrend(int atr_period = 10, double mult = 3.0) noexcept
        : atr_(atr_period), mult_(mult) {}

    // update: feed the next price. The ATR leg warms up first; the band
    // logic starts on the first bar where the ATR is ready. Invalid prices
    // (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        atr_.update(price);
        if (!atr_.ready()) { prev_price_ = price; return; }

        const double a = atr_.value();
        const double basic_upper = price + mult_ * a;
        const double basic_lower = price - mult_ * a;

        if (!started_) {
            // Seed: start long with the line at the lower band.
            final_upper_ = basic_upper;
            final_lower_ = basic_lower;
            dir_  = 1;
            line_ = final_lower_;
            started_ = true;
            prev_price_ = price;
            return;
        }

        // Lock the bands: each only ratchets toward the price.
        final_upper_ = (basic_upper < final_upper_ || prev_price_ > final_upper_)
                           ? basic_upper : final_upper_;
        final_lower_ = (basic_lower > final_lower_ || prev_price_ < final_lower_)
                           ? basic_lower : final_lower_;

        // A close through the active band flips the regime.
        if (dir_ == 1) dir_ = (price < final_lower_) ? -1 : 1;
        else           dir_ = (price > final_upper_) ?  1 : -1;

        line_ = (dir_ == 1) ? final_lower_ : final_upper_;
        prev_price_ = price;
    }

    // value: the SuperTrend line (the active locked band). 0 until ready.
    double value() const noexcept { return line_; }
    // direction: +1 uptrend (line below price), -1 downtrend. +1 before the
    // first computable bar (the seed regime).
    int    direction()  const noexcept { return dir_; }
    bool   is_uptrend() const noexcept { return dir_ == 1; }
    bool   ready()      const noexcept { return started_; }
    double atr()        const noexcept { return atr_.value(); }

    void reset() noexcept {
        atr_.reset();
        final_upper_ = final_lower_ = line_ = prev_price_ = 0.0;
        dir_ = 1; started_ = false;
    }
};
