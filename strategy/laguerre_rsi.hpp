/*
 * LaguerreRSI — John Ehlers' Laguerre-filter RSI (expansion #558).
 *
 * A four-stage Laguerre filter cascade replaces the fixed lookback window of a
 * classic RSI (gamma in (0,1) sets the effective memory):
 *   L0 = (1-g)*P + g*L0'      L1 = -g*L0 + L0' + g*L1'
 *   L2 = -g*L1 + L1' + g*L2'  L3 = -g*L2 + L2' + g*L3'
 * then the up/down pressure is read off the cascade's shape:
 *   CU = Σ max(Lk - Lk+1, 0),  CD = Σ max(Lk+1 - Lk, 0)   (k = 0..2)
 *   LaRSI = CU / (CU + CD)  in [0, 1]
 * In a steady uptrend every stage leads the next (CD = 0) and the oscillator
 * PINS at exactly 1.0; a steady downtrend pins 0.0; a flat cascade (all stages
 * equal) has no pressure either way and reads a NEUTRAL 0.5 here (Ehlers'
 * original holds its previous value; a defined neutral is friendlier to a
 * stateless caller). The Laguerre warp gives it the RSI shape with far less
 * lag and almost no whipsaw at small gamma — Ehlers' fix for the classic
 * RSI's tradeoff between speed and noise.
 *
 * Distinct from RSIStrategy (#135, a per-symbol SIGNAL machine on a fixed
 * window): this is a reusable single-series primitive, and the smoothing is a
 * recursive filter cascade, not a window. Classic thresholds: > 0.8 overbought,
 * < 0.2 oversold. gamma 0.5 default. update(price) convention. Header-only.
 */
#pragma once


class LaguerreRSI {
    double g_;
    double l0_ = 0.0, l1_ = 0.0, l2_ = 0.0, l3_ = 0.0;
    bool   seeded_ = false;

public:
    explicit LaguerreRSI(double gamma = 0.5) noexcept
        : g_(gamma > 0.0 && gamma < 1.0 ? gamma : 0.5) {}

    void update(double price) noexcept {
        if (!(price > 0.0)) return;              // invalid prints ignored
        if (!seeded_) { l0_ = l1_ = l2_ = l3_ = price; seeded_ = true; return; }
        const double l0p = l0_, l1p = l1_, l2p = l2_;
        l0_ = (1.0 - g_) * price + g_ * l0_;
        l1_ = -g_ * l0_ + l0p + g_ * l1_;
        l2_ = -g_ * l1_ + l1p + g_ * l2_;
        l3_ = -g_ * l2_ + l2p + g_ * l3_;
    }

    double value() const noexcept {
        if (!seeded_) return 0.0;
        double cu = 0.0, cd = 0.0;
        acc(l0_, l1_, cu, cd);
        acc(l1_, l2_, cu, cd);
        acc(l2_, l3_, cu, cd);
        const double tot = cu + cd;
        return tot > 0.0 ? cu / tot : 0.5;       // flat cascade -> neutral
    }

    bool ready()      const noexcept { return seeded_; }
    bool overbought() const noexcept { return seeded_ && value() > 0.8; }
    bool oversold()   const noexcept { return seeded_ && value() < 0.2; }
    void reset() noexcept { l0_ = l1_ = l2_ = l3_ = 0.0; seeded_ = false; }

private:
    static void acc(double a, double b, double& cu, double& cd) noexcept {
        if (a >= b) cu += a - b; else cd += b - a;
    }
};
