/*
 * AwesomeOsc — Bill Williams' Awesome Oscillator (expansion #438).
 *
 *   AO = SMA(price, fast) - SMA(price, slow)      (classic 5 / 34)
 *
 * Momentum as the gap between two PLAIN rolling means: positive and
 * widening = the recent market trades above its slower consensus and is
 * accelerating; a zero cross is the classic momentum-flip signal. MACD
 * (#182) and PPO (#365) build the same idea from EMAs — smoother but
 * laggier and never exactly zero on a flat tape; the AO's simple means
 * make it exactly 0 there and exactly readable in tests. Bar-based AO
 * uses the bar midpoint (H+L)/2; on a close/trade stream each print is
 * the price — the same adaptation CloseATR (#406) documents.
 *
 * update(price) convention. O(1) per print, O(period) on read (exact
 * recompute — no running-sum float drift, like VWMA #390 / MFI #382).
 */
#pragma once

class AwesomeOsc {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double ring_[MAX_PERIOD] = {};
    int    fast_, slow_;
    int    head_  = 0;
    int    count_ = 0;

    // mean of the LAST n prices (n <= count_): walk back from the newest.
    double mean_last(int n) const noexcept {
        double s = 0.0;
        int idx = head_;                      // head_ = next write = oldest slot
        for (int i = 0; i < n; ++i) {
            idx = (idx == 0 ? slow_ : idx) - 1;
            s += ring_[idx];
        }
        return s / static_cast<double>(n);
    }

public:
    explicit AwesomeOsc(int fast = 5, int slow = 34) noexcept
        : fast_(fast < 1 ? 1 : fast),
          slow_(slow < 2 ? 2 : (slow > MAX_PERIOD ? MAX_PERIOD : slow)) {
        if (fast_ >= slow_) fast_ = slow_ - 1;   // fast must be the shorter leg
    }

    // update: feed the next price. Invalid prices (non-positive) are ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        ring_[head_] = price;
        if (++head_ == slow_) head_ = 0;
        if (count_ < slow_) ++count_;
    }

    double fast_ma() const noexcept { return count_ >= fast_ ? mean_last(fast_) : 0.0; }
    double slow_ma() const noexcept { return count_ >= slow_ ? mean_last(slow_) : 0.0; }

    // value: fast mean - slow mean. 0 until the slow window fills (and
    // exactly 0 on a flat tape thereafter).
    double value() const noexcept {
        return count_ >= slow_ ? mean_last(fast_) - mean_last(slow_) : 0.0;
    }

    bool ready()  const noexcept { return count_ >= slow_; }
    int  fast()   const noexcept { return fast_; }
    int  slow()   const noexcept { return slow_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) ring_[i] = 0.0;
        head_ = 0; count_ = 0;
    }
};
