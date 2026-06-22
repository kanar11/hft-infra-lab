/*
 * VolatilityEstimator — rolling volatility of returns (expansion #165).
 *
 * Rolling standard deviation of returns ((p - p_prev)/p_prev) over a window of N. The
 * basis of vol-targeting and position sizing: higher volatility -> smaller position (a
 * constant risk budget). A reusable module independent of any specific strategy.
 *
 * O(1) per price thanks to a running sum / sum-of-squares. Header-only.
 */
#pragma once

#include <cmath>
#include <cstdint>


class VolatilityEstimator {
    static constexpr int CAP = 256;
    int    window_;
    double rets_[CAP];
    double sum_, sum_sq_;
    int    count_, head_;
    double prev_;
    bool   has_prev_;

public:
    explicit VolatilityEstimator(int window = 20) noexcept
        : window_(window < 1 ? 1 : (window > CAP ? CAP : window)),
          rets_{}, sum_(0.0), sum_sq_(0.0), count_(0), head_(0), prev_(0.0), has_prev_(false) {}

    // on_price: add a new price. The first = baseline (no return).
    void on_price(double price) noexcept {
        if (price <= 0.0) return;
        if (!has_prev_) { prev_ = price; has_prev_ = true; return; }
        const double r = (price - prev_) / prev_;
        prev_ = price;
        if (count_ == window_) {                      // evict the oldest return
            const double old = rets_[head_];
            sum_ -= old; sum_sq_ -= old * old;
        }
        rets_[head_] = r; sum_ += r; sum_sq_ += r * r;
        head_ = (head_ + 1) % window_;
        if (count_ < window_) ++count_;
    }

    // volatility: standard deviation of returns in the window (0 when <2 samples).
    double volatility() const noexcept {
        if (count_ < 2) return 0.0;
        const double mean = sum_ / count_;
        const double var  = sum_sq_ / count_ - mean * mean;
        return var > 0.0 ? std::sqrt(var) : 0.0;
    }

    // target_size: position size for a given risk budget (vol-targeting):
    // base_size * (target_vol / current_vol), when vol>0; otherwise base_size.
    int32_t target_size(int32_t base_size, double target_vol) const noexcept {
        const double v = volatility();
        if (v <= 0.0 || target_vol <= 0.0) return base_size;
        const double scaled = base_size * (target_vol / v);
        return scaled < 1.0 ? 1 : static_cast<int32_t>(scaled);
    }

    int    samples() const noexcept { return count_; }
};
