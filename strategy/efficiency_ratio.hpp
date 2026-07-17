/*
 * EfficiencyRatio — Kaufman's Efficiency Ratio as its own primitive
 * (expansion #590, MILESTONE 590).
 *
 *   ER = |P_t - P_{t-N}| / Σ_{i=t-N+1..t} |P_i - P_{i-1}|   in [0, 1]
 *
 * Net progress over total distance travelled: 1.0 means every tick moved the
 * same direction (a perfectly efficient straight line), 0.0 means the path
 * went nowhere however much it moved (a round trip nets |ΔP| = 0). THE
 * trendiness gauge, and the adaptivity engine inside KAMA (#300) — which uses
 * it buried in its smoothing constant and never exposes the number, though
 * the number is the read a regime filter or a position-sizer actually wants:
 * scale size UP when the market pays for direction, DOWN when it chops.
 *
 * Distinct from ChoppinessIndex (#478), the other regime read: CI compares
 * the summed TRUE RANGE to the window's high-low SPAN (log-scaled, 0..100,
 * high = choppy), ER compares NET DISPLACEMENT to the path length (linear,
 * 0..1, high = trending) — direction-blind vs endpoint-anchored, so a window
 * that ends where it started can read mid-range on CI yet EXACTLY 0 here.
 * Both endpoints of the scale are exact and asserted. Classic param: 10.
 * update(price) convention; O(period) on read. Header-only.
 */
#pragma once

#include <deque>


class EfficiencyRatio {
    int period_;
    std::deque<double> w_;   // holds period_+1 prices (N deltas)

public:
    explicit EfficiencyRatio(int period = 10) noexcept
        : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        w_.push_back(price);
        while (static_cast<int>(w_.size()) > period_ + 1) w_.pop_front();
    }

    double value() const noexcept {
        const int n = static_cast<int>(w_.size());
        if (n < period_ + 1) return 0.0;
        double path = 0.0;
        for (int i = 1; i < n; ++i) {
            const double d = w_[static_cast<std::size_t>(i)]
                           - w_[static_cast<std::size_t>(i - 1)];
            path += d < 0.0 ? -d : d;
        }
        if (path <= 0.0) return 0.0;               // flat window: no path, no trend
        const double net = w_.back() - w_.front();
        return (net < 0.0 ? -net : net) / path;
    }

    bool ready() const noexcept { return static_cast<int>(w_.size()) >= period_ + 1; }
    void reset() noexcept { w_.clear(); }
};
