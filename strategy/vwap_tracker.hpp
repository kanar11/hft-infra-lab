/*
 * VWAPTracker — tracking the market VWAP + execution slippage (expansion #113).
 *
 * VWAP (volume-weighted average price) is the standard benchmark for execution
 * quality: "did I buy cheaper than the market's volume-weighted average?". The tracker
 * accumulates notional and volume from market prints (TradeMsg/Executed) and computes:
 *   - vwap()                — the current VWAP
 *   - slippage_bps(px, buy) — how much worse (>0) / better (<0) our price was
 *                             relative to VWAP, in basis points
 *
 * One tracker per symbol (the caller keeps a map). Header-only, O(1) per print.
 */
#pragma once

#include <cstdint>

class VWAPTracker {
    double  notional_ = 0.0;   // Σ price*volume
    int64_t volume_   = 0;     // Σ volume

public:
    // on_trade: add a market print (price, volume).
    void on_trade(double price, int64_t vol) noexcept {
        if (price > 0.0 && vol > 0) {
            notional_ += price * static_cast<double>(vol);
            volume_   += vol;
        }
    }

    double  vwap()   const noexcept { return volume_ > 0 ? notional_ / static_cast<double>(volume_) : 0.0; }
    int64_t volume() const noexcept { return volume_; }

    // slippage_bps: execution quality relative to VWAP, in bps. For a BUY paying
    // ABOVE VWAP = positive (worse); for a SELL selling BELOW VWAP = positive.
    // <0 = we beat VWAP (better). 0 when there is no VWAP data.
    double slippage_bps(double exec_price, bool is_buy) const noexcept {
        const double v = vwap();
        if (v <= 0.0) return 0.0;
        const double diff = is_buy ? (exec_price - v) : (v - exec_price);
        return diff / v * 10000.0;
    }

    void reset() noexcept { notional_ = 0.0; volume_ = 0; }
};
