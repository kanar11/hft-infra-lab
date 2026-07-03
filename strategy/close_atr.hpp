/*
 * CloseATR — close-to-close Average True Range (expansion #406).
 *
 * Wilder's ATR on a trade/close stream: with no high/low bars the true
 * range degenerates to |price_t - price_{t-1}|, smoothed the classic way:
 *   warmup  (first N ranges):  running simple mean
 *   after:                     ATR += (TR - ATR) / N     (Wilder smoothing)
 *
 * Volatility in PRICE UNITS ($ per share), which is what stop distances,
 * trailing offsets (TrailingStop #147 takes a fixed offset — this is how
 * you size it) and band widths are quoted in. The sigma-based estimators
 * (RollingStdDev #316, VolatilityEstimator #165) answer "how noisy in %
 * terms"; ATR answers "how many dollars does this thing move per print".
 * Same update(price) convention as RollingStdDev/ZScore. O(1) per print.
 */
#pragma once

class CloseATR {
    double atr_       = 0.0;
    double last_      = 0.0;
    int    period_;
    int    count_     = 0;      // ranges absorbed, saturates at period_
    bool   has_last_  = false;

public:
    explicit CloseATR(int period = 14) noexcept
        : period_(period < 1 ? 1 : period) {}

    // update: feed the next price. The first valid price only seeds the
    // baseline (no previous close to take a range from); invalid prices
    // (non-positive) are ignored entirely.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        if (!has_last_) { last_ = price; has_last_ = true; return; }
        const double tr = (price > last_) ? price - last_ : last_ - price;
        if (count_ < period_) {
            // Warmup: exact running mean of the first N true ranges.
            atr_ += (tr - atr_) / static_cast<double>(count_ + 1);
            ++count_;
        } else {
            // Wilder smoothing: ATR = (ATR*(N-1) + TR) / N.
            atr_ += (tr - atr_) / static_cast<double>(period_);
        }
        last_ = price;
    }

    // value: the current ATR in price units. 0 before any range.
    double value() const noexcept { return atr_; }
    // ready: a full period of ranges has been absorbed.
    bool   ready()  const noexcept { return count_ >= period_; }
    int    period() const noexcept { return period_; }

    void reset() noexcept {
        atr_ = 0.0; last_ = 0.0; count_ = 0; has_last_ = false;
    }
};
