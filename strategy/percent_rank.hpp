/*
 * RollingPercentRank — non-parametric price position (expansion #486).
 *
 *   rank = 100 * (# of the last N prices strictly below the current) / (N-1)
 *
 * Where the latest price sits in the DISTRIBUTION of its recent window, in
 * [0,100]: 100 when it is the highest of the window, 0 when the lowest,
 * 50 when half are below. Stochastic %K (#94) measures position in the
 * min-max RANGE (value distance); this counts HOW MANY prints are below
 * (rank distance). They agree on a uniform ramp but diverge hard on a
 * skewed window: with {10,11,12,13,100} a print of 13 is a 3% stochastic
 * (barely off the low of a wide range) yet a 75% rank (three of four
 * below it) — the rank is immune to a single outlier stretching the
 * range, the way RollingMedian (#454) is for the center.
 *
 * update(price) convention. O(period) on read; O(1) per print.
 */
#pragma once

class RollingPercentRank {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double ring_[MAX_PERIOD] = {};
    int    period_;
    int    head_  = 0;
    int    count_ = 0;

public:
    explicit RollingPercentRank(int period = 20) noexcept
        : period_(period < 2 ? 2 : (period > MAX_PERIOD ? MAX_PERIOD : period)) {}

    // update: add a market print. Invalid prints (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        ring_[head_] = price;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
    }

    // value: the current price's percent rank within the window, in [0,100].
    // The current print is the most recently written slot. 50 (neutral)
    // until at least two prints exist (no distribution yet).
    double value() const noexcept {
        if (count_ < 2) return 50.0;
        // newest slot = the one before head_ (head_ points at the next write).
        const int cur_idx = (head_ == 0 ? period_ : head_) - 1;
        const double cur = ring_[cur_idx];
        int below = 0;
        for (int i = 0; i < count_; ++i)
            if (i != cur_idx && ring_[i] < cur) ++below;
        return 100.0 * static_cast<double>(below) / static_cast<double>(count_ - 1);
    }

    bool ready()  const noexcept { return count_ >= period_; }
    int  period() const noexcept { return period_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
    }
};
