/*
 * POVExecutor — Percentage-of-Volume execution algo (expansion #99).
 *
 * TWAP slices an order by TIME, VWAP by a volume profile set in advance; POV reacts
 * ADAPTIVELY to the current market volume: in each interval it sends a child =
 * participation_rate * observed volume, until the parent order is exhausted.
 * This way it participates proportionally to liquidity — more when the market is
 * active, less when quiet (lower market impact, less signaling).
 *
 *   child = round(rate * market_volume), clamp to the parent's remaining quantity
 *
 * Header-only, deterministic, zero allocation. The caller feeds on_market_volume()
 * the observed volume (e.g. from TradeMsg/Executed) and sends the returned child.
 */
#pragma once

#include <cstdint>

class POVExecutor {
    int64_t  remaining_;   // how much of the parent is still to execute
    int64_t  total_;       // the original parent quantity
    int64_t  executed_;    // how much has been sent in children
    double   rate_;        // participation in volume [0..1]
    uint64_t slices_;      // how many children were generated

public:
    // parent_qty: total quantity to execute; participation_rate: e.g. 0.10 = 10%.
    POVExecutor(int64_t parent_qty, double participation_rate) noexcept
        : remaining_(parent_qty > 0 ? parent_qty : 0),
          total_(remaining_),
          executed_(0),
          rate_(participation_rate < 0.0 ? 0.0 : (participation_rate > 1.0 ? 1.0 : participation_rate)),
          slices_(0) {}

    // on_market_volume: observed volume in this interval → the child size.
    // 0 when the parent is exhausted, there is no volume, or rate*vol < 0.5 (too little
    // to participate meaningfully in this interval).
    int64_t on_market_volume(int64_t market_volume) noexcept {
        if (remaining_ <= 0 || market_volume <= 0) return 0;
        int64_t child = static_cast<int64_t>(rate_ * static_cast<double>(market_volume) + 0.5);
        if (child <= 0) return 0;
        if (child > remaining_) child = remaining_;   // do not exceed the parent
        remaining_ -= child;
        executed_  += child;
        ++slices_;
        return child;
    }

    bool     done()       const noexcept { return remaining_ <= 0; }
    int64_t  remaining()  const noexcept { return remaining_; }
    int64_t  executed()   const noexcept { return executed_; }
    uint64_t slices()     const noexcept { return slices_; }
    // realized_participation: the actual participation = executed / parent (sanity).
    double   completion() const noexcept {
        return total_ > 0 ? static_cast<double>(executed_) / static_cast<double>(total_) : 0.0;
    }
};
