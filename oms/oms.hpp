/*
 * Order Management System (OMS) — C++.
 *
 * The OMS sits on the critical path between strategy and exchange: every order
 * is validated here, pre-trade-checked, stored in a map by ID, and after a
 * fill it updates the position and realized P&L.
 *
 * Performance: ~10-50 M orders/sec (Python equivalent ~100-500 K/s).
 *
 * Lifecycle: NEW → SENT → FILLED | PARTIAL | CANCELLED | REJECTED.
 *
 * Key design decisions:
 *   - Header-only — just #include "oms.hpp".
 *   - Fixed-point int64 prices (× 10000) — no float on the hot path,
 *     avoids rounding errors.
 *   - Tickers as char[9] (max 8 chars + null) — no std::string
 *     allocation.
 *   - Map keyed by sym_to_key(symbol) — packed uint64_t,
 *     same scheme as RiskManager.
 *   - Pending exposure (Position::pending_qty) tracked in submit /
 *     fill / cancel — the pre-trade check counts realized + pending + new,
 *     preventing over-commit when many submits race with fills.
 *   - noexcept on the hot path — the compiler generates no stack-unwind.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <cstdio>
#include <cmath>

#include "../common/types.hpp"
#include "../common/symbol_key.hpp"
#include "../common/time_utils.hpp"


// Multiplier for fixed-point prices: $150.25 → 1502500.
static constexpr int64_t PRICE_SCALE = 10000;

// to_fixed: double price → int64 fixed-point. Half-up rounding.
inline int64_t to_fixed(double price) noexcept {
    return static_cast<int64_t>(price * PRICE_SCALE + 0.5);
}

// to_float: int64 fixed-point → double (display only).
inline double to_float(int64_t fixed_price) noexcept {
    return static_cast<double>(fixed_price) / PRICE_SCALE;
}


// Order lifecycle status. uint8_t — in an OMS with millions of orders
// the 3-byte-per-Order saving adds up.
enum class OrderStatus : uint8_t {
    NEW       = 0,   // created locally, not yet sent
    SENT      = 1,   // sent to the exchange (or simulator)
    FILLED    = 2,   // fully executed
    PARTIAL   = 3,   // partially executed, remainder still active
    CANCELLED = 4,   // cancelled (by us)
    REJECTED  = 5    // rejected (by the exchange / risk manager)
};

inline const char* status_str(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::NEW:       return "NEW";
        case OrderStatus::SENT:      return "SENT";
        case OrderStatus::FILLED:    return "FILLED";
        case OrderStatus::PARTIAL:   return "PARTIAL";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED:  return "REJECTED";
        default:                     return "UNKNOWN";
    }
}

// (Side enum + side_str are in common/types.hpp.)


// OMSReject — reason an order was rejected in submit_order (#88). Previously submit
// returned just nullptr with no info on WHY — the caller could not tell an input
// error from a value or position limit. Mirrors RejectReason from orderbook_pro.
enum class OMSReject : uint8_t {
    NONE           = 0,   // no rejection (success)
    INVALID_INPUT  = 1,   // NaN / price≤0 / qty=0 / empty symbol
    ORDER_VALUE    = 2,   // price × qty > max_order_value
    POSITION_LIMIT = 3,   // |realized + pending + new| > max_position
};

inline const char* oms_reject_str(OMSReject r) noexcept {
    switch (r) {
        case OMSReject::NONE:           return "NONE";
        case OMSReject::INVALID_INPUT:  return "INVALID_INPUT";
        case OMSReject::ORDER_VALUE:    return "ORDER_VALUE";
        case OMSReject::POSITION_LIMIT: return "POSITION_LIMIT";
        default:                        return "UNKNOWN";
    }
}


// Order — a single order. Stored in OMS::orders_ keyed by order_id.
//
// All times are nanoseconds from CLOCK_MONOTONIC (mono_ns). To measure
// submit-to-fill latency just subtract filled_ns - sent_ns.
struct Order {
    uint64_t    order_id;
    char        symbol[9];      // fixed-size, NASDAQ tickers are max 8 chars
    Side        side;
    int64_t     price;          // fixed-point: $150.25 → 1502500
    uint32_t    quantity;
    uint32_t    filled_qty;     // how much has been executed so far
    OrderStatus status;
    int64_t     created_ns;     // time accepted in submit_order
    int64_t     sent_ns;        // time sent (status → SENT)
    int64_t     filled_ns;      // time of the last fill
    int64_t     fill_notional;  // Σ fill_qty × fill_price (#141) — for avg fill price
    int64_t     expire_ns;      // GTD: expiry (#172); 0 = no expiry (DAY/GTC)
    uint32_t    fill_count = 0; // how many execution slices filled this order (#444)
    int64_t     cancelled_ns = 0; // when the cancel was applied; 0 = never (#452)

    Order() noexcept
        : order_id(0), side(Side::BUY), price(0), quantity(0),
          filled_qty(0), status(OrderStatus::NEW),
          created_ns(0), sent_ns(0), filled_ns(0), fill_notional(0), expire_ns(0) {
        symbol[0] = '\0';
    }

    Order(uint64_t id, const char* sym, Side s, int64_t px, uint32_t qty) noexcept
        : order_id(id), side(s), price(px), quantity(qty),
          filled_qty(0), status(OrderStatus::NEW),
          created_ns(0), sent_ns(0), filled_ns(0), fill_notional(0), expire_ns(0) {
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
    }

    // avg_fill_price: average execution price (fixed-point) — an order can be
    // filled at different prices; 0 when there is no fill. (#141)
    int64_t avg_fill_price() const noexcept {
        return filled_qty > 0 ? fill_notional / static_cast<int64_t>(filled_qty) : 0;
    }
};


// Position — aggregated exposure to one symbol. Stored in OMS::positions_
// keyed by sym_to_key(symbol).
//
// net_qty updates only in fill_order; pending_qty grows in submit and
// shrinks in fill/cancel. The sum (net + pending) is the "total committed
// limit" we guard in the pre-trade check.
struct Position {
    char    symbol[9];
    int32_t net_qty;         // realized (positive = long, negative = short)
    int32_t pending_qty;     // in flight (BUY+, SELL-)
    int64_t avg_price;       // average price of the realized long, fixed-point
    int64_t realized_pnl;    // cumulative P&L (gross) × PRICE_SCALE
    int64_t total_cost;      // sum of qty × price for the current long
    int64_t fees;            // cumulative commissions × PRICE_SCALE (#83)
    uint32_t round_trips = 0;   // flat-to-flat cycles completed by THIS name (#436)

    Position() noexcept
        : net_qty(0), pending_qty(0), avg_price(0), realized_pnl(0), total_cost(0), fees(0) {
        symbol[0] = '\0';
    }

    explicit Position(const char* sym) noexcept
        : net_qty(0), pending_qty(0), avg_price(0), realized_pnl(0), total_cost(0), fees(0) {
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
    }

    // net_pnl: P&L after subtracting commissions (what you actually keep).
    int64_t net_pnl() const noexcept { return realized_pnl - fees; }

    // unrealized_pnl: mark-to-market of the open position at the mark price (#96).
    // Signed: a long (net>0) profits when mark>avg; a short (net<0) when mark<avg —
    // net_qty * (mark - avg) covers both. Fixed-point (×PRICE_SCALE).
    int64_t unrealized_pnl(int64_t mark_price) const noexcept {
        return static_cast<int64_t>(net_qty) * (mark_price - avg_price);
    }

    // total_pnl: realized (gross) + unrealized - fees at the given mark price.
    int64_t total_pnl(int64_t mark_price) const noexcept {
        return realized_pnl + unrealized_pnl(mark_price) - fees;
    }
};


// OMS — the main class. Hot-path API:
//   submit_order  — pre-trade check + creation + pending reservation
//   fill_order    — pending → realized flow + P&L recomputation
//   cancel_order  — release pending for the unfilled remainder
//
// Read-only accessors: get_order, get_position, order_count, position_count.
class OMS {
    std::unordered_map<uint64_t, Order>    orders_;     // ID → Order
    std::unordered_map<uint64_t, Position> positions_;  // sym_to_key → Position

    uint64_t next_id_;
    int32_t  max_position_;     // limit |net + pending + new| per symbol
    int64_t  max_order_value_;  // limit price × qty per single order (in dollars)
    int64_t  commission_fp_;    // commission per share, fixed-point (×PRICE_SCALE)
    int64_t  total_fees_;       // cumulative commissions for the whole OMS, fixed-point
    OMSReject last_reject_ = OMSReject::NONE;  // reason for the last rejection (#88)
    uint64_t  reject_counts_[4] = {0, 0, 0, 0}; // rejection counter per OMSReject (#136)
    uint64_t  total_submitted_ = 0;  // accepted submits (#160)
    uint64_t  total_fills_    = 0;   // lifecycle operation counters (#151)
    uint64_t  total_cancels_  = 0;
    uint64_t  total_replaces_ = 0;
    uint64_t  total_ordered_shares_ = 0;  // sum of ordered shares (#228)
    uint64_t  total_filled_shares_  = 0;  // sum of executed shares (#228)
    double    total_traded_notional_    = 0.0; // cumulative $ value of all fills (#266)
    double    total_submitted_notional_ = 0.0; // cumulative $ value of all submitted orders (#322)
    int64_t   price_improvement_fp_     = 0;   // signed fill-vs-limit improvement, fixed-point (#396)
    uint64_t  round_trips_              = 0;   // positions closed to (or through) flat (#420)

public:
    // commission_per_share: e.g. 0.0035 = $0.0035/share (typical taker fee).
    OMS(int32_t max_pos = 1000, double max_val = 100000.0,
        double commission_per_share = 0.0) noexcept
        : next_id_(1),
          max_position_(max_pos),
          max_order_value_(static_cast<int64_t>(max_val)),
          commission_fp_(static_cast<int64_t>(commission_per_share * PRICE_SCALE + 0.5)),
          total_fees_(0) {}

    // submit_order: input validation, two pre-trade checks, Order creation,
    // pending reservation in Position. Returns a pointer to the order in the
    // map (stable while the OMS lives) or nullptr on rejection.
    // out_reason (optional, #88): on nullptr it receives the rejection reason.
    Order* submit_order(const char* symbol, Side side, double price_f,
                        uint32_t quantity, OMSReject* out_reason = nullptr,
                        int64_t expire_ns = 0) noexcept {
        const int64_t t0    = mono_ns();
        const int64_t price = to_fixed(price_f);
        auto fail = [&](OMSReject r) -> Order* {
            last_reject_ = r;
            ++reject_counts_[static_cast<int>(r)];   // #136 stats per reason
            if (out_reason) *out_reason = r;
            return nullptr;
        };

        // Input validation. NaN, negative/zero price, zero shares,
        // empty/null symbol — reject immediately.
        if (std::isnan(price_f) || price <= 0 || quantity == 0
            || symbol == nullptr || symbol[0] == '\0') {
            return fail(OMSReject::INVALID_INPUT);
        }

        // Pre-trade check #1: order value in dollars.
        const int64_t order_value = (price / PRICE_SCALE) * static_cast<int64_t>(quantity);
        if (order_value > max_order_value_) return fail(OMSReject::ORDER_VALUE);

        // Pre-trade check #2: position limit (realized + pending + new).
        const uint64_t sym_key  = sym_to_key(symbol);
        const int32_t  signed_n = signed_qty(side, quantity);
        auto           pos_it   = positions_.find(sym_key);
        const int32_t  cur_real = (pos_it != positions_.end()) ? pos_it->second.net_qty     : 0;
        const int32_t  cur_pend = (pos_it != positions_.end()) ? pos_it->second.pending_qty : 0;
        if (std::abs(cur_real + cur_pend + signed_n) > max_position_)
            return fail(OMSReject::POSITION_LIMIT);

        last_reject_ = OMSReject::NONE;
        if (out_reason) *out_reason = OMSReject::NONE;

        // Create the Order IN PLACE: try_emplace constructs it directly inside the
        // map node from the constructor args, avoiding the stack temporary and the
        // ~80-byte struct copy that emplace(id, order) did on every submit (hot path).
        // id is unique (next_id_++), so the insert always succeeds.
        const uint64_t id = next_id_++;
        auto it = orders_.try_emplace(id, id, symbol, side, price, quantity).first;
        Order& order = it->second;
        order.created_ns = t0;
        order.sent_ns    = mono_ns();
        order.status     = OrderStatus::SENT;
        order.expire_ns  = expire_ns;   // #172 GTD

        // Reserve pending. We create the Position lazily here (rather than at
        // the first fill) so that pending accounting is consistent on every
        // path.
        if (pos_it == positions_.end()) {
            pos_it = positions_.emplace(sym_key, Position(symbol)).first;
        }
        pos_it->second.pending_qty += signed_n;
        ++total_submitted_;   // #160
        total_ordered_shares_ += quantity;   // #228
        total_submitted_notional_ += price_f * static_cast<double>(quantity);  // #322
        return &it->second;
    }

    // fill_order: execution report from the exchange. Updates filled_qty + status
    // in Order, flows qty from pending to net in Position, recomputes avg_price
    // (BUY) or realized_pnl (SELL). Returns the qty actually applied
    // (0 when the order is unknown / already filled / fill_qty=0); may be SMALLER
    // than requested on an over-fill attempt (clamp to remaining + warn).
    uint32_t fill_order(uint64_t order_id, uint32_t fill_qty, double fill_price_f) noexcept {
        if (fill_qty == 0) return 0;
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: fill for unknown order_id=%lu\n", (unsigned long)order_id);
            return 0;
        }
        Order& order = it->second;
        const int64_t fill_price = to_fixed(fill_price_f);

        // Over-fill protection — venue ack > remaining qty = a bug on the
        // exchange side / the same fill twice / a strategy bug. Clamp to remaining,
        // record only the valid part, log it as an anomaly (audit + regression).
        const uint32_t remaining = (order.filled_qty < order.quantity)
            ? (order.quantity - order.filled_qty) : 0;
        if (fill_qty > remaining) {
            printf("[OMS] WARNING: over-fill on order_id=%lu (req=%u, remaining=%u) — clamping\n",
                   (unsigned long)order_id, fill_qty, remaining);
            fill_qty = remaining;
            if (fill_qty == 0) return 0;  // already fully filled
        }

        order.filled_qty    += fill_qty;
        order.fill_notional += static_cast<int64_t>(fill_qty) * fill_price;  // #141
        ++order.fill_count;  // #444: one more slice on THIS order
        ++total_fills_;   // #151
        total_filled_shares_ += fill_qty;   // #228
        total_traded_notional_ += static_cast<double>(fill_qty) * fill_price_f;  // #266
        // #396: signed price improvement vs the order's limit. BUY filling
        // BELOW the limit and SELL filling ABOVE it are positive; a fill at
        // the limit contributes 0; negative = worse than the limit
        // (slippage on marketable orders / venue misbehaviour).
        price_improvement_fp_ += ((order.side == Side::BUY)
                                      ? order.price - fill_price
                                      : fill_price - order.price)
                                 * static_cast<int64_t>(fill_qty);
        order.status      = (order.filled_qty >= order.quantity)
                            ? OrderStatus::FILLED
                            : OrderStatus::PARTIAL;
        order.filled_ns   = mono_ns();

        // Get / create the Position for this symbol. Normally the Position
        // already exists (created in submit_order), but this is a fallback for
        // paths that bypass submit (e.g. legacy tests).
        const uint64_t sym_key = sym_to_key(order.symbol);
        auto pos_it = positions_.find(sym_key);
        if (pos_it == positions_.end()) {
            pos_it = positions_.emplace(sym_key, Position(order.symbol)).first;
        }
        Position& pos = pos_it->second;

        // Flow qty from pending to net. pos.net_qty + pos.pending_qty stays
        // unchanged, so the total-exposure invariant is automatically
        // preserved.
        const int32_t signed_fill = signed_qty(order.side, fill_qty);
        pos.pending_qty -= signed_fill;
        const int32_t net_before = pos.net_qty;   // #420: flat-crossing detection
        apply_fill_to_position(pos, signed_fill, static_cast<int64_t>(fill_qty), fill_price);
        // #420: a non-flat position LANDING on flat or FLIPPING through it
        // completes a round trip — the flat-to-flat cycle whose count is the
        // denominator for per-trade session analytics. Opening from flat and
        // partial reductions do not count.
        if (net_before != 0
            && (pos.net_qty == 0 || (net_before > 0) != (pos.net_qty > 0))) {
            ++round_trips_;
            ++pos.round_trips;   // #436: the per-name share of the cycle count
        }

        // Commission: charged on every executed share (taker fee). It reduces
        // net P&L, not gross — realized_pnl stays "clean" for attribution.
        if (commission_fp_ != 0) {
            const int64_t fee = static_cast<int64_t>(fill_qty) * commission_fp_;
            pos.fees    += fee;
            total_fees_ += fee;
        }
        return fill_qty;  // how much was actually applied (after clamp)
    }

    // cancel_order: releases the unfilled remainder from pending. Only for
    // SENT or PARTIAL statuses — the rest are errors and get reported.
    void cancel_order(uint64_t order_id) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: cancel for unknown order_id=%lu\n", (unsigned long)order_id);
            return;
        }
        Order& order = it->second;
        if (order.status != OrderStatus::SENT && order.status != OrderStatus::PARTIAL) {
            // FILLED / CANCELLED / REJECTED — silently accepting would hide a
            // double-cancel bug on the caller's side. Log it.
            printf("[OMS] WARNING: cancel for inactive order_id=%lu (status=%s)\n",
                   (unsigned long)order_id, status_str(order.status));
            return;
        }
        const int32_t remaining        = static_cast<int32_t>(order.quantity)
                                       - static_cast<int32_t>(order.filled_qty);
        const int32_t signed_remaining = signed_qty(order.side, remaining);
        auto pos_it = positions_.find(sym_to_key(order.symbol));
        if (pos_it != positions_.end()) {
            pos_it->second.pending_qty -= signed_remaining;
        }
        order.status       = OrderStatus::CANCELLED;
        order.cancelled_ns = mono_ns();   // #452: quote-lifetime endpoint
        ++total_cancels_;   // #151
    }

    // replace_order: amend (cancel/replace) the price and/or quantity of an open order.
    // Maps to OUCH 'U' (Replace) and FIX 'G' (OrderCancelReplaceRequest).
    //
    // Rules:
    //   - only SENT / PARTIAL (like cancel)
    //   - new quantity ≥ filled_qty (cannot "un-fill"); ==filled → FILLED
    //   - pre-trade re-validation: order value + position limit on the NEW
    //     remaining exposure; breach → amend rejected, order unchanged
    //   - pending shifted by (new_remaining - old_remaining)
    //
    // Returns true when the amend is applied, false on rejection / error.
    bool replace_order(uint64_t order_id, double new_price_f, uint32_t new_quantity) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: replace for unknown order_id=%lu\n", (unsigned long)order_id);
            return false;
        }
        Order& order = it->second;
        if (order.status != OrderStatus::SENT && order.status != OrderStatus::PARTIAL) {
            printf("[OMS] WARNING: replace for inactive order_id=%lu (status=%s)\n",
                   (unsigned long)order_id, status_str(order.status));
            return false;
        }
        const int64_t new_price = to_fixed(new_price_f);
        if (std::isnan(new_price_f) || new_price <= 0 || new_quantity == 0
            || new_quantity < order.filled_qty) {
            return false;
        }

        // Pre-trade #1: order value at the new price/quantity.
        const int64_t order_value = (new_price / PRICE_SCALE) * static_cast<int64_t>(new_quantity);
        if (order_value > max_order_value_) return false;

        // Pre-trade #2: position limit on the replaced remaining exposure.
        const uint64_t sym_key       = sym_to_key(order.symbol);
        const int32_t  old_remaining = static_cast<int32_t>(order.quantity) - static_cast<int32_t>(order.filled_qty);
        const int32_t  new_remaining = static_cast<int32_t>(new_quantity)   - static_cast<int32_t>(order.filled_qty);
        const int32_t  old_pend      = signed_qty(order.side, old_remaining);
        const int32_t  new_pend      = signed_qty(order.side, new_remaining);
        auto           pos_it        = positions_.find(sym_key);
        const int32_t  cur_real      = (pos_it != positions_.end()) ? pos_it->second.net_qty     : 0;
        const int32_t  cur_pend      = (pos_it != positions_.end()) ? pos_it->second.pending_qty : 0;
        const int32_t  projected     = cur_real + (cur_pend - old_pend + new_pend);
        if (std::abs(projected) > max_position_) return false;

        // Commit: shift pending, swap price/quantity, recompute status.
        if (pos_it != positions_.end()) pos_it->second.pending_qty += (new_pend - old_pend);
        order.price    = new_price;
        order.quantity = new_quantity;
        order.status   = (order.filled_qty >= order.quantity) ? OrderStatus::FILLED
                       : (order.filled_qty > 0)               ? OrderStatus::PARTIAL
                                                              : OrderStatus::SENT;
        ++total_replaces_;   // #151
        return true;
    }

    // amend_quantity: in-place quantity REDUCTION, same price (#188). A reduce-only
    // amend usually KEEPS queue priority on the exchange (unlike a full
    // cancel/replace that changes price). Enforces: order active, new quantity
    // smaller than the current one and not below what is already filled. Delegates
    // to replace_order (same price), so pending/position accounting stays consistent.
    bool amend_quantity(uint64_t order_id, uint32_t new_quantity) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return false;
        const Order& order = it->second;
        if (order.status != OrderStatus::SENT && order.status != OrderStatus::PARTIAL) return false;
        if (new_quantity >= order.quantity)   return false;   // reduction only
        if (new_quantity <  order.filled_qty) return false;   // not below what is filled
        return replace_order(order_id, to_float(order.price), new_quantity);
    }

    // cancel_all: mass-cancel all ACTIVE orders (#100). Risk-off / panic button —
    // on a kill switch or at end of session we pull all open orders (releases
    // pending). Returns how many were cancelled. Iterating over orders_ is safe:
    // cancel_order only changes status + pending_, not the structure of the
    // orders_ map.
    size_t cancel_all() noexcept {
        size_t n = 0;
        for (auto& [id, o] : orders_) {
            if (o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL) {
                cancel_order(id);
                ++n;
            }
        }
        return n;
    }

    // purge_expired: cancel all ACTIVE GTD orders whose expiry time has passed
    // (#172; expire_ns > 0 && <= now_ns). Returns how many were cancelled.
    // Call from the session loop with the current mono_ns().
    size_t purge_expired(int64_t now_ns) noexcept {
        size_t n = 0;
        for (auto& [id, o] : orders_) {
            if ((o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL)
                && o.expire_ns > 0 && o.expire_ns <= now_ns) {
                cancel_order(id);
                ++n;
            }
        }
        return n;
    }

    // open_order_notional: total notional ($) of the UNFILLED part of all working
    // orders (SENT/PARTIAL) (#180). Capital tied up in the order book — a pre-trade
    // exposure view independent of positions (those are covered by unrealized_pnl).
    // Counts only the remainder (quantity - filled_qty), so partials count fairly.
    double open_order_notional() const noexcept {
        double sum = 0.0;
        for (const auto& [id, o] : orders_) {
            if (o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL) {
                const uint32_t remaining = (o.filled_qty < o.quantity)
                    ? (o.quantity - o.filled_qty) : 0;
                sum += to_float(o.price) * static_cast<double>(remaining);
            }
        }
        return sum;
    }

    // open_order_notional_symbol: the per-NAME slice of open_order_notional
    // (#404) — capital tied up in working orders on ONE ticker. The $ view
    // of per-symbol working exposure: Position.pending_qty gives the signed
    // SHARES, this prices the unfilled remainders at their limit prices
    // (same convention as #180, and the per-symbol slices sum to it). Pairs
    // with risk's max_symbol_notional-style per-name limits: how much of
    // the name's budget is already committed to the book. 0 for an unknown
    // symbol or one with nothing working.
    double open_order_notional_symbol(const char* symbol) const noexcept {
        const uint64_t key = sym_to_key(symbol);
        double sum = 0.0;
        for (const auto& [id, o] : orders_) {
            if ((o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL)
                && sym_to_key(o.symbol) == key) {
                const uint32_t remaining = (o.filled_qty < o.quantity)
                    ? (o.quantity - o.filled_qty) : 0;
                sum += to_float(o.price) * static_cast<double>(remaining);
            }
        }
        return sum;
    }

    // open_position_count: number of symbols with a NON-ZERO net position (#196).
    // Unlike position_count() (= number of map entries, including ones closed to
    // 0): this is the true breadth of the portfolio — how many instruments we hold.
    size_t open_position_count() const noexcept {
        size_t c = 0;
        for (const auto& [key, p] : positions_) if (p.net_qty != 0) ++c;
        return c;
    }

    // gross_position_shares: sum of |net_qty| over all symbols (#220) — total GROSS
    // directional exposure in shares (long + |short|). Unlike net (where long and
    // short would cancel) — a measure of the total size of the book.
    int64_t gross_position_shares() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_) s += std::abs(static_cast<int64_t>(p.net_qty));
        return s;
    }

    // largest_position: the largest SINGLE position |net_qty| (#220) — risk
    // concentration in one instrument (for limits/dashboard). 0 when flat.
    int32_t largest_position() const noexcept {
        int32_t m = 0;
        for (const auto& [key, p] : positions_) {
            const int32_t a = std::abs(p.net_qty);
            if (a > m) m = a;
        }
        return m;
    }
    // inventory_value: cost basis ($) of all open inventory (#314) = Σ |net_qty| *
    // avg_price across positions, fixed-point (×PRICE_SCALE). The capital deployed
    // in HELD positions, vs open_order_notional (#180, capital in WORKING orders).
    // Longs and shorts both add (absolute commitment). 0 when flat.
    int64_t inventory_value() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_)
            s += std::abs(static_cast<int64_t>(p.net_qty)) * p.avg_price;
        return s;
    }

    // net_position_shares: the SIGNED sum of net_qty across all symbols
    // (#484) — the book's directional tilt in shares, positive = net long.
    // The signed companion to gross_position_shares (#220, the absolute
    // sum): a market-neutral book has a large gross and a near-zero net,
    // and the gap between them is how directional the desk actually is
    // (net == gross means every name leans the same way). 0 when flat.
    int64_t net_position_shares() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_) s += p.net_qty;
        return s;
    }

    // net_inventory_value: the SIGNED cost-basis value of the book (#484) =
    // Σ net_qty * avg_price, fixed-point (×PRICE_SCALE). The directional $
    // companion to inventory_value (#314, the gross absolute value): long
    // positions add, shorts subtract, so this is the net long-minus-short
    // dollar exposure at cost. A market-neutral book nets near zero here
    // even with a large gross inventory. to_float for dollars.
    int64_t net_inventory_value() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_)
            s += static_cast<int64_t>(p.net_qty) * p.avg_price;
        return s;
    }

    // largest_position_notional: the biggest SINGLE position by absolute $ value (#330)
    // = max over positions of |net_qty| * avg_price, fixed-point (×PRICE_SCALE). The
    // dollar companion to largest_position (#220, shares): risk limits care about
    // capital at risk, and 100 shares of a $3000 name dwarf 1000 shares of a $2 name.
    // Pinpoints the instrument holding the most concentration risk in dollars. 0 flat.
    int64_t largest_position_notional() const noexcept {
        int64_t m = 0;
        for (const auto& [key, p] : positions_) {
            const int64_t v = std::abs(static_cast<int64_t>(p.net_qty)) * p.avg_price;
            if (v > m) m = v;
        }
        return m;
    }

    // pending_buy_shares / pending_sell_shares: total working (pending) shares per
    // side across all symbols (#251). pending_qty is signed (buy +, sell -); these
    // split it into directional exposure of live orders not yet filled. Pre-trade
    // view of how much buy vs sell pressure is in flight.
    int64_t pending_buy_shares() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_) if (p.pending_qty > 0) s += p.pending_qty;
        return s;
    }
    int64_t pending_sell_shares() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_) if (p.pending_qty < 0) s += -p.pending_qty;
        return s;
    }

    // is_flat: no open positions AND no working orders (#196).
    // End-of-day / reconciliation check: whether the desk is fully closed.
    bool is_flat() const noexcept {
        if (open_position_count() != 0) return false;
        for (const auto& [id, o] : orders_)
            if (o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL) return false;
        return true;
    }

    // cancel_all_symbol: like cancel_all, but only for one ticker (e.g. after a
    // halt of a specific symbol).
    size_t cancel_all_symbol(const char* symbol) noexcept {
        const uint64_t key = sym_to_key(symbol);
        size_t n = 0;
        for (auto& [id, o] : orders_) {
            if ((o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL)
                && sym_to_key(o.symbol) == key) {
                cancel_order(id);
                ++n;
            }
        }
        return n;
    }

    // Accessors (read-only).
    const Order*    get_order(uint64_t id)         const noexcept {
        auto it = orders_.find(id);
        return (it != orders_.end()) ? &it->second : nullptr;
    }
    const Position* get_position(const char* sym)  const noexcept {
        auto it = positions_.find(sym_to_key(sym));
        return (it != positions_.end()) ? &it->second : nullptr;
    }
    size_t order_count()    const noexcept { return orders_.size(); }
    size_t position_count() const noexcept { return positions_.size(); }
    // count_by_status: how many orders are in a given state (#128) — observability/
    // monitoring (e.g. how many active SENT/PARTIAL, how many rejected).
    size_t count_by_status(OrderStatus st) const noexcept {
        size_t n = 0;
        for (const auto& kv : orders_) if (kv.second.status == st) ++n;
        return n;
    }
    // working_order_count / done_order_count: live working orders (SENT or PARTIAL,
    // still resting on the exchange) vs terminal ones (FILLED/CANCELLED/REJECTED)
    // (#290). A one-call view of the order book's state — working is what the desk
    // still has exposure to; the two together split order_count.
    size_t working_order_count() const noexcept {
        size_t n = 0;
        for (const auto& [id, o] : orders_)
            if (o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL) ++n;
        return n;
    }
    size_t done_order_count() const noexcept {
        size_t n = 0;
        for (const auto& [id, o] : orders_)
            if (o.status == OrderStatus::FILLED || o.status == OrderStatus::CANCELLED
                || o.status == OrderStatus::REJECTED) ++n;
        return n;
    }
    // oldest_working_order_age_ns: age of the LONGEST-RESTING working order
    // (SENT/PARTIAL, #290's split) against a caller-supplied clock (#388) =
    // now_ns - min(sent_ns). The stale-order detector: a resting order nobody
    // remembers is unpriced risk — avg_time_to_fill_ns (#380) only sees
    // orders AFTER completion, this watches them WHILE they rest. Pass
    // mono_ns() in production; tests pass a synthetic now for determinism.
    // 0 when nothing is working or now precedes the oldest sent timestamp.
    int64_t oldest_working_order_age_ns(int64_t now_ns) const noexcept {
        int64_t oldest_sent = 0;
        bool    found = false;
        for (const auto& [id, o] : orders_) {
            if (o.status != OrderStatus::SENT && o.status != OrderStatus::PARTIAL) continue;
            if (!found || o.sent_ns < oldest_sent) { oldest_sent = o.sent_ns; found = true; }
        }
        return (found && now_ns > oldest_sent) ? now_ns - oldest_sent : 0;
    }
    // oldest_working_order_id: the ID of that longest-resting working order
    // (#388) — the actionable half: WHICH order to chase, reprice or pull.
    // 0 when nothing is working.
    uint64_t oldest_working_order_id() const noexcept {
        int64_t  oldest_sent = 0;
        uint64_t oid   = 0;
        bool     found = false;
        for (const auto& [id, o] : orders_) {
            if (o.status != OrderStatus::SENT && o.status != OrderStatus::PARTIAL) continue;
            if (!found || o.sent_ns < oldest_sent) { oldest_sent = o.sent_ns; oid = id; found = true; }
        }
        return oid;
    }
    // avg_working_order_age_ns: MEAN age of the working book (#412) against
    // a caller-supplied clock — the whole-book staleness companion to
    // oldest_working_order_age_ns' (#388) single worst order. A high mean
    // says the entire book is stale (quotes not being refreshed), while a
    // high oldest with a low mean is one forgotten order. Same clock
    // injection and SENT/PARTIAL filter as #388; ages clamp at 0 when now
    // precedes a stamp. 0 when nothing is working.
    int64_t avg_working_order_age_ns(int64_t now_ns) const noexcept {
        int64_t sum = 0, n = 0;
        for (const auto& [id, o] : orders_) {
            if (o.status != OrderStatus::SENT && o.status != OrderStatus::PARTIAL) continue;
            if (now_ns > o.sent_ns) sum += now_ns - o.sent_ns;
            ++n;
        }
        return n > 0 ? sum / n : 0;
    }
    // total_fees: cumulative commissions for the whole OMS (fixed-point ×PRICE_SCALE).
    int64_t total_fees() const noexcept { return total_fees_; }
    // avg_commission_per_share: average commission ($) per EXECUTED share (#236) =
    // total_fees / total_filled_shares. A TCA measure of execution cost on the OMS
    // side (mirror of router avg_fee_per_share #232). 0 when nothing was executed.
    double  avg_commission_per_share() const noexcept {
        return total_filled_shares_ > 0
            ? to_float(total_fees_) / static_cast<double>(total_filled_shares_)
            : 0.0;
    }
    // Runtime commission change (#166) — the fee schedule can change during a session
    // (volume tier, promotion). Subsequent fills use the new rate.
    void   set_commission(double per_share) noexcept {
        commission_fp_ = static_cast<int64_t>(per_share * PRICE_SCALE + 0.5);
    }
    double commission_per_share() const noexcept {
        return static_cast<double>(commission_fp_) / PRICE_SCALE;
    }

    // Portfolio P&L aggregates (#120) — sum over all positions. Fixed-point
    // (×PRICE_SCALE); to_float for dollars. Replaces a manual loop on the caller's
    // side (sim/backtest used to sum positions itself).
    int64_t total_realized_pnl() const noexcept {
        int64_t s = 0;
        for (const auto& kv : positions_) s += kv.second.realized_pnl;
        return s;
    }
    int64_t total_net_pnl() const noexcept {     // realized - fees across the portfolio
        int64_t s = 0;
        for (const auto& kv : positions_) s += kv.second.net_pnl();
        return s;
    }
    // winning_symbols / losing_symbols: number of symbols with positive / negative
    // realized P&L (#244) — instrument-level hit rate (attribution: how many names
    // make money vs lose). Symbols at zero are skipped in both.
    size_t winning_symbols() const noexcept {
        size_t c = 0;
        for (const auto& [key, p] : positions_) if (p.realized_pnl > 0) ++c;
        return c;
    }
    size_t losing_symbols() const noexcept {
        size_t c = 0;
        for (const auto& [key, p] : positions_) if (p.realized_pnl < 0) ++c;
        return c;
    }
    // symbol_win_rate: fraction of symbols with positive realized P&L among those
    // that ended non-flat (#298) = winning_symbols / (winning + losing). The
    // instrument-level hit rate as a ratio (vs the raw counts in #244). Symbols at
    // exactly zero realized P&L are excluded from both. 0 when none have realized.
    double symbol_win_rate() const noexcept {
        const size_t w = winning_symbols();
        const size_t decided = w + losing_symbols();
        return decided > 0 ? static_cast<double>(w) / static_cast<double>(decided) : 0.0;
    }
    // gross_profit / gross_loss: the dollar attribution behind winning_symbols /
    // losing_symbols (#244) — gross_profit sums the realized P&L of the profitable
    // symbols, gross_loss the MAGNITUDE of the losing ones (returned positive). In
    // raw P&L units (×PRICE_SCALE, like total_realized_pnl). Together they show not
    // just HOW MANY names win/lose but by HOW MUCH — a few big losers can sink a
    // high symbol win-rate. Symbols at exactly zero realized P&L are skipped.
    int64_t gross_profit() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_) if (p.realized_pnl > 0) s += p.realized_pnl;
        return s;
    }
    int64_t gross_loss() const noexcept {
        int64_t s = 0;
        for (const auto& [key, p] : positions_) if (p.realized_pnl < 0) s -= p.realized_pnl;
        return s;   // positive magnitude of the losing symbols
    }
    // best_realized_symbol / worst_realized_symbol: the NAME of the biggest
    // realized winner / loser by P&L (#492) — the actionable attribution
    // behind gross_profit / gross_loss (#339, the totals): which single
    // name carried the day and which dragged it down. Writes the ticker
    // into out (>= 9 bytes, always nul-terminated) and returns its realized
    // P&L in dollars (best is the max, worst the min — so worst is negative
    // when any name lost). Only symbols with NON-ZERO realized P&L are
    // considered; out[0] == '\0' and 0 when none has realized anything.
    // The P&L-attribution companion to largest_position_notional (#330,
    // which names by SIZE, not P&L).
    double best_realized_symbol(char* out) const noexcept {
        int64_t best = 0; const Position* who = nullptr;
        for (const auto& [key, p] : positions_) {
            if (p.realized_pnl == 0) continue;
            if (who == nullptr || p.realized_pnl > best) { best = p.realized_pnl; who = &p; }
        }
        // symbol is char[9], already nul-terminated -> copy all 9 bytes
        // (memcpy avoids the -Werror=stringop-truncation strncpy heuristic).
        if (who) { std::memcpy(out, who->symbol, 9); return to_float(best); }
        out[0] = '\0'; return 0.0;
    }
    double worst_realized_symbol(char* out) const noexcept {
        int64_t worst = 0; const Position* who = nullptr;
        for (const auto& [key, p] : positions_) {
            if (p.realized_pnl == 0) continue;
            if (who == nullptr || p.realized_pnl < worst) { worst = p.realized_pnl; who = &p; }
        }
        if (who) { std::memcpy(out, who->symbol, 9); return to_float(worst); }
        out[0] = '\0'; return 0.0;
    }
    // profit_factor: gross_profit / gross_loss (#339) — the classic ratio (>1 =
    // realized winners outweigh losers). Same convention as the Backtester: with no
    // losing symbols it is +inf when there is any profit, 0 when nothing is realized.
    // The P&L-weighted counterpart to symbol_win_rate (#298, a pure count ratio).
    double profit_factor() const noexcept {
        const int64_t gp = gross_profit();
        const int64_t gl = gross_loss();
        if (gl > 0) return static_cast<double>(gp) / static_cast<double>(gl);
        return gp > 0 ? std::numeric_limits<double>::infinity() : 0.0;
    }
    // avg_win_per_symbol / avg_loss_per_symbol: mean realized P&L magnitude
    // across winning / losing symbols (#347) = gross_profit / winning_symbols
    // and gross_loss / losing_symbols. Where gross_profit/gross_loss (#339)
    // show the total dollars won/lost, these show the dollars won/lost PER
    // NAME — e.g. a high win rate (#298) built on many small avg_win symbols
    // next to one huge avg_loss can still net negative. 0 when there are no
    // winning / losing symbols respectively.
    double avg_win_per_symbol() const noexcept {
        const size_t w = winning_symbols();
        return w > 0 ? to_float(gross_profit()) / static_cast<double>(w) : 0.0;
    }
    double avg_loss_per_symbol() const noexcept {
        const size_t l = losing_symbols();
        return l > 0 ? to_float(gross_loss()) / static_cast<double>(l) : 0.0;
    }
    // largest_win / largest_loss: the SINGLE biggest winning / losing symbol by
    // realized P&L (#355). Complements avg_win_per_symbol/avg_loss_per_symbol
    // (#347, the mean): one big winner or loser can dominate the average, and
    // this shows how much of it is concentrated in a single name. Fixed-point
    // (×PRICE_SCALE), same raw units as gross_profit/gross_loss (#339);
    // largest_loss is returned as a positive magnitude. 0 when there are no
    // winning / losing symbols respectively.
    int64_t largest_win() const noexcept {
        int64_t mx = 0;
        for (const auto& [key, p] : positions_) if (p.realized_pnl > mx) mx = p.realized_pnl;
        return mx;
    }
    int64_t largest_loss() const noexcept {
        int64_t mx = 0;
        for (const auto& [key, p] : positions_)
            if (p.realized_pnl < 0 && -p.realized_pnl > mx) mx = -p.realized_pnl;
        return mx;
    }
    // expectancy_per_symbol: the expected realized P&L per DECIDED symbol (#363),
    // the frequency×magnitude composite of symbol_win_rate (#298) and
    // avg_win_per_symbol/avg_loss_per_symbol (#347):
    //   win_rate * avg_win - loss_rate * avg_loss
    // which algebraically collapses to net realized P&L over the decided symbols
    // (winners + losers, flat names excluded). A single number that captures the
    // whole edge — a high win rate can still yield NEGATIVE expectancy if the
    // average loss dwarfs the average win. In dollars (to_float of the fixed-point
    // accumulators). 0 when no symbol has resolved. Same shape as the Backtester's
    // per-trade expectancy, but per symbol.
    double expectancy_per_symbol() const noexcept {
        const size_t decided = winning_symbols() + losing_symbols();
        if (decided == 0) return 0.0;
        const double net = to_float(gross_profit()) - to_float(gross_loss());
        return net / static_cast<double>(decided);
    }
    // last_reject: reason for the last submit_order rejection (#88).
    OMSReject last_reject() const noexcept { return last_reject_; }
    // reject_count: how many orders were rejected for a given reason (#136, observability).
    uint64_t reject_count(OMSReject r) const noexcept { return reject_counts_[static_cast<int>(r)]; }
    // total_rejects: total number of rejected submits (#212) — sums all reasons
    // EXCEPT NONE (index 0 is success, never incremented in fail()).
    uint64_t total_rejects() const noexcept {
        return reject_counts_[1] + reject_counts_[2] + reject_counts_[3];
    }
    // submit_reject_rate: fraction of submit attempts that ended in rejection (#212) =
    // rejects / (accepted + rejects). Observability of pre-trade quality (bad limit
    // tuning / a runaway algo give a high rate). 0 when there are no attempts.
    double submit_reject_rate() const noexcept {
        const uint64_t rej = total_rejects();
        const uint64_t tot = total_submitted_ + rej;
        return tot ? static_cast<double>(rej) / static_cast<double>(tot) : 0.0;
    }
    // Lifecycle operation counters (#151/#160, observability/dashboard).
    uint64_t total_submitted() const noexcept { return total_submitted_; }
    uint64_t total_fills()    const noexcept { return total_fills_; }
    // avg_fill_size: average shares per fill (#274) = total_filled_shares /
    // total_fills. Execution granularity — a small average means orders are being
    // chopped into many small fills (more exchange messages and per-fill fees,
    // and possible information leakage). 0 when nothing filled.
    double   avg_fill_size() const noexcept {
        return total_fills_ > 0
            ? static_cast<double>(total_filled_shares_) / static_cast<double>(total_fills_)
            : 0.0;
    }
    uint64_t total_cancels()  const noexcept { return total_cancels_; }
    uint64_t total_replaces() const noexcept { return total_replaces_; }
    uint64_t total_ordered_shares() const noexcept { return total_ordered_shares_; }
    // avg_submitted_size: mean order size in SHARES across all submits (#371) =
    // total_ordered_shares / total_submitted. The entry-side companion to
    // avg_fill_size (#274, shares per FILL): comparing them shows how the venue
    // slices orders — avg_submitted_size >> avg_fill_size means each order is
    // broken into many small fills. In shares (unlike avg_submitted_notional
    // #322, which is $). 0 when nothing has been submitted.
    double avg_submitted_size() const noexcept {
        return total_submitted_ > 0
            ? static_cast<double>(total_ordered_shares_) / static_cast<double>(total_submitted_)
            : 0.0;
    }
    // avg_time_to_fill_ns: mean submit→completion latency over fully FILLED
    // orders (#380) = mean(filled_ns - sent_ns). filled_ns records the LAST
    // fill, so this measures time to COMPLETE an order, not to its first
    // slice. The speed axis of execution TCA next to the size axis of
    // avg_fill_size (#274): same sizes but growing time-to-fill points at
    // passive pricing or thinning venues. Working/partial orders are skipped
    // (no completion yet). 0 when nothing has completed.
    int64_t avg_time_to_fill_ns() const noexcept {
        int64_t sum = 0, n = 0;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (o.status == OrderStatus::FILLED && o.filled_ns > o.sent_ns) {
                sum += o.filled_ns - o.sent_ns;
                ++n;
            }
        }
        return n > 0 ? sum / n : 0;
    }
    // max_time_to_fill_ns: the SLOWEST submit→completion latency (#380) — the
    // tail companion to avg_time_to_fill_ns' mean (one stuck order hides in a
    // healthy average). 0 when nothing has completed.
    int64_t max_time_to_fill_ns() const noexcept {
        int64_t mx = 0;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (o.status == OrderStatus::FILLED && o.filled_ns > o.sent_ns) {
                const int64_t d = o.filled_ns - o.sent_ns;
                if (d > mx) mx = d;
            }
        }
        return mx;
    }
    // total_price_improvement: cumulative signed fill-vs-limit improvement in
    // $ (#396). BUY filling below its limit and SELL filling above accrue
    // positive; a fill AT the limit contributes 0; negative = filled past
    // the limit (slippage/venue misbehaviour). The PRICE-quality axis of
    // execution TCA next to speed (#380) and size (#274/#371).
    double total_price_improvement() const noexcept {
        return to_float(price_improvement_fp_);
    }
    // avg_price_improvement_per_share: improvement per EXECUTED share (#396)
    // = total improvement / total_filled_shares — comparable across sessions
    // and directly against avg_commission_per_share (#236): improvement
    // below the fee means the better prices do not pay for the executions.
    // 0 when nothing has filled. (Fixed-point: to_float BEFORE dividing.)
    double avg_price_improvement_per_share() const noexcept {
        return total_filled_shares_ > 0
            ? to_float(price_improvement_fp_) / static_cast<double>(total_filled_shares_)
            : 0.0;
    }
    // round_trips: completed flat-to-flat position cycles (#420, MILESTONE
    // 420) — a non-flat position landing on flat, or flipping through it,
    // closes one. The per-TRADE denominator the per-fill and per-order
    // counters cannot provide: realized P&L / round_trips is the average
    // P&L per completed trade, the number a session review actually wants.
    // Reset by reset_session_counters.
    uint64_t round_trips() const noexcept { return round_trips_; }
    // avg_pnl_per_round_trip: realized P&L per completed flat-to-flat cycle
    // (#428) — the number #420 promised, delivered: total_realized_pnl /
    // round_trips, in dollars. Per-TRADE expectancy of the whole book,
    // where expectancy_per_symbol (#363) is per NAME: ten scratches and one
    // winner average very differently from eleven small winners even at the
    // same total. Fixed-point accumulator: to_float BEFORE dividing (#347
    // lesson). 0 before the first completed cycle. NOTE: realized P&L on a
    // cycle still OPEN (a flip's carried leg) is included in the numerator
    // — over a flat-ending session the two agree exactly.
    double avg_pnl_per_round_trip() const noexcept {
        return round_trips_ > 0
            ? to_float(total_realized_pnl()) / static_cast<double>(round_trips_)
            : 0.0;
    }
    // realized_pnl_per_share: the average $ edge captured per executed
    // share (#460, MILESTONE 460) = total_realized_pnl / total_filled_
    // shares. The headline efficiency number: avg_pnl_per_round_trip (#428)
    // is per completed TRADE, this is per SHARE of flow — directly
    // comparable against avg_commission_per_share (#236) to see whether the
    // edge survives fees (realized_pnl_per_share - avg_commission_per_share
    // is the net per-share take). Fixed-point accumulator: to_float BEFORE
    // dividing (the #347 lesson). 0 before any fill. Both opening and
    // closing legs count in the denominator, so this is edge per share of
    // TURNOVER, not per round trip.
    double realized_pnl_per_share() const noexcept {
        return total_filled_shares_ > 0
            ? to_float(total_realized_pnl()) / static_cast<double>(total_filled_shares_)
            : 0.0;
    }
    // net_pnl_per_share: the per-share edge AFTER commissions (#468) =
    // total_net_pnl / total_filled_shares. realized_pnl_per_share (#460) is
    // GROSS; this is the honest number that decides whether the strategy
    // makes money once it pays to trade — it equals realized_pnl_per_share
    // minus avg_commission_per_share (#236) by construction, and a strategy
    // with a positive gross edge below its per-share fee reads NEGATIVE
    // here. Fixed-point converted with to_float before dividing (#347). 0
    // before any fill.
    double net_pnl_per_share() const noexcept {
        return total_filled_shares_ > 0
            ? to_float(total_net_pnl()) / static_cast<double>(total_filled_shares_)
            : 0.0;
    }
    // max_time_to_cancel_ns: the LONGEST quote life before a cancel (#460) —
    // the tail companion to avg_time_to_cancel_ns' (#452) mean, exactly as
    // max_time_to_fill_ns (#380) tails avg_time_to_fill_ns. One quote that
    // rested for minutes hides in a healthy average. 0 when nothing was
    // cancelled.
    int64_t max_time_to_cancel_ns() const noexcept {
        int64_t mx = 0;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (o.status == OrderStatus::CANCELLED && o.cancelled_ns > o.sent_ns) {
                const int64_t d = o.cancelled_ns - o.sent_ns;
                if (d > mx) mx = d;
            }
        }
        return mx;
    }

    // avg_time_to_cancel_ns: mean QUOTE LIFETIME of cancelled orders (#452)
    // = mean(cancelled_ns - sent_ns) over CANCELLED records. The other half
    // of order-lifetime TCA: avg_time_to_fill_ns (#380) times the orders
    // the market took, this times the ones we pulled — a market maker's
    // quote life. Short lives with a high cancel_rate (#258) mean quote
    // churn (message-rate fees, surveillance flags); very long lives mean
    // quotes rest far from the action. 0 when nothing was cancelled.
    int64_t avg_time_to_cancel_ns() const noexcept {
        int64_t sum = 0, n = 0;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (o.status == OrderStatus::CANCELLED && o.cancelled_ns > o.sent_ns) {
                sum += o.cancelled_ns - o.sent_ns;
                ++n;
            }
        }
        return n > 0 ? sum / n : 0;
    }

    // symbol_round_trips: flat-to-flat cycles completed by ONE name (#436) —
    // WHICH names actually recycle capital, where round_trips (#420) only
    // totals them. A name with many cycles and thin realized P&L churns;
    // one cycle carrying most of the profit is where the edge lives.
    // LIFETIME NOTE: this lives on the Position (kept across
    // reset_session_counters, like realized_pnl), while the global #420
    // counter is a session stat and zeroes — the divergence is pinned by
    // test. 0 for an unknown symbol.
    uint32_t symbol_round_trips(const char* symbol) const noexcept {
        const auto it = positions_.find(sym_to_key(symbol));
        return it != positions_.end() ? it->second.round_trips : 0;
    }
    // order_fill_count: how many execution slices filled ONE order (#444) —
    // the per-order face of avg_fill_size's (#274) global mean, and the OMS
    // analog of the OUCH tracker's executions_per_order (#337). 0 for an
    // unknown id.
    uint32_t order_fill_count(uint64_t order_id) const noexcept {
        const auto it = orders_.find(order_id);
        return it != orders_.end() ? it->second.fill_count : 0;
    }
    // max_order_fill_count: the most-fragmented order's slice count (#444).
    // avg_fill_size can look healthy while one order got shredded into
    // dozens of odd lots (per-fill fees, information leakage) — the worst
    // case is the number the venue-quality review wants. 0 when nothing
    // has filled.
    uint32_t max_order_fill_count() const noexcept {
        uint32_t mx = 0;
        for (const auto& [id, o] : orders_)
            if (o.fill_count > mx) mx = o.fill_count;
        return mx;
    }
    // order_fill_rate: fraction of submitted orders that FULLY filled (#476)
    // = count_by_status(FILLED) / total_submitted. Completes the lifecycle
    // ratio family alongside cancel_rate (#258), replace_rate (#282),
    // submit_reject_rate (#212) — those measure how orders DIE, this
    // measures how many reach the goal. Per-ORDER (an order counts once no
    // matter its share count), unlike fill_ratio (#228, shares filled /
    // shares ordered); the two diverge when orders fill partially. The OMS
    // analog of the OUCH tracker's order_fill_rate (#361). 0 when nothing
    // submitted; partially-filled working orders do NOT count until FILLED.
    double order_fill_rate() const noexcept {
        return total_submitted_ > 0
            ? static_cast<double>(count_by_status(OrderStatus::FILLED))
                  / static_cast<double>(total_submitted_)
            : 0.0;
    }
    // cancel_rate: fraction of submitted orders that were cancelled (#258) =
    // total_cancels / total_submitted. A churn / quote-stuffing indicator: a high
    // ratio means most orders never rest long (exchange msg-rate fees, surveillance
    // flags). 0 when nothing submitted.
    double cancel_rate() const noexcept {
        return total_submitted_ > 0
            ? static_cast<double>(total_cancels_) / static_cast<double>(total_submitted_)
            : 0.0;
    }
    // replace_rate: fraction of submitted orders that were amended (#282) =
    // total_replaces / total_submitted. High = lots of repricing/resizing (chasing
    // the market, quote churn, message-rate cost). Completes the OMS ratio family
    // alongside cancel_rate (#258), submit_reject_rate (#212), fill_ratio (#228).
    double replace_rate() const noexcept {
        return total_submitted_ > 0
            ? static_cast<double>(total_replaces_) / static_cast<double>(total_submitted_)
            : 0.0;
    }
    uint64_t total_filled_shares()  const noexcept { return total_filled_shares_; }
    // total_traded_notional: cumulative $ value of every fill (Σ fill_qty *
    // fill_price) (#266) — session turnover. Basis for commission/turnover
    // analysis and exchange-tier volume tracking; independent of position (a
    // round-trip trades twice the notional but nets zero position).
    double   total_traded_notional() const noexcept { return total_traded_notional_; }
    // total_submitted_notional: Σ (price × qty) across all accepted submits (#322) —
    // the gross dollar volume the desk ATTEMPTED to trade, whether filled or not.
    // Compare to total_traded_notional (#266) to measure execution completeness in $.
    double   total_submitted_notional() const noexcept { return total_submitted_notional_; }
    // avg_submitted_notional: total_submitted_notional / total_submitted (#322) — the
    // average order size in dollars. Rising avg signals increasing block-trade activity
    // vs. small-lot order routing; falling avg suggests fragmentation.
    double   avg_submitted_notional() const noexcept {
        return total_submitted_ > 0
            ? total_submitted_notional_ / static_cast<double>(total_submitted_)
            : 0.0;
    }
    // avg_trade_price: blended VWAP across EVERY fill (#306) = total_traded_notional
    // / total_filled_shares. The single execution price the whole session achieved,
    // independent of side or position — the TCA benchmark you compare arrival/VWAP
    // slippage against. 0 when nothing filled.
    double   avg_trade_price() const noexcept {
        return total_filled_shares_ > 0
            ? total_traded_notional_ / static_cast<double>(total_filled_shares_)
            : 0.0;
    }
    // fill_ratio: what fraction of ORDERED volume (shares) was executed (#228) =
    // filled / ordered. Execution quality: low = many unfilled / cancelled orders
    // (bad limit prices, poor liquidity). 0 when nothing was ordered.
    double fill_ratio() const noexcept {
        return total_ordered_shares_
            ? static_cast<double>(total_filled_shares_) / static_cast<double>(total_ordered_shares_)
            : 0.0;
    }

    // reset_session_counters: zero the lifecycle counters (submitted/fills/cancels/
    // replaces) AND per-reason rejections for a new session (#204). Does NOT touch
    // positions or active orders — only observability stats (analogous to router
    // reset_session_stats #192). Call once at the open of the day.
    void reset_session_counters() noexcept {
        total_submitted_ = 0;
        total_fills_     = 0;
        total_cancels_   = 0;
        total_replaces_  = 0;
        total_ordered_shares_ = 0;   // #228
        total_filled_shares_  = 0;
        total_traded_notional_    = 0.0;   // #266
        total_submitted_notional_ = 0.0;   // #322
        price_improvement_fp_     = 0;     // #396
        round_trips_              = 0;     // #420
        for (auto& c : reject_counts_) c = 0;
    }

    void print_orders() const {
        printf("\n=== ORDERS ===\n");
        for (const auto& [id, o] : orders_) {
            printf("  #%lu: %s %u %s @ %.2f [%s] filled=%u\n",
                   (unsigned long)id, side_str(o.side), o.quantity,
                   o.symbol, to_float(o.price), status_str(o.status),
                   o.filled_qty);
        }
    }

    void print_positions() const {
        printf("\n=== POSITIONS ===\n");
        for (const auto& [sym, pos] : positions_) {
            (void)sym;
            printf("  %s: qty=%d avg=%.2f realized_pnl=$%.2f\n",
                   pos.symbol, pos.net_qty, to_float(pos.avg_price),
                   to_float(pos.realized_pnl));
        }
    }

private:
    // signed_qty: convert (side, qty) to a signed quantity. BUY → +qty, SELL → -qty.
    // Used 3× in the class (submit, fill, cancel) — extracted to avoid repetition.
    static int32_t signed_qty(Side side, uint32_t qty) noexcept {
        return (side == Side::BUY) ? static_cast<int32_t>(qty) : -static_cast<int32_t>(qty);
    }

    // apply_fill_to_position: one symmetric accounting model for long,
    // short and FLIP (long↔short crossing in a single fill). avg_price holds
    // the average ENTRY price of the current leg (positive or negative); realized_pnl
    // is booked only on reduction/close.
    //
    //   - open / increase the same side → weighted average entry
    //   - reduce / close → realize: a long closed by a sell = (sell-avg),
    //     a short closed by a buy = (avg-buy), on min(|net|, fill) shares
    //   - flip: the remainder after closing opens a new leg at the fill price
    //
    // Everything fixed-point (×PRICE_SCALE); realized_pnl in dollars×PRICE_SCALE.
    // total_cost is kept as the basis of the current leg (= avg×|net|) — a field
    // exposed by the Python binding, so it must stay consistent.
    static void apply_fill_to_position(Position& pos, int32_t signed_fill,
                                       int64_t fill_qty, int64_t fill_price) noexcept {
        const int32_t old_net = pos.net_qty;
        const int32_t new_net = old_net + signed_fill;

        const bool same_dir = (old_net == 0) || ((old_net > 0) == (signed_fill > 0));
        if (same_dir) {
            // Weighted average entry on the side of the leg being increased.
            const int64_t old_abs = std::abs(old_net);
            const int64_t tot_abs = old_abs + fill_qty;
            pos.avg_price = (pos.avg_price * old_abs + fill_price * fill_qty + tot_abs / 2) / tot_abs;
            pos.net_qty   = new_net;
        } else {
            // Reduce/close/flip — realize on the part being closed.
            const int64_t close_qty = std::min<int64_t>(std::abs(old_net), fill_qty);
            if (old_net > 0) pos.realized_pnl += close_qty * (fill_price - pos.avg_price);
            else             pos.realized_pnl += close_qty * (pos.avg_price - fill_price);
            pos.net_qty = new_net;
            if (new_net == 0) {
                pos.avg_price = 0;                       // flat — clean average
            } else if ((old_net > 0) != (new_net > 0)) {
                pos.avg_price = fill_price;              // flip — new leg at the fill price
            }
            // otherwise: same side, just reduced → avg_price unchanged
        }
        pos.total_cost = pos.avg_price * std::abs(pos.net_qty);
    }
};
