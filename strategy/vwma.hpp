/*
 * VWMA — Volume-Weighted Moving Average (expansion #390).
 *
 *   VWMA = Σ (price_i * volume_i) / Σ volume_i   over the last N prints
 *
 * The volume-weighted member of the moving-average family: every MA in the
 * lab so far (EMA #173, WMA #198, HullMA #206, DEMA #214, TEMA #222, KAMA
 * #300) weights prices by TIME position only — a 100-share print moves them
 * exactly as much as a 100k-share print. VWMA weights each print by the
 * size that actually traded, so the average gravitates toward prices where
 * real volume changed hands (support/resistance the market validated with
 * size). With constant volume it degenerates to the plain rolling mean.
 *
 * Distinct from VWAPTracker (#113): that is the SESSION-cumulative VWAP
 * benchmark (never forgets), this is a rolling N-print window that adapts
 * as flow moves. Same on_trade(price, volume) convention as OBV/PVT/
 * ForceIndex/MFI. Header-only; O(1) per print, O(period) on read (exact
 * recompute — no running-sum float drift).
 */
#pragma once

#include <cstdint>

class VWMA {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double prices_[MAX_PERIOD]  = {};
    double volumes_[MAX_PERIOD] = {};
    int    period_;
    int    head_  = 0;
    int    count_ = 0;

public:
    explicit VWMA(int period = 20) noexcept
        : period_(period < 1 ? 1 : (period > MAX_PERIOD ? MAX_PERIOD : period)) {}

    // on_trade: add a market print. Invalid prints (non-positive price or
    // volume) are ignored and do not consume a window slot.
    void on_trade(double price, std::int64_t volume) noexcept {
        if (!(price > 0.0) || volume <= 0) return;
        prices_[head_]  = price;
        volumes_[head_] = static_cast<double>(volume);
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
    }

    // value: Σ(p·v)/Σv over the current window. 0 before any print.
    double value() const noexcept {
        double pv = 0.0, v = 0.0;
        for (int i = 0; i < count_; ++i) {
            pv += prices_[i] * volumes_[i];
            v  += volumes_[i];
        }
        return v > 0.0 ? pv / v : 0.0;
    }

    // ready: the window holds a full period of prints.
    bool ready()  const noexcept { return count_ >= period_; }
    int  period() const noexcept { return period_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) { prices_[i] = 0.0; volumes_[i] = 0.0; }
        head_ = 0; count_ = 0;
    }
};
