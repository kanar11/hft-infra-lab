/*
 * ChoppinessIndex — trend-vs-range regime classifier (expansion #478).
 *
 *   CI = 100 * log10( Σ TR(n) / (highest(n) - lowest(n)) ) / log10(n)
 *
 * Not a direction signal but a REGIME one: when price trends, the summed
 * true range barely exceeds the window's high-low span (the path is
 * efficient), so the ratio -> 1, log10 -> 0, CI -> 0. When price chops
 * inside a range, the summed range dwarfs the span (lots of motion, no
 * progress), the ratio -> n and CI -> 100. Momentum oscillators
 * (Ultimate #470, RSI #135) tell you WHICH way; this tells you whether
 * there is a trend to ride at all — a filter that turns a breakout system
 * off in the chop and a mean-reversion system off in the trend.
 *
 * On a bar feed TR uses the true high/low; on a close/trade stream both
 * collapse to the price (TR = |dprice|, the CloseATR #406 adaptation).
 * update(price) convention; O(period) on read.
 */
#pragma once

#include <cmath>

class ChoppinessIndex {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double tr_[MAX_PERIOD]    = {};   // true range per decided tick
    double price_[MAX_PERIOD] = {};   // price per decided tick (for high/low)
    int    period_;
    int    head_       = 0;
    int    count_      = 0;
    double last_price_ = 0.0;
    bool   has_last_   = false;

public:
    explicit ChoppinessIndex(int period = 14) noexcept
        : period_(period < 2 ? 2 : (period > MAX_PERIOD ? MAX_PERIOD : period)) {}

    // update: add a market print. The first valid print seeds the baseline
    // (no prior price for a range). Invalid prints (non-positive) ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        if (!has_last_) { last_price_ = price; has_last_ = true; return; }
        const double d = price - last_price_;
        tr_[head_]    = d > 0.0 ? d : -d;
        price_[head_] = price;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
        last_price_ = price;
    }

    // value: the Choppiness Index in [0,100]. 50 (neutral) until the window
    // fills. Low = trending, high = choppy/ranging.
    double value() const noexcept {
        if (count_ < period_) return 50.0;
        double sum_tr = 0.0, hi = price_[0], lo = price_[0];
        for (int i = 0; i < count_; ++i) {
            sum_tr += tr_[i];
            if (price_[i] > hi) hi = price_[i];
            if (price_[i] < lo) lo = price_[i];
        }
        const double range = hi - lo;
        if (range <= 0.0) return 0.0;              // degenerate flat window
        double ratio = sum_tr / range;
        if (ratio < 1.0) ratio = 1.0;              // path cannot be shorter than the span
        const double ci = 100.0 * std::log10(ratio) / std::log10(static_cast<double>(period_));
        return ci < 0.0 ? 0.0 : (ci > 100.0 ? 100.0 : ci);
    }

    bool ready()  const noexcept { return count_ >= period_; }
    int  period() const noexcept { return period_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) { tr_[i] = 0.0; price_[i] = 0.0; }
        head_ = 0; count_ = 0; last_price_ = 0.0; has_last_ = false;
    }
};
