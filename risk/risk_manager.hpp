/*
 * ============================================================================
 *  Risk Manager (C++)
 * ============================================================================
 *
 *  SUMMARY
 *  -------
 *  The risk manager is the "gatekeeper" on the order hot path: every order
 *  from `strategy` or `router` MUST pass through `check_order()` before it
 *  reaches `OMS`. If the check refuses, the order never goes to the exchange.
 *
 *  Place in the pipeline:
 *      Strategy → Router → **Risk Manager** → OMS → Exchange
 *
 *  ARCHITECTURE — THREE STATES
 *  ---------------------------
 *  The class holds three orthogonal groups of state, each with its own purpose:
 *
 *  1. Positions (positions_ + pending_)
 *     What we currently hold + what we just sent to the exchange but is not
 *     yet filled. positions_[s] updates ONLY on a fill;
 *     pending_[s] grows on submit, shrinks on fill or cancel.
 *
 *  2. P&L (daily_pnl_ + peak_pnl_)
 *     daily_pnl_ is the sum of realized profit/loss since the start of the session.
 *     peak_pnl_ is the maximum reached — the reference point for drawdown.
 *
 *  3. Rate limiter (rate_ring_)
 *     A circular queue of timestamps of the most recent orders. Stale entries
 *     (> 1s) are evicted on every check, so the size of this queue is the
 *     number of orders in the last second.
 *
 *  Plus one **denormalized** value for performance:
 *      total_abs_exposure_ = sum_s |positions_[s] + pending_[s]|
 *  Maintained as an invariant by `adjust_pending()`. Thanks to it the
 *  portfolio-exposure check in `check_order()` is O(1) instead of
 *  O(number_of_symbols).
 *
 *  KILL SWITCH — STATE MACHINE
 *  ---------------------------
 *  kill_switch_active_ has only two states: false (trading enabled) and
 *  true (everything rejected). It activates in 3 ways:
 *      - circuit breaker: daily_pnl_ < -max_daily_loss
 *      - drawdown breach: (peak - daily) / peak > max_drawdown_pct
 *      - manually: activate_kill_switch()
 *  It deactivates in 2 ways:
 *      - reset_daily() (e.g. at the start of a new session)
 *      - deactivate_kill_switch() (manually, e.g. after fixing the problem)
 *
 *  PERFORMANCE
 *  -----------
 *  Each `check_order()` performs 7 checks in O(1):
 *      1. kill switch               — atomic bool read
 *      2. order value               — int * int compare
 *      3. per-symbol position limit — 2× hash lookup + abs
 *      4. portfolio exposure        — 1× hash lookup + 1× int subtract/add
 *      5. circuit breaker           — 1× double compare
 *      6. drawdown                  — 1× float divide
 *      7. rate limit                — pop_front loop + size compare
 *
 *  The Python equivalent reached ~200K checks/sec; this C++ version reaches
 *  ~30-50M checks/sec.
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>     // std::remove
#include <cmath>
#include <string>      // persist_path_
#include <atomic>      // kill switch — read/written from multiple threads (threaded pipeline)
#include <unistd.h>    // ::fsync, ::fileno

#include "../common/types.hpp"
#include "../common/symbol_key.hpp"
#include "../common/time_utils.hpp"
#include "../common/ring_counter.hpp"


// ============================================================================
// RiskAction — what the risk manager decides for an order
// ============================================================================
//
// ALLOW  : the order may proceed to the OMS
// REJECT : a single order rejected (does not change manager state)
// KILL   : critical — the kill switch has been activated, subsequent orders
//          will also be REJECT until you call deactivate_kill_switch()
//          or reset_daily()
//
// Represented as uint8_t (1 byte) — in the packed RiskCheckResult struct
// we do not waste memory.
// ============================================================================

enum class RiskAction : uint8_t {
    ALLOW  = 0,
    REJECT = 1,
    KILL   = 2
};

// action_str: convert enum → string for printf/log
inline const char* action_str(RiskAction a) noexcept {
    switch (a) {
        case RiskAction::ALLOW:  return "ALLOW";
        case RiskAction::REJECT: return "REJECT";
        case RiskAction::KILL:   return "KILL";
        default:                 return "UNKNOWN";
    }
}


// KillReason — WHY the kill switch latched (#121). For post-mortem/ops:
// distinguishes a manual halt from an automatic breaker and which limit fired it.
enum class KillReason : uint8_t {
    NONE                = 0,   // kill switch inactive
    MANUAL              = 1,   // activate_kill_switch() (admin)
    CIRCUIT_BREAKER     = 2,   // daily loss > max_daily_loss
    DRAWDOWN            = 3,   // drop from peak > max_drawdown_pct
    CONSECUTIVE_LOSSES  = 4,   // streak of losing fills > limit
};

inline const char* kill_reason_str(KillReason r) noexcept {
    switch (r) {
        case KillReason::NONE:               return "NONE";
        case KillReason::MANUAL:             return "MANUAL";
        case KillReason::CIRCUIT_BREAKER:    return "CIRCUIT_BREAKER";
        case KillReason::DRAWDOWN:           return "DRAWDOWN";
        case KillReason::CONSECUTIVE_LOSSES: return "CONSECUTIVE_LOSSES";
        default:                             return "UNKNOWN";
    }
}


// ============================================================================
// RiskCheckResult — what `check_order()` returns
// ============================================================================
//
// Fields:
//   action     : decision (ALLOW / REJECT / KILL)
//   reason     : a short description of why (e.g. "Position limit exceeded").
//                A fixed-size 128-byte buffer — no heap allocation,
//                copied via strncpy. Short reasons
//                (up to ~50 chars) fit with room to spare.
//   latency_ns : how many nanoseconds the check itself took (mono_ns from common/).
//                Used to aggregate performance statistics.
//
// Default constructor → ALLOW with an empty reason (a sensible default if
// someone simply declares `RiskCheckResult r;`).
//
// The 3-argument constructor → takes everything, safely copies the
// reason (strncpy + null terminator).
// ============================================================================

struct RiskCheckResult {
    RiskAction action;
    char       reason[128];
    int64_t    latency_ns;

    RiskCheckResult() noexcept
        : action(RiskAction::ALLOW), latency_ns(0) {
        reason[0] = '\0';
    }

    RiskCheckResult(RiskAction a, const char* r, int64_t lat) noexcept
        : action(a), latency_ns(lat) {
        std::strncpy(reason, r, 127);
        reason[127] = '\0';
    }
};


// ============================================================================
// RiskLimits — configurable thresholds
// ============================================================================
//
// All of these fields are read only by `RiskManager`. The defaults match
// a "reasonable mid-size firm" — in production you load them from config.yaml
// via config_loader.hpp.
//
// max_position_per_symbol : how many shares of one ticker you can hold long
//                            or short at most (abs(net) ≤ limit)
// max_portfolio_exposure  : sum of absolute positions over all tickers
// max_daily_loss          : daily loss limit in dollars — exceeding it
//                            turns on the kill switch (circuit breaker)
// max_orders_per_second   : rate limit, deque pruning over a 1-sec window
// max_order_value         : the maximum value of ONE order (price × qty)
// max_drawdown_pct        : maximum % drop from peak_pnl_, e.g. 5.0 = 5%
// ============================================================================

struct RiskLimits {
    int32_t  max_position_per_symbol;
    int32_t  max_portfolio_exposure;
    int64_t  max_daily_loss;
    int32_t  max_orders_per_second;
    int64_t  max_order_value;
    double   max_drawdown_pct;
    // Fat-finger / price-band: max % deviation of the order price from the
    // symbol's reference price (last/mid from market data). ≤ 0 = disabled.
    // 20% = a standard loose band (NMS LULD is tighter per-tier, but this
    // check mainly catches gross mistakes: 1500 instead of 150).
    double   max_price_band_pct;
    // Fat-finger on QUANTITY: max shares in ONE order, regardless of price
    // (notional can be small for a cheap stock, but 10M shares is still a mistake).
    // 0 = disabled.
    int32_t  max_shares_per_order;
    // A separate (tighter) SHORT position limit per symbol. 0 = symmetric
    // (use max_position_per_symbol on both sides).
    int32_t  max_short_per_symbol;
    // Loss-streak breaker: N consecutive losing fills (update_pnl<0) in a row
    // trips the kill switch. Catches a "death spiral" before the drawdown grows.
    // 0 = disabled.
    int32_t  max_consecutive_losses;
    // Daily TURNOVER limit (sum of notional of executed trades, $).
    // Catches excessive activity/churn regardless of position. 0 = disabled.
    double   max_daily_traded_notional;
    // Minimum allowed order price ($). Blocks penny stocks (often subject to
    // restrictions: worse spread, manipulation risk) and gross mistakes like
    // 0.15 instead of 150. 0 = disabled.
    double   min_price;
    // VALUE ($) limit of the aggregated per-symbol position: |projected| * price.
    // Catches expensive symbols whose share-based max_position_per_symbol does not
    // bound by value (100 shares at $3000 is $300k). 0 = disabled.
    double   max_symbol_notional;

    RiskLimits() noexcept
        : max_position_per_symbol(5000),
          max_portfolio_exposure(50000),
          max_daily_loss(100000),
          max_orders_per_second(1000),
          max_order_value(500000),
          max_drawdown_pct(5.0),
          max_price_band_pct(20.0),
          max_shares_per_order(100000),
          max_short_per_symbol(0),
          max_consecutive_losses(0),
          max_daily_traded_notional(0.0),
          min_price(0.0),
          max_symbol_notional(0.0) {}
};


// ============================================================================
// RiskManager — the main class
// ============================================================================
//
// API split into three groups:
//
//   Hot path (called by strategy / router / OMS on every order):
//     check_order(symbol, side, price, qty) → RiskCheckResult
//
//   State mutators (called after a positive check + exchange action):
//     on_order_sent(symbol, side, qty)        — reserve pending
//     on_order_cancelled(symbol, side, rem)   — release pending
//     update_position(symbol, side, qty)      — pending→realized flow
//     update_pnl(pnl_change)                  — record profit/loss
//
//   Control (admin / new session):
//     activate_kill_switch()       — force the KILL state
//     deactivate_kill_switch()     — allow trading to resume
//     reset_daily()                — new day: zero P&L, pending, rate limit
//
//   Accessors (debug / monitoring):
//     get_position(symbol), get_pending(symbol), get_daily_pnl(),
//     get_total_checks(), get_total_rejects(), is_kill_switch_active(),
//     print_stats()
// ============================================================================

class RiskManager {

    // --------------------------------------------------------------------
    // State: thresholds (read on every check_order)
    // --------------------------------------------------------------------
    RiskLimits limits_;

    // --------------------------------------------------------------------
    // State: positions
    //
    // positions_[key]  = realized quantity (signed) — updated
    //                    only in update_position() after a fill
    // pending_[key]    = in-flight quantity (signed: BUY = +qty, SELL = -qty)
    //                    grows on on_order_sent, shrinks on
    //                    on_order_cancelled or update_position
    //
    // Key = sym_to_key(const char*) from common/symbol_key.hpp.
    // Packed uint64_t (up to 8 ASCII chars as bits) — no std::string
    // allocation on every lookup. Same scheme as OMS.
    // --------------------------------------------------------------------
    std::unordered_map<uint64_t, int32_t> positions_;
    std::unordered_map<uint64_t, int32_t> pending_;

    // --------------------------------------------------------------------
    // State: denormalized portfolio exposure
    //
    // total_abs_exposure_ = sum_s |positions_[s] + pending_[s]|
    //
    // Maintained as an invariant by `adjust_pending()`. Without it,
    // check #4 in check_order would have to iterate over all
    // symbols — O(N). With the invariant it is O(1).
    // --------------------------------------------------------------------
    int64_t total_abs_exposure_;

    // --------------------------------------------------------------------
    // State: P&L
    //
    // daily_pnl_ : cumulative realized P&L since the start of the day
    // peak_pnl_  : the highest daily_pnl_ seen so far (for drawdown)
    //
    // Updated by update_pnl(). Reset by reset_daily().
    // --------------------------------------------------------------------
    double daily_pnl_;
    double peak_pnl_;
    double max_drawdown_dollars_ = 0.0;  // worst peak-to-trough $ decline this session (#340)
    int32_t consec_losses_ = 0;   // streak of losing update_pnl (#114)
    int32_t consec_wins_   = 0;   // streak of winning update_pnl (#348)
    int32_t max_consec_losses_ = 0;  // worst losing streak seen this session (#364)
    uint64_t winning_updates_ = 0;   // count of profitable update_pnl calls (#372)
    uint64_t losing_updates_  = 0;   // count of losing update_pnl calls (#372)
    double win_pnl_sum_  = 0.0;      // sum of profitable update_pnl deltas (#381)
    double loss_pnl_sum_ = 0.0;      // sum of losing update_pnl magnitudes (#381)
    double pnl_sum_      = 0.0;      // Σ every update_pnl delta (#477)
    double pnl_sumsq_    = 0.0;      // Σ delta² over every update_pnl (#477)
    double pnl_sumcube_  = 0.0;      // Σ delta³ over every update_pnl (#533)
    double pnl_sumquart_ = 0.0;      // Σ delta⁴ over every update_pnl (#549)
    uint64_t pnl_updates_ = 0;      // count of update_pnl calls, incl. flat (#477)
    double loss_sumsq_   = 0.0;      // Σ delta² over LOSING update_pnl (#485)
    double max_gain_     = 0.0;      // largest single winning update_pnl (#501)
    double max_loss_     = 0.0;      // largest single losing update_pnl magnitude (#501)
    int32_t underwater_updates_     = 0;  // consecutive updates below the P&L peak (#397)
    int32_t max_underwater_updates_ = 0;  // longest such spell this session (#397)
    uint64_t total_underwater_updates_ = 0;  // cumulative updates below the peak (#517)
    int32_t max_consec_wins_        = 0;  // best winning streak seen this session (#405)
    uint64_t kill_activations_      = 0;  // fresh false->true kill-switch latches (#421)
    uint64_t kill_counts_[5]        = {}; // fresh latches per KillReason (#541; index = enum)
    double  traded_notional_ = 0.0;  // daily notional turnover (#144)

    // --------------------------------------------------------------------
    // State: kill switch + optional persistence
    // --------------------------------------------------------------------
    // atomic — in run_pipeline_threaded check_order (the consumer thread) reads
    // concurrently with writes by the breaker/manual path. A plain bool = data race (UB);
    // the comment in check_order assumed an "atomic bool read" from the start.
    std::atomic<bool> kill_switch_active_;
    KillReason  kill_reason_ = KillReason::NONE;   // why it last tripped (#121)
    std::string persist_path_;     // empty = persistence disabled

    // Reference prices per symbol (last/mid from market data) for the price band.
    // Updated by update_reference_price(); empty = the check is skipped
    // for symbols without a known price.
    std::unordered_map<uint64_t, double> ref_price_;

    // List of restricted symbols (#84) — exchange halt, Reg SHO restriction,
    // no locate on a short, compliance freeze. Every order on a symbol from this
    // list is rejected before the remaining position checks.
    std::unordered_set<uint64_t> restricted_;

    // Per-symbol position-limit override (#161). Volatile/illiquid symbols often
    // get a tighter cap than the global max_position_per_symbol. No entry =
    // use the global one.
    std::unordered_map<uint64_t, int32_t> symbol_pos_limit_;

    // --------------------------------------------------------------------
    // State: rate limit
    //
    // Fixed circular ring buffer of order timestamps (CLOCK_MONOTONIC),
    // shared mechanics with multicast's FeedRateMeter/SlidingWindowRate —
    // see common/ring_counter.hpp. Covers up to 4096 orders/sec.
    // --------------------------------------------------------------------
    TimestampRing<4096> rate_ring_;

    // --------------------------------------------------------------------
    // State: statistics (for print_stats / monitoring)
    // --------------------------------------------------------------------
    uint64_t total_checks_;
    uint64_t total_rejects_;
    uint64_t total_latency_ns_;

public:

    // ====================================================================
    // Constructor
    // ====================================================================
    //
    // By default uses RiskLimits() (sensible firm thresholds). You can pass
    // your own limits loaded from config.yaml.
    // ====================================================================

    explicit RiskManager(const RiskLimits& limits = RiskLimits()) noexcept
        : limits_(limits),
          total_abs_exposure_(0),
          daily_pnl_(0.0),
          peak_pnl_(0.0),
          kill_switch_active_(false),
          total_checks_(0),
          total_rejects_(0),
          total_latency_ns_(0) {}


    // ====================================================================
    // check_order — seven checks on the hot path
    // ====================================================================
    //
    // The order of the checks is deliberate — cheapest first, so that on a
    // rejection we do not waste work:
    //
    //   1. Kill switch        : a single bool, ~1 ns
    //   2. Order value        : multiplication + compare, ~5 ns
    //   3. Per-symbol limit   : 2× hash lookup, ~30 ns
    //   4. Portfolio exposure : subtract/add from the invariant, ~5 ns
    //   5. Circuit breaker    : double compare, ~2 ns
    //   6. Drawdown           : divide + compare, ~5 ns
    //   7. Rate limit         : deque pop loop + compare, ~10-50 ns
    //
    // In total ~60-100 ns for the allow path (the hot case).
    //
    // Returns RiskCheckResult with action + reason + latency_ns. Does NOT throw
    // exceptions (noexcept) — a production hot path cannot unexpectedly
    // unwind the stack.
    // ====================================================================

    RiskCheckResult check_order(const char* symbol, Side side,
                                 double price, int32_t quantity) noexcept {
        const int64_t t0 = mono_ns();
        total_checks_++;

        // 1. Kill switch — reject everything if active
        if (kill_switch_active_) return make_reject("Kill switch active", t0);

        // Pack the ticker into its key ONCE and reuse it everywhere below.
        // sym_to_key loops over up to 8 chars; the restricted-set, price-band and
        // position checks each used to recompute it, so the allow path packed the
        // symbol 3× per order. Computed after the kill switch so an active halt
        // (the cheapest reject) still pays nothing for it.
        const uint64_t key = sym_to_key(symbol);

        // 1b. Restricted symbol — halt / Reg SHO / no locate / freeze.
        if (!restricted_.empty() && restricted_.count(key))
            return make_reject("Symbol restricted", t0);

        // 2. Single-order value
        const int64_t order_value = static_cast<int64_t>(price * quantity);
        if (order_value > limits_.max_order_value)
            return make_reject("Order value exceeds limit", t0);

        // 2a. Fat-finger on quantity — single-order qty above the limit.
        if (limits_.max_shares_per_order > 0 && quantity > limits_.max_shares_per_order)
            return make_reject("Order quantity exceeds limit", t0);

        // 2a'. Minimum price threshold — penny stocks / micro-price mistakes (#175).
        if (limits_.min_price > 0.0 && price < limits_.min_price)
            return make_reject("Price below minimum (penny stock)", t0);

        // 2b. Price band (fat-finger) — price too far from the reference.
        //     Catches gross mistakes (e.g. 1500.00 instead of 150.00) before they
        //     reach the market. Skipped when the band is off or there is no ref price for the symbol.
        if (limits_.max_price_band_pct > 0.0) {
            const auto rp = ref_price_.find(key);
            if (rp != ref_price_.end() && rp->second > 0.0) {
                const double dev_pct = std::fabs(price - rp->second) / rp->second * 100.0;
                if (dev_pct > limits_.max_price_band_pct)
                    return make_reject("Price band breach (fat-finger)", t0);
            }
        }

        // 2c. Daily turnover limit — excessive activity/churn (#144).
        if (limits_.max_daily_traded_notional > 0.0
            && traded_notional_ >= limits_.max_daily_traded_notional)
            return make_reject("Daily traded notional limit", t0);

        // 3 + 4. Position limits (per-symbol and portfolio) — reuse `key` from above
        const int32_t  signed_n  = signed_qty(side, quantity);
        const int32_t  cur_pos   = lookup(positions_, key);
        const int32_t  cur_pend  = lookup(pending_,   key);
        const int32_t  projected = cur_pos + cur_pend + signed_n;
        // Long limit: per-symbol override (#161) or global.
        int32_t long_cap = limits_.max_position_per_symbol;
        if (const auto ov = symbol_pos_limit_.find(key); ov != symbol_pos_limit_.end())
            long_cap = ov->second;
        // Asymmetric limits (#106): long vs short (short has its own cap; 0 = symmetric).
        if (projected >= 0) {
            if (projected > long_cap)
                return make_reject("Position limit exceeded", t0);
        } else {
            const int32_t short_cap = (limits_.max_short_per_symbol > 0)
                ? limits_.max_short_per_symbol : long_cap;
            if (-projected > short_cap)
                return make_reject("Short position limit exceeded", t0);
        }

        // Per-symbol position VALUE limit (#189) — $ cap on |projected| * price.
        if (limits_.max_symbol_notional > 0.0
            && std::abs(projected) * price > limits_.max_symbol_notional)
            return make_reject("Symbol notional limit", t0);

        // Portfolio exposure: O(1) thanks to the total_abs_exposure_ invariant
        const int32_t old_contrib = std::abs(cur_pos + cur_pend);
        const int32_t new_contrib = std::abs(projected);
        if (total_abs_exposure_ - old_contrib + new_contrib > limits_.max_portfolio_exposure)
            return make_reject("Portfolio exposure exceeded", t0);

        // 5 + 6. P&L checks (circuit breaker + drawdown). Both can
        //        turn on the kill switch — then subsequent checks also get REJECT.
        if (const char* fail = check_pnl_breakers())
            return make_reject(fail, t0);

        // 7. Rate limit (separate helper — cleaner main flow)
        const int64_t now = mono_ns();
        if (!check_rate_limit(now))
            return make_reject("Rate limit exceeded", t0);

        // All 7 checks passed
        const int64_t elapsed = mono_ns() - t0;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::ALLOW, "All checks passed", elapsed);
    }


    // ====================================================================
    // Mutator: on_order_sent
    // ====================================================================
    //
    // Called AFTER `check_order()` if ALLOW and the order has been
    // sent to the exchange. Reserves room in pending_, so that the next
    // check_order for the same symbol already knows the "in-flight" exposure.
    //
    // BUY: pending += qty, SELL: pending -= qty.
    // ====================================================================
    void on_order_sent(const char* symbol, Side side, int32_t quantity) noexcept {
        adjust_pending(sym_to_key(symbol), signed_qty(side, quantity));
    }


    // ====================================================================
    // Mutator: on_order_cancelled
    // ====================================================================
    //
    // Called when an order was cancelled BEFORE a fill (or
    // partially filled and the rest cancelled). Releases as much pending
    // as was not filled (the `remaining` parameter).
    //
    // Algebra: opposite sign to on_order_sent (BUY cancel = -, SELL cancel = +).
    // ====================================================================
    void on_order_cancelled(const char* symbol, Side side, int32_t remaining) noexcept {
        adjust_pending(sym_to_key(symbol), -signed_qty(side, remaining));
    }


    // ====================================================================
    // Mutator: update_position
    // ====================================================================
    //
    // Called after a fill. Flow of quantity from pending to positions:
    //   positions_[s] += signed_q
    //   pending_[s]   -= signed_q
    //
    // The sum pos+pend stays UNCHANGED, so total_abs_exposure_
    // needs no update. That is why pending is separated from positions —
    // so that a fill does not require recomputing the invariant.
    // ====================================================================
    void update_position(const char* symbol, Side side, int32_t quantity) noexcept {
        const int32_t  signed_q = signed_qty(side, quantity);
        const uint64_t key      = sym_to_key(symbol);
        positions_[key] += signed_q;
        pending_[key]   -= signed_q;
    }


    // ====================================================================
    // Mutator: update_pnl
    // ====================================================================
    //
    // Called after every fill with realized P&L (a delta — not the total).
    // Updates:
    //   daily_pnl_ += pnl_change
    //   peak_pnl_  = max(peak_pnl_, daily_pnl_)
    //
    // peak_pnl_ is the high-water mark for computing drawdown in check #6.
    // ====================================================================
    void update_pnl(double pnl_change) noexcept {
        // #477: accumulate the P&L event series (every update, flat included)
        // for volatility / risk-adjusted reads.
        pnl_sum_   += pnl_change;
        pnl_sumsq_ += pnl_change * pnl_change;
        pnl_sumcube_ += pnl_change * pnl_change * pnl_change;   // #533
        const double d2_549 = pnl_change * pnl_change;
        pnl_sumquart_ += d2_549 * d2_549;                       // #549
        ++pnl_updates_;
        daily_pnl_ += pnl_change;
        if (daily_pnl_ > peak_pnl_) peak_pnl_ = daily_pnl_;
        // Track the worst peak-to-trough decline ever seen this session (#340).
        const double dd_now = peak_pnl_ - daily_pnl_;
        if (dd_now > max_drawdown_dollars_) max_drawdown_dollars_ = dd_now;
        // Drawdown DURATION (#397): consecutive updates spent below the peak
        // — the time axis to #340's depth. FLAT updates extend the spell
        // (time passes underwater even when nothing changes), unlike the
        // win/loss counters (#372/#381), which exclude them.
        if (daily_pnl_ < peak_pnl_) {
            ++underwater_updates_;
            ++total_underwater_updates_;   // #517: cumulative time in drawdown
            if (underwater_updates_ > max_underwater_updates_)
                max_underwater_updates_ = underwater_updates_;
        } else {
            underwater_updates_ = 0;   // at/above the peak — the spell ends
        }
        // Loss-streak breaker (#114): N losses in a row -> kill switch.
        // consec_wins_ (#348) is the symmetric win-streak counter: a loss resets
        // it just as a profit resets consec_losses_.
        if (pnl_change < 0.0) {
            ++consec_losses_;
            consec_wins_ = 0;
            ++losing_updates_;   // #372
            loss_pnl_sum_ -= pnl_change;   // #381: accumulate the magnitude
            loss_sumsq_   += pnl_change * pnl_change;   // #485: downside deviation
            if (-pnl_change > max_loss_) max_loss_ = -pnl_change;   // #501: worst single loss
            if (consec_losses_ > max_consec_losses_) max_consec_losses_ = consec_losses_;  // #364
            if (limits_.max_consecutive_losses > 0
                && consec_losses_ >= limits_.max_consecutive_losses) {
                latch_kill_switch(KillReason::CONSECUTIVE_LOSSES);
            }
        } else if (pnl_change > 0.0) {
            consec_losses_ = 0;   // a profit resets the streak
            ++consec_wins_;
            ++winning_updates_;   // #372
            win_pnl_sum_ += pnl_change;   // #381
            if (pnl_change > max_gain_) max_gain_ = pnl_change;   // #501: best single gain
            if (consec_wins_ > max_consec_wins_) max_consec_wins_ = consec_wins_;  // #405
        }
    }


    // ====================================================================
    // Mutator: update_reference_price
    // ====================================================================
    //
    // Called from market data (last trade / mid) for the price band. Without a
    // known reference price the fat-finger check for that symbol is skipped —
    // the first order on a fresh symbol passes, subsequent ones are validated
    // against the last price.
    // ====================================================================
    void update_reference_price(const char* symbol, double price) noexcept {
        if (price > 0.0) ref_price_[sym_to_key(symbol)] = price;
    }

    // add_traded_notional: add the notional of an executed trade ($) to the
    // daily turnover (#144). Call after a fill (price*qty). Getter for monitoring.
    void   add_traded_notional(double notional) noexcept { if (notional > 0.0) traded_notional_ += notional; }
    double get_traded_notional() const noexcept { return traded_notional_; }
    // daily_turnover_pct: daily turnover as % of the max_daily_traded_notional
    // limit (#221) — how much of the turnover budget is used (mirror of
    // exposure_utilization_pct #181 for churn/activity). 0 when the limit is off.
    double daily_turnover_pct() const noexcept {
        if (limits_.max_daily_traded_notional <= 0.0) return 0.0;
        return traded_notional_ / limits_.max_daily_traded_notional * 100.0;
    }


    // ====================================================================
    // Restricted-symbol list (#84)
    // ====================================================================
    //
    // restrict_symbol  — put a symbol on the restricted list (halt/Reg SHO/freeze)
    // allow_symbol     — remove from the list (e.g. resume after a halt)
    // is_restricted    — whether a symbol is currently restricted
    // clear_restricted — clear the whole list (e.g. new session)
    // ====================================================================
    // set_symbol_position_limit: per-symbol position-limit override (#161);
    // limit<=0 removes the override (back to the global one).
    void set_symbol_position_limit(const char* symbol, int32_t limit) noexcept {
        const uint64_t k = sym_to_key(symbol);
        if (limit > 0) symbol_pos_limit_[k] = limit;
        else           symbol_pos_limit_.erase(k);
    }

    void restrict_symbol(const char* symbol) noexcept { restricted_.insert(sym_to_key(symbol)); }
    void allow_symbol(const char* symbol)    noexcept { restricted_.erase(sym_to_key(symbol)); }
    bool is_restricted(const char* symbol) const noexcept {
        return restricted_.count(sym_to_key(symbol)) != 0;
    }
    void clear_restricted() noexcept { restricted_.clear(); }


    // ====================================================================
    // Kill switch — manual control + persistence
    // ====================================================================
    //
    // After a trip (manual OR automatic via update_pnl) we save the
    // state to disk. A process restart during the trading day must SEE
    // that the limit was already breached today — otherwise the trader just
    // bypassed the halt by restarting the process. load_persisted_state() reads it at startup.
    //
    // File format (text for audit): "active=1\nlast_pnl=-12345.67\n"
    // After a manual deactivate you additionally need to call clear_persisted_state().
    void activate_kill_switch() noexcept {
        latch_kill_switch(KillReason::MANUAL);
    }
    // latch_kill_switch: the single chokepoint for FRESH activations (#421)
    // — counts only false->true transitions (repeated latch conditions,
    // e.g. every further loss past the streak limit, re-assert the switch
    // but do not re-count), sets the reason and persists. The persisted-
    // state RESTORE path does not come through here: restoring yesterday's
    // trip is not a new activation.
    void latch_kill_switch(KillReason r) noexcept {
        if (!kill_switch_active_) {
            ++kill_activations_;
            ++kill_counts_[static_cast<int>(r)];   // #541: per-reason histogram
        }
        kill_switch_active_ = true;
        kill_reason_        = r;
        persist_state();
    }
    void deactivate_kill_switch() noexcept {
        kill_switch_active_ = false;
        kill_reason_        = KillReason::NONE;
        persist_state();   // persist "off" too so a restart sees the current state
    }
    bool       is_kill_switch_active() const noexcept { return kill_switch_active_; }
    KillReason get_kill_reason()       const noexcept { return kill_reason_; }   // #121
    // kill_count: how many FRESH kill-switch activations a given reason caused
    // this session (#541) — the per-reason histogram behind kill_activations_
    // (#421, the total) and get_kill_reason (#121, only the LAST one standing):
    // a session that halted five times cannot say from those two whether it was
    // one flaky breaker or five different problems; this can, and it is the
    // mirror of OMS reject_count (#136) for the halt side. Counts only
    // false->true transitions (re-asserts while already halted do not add,
    // matching #421); the persisted-state restore path never comes through the
    // latch, so yesterday's trip is not counted today. Reset by reset_daily.
    uint64_t kill_count(KillReason r) const noexcept {
        return kill_counts_[static_cast<int>(r)];
    }

    // Runtime limit update (#129) — risk often tightens limits intraday
    // (e.g. after a loss) without a restart. get_limits for inspection/dashboard.
    const RiskLimits& get_limits() const noexcept { return limits_; }
    void set_limits(const RiskLimits& l) noexcept { limits_ = l; }

    // set_persist_path: optional file for persisting the kill-switch state.
    // Call once after construction. Without it persistence is a no-op (backward-
    // compatible behavior).
    void set_persist_path(const char* path) noexcept {
        if (path && *path) persist_path_ = path;
        else                persist_path_.clear();
    }

    // load_persisted_state: load the state saved in persist_path_. Call
    // AFTER construction + set_persist_path, BEFORE accepting orders. Returns
    // true when the file was read (active+pnl updated), false when missing.
    bool load_persisted_state() noexcept {
        if (persist_path_.empty()) return false;
        FILE* f = std::fopen(persist_path_.c_str(), "r");
        if (!f) return false;
        int    active = 0;
        double pnl    = 0.0;
        const int n = std::fscanf(f, "active=%d\nlast_pnl=%lf\n", &active, &pnl);
        std::fclose(f);
        if (n < 1) return false;
        kill_switch_active_ = (active != 0);
        if (n >= 2) daily_pnl_ = pnl;  // restore P&L too so drawdown does not start from zero
        return true;
    }

    // clear_persisted_state: remove the file (e.g. after reset_daily for a new day).
    void clear_persisted_state() noexcept {
        if (!persist_path_.empty()) std::remove(persist_path_.c_str());
    }


    // ====================================================================
    // reset_daily — start of a new trading day
    // ====================================================================
    //
    // Zeroes all "daily" state:
    //   - daily_pnl_ and peak_pnl_  → 0
    //   - rate limiter              → empty (a new minute from zero)
    //   - pending_                  → empty (open orders from the previous
    //                                day are cancelled by the exchange at midnight)
    //   - kill_switch_active_       → false (a clean slate)
    //
    // Positions (positions_) are KEPT — overnight holdings remain.
    // After reset, total_abs_exposure_ must be recomputed from positions_
    // alone, because pending_ is now empty.
    // ====================================================================
    void reset_daily() noexcept {
        daily_pnl_ = 0.0;
        peak_pnl_  = 0.0;
        max_drawdown_dollars_ = 0.0;   // #340: new session, fresh high-water mark
        consec_losses_ = 0;
        consec_wins_   = 0;   // #348
        max_consec_losses_ = 0;   // #364
        winning_updates_ = 0;     // #372
        losing_updates_  = 0;     // #372
        win_pnl_sum_  = 0.0;      // #381
        loss_pnl_sum_ = 0.0;      // #381
        pnl_sum_ = 0.0;           // #477
        pnl_sumsq_ = 0.0;         // #477
        pnl_sumcube_ = 0.0;       // #533
        pnl_sumquart_ = 0.0;      // #549
        pnl_updates_ = 0;         // #477
        loss_sumsq_ = 0.0;        // #485
        max_gain_ = 0.0;          // #501
        max_loss_ = 0.0;          // #501
        underwater_updates_     = 0;  // #397
        max_underwater_updates_ = 0;  // #397
        total_underwater_updates_ = 0;  // #517
        max_consec_wins_        = 0;  // #405
        kill_activations_       = 0;  // #421
        for (int i = 0; i < 5; ++i) kill_counts_[i] = 0;   // #541
        traded_notional_ = 0.0;
        rate_ring_.reset();
        pending_.clear();
        kill_switch_active_ = false;
        kill_reason_        = KillReason::NONE;
        total_abs_exposure_ = 0;
        for (const auto& kv : positions_) total_abs_exposure_ += std::abs(kv.second);
        clear_persisted_state();   // new day = clean account, the file is stale
    }


    // ====================================================================
    // Accessors (read-only)
    // ====================================================================
    int32_t  get_position(const char* symbol) const noexcept { return lookup(positions_, sym_to_key(symbol)); }
    int32_t  get_pending(const char* symbol)  const noexcept { return lookup(pending_,   sym_to_key(symbol)); }
    // get_exposure: |position + pending| for a symbol (#137) — what check #3 computes.
    int32_t  get_exposure(const char* symbol) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        return std::abs(lookup(positions_, k) + lookup(pending_, k));
    }
    // get_total_exposure: total portfolio exposure — the maintained O(1) invariant.
    int64_t  get_total_exposure() const noexcept { return total_abs_exposure_; }
    // total_pending_exposure: sum of |pending| shares across symbols (#283) — the
    // exposure tied up in WORKING (not-yet-filled) orders, isolated from filled
    // positions (total_abs_exposure_ counts both). How much is at risk in orders
    // still resting on the book.
    int64_t  total_pending_exposure() const noexcept {
        int64_t s = 0;
        for (const auto& [key, v] : pending_) s += std::abs(v);
        return s;
    }
    // net_exposure: SIGNED total shares across the book (#299) = sum(positions) +
    // sum(pending), keeping direction. Positive = net long, negative = net short —
    // the book's directional tilt, the opposite view to get_total_exposure() which
    // is gross |.| and hides offsetting longs/shorts. A market-neutral book nets ~0
    // even with large gross exposure.
    int64_t  net_exposure() const noexcept {
        int64_t s = 0;
        for (const auto& [key, v] : positions_) s += v;
        for (const auto& [key, v] : pending_)   s += v;
        return s;
    }
    // long_exposure / short_exposure: the directional split of book exposure (#315),
    // derived from gross (get_total_exposure) and net (net_exposure #299):
    //   long  = (gross + net) / 2,  short = (gross - net) / 2.
    // long = shares committed to net-long symbols, short = |net-short symbols|;
    // together they reconstruct gross (long + short) and net (long - short). The
    // sums are always even (gross +/- net = Σ 2·max(±x,0)), so the halving is exact.
    // A market-neutral desk watches the two legs stay balanced.
    int64_t  long_exposure() const noexcept {
        return (get_total_exposure() + net_exposure()) / 2;
    }
    int64_t  short_exposure() const noexcept {
        return (get_total_exposure() - net_exposure()) / 2;
    }
    // portfolio_skew: directional tilt = net_exposure / gross_exposure (#323), in
    // [-1, 1]. +1.0 = fully long (no offsetting shorts), -1.0 = fully short, 0.0 =
    // market-neutral. Derived from existing (#299/#315) fields — no extra state.
    // 0.0 when the book is flat (gross == 0) to avoid division by zero.
    double portfolio_skew() const noexcept {
        const int64_t gross = get_total_exposure();
        return gross > 0
            ? static_cast<double>(net_exposure()) / static_cast<double>(gross)
            : 0.0;
    }
    // largest_symbol_exposure: the biggest single-symbol |position+pending| across the
    // book (#331), in shares. The risk-concentration peak — which one instrument
    // carries the most directional commitment (filled + working). Each symbol's
    // |pos+pending| is one term of the total_abs_exposure_ sum, so this never exceeds
    // get_total_exposure(). 0 when flat.
    int64_t  largest_symbol_exposure() const noexcept {
        int64_t m = 0;
        for (const auto& [key, v] : positions_) {
            const int64_t e = std::abs(static_cast<int64_t>(v) + lookup(pending_, key));
            if (e > m) m = e;
        }
        for (const auto& [key, v] : pending_) {
            if (positions_.find(key) != positions_.end()) continue;   // already counted above
            const int64_t e = std::abs(static_cast<int64_t>(v));
            if (e > m) m = e;
        }
        return m;
    }
    // largest_exposure_symbol: the TICKER carrying that biggest |position +
    // pending| (#389) — the actionable WHICH to largest_symbol_exposure's
    // (#331) how-much, unpacked from the uint64_t key via key_to_sym (the
    // packing is lossless, so the name comes back exactly). Writes the
    // symbol into out (>= 9 bytes, always nul-terminated) and returns its
    // exposure in shares. Flat book: returns 0 and out[0] == '\0'. Same
    // two-pass walk as #331, so the returned value always equals it.
    int64_t largest_exposure_symbol(char* out) const noexcept {
        int64_t  m = 0;
        uint64_t mkey = 0;
        for (const auto& [key, v] : positions_) {
            const int64_t e = std::abs(static_cast<int64_t>(v) + lookup(pending_, key));
            if (e > m) { m = e; mkey = key; }
        }
        for (const auto& [key, v] : pending_) {
            if (positions_.find(key) != positions_.end()) continue;   // already counted above
            const int64_t e = std::abs(static_cast<int64_t>(v));
            if (e > m) { m = e; mkey = key; }
        }
        if (m > 0) key_to_sym(mkey, out); else out[0] = '\0';
        return m;
    }
    // exposure_concentration: largest_symbol_exposure / get_total_exposure (#331), in
    // [0, 1]. 1.0 = the whole book is one instrument (max concentration risk), near 0 =
    // exposure spread across many names (diversified). The answer to "how many eggs in
    // one basket" — a desk caps this to force diversification, complementing the
    // aggregate tilt views (net/long/short #299/#315). 0 when the book is flat.
    double   exposure_concentration() const noexcept {
        const int64_t total = get_total_exposure();
        return total > 0
            ? static_cast<double>(largest_symbol_exposure()) / static_cast<double>(total)
            : 0.0;
    }
    // exposure_utilization_pct: portfolio exposure as % of the limit (#181). How
    // much of the risk budget is used — the portfolio analog of is_near_position_limit.
    // 0 when the limit is off. Can exceed 100 only from a pre-limit state.
    double exposure_utilization_pct() const noexcept {
        if (limits_.max_portfolio_exposure <= 0) return 0.0;
        return static_cast<double>(total_abs_exposure_)
             / static_cast<double>(limits_.max_portfolio_exposure) * 100.0;
    }
    // exposure_headroom: remaining absolute portfolio exposure capacity (#252) =
    // max_portfolio_exposure - total_abs_exposure_, clamped to >= 0. Portfolio-level
    // analog of headroom_shares (#245): how much more |exposure| can be added before
    // the portfolio cap. 0 when the limit is disabled or already reached.
    int64_t exposure_headroom() const noexcept {
        const int64_t cap = limits_.max_portfolio_exposure;
        if (cap <= 0) return 0;
        return cap > total_abs_exposure_ ? cap - total_abs_exposure_ : 0;
    }

    // is_near_position_limit: whether the symbol's exposure reached warn_pct% of the cap
    // (#167) — an early warning BEFORE check_order hard-rejects. Respects the
    // per-symbol override (#161). false when the cap is off.
    bool is_near_position_limit(const char* symbol, double warn_pct) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        int32_t cap = limits_.max_position_per_symbol;
        if (const auto ov = symbol_pos_limit_.find(k); ov != symbol_pos_limit_.end()) cap = ov->second;
        if (cap <= 0) return false;
        const int32_t exposure = std::abs(lookup(positions_, k) + lookup(pending_, k));
        return static_cast<double>(exposure) >= static_cast<double>(cap) * warn_pct / 100.0;
    }
    // position_utilization_pct: symbol exposure as % of its limit (#237) —
    // the per-symbol analog of exposure_utilization_pct (#181). Returns a VALUE
    // (vs the bool from is_near_position_limit #167). Respects the per-symbol override
    // (#161). 0 when the cap is off.
    double position_utilization_pct(const char* symbol) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        int32_t cap = limits_.max_position_per_symbol;
        if (const auto ov = symbol_pos_limit_.find(k); ov != symbol_pos_limit_.end()) cap = ov->second;
        if (cap <= 0) return 0.0;
        const int32_t exposure = std::abs(lookup(positions_, k) + lookup(pending_, k));
        return static_cast<double>(exposure) / static_cast<double>(cap) * 100.0;
    }
    // headroom_shares: how many MORE shares may be added on a given side before
    // the projected position exceeds the limit (#245). BUY -> cap - cur; SELL -> short_cap
    // + cur (you can sell down to -short_cap, including a flip from long). Respects
    // the override (#161) and the asymmetric short cap (#106). 0 when the cap is off / no room.
    // projected_exposure: absolute symbol position if this order fully fills (#259)
    // = |current_net + current_pending + signed(side, qty)|. Pre-trade what-if that
    // returns the RESULTING exposure (vs headroom_shares #245 which returns the
    // remaining room). For sizing decisions before submitting.
    int32_t projected_exposure(const char* symbol, Side side, int32_t qty) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        const int32_t cur = lookup(positions_, k) + lookup(pending_, k);
        return std::abs(cur + signed_qty(side, qty));
    }

    // would_breach_position: pre-trade predicate — would this order breach the
    // per-symbol position limit (#291)? Mirrors check_order's position check exactly
    // (asymmetric long_cap vs short_cap #106, per-symbol override #161) but is const
    // and side-effect-free — no stats, no latency accounting. For what-if sizing
    // before committing. false when the cap is disabled.
    bool would_breach_position(const char* symbol, Side side, int32_t qty) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        int32_t long_cap = limits_.max_position_per_symbol;
        if (const auto ov = symbol_pos_limit_.find(k); ov != symbol_pos_limit_.end())
            long_cap = ov->second;
        if (long_cap <= 0) return false;
        const int32_t projected = lookup(positions_, k) + lookup(pending_, k) + signed_qty(side, qty);
        if (projected >= 0) return projected > long_cap;
        const int32_t short_cap = (limits_.max_short_per_symbol > 0)
            ? limits_.max_short_per_symbol : long_cap;
        return -projected > short_cap;
    }

    // rate_limit_headroom: how many MORE orders fit in the current
    // one-second window (#429) — the pre-trade probe for check #7. A burst
    // scheduler paces submissions with this instead of eating rejects.
    // check_rate_limit consumes a slot when it allows; this only evicts
    // expired timestamps (exactly what the next check would do with the
    // same clock) and reports limit - count, floored at 0. Pass the same
    // clock as production (mono_ns()) — a fabricated future `now` would
    // evict entries that have not really expired. -1 when the limit is
    // disabled (<= 0). No stats, no latency accounting.
    int32_t rate_limit_headroom(int64_t now_ns) noexcept {
        if (limits_.max_orders_per_second <= 0) return -1;
        rate_ring_.evict(now_ns - 1'000'000'000);
        const int32_t room = limits_.max_orders_per_second
                           - static_cast<int32_t>(rate_ring_.count());
        return room > 0 ? room : 0;
    }

    // would_breach_price_band: pre-trade predicate — would this price trip
    // the fat-finger band (#445)? Mirrors check_order's check #2b exactly:
    // |price - reference| / reference in percent vs max_price_band_pct.
    // false when the band is disabled OR the symbol has no reference price
    // — the real check SKIPS in both cases, and the probe must not be
    // stricter than the check it predicts. The remaining member of the
    // what-if family (#291/#413/#429/#437): a pricing engine can sanity-
    // check a quote before submitting instead of learning from the reject.
    bool would_breach_price_band(const char* symbol, double price) const noexcept {
        if (limits_.max_price_band_pct <= 0.0) return false;
        const auto rp = ref_price_.find(sym_to_key(symbol));
        if (rp == ref_price_.end() || rp->second <= 0.0) return false;
        const double dev_pct = std::fabs(price - rp->second) / rp->second * 100.0;
        return dev_pct > limits_.max_price_band_pct;
    }

    // would_breach_symbol_notional: pre-trade predicate — would this order
    // push the symbol's $ position value past max_symbol_notional (#437)?
    // Mirrors check_order's #189 check exactly (|projected shares| * price
    // vs the cap) as a const, side-effect-free read, completing the what-if
    // family: position #291, portfolio exposure #413, rate headroom #429,
    // and now the per-name $ cap. false when the cap is disabled (<= 0).
    bool would_breach_symbol_notional(const char* symbol, Side side,
                                      int32_t qty, double price) const noexcept {
        if (limits_.max_symbol_notional <= 0.0) return false;
        const uint64_t k = sym_to_key(symbol);
        const int32_t projected = lookup(positions_, k) + lookup(pending_, k)
                                + signed_qty(side, qty);
        return std::abs(projected) * price > limits_.max_symbol_notional;
    }

    // would_breach_exposure: pre-trade predicate — would this order breach
    // the PORTFOLIO exposure limit (#413)? The portfolio-level companion to
    // would_breach_position (#291, per-symbol caps): mirrors check_order's
    // O(1) invariant math exactly (total_abs_exposure_ - old symbol
    // contribution + projected contribution vs max_portfolio_exposure) but
    // is const and side-effect-free — no stats, no latency accounting, so a
    // sizing loop can probe it freely without polluting the check counters.
    // false when the cap is disabled (<= 0).
    bool would_breach_exposure(const char* symbol, Side side, int32_t qty) const noexcept {
        if (limits_.max_portfolio_exposure <= 0) return false;
        const uint64_t k = sym_to_key(symbol);
        const int32_t cur = lookup(positions_, k) + lookup(pending_, k);
        const int32_t old_contrib = std::abs(cur);
        const int32_t new_contrib = std::abs(cur + signed_qty(side, qty));
        return total_abs_exposure_ - old_contrib + new_contrib
             > limits_.max_portfolio_exposure;
    }

    int32_t headroom_shares(const char* symbol, Side side) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        int32_t cap = limits_.max_position_per_symbol;
        if (const auto ov = symbol_pos_limit_.find(k); ov != symbol_pos_limit_.end()) cap = ov->second;
        if (cap <= 0) return 0;
        const int32_t cur = lookup(positions_, k) + lookup(pending_, k);   // signed position
        int32_t room;
        if (side == Side::BUY) {
            room = cap - cur;
        } else {
            const int32_t short_cap = (limits_.max_short_per_symbol > 0)
                ? limits_.max_short_per_symbol : cap;
            room = short_cap + cur;
        }
        return room > 0 ? room : 0;
    }
    // get_position_notional: symbol exposure in DOLLARS (#153) = |pos+pending|
    // * reference price. 0 when there is no ref price. Risk in shares does not show
    // the real $ risk across symbols with different prices.
    double   get_position_notional(const char* symbol) const noexcept {
        const uint64_t k = sym_to_key(symbol);
        const auto rp = ref_price_.find(k);
        if (rp == ref_price_.end() || rp->second <= 0.0) return 0.0;
        return std::abs(lookup(positions_, k) + lookup(pending_, k)) * rp->second;
    }
    // symbol_notional_utilization_pct: get_position_notional / max_symbol_notional
    // as a percent (#356) — the $ analog of position_utilization_pct (#237, which
    // is share-count-based). A symbol can sit well under its SHARE limit while
    // already near its $ limit if the price has run up, so the two can disagree;
    // this tracks the check enforced in check_order (max_symbol_notional). 0 when
    // the limit is off or there is no reference price yet.
    double symbol_notional_utilization_pct(const char* symbol) const noexcept {
        if (limits_.max_symbol_notional <= 0.0) return 0.0;
        return get_position_notional(symbol) / limits_.max_symbol_notional * 100.0;
    }
    double   get_daily_pnl()                  const noexcept { return daily_pnl_; }
    double   get_peak_pnl()                   const noexcept { return peak_pnl_; }
    // current_drawdown_pct: current % drop from the high-water mark (#197) — the same
    // formula as the drawdown breaker (check #6), exposed for monitoring BEFORE
    // a trip. 0 when peak <= 0 (no profit, no reference). For a dashboard/alert.
    double   current_drawdown_pct() const noexcept {
        if (peak_pnl_ <= 0.0) return 0.0;
        return (peak_pnl_ - daily_pnl_) / peak_pnl_ * 100.0;
    }
    // drawdown_headroom_pct: percentage points of drawdown left before the drawdown
    // circuit breaker trips (#267) = max_drawdown_pct - current_drawdown_pct, clamped
    // >= 0. Early warning in the same "headroom" family as remaining_loss_budget
    // (#213) and consecutive_losses_remaining (#205). 0 when the limit is disabled
    // or already breached.
    double   drawdown_headroom_pct() const noexcept {
        if (limits_.max_drawdown_pct <= 0.0) return 0.0;
        const double room = limits_.max_drawdown_pct - current_drawdown_pct();
        return room > 0.0 ? room : 0.0;
    }
    // remaining_turnover_budget: how many more $ can trade today before the
    // turnover check (#144, check 2c) starts rejecting (#453) — the
    // remaining-budget face of daily_turnover_pct's (#221) utilization,
    // completing the headroom family: remaining_loss_budget #213 /
    // consecutive_losses_remaining #205 / drawdown_headroom_pct #267 /
    // headroom_shares #245. An execution scheduler paces the tail of the
    // day with this number instead of discovering the wall from rejects.
    // -1 when the limit is disabled (unlimited); 0 when already exhausted.
    double remaining_turnover_budget() const noexcept {
        if (limits_.max_daily_traded_notional <= 0.0) return -1.0;
        const double room = limits_.max_daily_traded_notional - traded_notional_;
        return room > 0.0 ? room : 0.0;
    }
    // current_drawdown_dollars: absolute drawdown from the high-water mark in $
    // (#275) = peak_pnl_ - daily_pnl_, clamped >= 0. The dollar companion to
    // current_drawdown_pct (#197) — some desks size and alert on absolute drawdown
    // rather than a percentage. 0 at a new high.
    double   current_drawdown_dollars() const noexcept {
        const double dd = peak_pnl_ - daily_pnl_;
        return dd > 0.0 ? dd : 0.0;
    }
    // max_drawdown_dollars: the WORST peak-to-trough decline observed this session
    // (#340) — the running maximum of current_drawdown_dollars (#275), updated on
    // every update_pnl. current_drawdown shows the drop RIGHT NOW (0 at a new high);
    // this remembers the deepest trough the book ever sat in, even after a full
    // recovery — the number a desk reports as the session's max drawdown and sizes
    // limits on. The live counterpart to the Backtester's max_drawdown. Reset by
    // reset_daily; 0 when the book has never been underwater.
    double   max_drawdown_dollars() const noexcept { return max_drawdown_dollars_; }
    int32_t  get_consecutive_losses()         const noexcept { return consec_losses_; }
    // get_consecutive_wins: current winning streak (#348) — the symmetric
    // counterpart to get_consecutive_losses/consec_losses_ (#114). There is no
    // breaker on the win side (only losses trip the kill switch); this is a
    // pure read for reporting/sizing (e.g. some desks scale UP size on a hot
    // streak, or flag it as a signal to check for a stale/broken strategy).
    // Reset by reset_daily; any loss resets it to 0.
    int32_t  get_consecutive_wins()           const noexcept { return consec_wins_; }
    // max_consecutive_losses_seen: the WORST losing streak reached this session
    // (#364) — the running maximum of consec_losses_ (#114), the loss-streak
    // analog of max_drawdown_dollars (#340, the high-water mark of drawdown).
    // consecutive_losses_remaining (#205) shows how close the breaker is RIGHT
    // NOW; this remembers how deep the streak ever got, even after a win reset
    // it — the number a post-session review checks ("we hit 5 in a row, one shy
    // of the breaker"). Reset by reset_daily; 0 when there has been no loss.
    int32_t  max_consecutive_losses_seen()    const noexcept { return max_consec_losses_; }
    // max_consecutive_wins_seen: the BEST winning streak reached this session
    // (#405) — the running maximum of consec_wins_ (#348), completing the
    // streak family symmetrically with max_consecutive_losses_seen (#364).
    // A post-session review compares the two: a best-streak far above the
    // worst says the edge clusters; a hot streak far above HISTORY's normal
    // is also the classic signature of a stale feed marking every trade
    // profitable — worth checking, not celebrating. Reset by reset_daily.
    int32_t  max_consecutive_wins_seen()      const noexcept { return max_consec_wins_; }
    // kill_switch_activations: how many times the switch FRESHLY latched
    // this session (#421) — false->true transitions only, across every
    // trigger (manual, circuit breaker, drawdown, loss streak). is_kill_
    // switch_active() says whether the desk is halted NOW; a review wants
    // to know it halted three times TODAY even if each halt was cleared.
    // Restoring a persisted trip at startup does not count. Reset by
    // reset_daily.
    uint64_t kill_switch_activations() const noexcept { return kill_activations_; }
    // pnl_win_rate: fraction of P&L updates that were profitable (#372) =
    // winning_updates / (winning + losing), in [0, 1]. update_pnl is called once
    // per realized fill/mark, so this is the hit rate on individual P&L events —
    // distinct from the streak counters (#114/#348, runs) and from the OMS
    // symbol_win_rate (#298, which is per SYMBOL). Flat (zero) updates are
    // excluded from both sides. Reset by reset_daily; 0 before any decided update.
    double   pnl_win_rate() const noexcept {
        const uint64_t decided = winning_updates_ + losing_updates_;
        return decided > 0 ? static_cast<double>(winning_updates_) / static_cast<double>(decided) : 0.0;
    }
    uint64_t winning_pnl_updates() const noexcept { return winning_updates_; }
    uint64_t losing_pnl_updates()  const noexcept { return losing_updates_; }
    // avg_pnl_win: mean size of a profitable update_pnl event (#381) =
    // win_pnl_sum / winning_updates. The MAGNITUDE axis that pnl_win_rate
    // (#372, frequency) lacks — a high hit rate with tiny wins can still lose
    // money overall. 0 before any winning update; reset by reset_daily.
    double avg_pnl_win() const noexcept {
        return winning_updates_ > 0
            ? win_pnl_sum_ / static_cast<double>(winning_updates_) : 0.0;
    }
    // avg_pnl_loss: mean MAGNITUDE of a losing update_pnl event (#381),
    // returned as a POSITIVE number. 0 before any losing update.
    double avg_pnl_loss() const noexcept {
        return losing_updates_ > 0
            ? loss_pnl_sum_ / static_cast<double>(losing_updates_) : 0.0;
    }
    // pnl_payoff_ratio: avg win / avg loss (#381). > 1 means the average
    // winner outweighs the average loser; combined with pnl_win_rate it gives
    // the live expectancy of the update stream (rate*avg_win -
    // (1-rate)*avg_loss). 0 until BOTH a win and a loss exist (no ratio yet).
    double pnl_payoff_ratio() const noexcept {
        const double aw = avg_pnl_win();
        const double al = avg_pnl_loss();
        return (aw > 0.0 && al > 0.0) ? aw / al : 0.0;
    }
    // kelly_fraction: the Kelly-criterion optimal risk fraction (#525) from the
    // live win rate and payoff = W - (1 - W)/b, where W = pnl_win_rate (#372)
    // and b = pnl_payoff_ratio (#381). The position-SIZING output the frequency
    // and magnitude axes were built toward: it answers what fraction of capital
    // an edge justifies risking per bet, fusing hit rate and payoff the way
    // pnl_expectancy (#461) does but in sizing units. A coin flip with a 2:1
    // payoff (W=0.5, b=2) returns 0.25; a NEGATIVE or zero result means no
    // exploitable edge — bet nothing. It is deliberately left UNCLAMPED and can
    // go negative (a losing edge) or above 1 (a huge one); the caller applies
    // its own risk appetite (half-Kelly, a hard cap). 0 until both a win and a
    // loss exist (b undefined before then). Reset by reset_daily via #372/#381.
    double kelly_fraction() const noexcept {
        const double b = pnl_payoff_ratio();
        if (b <= 0.0) return 0.0;
        const double w = pnl_win_rate();
        return w - (1.0 - w) / b;
    }
    // pnl_std_dev: population standard deviation of the update_pnl event
    // series (#477) — the volatility of the P&L stream, over EVERY update
    // including flat ones (a return series counts the zeros). Where
    // pnl_expectancy (#461) is the mean edge, this is how BUMPY the path to
    // it is: a strategy and its double-size clone have the same win rate
    // but twice the std. 0 with fewer than two updates.
    double pnl_std_dev() const noexcept {
        if (pnl_updates_ < 2) return 0.0;
        const double n    = static_cast<double>(pnl_updates_);
        const double mean = pnl_sum_ / n;
        const double var  = pnl_sumsq_ / n - mean * mean;
        return var > 0.0 ? std::sqrt(var) : 0.0;
    }
    // pnl_skewness: the (population) skewness of the update_pnl event series
    // (#533) = E[(x-μ)³] / σ³, over EVERY update including flat ones — the same
    // population pnl_std_dev (#477) uses, so the two are consistent. It is the
    // SHAPE of the return distribution that the second moment cannot see:
    // POSITIVE skew is a long right tail (many small losses, the occasional big
    // win — a trend-follower's payoff), NEGATIVE skew a long left tail (many
    // small wins, the occasional big loss — the premium-selling blow-up
    // profile). It puts a signed number on what pnl_tail_ratio (#509, the ratio
    // of extremes) and kelly_fraction (#525) only hint at: two strategies with
    // the same mean and volatility but opposite skew carry very different tail
    // risk. Built on a Σδ³ accumulator mirroring pnl_sumsq_ (#477). 0 before
    // two updates or on a zero-variance (constant) stream; reset by reset_daily.
    double pnl_skewness() const noexcept {
        if (pnl_updates_ < 2) return 0.0;
        const double n    = static_cast<double>(pnl_updates_);
        const double mean = pnl_sum_ / n;
        const double m2   = pnl_sumsq_ / n;
        const double var  = m2 - mean * mean;
        if (var <= 0.0) return 0.0;
        const double sd = std::sqrt(var);
        const double central3 = pnl_sumcube_ / n - 3.0 * mean * m2 + 2.0 * mean * mean * mean;
        return central3 / (sd * sd * sd);
    }
    // pnl_kurtosis: the EXCESS (population) kurtosis of the update_pnl event
    // series (#549) = E[(x-μ)⁴]/σ⁴ - 3, over the same all-updates population
    // as pnl_std_dev (#477) and pnl_skewness (#533). It completes the moment
    // family — mean (#461), variance (#477), skew (#533), and now the TAILS:
    // positive excess means fat tails (extreme events far more likely than a
    // Gaussian with the same σ predicts — the regime where σ-based reads like
    // pnl_sharpe #477 flatter the risk), negative means thin tails (outcomes
    // clustered, the two-point extreme reads -2). Skew says which SIDE the
    // tail is on; this says how HEAVY the tails are regardless of side — a
    // symmetric stream can still hide rare double-sided blow-ups, invisible
    // to #533. Built on a Σδ⁴ accumulator; 0 before two updates or on a
    // zero-variance stream; reset by reset_daily.
    double pnl_kurtosis() const noexcept {
        if (pnl_updates_ < 2) return 0.0;
        const double n    = static_cast<double>(pnl_updates_);
        const double mean = pnl_sum_ / n;
        const double m2   = pnl_sumsq_ / n;
        const double var  = m2 - mean * mean;
        if (var <= 0.0) return 0.0;
        const double m3 = pnl_sumcube_ / n;
        const double m4 = pnl_sumquart_ / n;
        const double mu2 = mean * mean;
        const double central4 = m4 - 4.0 * mean * m3 + 6.0 * mu2 * m2 - 3.0 * mu2 * mu2;
        return central4 / (var * var) - 3.0;
    }
    // pnl_value_at_risk: parametric (Gaussian) per-event VaR (#557) =
    // z*sigma - mean, floored at 0 and returned as a POSITIVE loss magnitude —
    // the loss a single update_pnl event is not expected to exceed at the
    // z-quantile confidence (z = 1.645 -> 95%, 2.326 -> 99%). Computed live
    // from the #477 accumulators (no history buffer, unlike the backtester's
    // empirical VaR): mean and sigma over EVERY update, so a positive mean
    // CUSHIONS the number and a strong enough edge floors it at 0 (no loss
    // expected at that confidence). CAVEAT the moment family exists to check:
    // this is Gaussian-parametric — with negative skew (#533) or positive
    // excess kurtosis (#549) it UNDERSTATES the true tail, so read those two
    // before trusting it. 0 before two updates or on a zero-variance stream.
    double pnl_value_at_risk(double z = 1.645) const noexcept {
        const double sd = pnl_std_dev();
        if (sd <= 0.0) return 0.0;
        const double mean = pnl_sum_ / static_cast<double>(pnl_updates_);
        const double v = z * sd - mean;
        return v > 0.0 ? v : 0.0;
    }
    // pnl_recovery_factor: the session return covered by its worst drawdown
    // (#493) = daily_pnl / max_drawdown_dollars — a Calmar/recovery-factor
    // read. > 1 means the desk has made more than its worst peak-to-trough
    // loss (it fully recovered and then some), < 1 means the deepest hole
    // still exceeds the standing profit. Where pnl_sharpe (#477) adjusts by
    // volatility and pnl_sortino (#485) by downside deviation, this adjusts
    // by the single worst DRAWDOWN (#340) — the loss path that actually
    // scares a desk, not its dispersion. 0 when there has been no drawdown
    // (an only-up session, reported as 0 rather than infinity). Can go
    // negative when the session is net down. Reset by reset_daily via its
    // inputs.
    double pnl_recovery_factor() const noexcept {
        return max_drawdown_dollars_ > 0.0 ? daily_pnl_ / max_drawdown_dollars_ : 0.0;
    }

    // pnl_sharpe: the risk-adjusted per-event edge (#477) = mean(update_pnl)
    // / pnl_std_dev — a Sharpe-style ratio on the P&L event stream (no
    // annualization, no risk-free rate; a pure reward-per-unit-volatility
    // read). Two strategies with the same expectancy but different
    // consistency rank differently here — the smoother one scores higher.
    // 0 when the volatility is zero (a constant P&L stream) or before two
    // updates. Distinct from pnl_expectancy (#461, the raw mean) and
    // pnl_profit_factor (#469, gross ratio): only this one penalizes
    // volatility.
    double pnl_sharpe() const noexcept {
        const double sd = pnl_std_dev();
        if (sd <= 0.0 || pnl_updates_ == 0) return 0.0;
        return (pnl_sum_ / static_cast<double>(pnl_updates_)) / sd;
    }
    // largest_pnl_gain / largest_pnl_loss: the single biggest winning and
    // losing update_pnl this session (#501) — the tail events of the P&L
    // stream, the loss returned as a POSITIVE magnitude. Where avg_pnl_win/
    // loss (#381) are the means, these are the extremes: a total that is
    // dominated by one huge gain (largest_pnl_gain near the whole win sum
    // #381) is a fragile result, and largest_pnl_loss is the worst hit the
    // desk took in one event — the number a risk review checks against the
    // per-event limits. 0 before any winning / losing update; reset by
    // reset_daily.
    double largest_pnl_gain() const noexcept { return max_gain_; }
    double largest_pnl_loss() const noexcept { return max_loss_; }
    // pnl_tail_ratio: the ratio of the EXTREMES (#509) = largest_pnl_gain /
    // largest_pnl_loss. It completes the ladder of gain-vs-loss ratios on
    // the update_pnl stream: pnl_payoff_ratio (#381) is the ratio of the
    // AVERAGES, pnl_profit_factor (#469) the ratio of the SUMS, and this the
    // ratio of the single biggest events. > 1 means the best event outweighs
    // the worst (convex tail); < 1 is the negative-skew signature — one loss
    // larger than any gain — the blow-up shape that a high win rate and a
    // healthy payoff ratio both hide (many small wins, one fat tail). 0 until
    // a losing update exists (division guarded); reset by reset_daily via the
    // #501 accumulators it reads.
    double pnl_tail_ratio() const noexcept {
        return max_loss_ > 0.0 ? max_gain_ / max_loss_ : 0.0;
    }

    // pnl_downside_dev: the downside deviation of the P&L event series
    // (#485) = sqrt( Σ loss² / N ) over ALL updates (winners and flats
    // contribute 0 to the numerator but still count in N — the standard
    // downside-deviation convention). Where pnl_std_dev (#477) penalizes
    // ALL variance, this penalizes only the losing side: a strategy with
    // big winners and small losers has a high std but a low downside dev.
    // 0 with no losses or fewer than one update.
    double pnl_downside_dev() const noexcept {
        if (pnl_updates_ == 0) return 0.0;
        const double dd = loss_sumsq_ / static_cast<double>(pnl_updates_);
        return dd > 0.0 ? std::sqrt(dd) : 0.0;
    }
    // pnl_sortino: the Sortino-style risk-adjusted edge (#485) =
    // mean(update_pnl) / pnl_downside_dev — like pnl_sharpe (#477) but
    // dividing by DOWNSIDE deviation instead of total, so it rewards
    // upside volatility instead of punishing it. Sortino >= Sharpe for any
    // series with upside dispersion (only bad volatility is penalized). 0
    // when there is no downside (a series with no losses is unbounded-good,
    // reported as 0 rather than infinity) or before any update.
    double pnl_sortino() const noexcept {
        const double dd = pnl_downside_dev();
        if (dd <= 0.0 || pnl_updates_ == 0) return 0.0;
        return (pnl_sum_ / static_cast<double>(pnl_updates_)) / dd;
    }

    // pnl_profit_factor: gross profit / gross loss over update_pnl events
    // (#469) = win_pnl_sum / loss_pnl_sum. The classic performance ratio:
    // > 1 the winners outweigh the losers in TOTAL dollars, < 1 the desk is
    // bleeding, = 1 breakeven. Distinct from pnl_payoff_ratio (#381, avg
    // win / avg loss): profit_factor = payoff_ratio * (winning / losing),
    // so it folds in the win/loss FREQUENCY the payoff ratio ignores — a
    // great payoff ratio with rare wins can still be a profit factor below
    // 1. The RiskManager per-event parallel of OMS profit_factor (#339,
    // per-symbol realized). 0 until at least one losing update exists (no
    // denominator). Reset by reset_daily via the accumulators.
    double pnl_profit_factor() const noexcept {
        return loss_pnl_sum_ > 0.0 ? win_pnl_sum_ / loss_pnl_sum_ : 0.0;
    }

    // pnl_expectancy: the expected P&L per DECIDED update (#461) =
    // win_rate*avg_win - loss_rate*avg_loss, which collapses to
    // (win_pnl_sum - loss_pnl_sum) / decided — the net realized P&L over
    // the updates that had a direction, per update. The single number that
    // says whether the edge is positive: frequency (#372) and magnitude
    // (#381) each tell half the story, this fuses them (a high win rate
    // with tiny wins and fat losses reads NEGATIVE here). Flat updates are
    // excluded from both the sum and the count. The RiskManager parallel of
    // OMS expectancy_per_symbol (#363). 0 before any decided update; reset
    // by reset_daily via its win/loss accumulators.
    double pnl_expectancy() const noexcept {
        const uint64_t decided = winning_updates_ + losing_updates_;
        return decided > 0
            ? (win_pnl_sum_ - loss_pnl_sum_) / static_cast<double>(decided)
            : 0.0;
    }
    // underwater_updates: consecutive update_pnl calls spent below the P&L
    // high-water mark (#397) — the LIVE duration of the current drawdown,
    // the time axis to max_drawdown_dollars' (#340) depth. A shallow but
    // long-lived drawdown reads healthy on the depth metric while the desk
    // has not made a new high in hours. 0 when at/above the peak.
    int32_t underwater_updates() const noexcept { return underwater_updates_; }
    // max_underwater_updates: the LONGEST underwater spell this session
    // (#397) — high-water mark of the counter above; reset by reset_daily.
    int32_t max_underwater_updates() const noexcept { return max_underwater_updates_; }
    // total_underwater_updates: the CUMULATIVE number of update_pnl events spent
    // below the P&L high-water mark this session (#517), summed across every
    // separate spell — where underwater_updates (#397) is only the CURRENT
    // spell and max_underwater_updates (#397) the longest single one. A book
    // with shallow, short individual drawdowns can still spend most of the
    // session underwater; this is the count that exposes it. Reset by
    // reset_daily.
    uint64_t total_underwater_updates() const noexcept { return total_underwater_updates_; }
    // underwater_fraction: the share of the session spent below the high-water
    // mark (#517) = total_underwater_updates / pnl_updates, in [0,1]. The
    // "pain fraction" in the spirit of the Ulcer Index: 0 means every update
    // set or held a new high (a monotonically rising equity curve), near 1
    // means the desk almost never recovered its peak. FLAT updates count as
    // underwater time (the position is still below the peak), matching #397.
    // It separates two strategies with the SAME max_drawdown_dollars (#340) by
    // how much of the ride was spent in the red — depth is not duration. 0
    // before any update.
    double underwater_fraction() const noexcept {
        return pnl_updates_ > 0
            ? static_cast<double>(total_underwater_updates_) / static_cast<double>(pnl_updates_)
            : 0.0;
    }
    // consecutive_losses_remaining: how many more losing fills IN A ROW until the
    // loss-streak breaker trips (#205, based on #114). -1 when the breaker is off,
    // 0 when it already tripped. Early warning before the desk is halted.
    int32_t  consecutive_losses_remaining() const noexcept {
        if (limits_.max_consecutive_losses <= 0) return -1;
        const int32_t rem = limits_.max_consecutive_losses - consec_losses_;
        return rem > 0 ? rem : 0;
    }
    // remaining_loss_budget: how much more ($) can be lost before the daily-loss
    // circuit breaker trips (#213). Breaker: daily_pnl_ < -max_daily_loss, so the
    // budget = max_daily_loss + daily_pnl_ (a profit increases it, a loss decreases it).
    // 0 = already at/past the threshold. Early warning for throttling positions.
    double remaining_loss_budget() const noexcept {
        const double budget = static_cast<double>(limits_.max_daily_loss) + daily_pnl_;
        return budget > 0.0 ? budget : 0.0;
    }
    // loss_budget_utilization_pct: how much of the daily-loss circuit-breaker budget
    // is consumed (#307), in [0, 100]. 0 while flat or in profit, 100 at the trip
    // point (daily_pnl_ == -max_daily_loss). The percentage companion to
    // remaining_loss_budget (#213, dollars) — dashboards alert on "85% of loss
    // budget used" more naturally than on a dollar figure. 0 when the limit is off.
    double loss_budget_utilization_pct() const noexcept {
        if (limits_.max_daily_loss <= 0) return 0.0;
        const double consumed = daily_pnl_ < 0.0 ? -daily_pnl_ : 0.0;
        const double pct = consumed / static_cast<double>(limits_.max_daily_loss) * 100.0;
        return pct > 100.0 ? 100.0 : pct;
    }
    uint64_t get_total_checks()               const noexcept { return total_checks_; }
    uint64_t get_total_rejects()              const noexcept { return total_rejects_; }
    // check_reject_rate: fraction of check_order checks that ended in rejection
    // (#229) = rejects / checks. High = the strategy often breaks limits (bad
    // tuning / aggressive algo). Mirror of OMS submit_reject_rate (#212). 0 when none.
    double check_reject_rate() const noexcept {
        return total_checks_ > 0
            ? static_cast<double>(total_rejects_) / static_cast<double>(total_checks_)
            : 0.0;
    }


    // ====================================================================
    // print_stats — dump for debug / monitoring
    // ====================================================================
    void print_stats() const {
        printf("\n=== Risk Manager Statistics ===\n");
        printf("  Total checks: %lu\n", (unsigned long)total_checks_);
        printf("  Allowed:      %lu\n", (unsigned long)(total_checks_ - total_rejects_));
        printf("  Rejected:     %lu\n", (unsigned long)total_rejects_);
        const double avg = total_checks_ > 0
            ? static_cast<double>(total_latency_ns_) / total_checks_ : 0.0;
        printf("  Avg latency:  %.0f ns/check\n", avg);
        printf("  Kill switch:  %s\n", kill_switch_active_ ? "ACTIVE" : "inactive");
        printf("  Daily P&L:    $%.2f\n", daily_pnl_);
    }


// ============================================================================
// Private part — internal helpers
// ============================================================================

private:

    // signed_qty: convert (side, qty) to a signed number.
    // BUY  → +qty
    // SELL → -qty
    // Used 4× in the class — extracted to avoid repeating the same
    // ternary.
    static int32_t signed_qty(Side side, int32_t qty) noexcept {
        return (side == Side::BUY) ? qty : -qty;
    }


    // lookup: read-only access to a map without inserting a default value.
    // Returns the value, or 0 if the key is absent. Used for both maps
    // (positions_, pending_).
    static int32_t lookup(const std::unordered_map<uint64_t, int32_t>& m,
                          uint64_t key) noexcept {
        auto it = m.find(key);
        return (it != m.end()) ? it->second : 0;
    }


    // adjust_pending: shared mutator for on_order_sent / on_order_cancelled.
    // Keeps the invariant total_abs_exposure_ = sum_s |pos[s] + pend[s]|.
    // Pure O(1).
    void adjust_pending(uint64_t key, int32_t delta) noexcept {
        const int32_t pos      = lookup(positions_, key);
        const int32_t old_pend = lookup(pending_,   key);
        const int32_t new_pend = old_pend + delta;
        pending_[key]         = new_pend;
        total_abs_exposure_  += std::abs(pos + new_pend) - std::abs(pos + old_pend);
    }


    // check_pnl_breakers: checks #5 and #6 from check_order.
    //
    // Returns:
    //   nullptr           — all OK, allow trading
    //   const char* str   — the rejection reason (the kill switch also turns
    //                        on as a side-effect of this function)
    //
    // After a breach kill_switch_active_ = true. Every subsequent check_order
    // rejects at #1 before it even reaches here.
    const char* check_pnl_breakers() noexcept {
        // 5. Circuit breaker — daily loss exceeded
        if (daily_pnl_ < -static_cast<double>(limits_.max_daily_loss)) {
            latch_kill_switch(KillReason::CIRCUIT_BREAKER);   // restart cannot bypass the trip
            return "Circuit breaker: daily loss limit";
        }
        // 6. Drawdown — % drop from peak_pnl_ exceeded the threshold
        if (peak_pnl_ > 0.0) {
            const double drawdown_pct = (peak_pnl_ - daily_pnl_) / peak_pnl_ * 100.0;
            if (drawdown_pct > limits_.max_drawdown_pct) {
                latch_kill_switch(KillReason::DRAWDOWN);
                return "Drawdown limit exceeded";
            }
        }
        return nullptr;
    }

    // persist_state: atomically write active+daily_pnl to persist_path_.
    // Atomic write = tmpfile + rename (rename on the same fs is atomic).
    // Without it a process crash mid-write would leave a corrupted file.
    void persist_state() noexcept {
        if (persist_path_.empty()) return;
        std::string tmp = persist_path_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "active=%d\nlast_pnl=%.6f\n",
                     kill_switch_active_ ? 1 : 0, daily_pnl_);
        std::fflush(f);
        ::fsync(::fileno(f));       // durable to the physical disk (POSIX, not std::)
        std::fclose(f);
        std::rename(tmp.c_str(), persist_path_.c_str());
    }


    // check_rate_limit: check #7 from check_order.
    //
    // Evicts stale entries (>1s old) from rate_ring_, then compares count
    // against the limit. O(1) amortized; contiguous memory avoids pointer
    // chasing from std::deque.
    //
    // Returns true (ALLOW), false (REJECT).
    bool check_rate_limit(int64_t now) noexcept {
        const int64_t one_sec_ago = now - 1'000'000'000;
        rate_ring_.evict(one_sec_ago);
        if (static_cast<int32_t>(rate_ring_.count()) >= limits_.max_orders_per_second) return false;
        rate_ring_.push(now);
        return true;
    }


    // make_reject: shortens the repetitive pattern in check_order.
    // Computes latency, increments total_rejects_, accumulates total_latency_ns_,
    // returns RiskCheckResult{REJECT, reason, elapsed}.
    RiskCheckResult make_reject(const char* reason, int64_t t0) noexcept {
        const int64_t elapsed = mono_ns() - t0;
        total_rejects_++;
        total_latency_ns_ += elapsed;
        return RiskCheckResult(RiskAction::REJECT, reason, elapsed);
    }
};
