/*
 * UltimateOsc — Larry Williams' Ultimate Oscillator (expansion #470).
 *
 *   buying_pressure (BP) = max(0, price - prev_price)   (close-stream form)
 *   true_range       (TR) = |price - prev_price|
 *   avg_n = Σ BP over n / Σ TR over n
 *   UO = 100 * (4*avg_short + 2*avg_mid + avg_long) / 7
 *
 * A momentum oscillator in [0,100] that fuses THREE timeframes so a single
 * window's whipsaw cannot swing it — Williams' answer to the false
 * divergences a one-window oscillator (Stochastic #94, RSI #135) throws.
 * The short window is weighted 4x, the mid 2x, the long 1x, so it reacts
 * to fresh momentum while the longer windows anchor it. On a bar feed BP
 * uses the true low; on a close/trade stream the low collapses to the
 * price, the same adaptation CloseATR (#406) documents.
 *
 * Pure uptick runs read 100 (all pressure is buying), pure downtick runs
 * read 0, a balanced tape sits near 50. update(price) convention;
 * O(long_period) on read (exact windowed sums, no running-sum drift).
 */
#pragma once

class UltimateOsc {
public:
    static constexpr int MAX_PERIOD = 64;

private:
    double bp_[MAX_PERIOD] = {};   // buying pressure per decided tick
    double tr_[MAX_PERIOD] = {};   // true range per decided tick
    int    p1_, p2_, p3_;          // short / mid / long windows
    int    head_       = 0;
    int    count_      = 0;
    double last_price_ = 0.0;
    bool   has_last_   = false;

    // sum of the last n entries of a ring (n <= count_): walk back from head.
    double sum_last(const double* ring, int n) const noexcept {
        double s = 0.0;
        int idx = head_;
        for (int i = 0; i < n; ++i) {
            idx = (idx == 0 ? p3_ : idx) - 1;
            s += ring[idx];
        }
        return s;
    }
    double avg_n(int n) const noexcept {
        const double tr = sum_last(tr_, n);
        return tr > 0.0 ? sum_last(bp_, n) / tr : 0.0;
    }

public:
    explicit UltimateOsc(int short_p = 7, int mid_p = 14, int long_p = 28) noexcept
        : p1_(short_p < 1 ? 1 : short_p),
          p2_(mid_p   < 1 ? 1 : mid_p),
          p3_(long_p  < 2 ? 2 : (long_p > MAX_PERIOD ? MAX_PERIOD : long_p)) {
        if (p1_ > p3_) p1_ = p3_;
        if (p2_ > p3_) p2_ = p3_;
    }

    // update: add a market print. The first valid print seeds the baseline
    // (no prior price for a range). Invalid prints (non-positive) ignored.
    void update(double price) noexcept {
        if (!(price > 0.0)) return;
        if (!has_last_) { last_price_ = price; has_last_ = true; return; }
        const double d  = price - last_price_;
        bp_[head_] = d > 0.0 ? d : 0.0;
        tr_[head_] = d > 0.0 ? d : -d;
        if (++head_ == p3_) head_ = 0;
        if (count_ < p3_) ++count_;
        last_price_ = price;
    }

    // value: the Ultimate Oscillator in [0,100]. Neutral 50 until the long
    // window has filled (not enough history to weight three timeframes).
    double value() const noexcept {
        if (count_ < p3_) return 50.0;
        const double uo = (4.0 * avg_n(p1_) + 2.0 * avg_n(p2_) + avg_n(p3_)) / 7.0;
        return 100.0 * uo;
    }

    bool ready() const noexcept { return count_ >= p3_; }

    void reset() noexcept {
        for (int i = 0; i < MAX_PERIOD; ++i) { bp_[i] = 0.0; tr_[i] = 0.0; }
        head_ = 0; count_ = 0; last_price_ = 0.0; has_last_ = false;
    }
};
