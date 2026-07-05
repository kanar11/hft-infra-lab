/*
 * CenterOfGravity — Ehlers' Center of Gravity oscillator (expansion #526).
 *
 * John Ehlers (2002) treats the last N prices as a distribution of "mass" and
 * measures where its balance point sits, then centers it:
 *   COG = (N+1)/2  -  Sum_{i=0..N-1} (i+1)*P[i] / Sum P[i]
 * with i=0 the MOST RECENT price (weight 1) and i=N-1 the oldest (weight N).
 * When recent prices are heavy (a rising window) the balance point shifts
 * toward the newest bars and COG turns POSITIVE; a falling window pushes it
 * negative; a flat window is perfectly centered and reads exactly 0. Because
 * it weights the whole window rather than differencing endpoints, it turns
 * with almost no lag — Ehlers designed it as an early turning-point detector.
 *
 * Distinct from every MA in the lab (EMA/WMA/Hull/KAMA/VIDYA ...): those output
 * a PRICE, this outputs a zero-centered POSITION oscillator with no smoothing
 * constant. Self-contained (no primitive dependency), price-only. Classic
 * param: period 10. update(price) convention. Header-only.
 */
#pragma once

#include <deque>


class CenterOfGravity {
    int period_;
    std::deque<double> w_;   // oldest at front, newest at back

public:
    explicit CenterOfGravity(int period = 10) noexcept : period_(period < 1 ? 1 : period) {}

    void update(double price) {
        w_.push_back(price);
        while (static_cast<int>(w_.size()) > period_) w_.pop_front();
    }

    double value() const noexcept {
        const int n = static_cast<int>(w_.size());
        if (n < period_) return 0.0;
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; ++i) {
            const double p = w_[n - 1 - i];   // i=0 -> newest (weight 1)
            num += static_cast<double>(i + 1) * p;
            den += p;
        }
        return den != 0.0 ? (n + 1) / 2.0 - num / den : 0.0;
    }

    bool ready() const noexcept { return static_cast<int>(w_.size()) >= period_; }
    void reset() noexcept { w_.clear(); }
};
