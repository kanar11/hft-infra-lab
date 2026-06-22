/*
 * LinReg — least-squares linear regression over a price window (expansion #308).
 *
 * Fits y = a + b*x to the last N prices (x = 0..N-1) by ordinary least squares and
 * exposes two views:
 *   slope()  = b   — the per-bar trend (sign = direction, magnitude = strength),
 *   value()  = a + b*(N-1) — the regression line evaluated at the LATEST bar, i.e.
 *              the Least Squares Moving Average (LSMA / "endpoint MA").
 *
 * LSMA tracks price with far less lag than an SMA because it projects the fitted
 * trend to the current bar instead of averaging the past; slope() is a clean,
 * noise-resistant trend filter (zero-cross = trend flip). Header-only, holds a
 * window of N prices. On a perfectly linear series slope and endpoint are exact.
 */
#pragma once

#include <deque>


class LinReg {
    int period_;
    std::deque<double> window_;

    // ols: compute slope b and intercept a over the current window (x = 0..n-1).
    void ols(double& a, double& b) const noexcept {
        const int n = static_cast<int>(window_.size());
        double sx = 0.0, sy = 0.0, sxy = 0.0, sxx = 0.0;
        for (int i = 0; i < n; ++i) {
            const double x = static_cast<double>(i), y = window_[static_cast<std::size_t>(i)];
            sx += x; sy += y; sxy += x * y; sxx += x * x;
        }
        const double denom = static_cast<double>(n) * sxx - sx * sx;
        b = denom != 0.0 ? (static_cast<double>(n) * sxy - sx * sy) / denom : 0.0;
        a = (sy - b * sx) / static_cast<double>(n);
    }

public:
    explicit LinReg(int period = 14) noexcept : period_(period < 2 ? 2 : period) {}

    void update(double price) {
        window_.push_back(price);
        while (static_cast<int>(window_.size()) > period_) window_.pop_front();
    }

    double slope() const noexcept {
        if (window_.size() < 2) return 0.0;
        double a = 0.0, b = 0.0; ols(a, b);
        return b;
    }
    // value: LSMA — the fitted line at the latest bar (x = n-1).
    double value() const noexcept {
        const int n = static_cast<int>(window_.size());
        if (n < 1) return 0.0;
        if (n < 2) return window_.back();
        double a = 0.0, b = 0.0; ols(a, b);
        return a + b * static_cast<double>(n - 1);
    }
    bool ready() const noexcept { return static_cast<int>(window_.size()) >= period_; }
    void reset() noexcept { window_.clear(); }
};
