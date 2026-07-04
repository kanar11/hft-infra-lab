/*
 * AccelDecel — Bill Williams' Accelerator/Decelerator Oscillator (expansion #446).
 *
 *   AC = AO - SMA(AO, smooth)          (classic smooth = 5)
 *
 * The DERIVATIVE of momentum: AO (#438) measures how far the fast market
 * trades from its slow consensus; AC measures whether that gap is still
 * GROWING. Momentum turns before price does, and acceleration turns
 * before momentum — Williams' early-warning ordering. On a LINEAR ramp
 * the AO is constant (pinned in #438), so AC reads exactly 0: constant
 * momentum is zero acceleration; only a bend in the path moves it.
 * Composed on the lab's own AwesomeOsc the way AC is defined — the same
 * composition story as Keltner-on-CloseATR (#414) and MACD-on-EMA (#182).
 *
 * update(price) convention. O(1) per print, O(period) on read.
 */
#pragma once

#include "awesome.hpp"

class AccelDecel {
public:
    static constexpr int MAX_SMOOTH = 32;

private:
    AwesomeOsc ao_;
    double     ring_[MAX_SMOOTH] = {};   // last `smooth_` AO readings
    int        smooth_;
    int        head_  = 0;
    int        count_ = 0;

public:
    explicit AccelDecel(int fast = 5, int slow = 34, int smooth = 5) noexcept
        : ao_(fast, slow),
          smooth_(smooth < 1 ? 1 : (smooth > MAX_SMOOTH ? MAX_SMOOTH : smooth)) {}

    // update: feed the next price; once the AO leg is ready its readings
    // start filling the smoothing ring. Invalid prices are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        ao_.update(price);
        if (!ao_.ready()) return;
        ring_[head_] = ao_.value();
        if (++head_ == smooth_) head_ = 0;
        if (count_ < smooth_) ++count_;
    }

    double ao() const noexcept { return ao_.value(); }

    // value: AO minus the mean of its own last `smooth` readings. 0 until
    // ready — and exactly 0 on constant momentum thereafter.
    double value() const noexcept {
        if (count_ < smooth_) return 0.0;
        double s = 0.0;
        for (int i = 0; i < smooth_; ++i) s += ring_[i];
        return ao_.value() - s / static_cast<double>(smooth_);
    }

    bool ready() const noexcept { return count_ >= smooth_; }

    void reset() noexcept {
        ao_.reset();
        for (int i = 0; i < MAX_SMOOTH; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
    }
};
