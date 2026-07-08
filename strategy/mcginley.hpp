/*
 * McGinleyDynamic — John McGinley's self-adjusting moving average (expansion #542).
 *
 *   MD_t = MD_{t-1} + (P_t - MD_{t-1}) / (k * N * (P_t / MD_{t-1})^4)
 *
 * seeded with the first price (k = 0.6 by convention, N the nominal period).
 * The (P/MD)^4 term is the whole point: when price runs ABOVE the line the
 * ratio exceeds 1 and the divisor grows, so MD slows down and refuses to chase
 * a rally; when price falls BELOW, the ratio shrinks the divisor and MD speeds
 * up, hugging a sell-off instead of being late to it. McGinley designed it as
 * a moving average with the lag problem corrected asymmetrically — markets
 * fall faster than they rise, so the average tracks down-moves faster too.
 *
 * Distinct from the lab's other adaptive MAs: KAMA (#300) scales by the
 * Efficiency Ratio and VIDYA (#518) by |CMO| — both symmetric in direction —
 * while McGinley's adjustment is a smooth function of price's POSITION vs the
 * line itself, with a deliberate down-side bias. Self-contained, close-only.
 *
 * Classic params: period 14, k 0.6. update(price) convention. Header-only.
 */
#pragma once


class McGinleyDynamic {
    double n_;              // nominal period
    double k_;              // smoothing constant (0.6 classic)
    double md_    = 0.0;
    bool   seeded_ = false;

public:
    explicit McGinleyDynamic(int period = 14, double k = 0.6) noexcept
        : n_(period < 1 ? 1.0 : static_cast<double>(period)),
          k_(k > 0.0 ? k : 0.6) {}

    void update(double price) noexcept {
        if (!(price > 0.0)) return;          // invalid prints ignored
        if (!seeded_) { md_ = price; seeded_ = true; return; }
        const double ratio = price / md_;
        const double r2    = ratio * ratio;
        md_ += (price - md_) / (k_ * n_ * r2 * r2);   // divisor: k*N*(P/MD)^4
    }

    double value() const noexcept { return md_; }
    bool   ready() const noexcept { return seeded_; }
    void   reset() noexcept { md_ = 0.0; seeded_ = false; }
};
