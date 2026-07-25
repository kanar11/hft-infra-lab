/*
 * SchaffTrendCycle — Doug Schaff's STC oscillator (expansion #605).
 *
 * MACD run through a DOUBLE stochastic, then smoothed — a cyclical momentum
 * oscillator bounded in [0, 100]:
 *   macd  = EMA(fast) - EMA(slow)
 *   %K1   = stochastic(macd, cycle)          (macd within its cycle-window range)
 *   %D1   = %K1 smoothed by `factor`         (a light EMA of %K1)
 *   %K2   = stochastic(%D1, cycle)           (the smoothed line within ITS range)
 *   STC   = %K2 smoothed by `factor`
 * Schaff's insight was that MACD is a good trend detector but a laggy, unbounded
 * one; wrapping it in a stochastic makes it CYCLICAL and bounded, so it turns
 * earlier and reads like an overbought/oversold gauge (>75 uptrend / overbought,
 * <25 downtrend / oversold). A flat window (no range to normalize) HOLDS the
 * previous value rather than dividing by zero, so a plateaued MACD freezes STC
 * where the trend left it.
 *
 * Distinct from the lab's other MACD reads: MACD (#182) is the raw line and
 * histogram, PPO (#365) its percentage form — both unbounded trend/momentum;
 * STC is a bounded CYCLE oscillator, a different question (where in the swing,
 * not how strong). Built on the EMA primitive (#173), the composite idiom
 * (Coppock/KST/T3). Bounded by construction: every stage lives in [0,100] and
 * the smoothing of in-range values stays in range. Classic params 23/50/10.
 * update(price) convention. Header-only.
 */
#pragma once

#include "ema.hpp"

#include <algorithm>
#include <deque>


class SchaffTrendCycle {
    EMA    fast_, slow_;
    int    cycle_;
    double factor_;
    std::deque<double> macd_win_;   // last `cycle` MACD values
    std::deque<double> d1_win_;     // last `cycle` %D1 values
    double pf_ = 0.0, pff_ = 0.0;   // the two smoothed lines (%D1, STC)
    bool   pf_seeded_ = false, pff_seeded_ = false;

    // stoch: place x within [min,max] of a window, in [0,100]; a flat window
    // (no range) has no answer, so the caller holds the previous value.
    static double stoch(const std::deque<double>& w, double x, bool& flat) noexcept {
        const double lo = *std::min_element(w.begin(), w.end());
        const double hi = *std::max_element(w.begin(), w.end());
        if (hi - lo > 0.0) { flat = false; return (x - lo) / (hi - lo) * 100.0; }
        flat = true; return 0.0;
    }

public:
    explicit SchaffTrendCycle(int fast = 23, int slow = 50, int cycle = 10,
                              double factor = 0.5) noexcept
        : fast_(EMA::from_period(fast)), slow_(EMA::from_period(slow)),
          cycle_(cycle < 1 ? 1 : cycle),
          factor_(factor > 0.0 && factor <= 1.0 ? factor : 0.5) {}

    void update(double price) {
        const double macd = fast_.update(price) - slow_.update(price);
        macd_win_.push_back(macd);
        while (static_cast<int>(macd_win_.size()) > cycle_) macd_win_.pop_front();

        bool flat = false;
        double k1 = stoch(macd_win_, macd, flat);
        if (flat) k1 = pf_seeded_ ? pf_ : 50.0;            // hold on a flat MACD window
        if (!pf_seeded_) { pf_ = k1; pf_seeded_ = true; }
        else             { pf_ += factor_ * (k1 - pf_); }  // %D1

        d1_win_.push_back(pf_);
        while (static_cast<int>(d1_win_.size()) > cycle_) d1_win_.pop_front();

        double k2 = stoch(d1_win_, pf_, flat);
        if (flat) k2 = pff_seeded_ ? pff_ : 50.0;
        if (!pff_seeded_) { pff_ = k2; pff_seeded_ = true; }
        else              { pff_ += factor_ * (k2 - pff_); }  // STC
    }

    double value() const noexcept { return pff_; }
    bool   ready() const noexcept {
        return static_cast<int>(macd_win_.size()) >= cycle_
            && static_cast<int>(d1_win_.size()) >= cycle_;
    }
    void reset() noexcept {
        fast_.reset(); slow_.reset();
        macd_win_.clear(); d1_win_.clear();
        pf_ = pff_ = 0.0; pf_seeded_ = pff_seeded_ = false;
    }
};
