/*
 * Chandelier Exit — ATR-anchored trailing stop levels (expansion #430).
 *
 *   long_stop  = highest(period) - mult * ATR
 *   short_stop = lowest(period)  + mult * ATR
 *
 * Le Beau's exit: hang the stop from the extreme of the recent move (the
 * chandelier hangs from the ceiling) at a distance of `mult` average true
 * ranges — the stop RATCHETS with new extremes but breathes with the
 * instrument's own volatility, so a quiet name is held tighter than a
 * wild one at the same multiplier. On a close-only stream the extremes
 * are rolling closes and the range leg is CloseATR (#406), the same
 * adaptation Keltner (#414) uses. This is the classic answer to sizing
 * TrailingStop's (#147) FIXED offset: the chandelier IS a trailing stop
 * whose offset is volatility. Defaults follow the convention: 22-bar
 * extremes, 3 ATRs.
 *
 * update(price) convention. O(1) per print, O(period) on an extreme read.
 */
#pragma once

#include "close_atr.hpp"

class Chandelier {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double   ring_[MAX_PERIOD] = {};
    int      period_;
    int      head_  = 0;
    int      count_ = 0;
    CloseATR atr_;

public:
    explicit Chandelier(int period = 22, int atr_period = 14) noexcept
        : period_(period < 1 ? 1 : (period > MAX_PERIOD ? MAX_PERIOD : period)),
          atr_(atr_period) {}

    // update: feed the next price into the extreme window and the range
    // leg. Invalid prices (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        ring_[head_] = price;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
        atr_.update(price);
    }

    // highest / lowest: the rolling extremes of the last `period` prices.
    // 0 before any print.
    double highest() const noexcept {
        double mx = 0.0;
        for (int i = 0; i < count_; ++i) if (ring_[i] > mx) mx = ring_[i];
        return mx;
    }
    double lowest() const noexcept {
        if (count_ == 0) return 0.0;
        double mn = ring_[0];
        for (int i = 1; i < count_; ++i) if (ring_[i] < mn) mn = ring_[i];
        return mn;
    }

    // long_stop: exit level for a LONG — `mult` ATRs below the rolling
    // high. Ratchets UP with new highs, widens only through the ATR.
    double long_stop(double mult = 3.0) const noexcept {
        return highest() - mult * atr_.value();
    }
    // short_stop: exit level for a SHORT — `mult` ATRs above the rolling low.
    double short_stop(double mult = 3.0) const noexcept {
        return lowest() + mult * atr_.value();
    }

    double atr() const noexcept { return atr_.value(); }
    // ready: a full extreme window AND a full ATR period absorbed.
    bool ready() const noexcept { return count_ >= period_ && atr_.ready(); }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
        atr_.reset();
    }
};
