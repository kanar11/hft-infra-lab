/*
 * RollingMedian — outlier-immune rolling price filter (expansion #454).
 *
 * The median of the last N prints. Every mean in the MA family (EMA, WMA,
 * VWMA, ZLEMA, ...) moves when a single absurd print lands — a fat-finger
 * trade or a stub-quote fill drags the average and everything built on it
 * (bands, oscillators, stops). The median ignores it entirely until HALF
 * the window agrees: the classic robust filter for the price feed a
 * signal actually consumes. Even windows average the two middle values.
 *
 * update(price) convention. O(1) per print; the read sorts a local copy —
 * O(N log N) with N <= 64, a primitive-tier cost like VWMA/MFI's exact
 * recompute reads.
 */
#pragma once

#include <algorithm>

class RollingMedian {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double ring_[MAX_PERIOD] = {};
    int    period_;
    int    head_  = 0;
    int    count_ = 0;

public:
    explicit RollingMedian(int period = 9) noexcept
        : period_(period < 1 ? 1 : (period > MAX_PERIOD ? MAX_PERIOD : period)) {}

    // update: feed the next price. Invalid prices (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        ring_[head_] = price;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
    }

    // value: the median of the current window (partial windows use what
    // has arrived). 0 before any print.
    double value() const noexcept {
        if (count_ == 0) return 0.0;
        double tmp[MAX_PERIOD];
        for (int i = 0; i < count_; ++i) tmp[i] = ring_[i];
        std::sort(tmp, tmp + count_);
        return (count_ & 1) ? tmp[count_ / 2]
                            : 0.5 * (tmp[count_ / 2 - 1] + tmp[count_ / 2]);
    }

    bool ready()  const noexcept { return count_ >= period_; }
    int  period() const noexcept { return period_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
    }
};
