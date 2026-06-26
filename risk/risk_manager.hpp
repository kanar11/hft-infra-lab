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
 *  3. Rate limiter (order_timestamps_)
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
#include <deque>
#include <cstdio>
#include <cstdlib>     // std::remove
#include <cmath>
#include <string>      // persist_path_
#include <atomic>      // kill switch — read/written from multiple threads (threaded pipeline)
#include <unistd.h>    // ::fsync, ::fileno

#include "../common/types.hpp"
#include "../common/symbol_key.hpp"
#include "../common/time_utils.hpp"


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
    int32_t consec_losses_ = 0;   // streak of losing update_pnl (#114)
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
    // A FIFO queue of timestamps (CLOCK_MONOTONIC). On every
    // check_order we evict from the front entries older than 1 second; if
    // the remaining size ≥ max_orders_per_second → REJECT.
    //
    // std::deque (not vector) because pop_front is O(1). With a vector you
    // would have to erase(begin, begin+N) = O(M) every time.
    // --------------------------------------------------------------------
    std::deque<int64_t> order_timestamps_;

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
        daily_pnl_ += pnl_change;
        if (daily_pnl_ > peak_pnl_) peak_pnl_ = daily_pnl_;
        // Loss-streak breaker (#114): N losses in a row -> kill switch.
        if (pnl_change < 0.0) {
            ++consec_losses_;
            if (limits_.max_consecutive_losses > 0
                && consec_losses_ >= limits_.max_consecutive_losses) {
                kill_switch_active_ = true;
                kill_reason_        = KillReason::CONSECUTIVE_LOSSES;
                persist_state();
            }
        } else if (pnl_change > 0.0) {
            consec_losses_ = 0;   // a profit resets the streak
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
        kill_switch_active_ = true;
        kill_reason_        = KillReason::MANUAL;
        persist_state();
    }
    void deactivate_kill_switch() noexcept {
        kill_switch_active_ = false;
        kill_reason_        = KillReason::NONE;
        persist_state();   // persist "off" too so a restart sees the current state
    }
    bool       is_kill_switch_active() const noexcept { return kill_switch_active_; }
    KillReason get_kill_reason()       const noexcept { return kill_reason_; }   // #121

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
        consec_losses_ = 0;
        traded_notional_ = 0.0;
        order_timestamps_.clear();
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
    // current_drawdown_dollars: absolute drawdown from the high-water mark in $
    // (#275) = peak_pnl_ - daily_pnl_, clamped >= 0. The dollar companion to
    // current_drawdown_pct (#197) — some desks size and alert on absolute drawdown
    // rather than a percentage. 0 at a new high.
    double   current_drawdown_dollars() const noexcept {
        const double dd = peak_pnl_ - daily_pnl_;
        return dd > 0.0 ? dd : 0.0;
    }
    int32_t  get_consecutive_losses()         const noexcept { return consec_losses_; }
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
            kill_switch_active_ = true;
            kill_reason_        = KillReason::CIRCUIT_BREAKER;
            persist_state();   // a process restart cannot bypass the trip
            return "Circuit breaker: daily loss limit";
        }
        // 6. Drawdown — % drop from peak_pnl_ exceeded the threshold
        if (peak_pnl_ > 0.0) {
            const double drawdown_pct = (peak_pnl_ - daily_pnl_) / peak_pnl_ * 100.0;
            if (drawdown_pct > limits_.max_drawdown_pct) {
                kill_switch_active_ = true;
                kill_reason_        = KillReason::DRAWDOWN;
                persist_state();
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
    // Strategy: keep a FIFO of order timestamps. Before checking,
    // evict all older than 1s (deque.pop_front). Then:
    //   if size ≥ limit → REJECT
    //   otherwise       → push_back(now), ALLOW
    //
    // Amortized O(1) per check (each timestamp is inserted once
    // and evicted once during its lifetime ≤ 1s).
    //
    // Returns true (ALLOW), false (REJECT).
    bool check_rate_limit(int64_t now) noexcept {
        const int64_t one_sec_ago = now - 1'000'000'000;
        while (!order_timestamps_.empty() && order_timestamps_.front() <= one_sec_ago)
            order_timestamps_.pop_front();
        if (static_cast<int32_t>(order_timestamps_.size()) >= limits_.max_orders_per_second)
            return false;
        order_timestamps_.push_back(now);
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
