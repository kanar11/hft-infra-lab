/*
 * exec_algo.hpp — execution algorithms: TWAP and VWAP.
 *
 * How does this differ from mean_reversion / market_maker?
 *
 *   - Mean reversion / market maker are **ALPHA strategies** — they try to
 *     predict where price is going and profit from the move.
 *   - VWAP / TWAP are **EXECUTION algorithms** — they do *not* predict price.
 *     They have a "parent" order (parent order: buy 100k AAPL by end of day)
 *     and must slice it over time so as to:
 *       a) execute the whole thing before the deadline
 *       b) NOT move the market with their own order flow (if a fund
 *          dumped 100k in one ticket, price would run up and
 *          they would pay more themselves — slippage)
 *       c) achieve an average price close to the benchmark (market VWAP)
 *
 * Who uses this: every buy-side (pension funds, ETFs, asset
 * managers) that has large orders to execute. A sell-side bank
 * sells execution algos as a product. It is the bread and butter
 * of institutional trading.
 *
 * TWAP (Time-Weighted Average Price):
 *   The simplest slicer. Splits the parent order into N equal pieces
 *   and sends one every T seconds. Pro: zero overhead, deterministic.
 *   Con: ignores the volume distribution — it sends the same amount at 9:35 (low
 *   volume, easy to move the market) as at 15:55 (high volume, easy
 *   to hide).
 *
 * VWAP (Volume-Weighted Average Price):
 *   A volume-profile-aware slicer. It receives a historical
 *   profile of "what % of daily volume is usually traded in slot X"
 *   (most often U-shaped: 30% morning, 40% middle, 30% near the close)
 *   and sends proportionally to the expected volume in that slot.
 *   Pro: average price closer to the market benchmark. Con: if the profile
 *   is wrong (today is a different day), you pay for it in slippage.
 *
 * Quality measure: **slippage in basis points (bps)** vs the market VWAP benchmark.
 *   1 bps = 0.01% = $0.01 per $100. Production HFT chases <1 bps
 *   on large orders; retail brokers run around 5-20 bps.
 */
#pragma once

#include "../common/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>


namespace exec {


// ParentOrder — a "parent" order to execute within a specified time window.
//
//   total_qty       — total quantity of shares to execute
//   start_ts_sec    — when we start (seconds from some zero point;
//                     typically "seconds from market open 09:30 ET")
//   duration_sec    — width of the window (e.g. 23400 = 6.5h, a full session)
//   num_slices      — into how many child orders we slice the parent
//
// Example: BUY 10000 AAPL over 60 seconds, 20 slots of 3 seconds each:
//   {symbol="AAPL", side=BUY, total_qty=10000, start=0, duration=60, slices=20}
struct ParentOrder {
    char     symbol[9];
    Side     side;
    int32_t  total_qty;
    int32_t  start_ts_sec;
    int32_t  duration_sec;
    int      num_slices;

    ParentOrder() noexcept : side(Side::BUY), total_qty(0),
                              start_ts_sec(0), duration_sec(0), num_slices(0) {
        symbol[0] = '\0';
    }
};


// ChildOrder — a single "piece" to send on this tick.
//   valid=false → send nothing on this tick (e.g. the time for the next
//                  slot has not arrived yet, or the parent is already executed).
struct ChildOrder {
    int32_t qty;          // shares in this child
    int32_t price_ticks;  // limit price in ticks (0 = market order)
    bool    valid;

    ChildOrder() noexcept : qty(0), price_ticks(0), valid(false) {}
};


// ExecStats — execution result. Accumulated by apply_fill().
struct ExecStats {
    int64_t  filled_qty;       // shares executed
    int64_t  cash_ticks;       // sum of qty*price_ticks over all fills (signed)
    int      num_fills;        // number of separate fills
    int      slices_emitted;   // number of child orders submitted
    int      slices_skipped;   // how many slots passed without emission (e.g. after hours)

    ExecStats() noexcept : filled_qty(0), cash_ticks(0), num_fills(0),
                            slices_emitted(0), slices_skipped(0) {}

    // Realized VWAP = sum(qty*price) / sum(qty), in ticks.
    int32_t realized_vwap_ticks() const noexcept {
        return filled_qty > 0 ? static_cast<int32_t>(cash_ticks / filled_qty) : 0;
    }
};


// Slippage in basis points vs a benchmark.
//   BUY:  if realized > benchmark, you pay more → positive bps is BAD
//   SELL: if realized < benchmark, you got less → positive bps is BAD
// Convention: positive bps = worse execution than the benchmark.
inline double slippage_bps(Side side, int32_t realized_ticks, int32_t benchmark_ticks) noexcept {
    if (benchmark_ticks <= 0) return 0.0;
    const double diff = static_cast<double>(realized_ticks - benchmark_ticks);
    const double bps  = (diff / benchmark_ticks) * 10000.0;
    return side == Side::BUY ? bps : -bps;
}


// TWAPExecutor — equal pieces, equal time intervals.
//
// Slot N starts at time start_ts + N * seconds_per_slice.
// Each slot submits an identical child order (total_qty / num_slices).
// The division remainder goes to the last slot (guarantees the sum = total).
class TWAPExecutor {
    ParentOrder parent_;
    int         seconds_per_slice_;
    int32_t     slice_size_;     // base (may be +remainder in the last slot)
    ExecStats   stats_;

public:
    explicit TWAPExecutor(const ParentOrder& parent) noexcept
        : parent_(parent),
          seconds_per_slice_(parent.num_slices > 0
              ? std::max(1, parent.duration_sec / parent.num_slices) : 1),
          slice_size_(parent.num_slices > 0
              ? parent.total_qty / parent.num_slices : 0) {}

    // on_tick: called every tick by the market data feed (usually every second).
    // Returns a ChildOrder to send (valid=true) or valid=false if there is nothing
    // to do on this tick. price_ticks=0 → market order
    // (the caller decides whether it wants a limit at the mid etc.).
    ChildOrder on_tick(int32_t now_sec) noexcept {
        ChildOrder o;
        if (parent_.num_slices <= 0 || stats_.filled_qty >= parent_.total_qty) return o;
        if (now_sec < parent_.start_ts_sec)                                     return o;

        // Which slot should be submitted now?
        const int32_t elapsed = now_sec - parent_.start_ts_sec;
        const int     slot    = elapsed / seconds_per_slice_;
        if (slot < stats_.slices_emitted) return o;  // not time yet
        if (slot >= parent_.num_slices)   return o;  // window closed

        // Slot size: base, the last slot gets the division remainder.
        int32_t qty = slice_size_;
        if (slot == parent_.num_slices - 1) {
            qty = parent_.total_qty - slice_size_ * (parent_.num_slices - 1);
        }
        // Do not submit more than remains of the total.
        const int32_t remaining = parent_.total_qty - static_cast<int32_t>(stats_.filled_qty);
        if (qty > remaining) qty = remaining;
        if (qty <= 0) return o;

        o.qty         = qty;
        o.price_ticks = 0;   // market order — let the caller decide
        o.valid       = true;
        ++stats_.slices_emitted;
        return o;
    }

    // apply_fill: called by the fills handler after execution on the exchange.
    void apply_fill(int32_t qty, int32_t price_ticks) noexcept {
        if (qty <= 0 || price_ticks <= 0) return;
        stats_.filled_qty += qty;
        stats_.cash_ticks += static_cast<int64_t>(qty) * price_ticks;
        ++stats_.num_fills;
    }

    const ParentOrder& parent() const noexcept { return parent_; }
    const ExecStats&   stats()  const noexcept { return stats_; }
    int32_t realized_vwap_ticks() const noexcept { return stats_.realized_vwap_ticks(); }
    bool    done() const noexcept { return stats_.filled_qty >= parent_.total_qty; }
};


// VWAPExecutor — a volume-profile-aware slicer.
//
// Takes a ParentOrder + vector<double> volume_profile (must have
// parent.num_slices elements, sum ~= 1.0). For each slot N it
// emits child = total_qty * volume_profile[N] (rounded).
// Remainders are accumulated in the current slot so that it ultimately executes
// exactly total_qty.
//
// If profile_ is empty, it falls back to equal pieces = TWAP.
class VWAPExecutor {
    ParentOrder         parent_;
    std::vector<double> profile_;             // sum = 1.0, parent.num_slices elements
    std::vector<int32_t> target_cumulative_;  // shares by the end of slot N
    int                 seconds_per_slice_;
    int                 next_slot_to_process_;  // next slot to handle (anti-double-process)
    int32_t             total_emitted_qty_;   // sum of qty of all emitted children
    ExecStats           stats_;

public:
    VWAPExecutor(const ParentOrder& parent, std::vector<double> volume_profile)
        : parent_(parent),
          profile_(std::move(volume_profile)),
          seconds_per_slice_(parent.num_slices > 0
              ? std::max(1, parent.duration_sec / parent.num_slices) : 1),
          next_slot_to_process_(0),
          total_emitted_qty_(0) {

        // Normalization: if the sum != 1.0, rescale.
        double sum = 0.0;
        for (const double v : profile_) sum += v;
        if (sum > 0.0) for (double& v : profile_) v /= sum;

        // Cumulative targets — rounded integers. The last slot = total_qty
        // always, to cancel out rounding errors.
        target_cumulative_.reserve(static_cast<std::size_t>(parent.num_slices));
        double cum = 0.0;
        for (int i = 0; i < parent.num_slices; ++i) {
            const double frac = (i < static_cast<int>(profile_.size())) ? profile_[i] : 0.0;
            cum += frac;
            int32_t target = static_cast<int32_t>(cum * parent.total_qty + 0.5);
            if (i == parent.num_slices - 1) target = parent.total_qty;  // exact
            target_cumulative_.push_back(target);
        }
    }

    ChildOrder on_tick(int32_t now_sec) noexcept {
        ChildOrder o;
        if (parent_.num_slices <= 0 || stats_.filled_qty >= parent_.total_qty) return o;
        if (now_sec < parent_.start_ts_sec)                                     return o;

        const int32_t elapsed = now_sec - parent_.start_ts_sec;
        const int     slot    = elapsed / seconds_per_slice_;
        if (slot < next_slot_to_process_) return o;  // this slot already handled
        if (slot >= parent_.num_slices)   return o;  // window closed

        // qty = cumulative target for this slot MINUS what we already emitted.
        // This way slots with profile=0% automatically catch up the volume
        // in the next active slot (and slices_skipped counts once per slot).
        const int32_t target_to_emit = target_cumulative_[slot];
        int32_t qty = target_to_emit - total_emitted_qty_;
        next_slot_to_process_ = slot + 1;  // regardless of qty — anti-double-process

        if (qty <= 0) { ++stats_.slices_skipped; return o; }

        o.qty         = qty;
        o.price_ticks = 0;
        o.valid       = true;
        total_emitted_qty_ += qty;
        ++stats_.slices_emitted;
        return o;
    }

    void apply_fill(int32_t qty, int32_t price_ticks) noexcept {
        if (qty <= 0 || price_ticks <= 0) return;
        stats_.filled_qty += qty;
        stats_.cash_ticks += static_cast<int64_t>(qty) * price_ticks;
        ++stats_.num_fills;
    }

    const ParentOrder& parent() const noexcept { return parent_; }
    const ExecStats&   stats()  const noexcept { return stats_; }
    int32_t realized_vwap_ticks() const noexcept { return stats_.realized_vwap_ticks(); }
    bool    done() const noexcept { return stats_.filled_qty >= parent_.total_qty; }
};


// MarketVWAPTracker — an independent measurement of the market VWAP from the same
// stream of trades. Used as a benchmark to compute the execution algo's slippage.
//
// Called on_trade(price, qty) for every real trade (not ours!)
// on the exchange. Market realized VWAP = sum(price*qty) / sum(qty).
class MarketVWAPTracker {
    int64_t cash_ticks_ = 0;
    int64_t volume_     = 0;

public:
    void on_trade(int32_t price_ticks, int32_t qty) noexcept {
        if (price_ticks <= 0 || qty <= 0) return;
        cash_ticks_ += static_cast<int64_t>(price_ticks) * qty;
        volume_     += qty;
    }

    int32_t vwap_ticks() const noexcept {
        return volume_ > 0 ? static_cast<int32_t>(cash_ticks_ / volume_) : 0;
    }

    int64_t volume() const noexcept { return volume_; }
};


// A standard U-shape volume profile for US equities. The most trading in the morning
// (open auction + reactions to overnight news) and near the close (closing
// auction, fund rebalancing). The middle of the day is quiet.
//
// Split into n slots: 30% in the first 20%, 40% in the middle 60%, 30% in
// the last 20%. Proportional within each of the 3 zones.
inline std::vector<double> u_shape_profile(int num_slices) {
    std::vector<double> p;
    if (num_slices <= 0) return p;
    p.reserve(static_cast<std::size_t>(num_slices));

    const int open_slots   = std::max(1, num_slices / 5);  // 20%
    const int close_slots  = std::max(1, num_slices / 5);  // 20%
    const int middle_slots = std::max(1, num_slices - open_slots - close_slots);

    // 30% / 40% / 30% of volume distributed proportionally within the zones.
    const double open_weight   = 0.30 / open_slots;
    const double middle_weight = 0.40 / middle_slots;
    const double close_weight  = 0.30 / close_slots;

    for (int i = 0; i < open_slots; ++i)    p.push_back(open_weight);
    for (int i = 0; i < middle_slots; ++i)  p.push_back(middle_weight);
    for (int i = 0; i < close_slots; ++i)   p.push_back(close_weight);
    return p;
}


}  // namespace exec
