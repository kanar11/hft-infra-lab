/*
 * TrailingStop — a trailing stop (expansion #147).
 *
 * A stop that FOLLOWS a favorable price move (ratchet) and never moves back:
 *   long  -> stop = max(stop, price - trail); exit when price <= stop
 *   short -> stop = min(stop, price + trail); exit when price >= stop
 * Protects profit: when price moves our way the stop tightens, and when it reverses by
 * `trail` from the peak/trough — we close the position. A reusable exit module for
 * any strategy (mean-reversion/momentum/...).
 *
 * Header-only, no dependencies. update(price) returns true when STOPPED OUT.
 */
#pragma once

class TrailingStop {
    bool   is_long_;
    double trail_;
    double stop_;
    bool   active_;

public:
    TrailingStop(bool is_long, double entry_price, double trail_amount) noexcept
        : is_long_(is_long),
          trail_(trail_amount > 0.0 ? trail_amount : 0.0),
          stop_(is_long ? entry_price - trail_amount : entry_price + trail_amount),
          active_(true) {}

    // update: pass a new price. Ratchets the stop in the favorable direction; returns true
    // when price breaks the stop (position closed). After exit, subsequent update -> false.
    bool update(double price) noexcept {
        if (!active_) return false;
        if (is_long_) {
            const double cand = price - trail_;
            if (cand > stop_) stop_ = cand;            // ratchet up
            if (price <= stop_) { active_ = false; return true; }
        } else {
            const double cand = price + trail_;
            if (cand < stop_) stop_ = cand;            // ratchet down
            if (price >= stop_) { active_ = false; return true; }
        }
        return false;
    }

    double stop()   const noexcept { return stop_; }
    bool   active() const noexcept { return active_; }
    bool   is_long() const noexcept { return is_long_; }
};
