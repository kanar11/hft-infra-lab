/*
 * MFI — Money Flow Index (expansion #382).
 *
 * A volume-weighted RSI in [0,100] over a rolling window of N decided ticks:
 *   raw money flow of a tick = price * volume
 *   an uptick's flow counts as POSITIVE, a downtick's as NEGATIVE,
 *   a flat print carries no directional information and is dropped,
 *   MFI = 100 * positive_flow / (positive_flow + negative_flow)
 *       (algebraically identical to the classic 100 - 100/(1 + ratio)).
 *
 * Where RSI (#135) weighs price CHANGES only, MFI weighs each tick's
 * direction by the DOLLARS that traded on it — a 100-share uptick and a
 * 100k-share uptick look identical to RSI but are very different money
 * flow. Completes the volume family with a BOUNDED oscillator: OBV (#341)
 * and PVT (#357) are unbounded cumulative levels, VolumeOscillator (#349)
 * is an EMA rate, ForceIndex (#373) is EMA-smoothed and unbounded.
 *
 * Bar-based MFI uses the typical price (H+L+C)/3 per bar; on a trade
 * stream each print IS the price, so direction comes from the previous
 * trade. Same on_trade(price, volume) convention as OBV/PVT/ForceIndex.
 * Header-only; O(period) on read, O(1) per print.
 */
#pragma once

#include <cstdint>

class MFI {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double flows_[MAX_PERIOD] = {};  // signed money flow per decided tick
    int    period_;
    int    head_       = 0;
    int    count_      = 0;
    double last_price_ = 0.0;
    bool   has_last_   = false;

public:
    explicit MFI(int period = 14) noexcept
        : period_(period < 1 ? 1 : (period > MAX_PERIOD ? MAX_PERIOD : period)) {}

    // on_trade: add a market print. The first valid print only seeds the
    // baseline (no prior price to take a direction from). Flat prints
    // (price unchanged) are ignored entirely: no flow enters the window
    // and no slot is consumed — the standard MFI treatment.
    void on_trade(double price, std::int64_t volume) noexcept {
        if (!(price > 0.0) || volume <= 0) return;
        if (!has_last_) { last_price_ = price; has_last_ = true; return; }
        if (price == last_price_) return;
        const double flow = price * static_cast<double>(volume);
        flows_[head_] = (price > last_price_) ? flow : -flow;
        if (++head_ == period_) head_ = 0;
        if (count_ < period_) ++count_;
        last_price_ = price;
    }

    // value: 100 * pos / (pos + neg) over the current window, in [0,100].
    // 100 = every dollar traded on upticks, 0 = every dollar on downticks,
    // 50 = balanced. Neutral 50 before any decided tick.
    double value() const noexcept {
        double pos = 0.0, neg = 0.0;
        for (int i = 0; i < count_; ++i) {
            const double f = flows_[i];
            if (f > 0.0) pos += f; else neg -= f;
        }
        const double total = pos + neg;
        return total > 0.0 ? 100.0 * pos / total : 50.0;
    }

    // ready: the window holds a full period of decided ticks.
    bool ready()  const noexcept { return count_ >= period_; }
    int  period() const noexcept { return period_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) flows_[i] = 0.0;
        head_ = 0; count_ = 0; last_price_ = 0.0; has_last_ = false;
    }
};
