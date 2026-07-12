/*
 * OUCHOrderTracker — a client-side OUCH order state machine (expansion #89).
 *
 * OUCHMessage encode/decode handles SINGLE messages; nobody tracked
 * the order LIFECYCLE on the client side (token -> live/partial/filled/
 * cancelled/rejected + how many shares are still open). This is the missing piece: after
 * sending an Enter Order the client must know what happens to it, when
 * Accepted/Executed/Cancelled reports arrive.
 *
 * Cycle: NEW (sent) -> LIVE (Accepted) -> [PARTIAL after a partial Executed] ->
 *       FILLED (remaining=0) | CANCELLED (Cancelled) | REJECTED (Error)
 *
 * Feed on_new() at send time, on_response() with each parsed OUCHResponse.
 */
#pragma once

#include "ouch_protocol.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ouch {

enum class OrderState : uint8_t {
    NEW       = 0,   // sent, awaiting Accepted
    LIVE      = 1,   // Accepted — order active in the book
    PARTIAL   = 2,   // partially filled, remainder active
    FILLED    = 3,   // fully filled
    CANCELLED = 4,   // cancelled
    REJECTED  = 5,   // rejected (Error)
};

inline const char* order_state_str(OrderState s) noexcept {
    switch (s) {
        case OrderState::NEW:       return "NEW";
        case OrderState::LIVE:      return "LIVE";
        case OrderState::PARTIAL:   return "PARTIAL";
        case OrderState::FILLED:    return "FILLED";
        case OrderState::CANCELLED: return "CANCELLED";
        case OrderState::REJECTED:  return "REJECTED";
        default:                    return "UNKNOWN";
    }
}

class OUCHOrderTracker {
    struct Record {
        OrderState state;
        int32_t    original;     // original quantity
        int32_t    remaining;    // how many still open
        int32_t    filled;       // how many filled
        int64_t    order_ref;    // from Accepted (0 until known)
        bool       pending_cancel; // Cancel sent, awaiting confirmation (#159)
        double     fill_notional = 0.0; // Σ exec shares × price, $ (#410)
        char       side = ' ';     // 'B'/'S' from Accepted; ' ' until acked (#450)
        double     price = 0.0;    // working limit price from Accepted/Replaced/Restated (#546)
    };
    std::unordered_map<std::string, Record> orders_;
    uint64_t live_ = 0, filled_ = 0, cancelled_ = 0, rejected_ = 0, broken_ = 0;
    int64_t  ordered_shares_ = 0;   // cumulative shares ever ordered (#250)
    int64_t  broken_shares_  = 0;   // total shares unwound by Broken Trade messages (#320)
    uint64_t exec_count_     = 0;   // number of Executed ('E') reports applied (#328)
    int64_t  exec_shares_    = 0;   // cumulative executed shares, as reported (#328)
    int64_t  max_exec_shares_ = 0;  // largest single Executed report (clamped) (#530)
    int64_t  cancelled_shares_ = 0; // shares freed by CANCELLED remainders + AIQ decrements (#538)
    uint64_t cancel_rejects_ = 0;   // Cancel Reject ('I') reports applied (#378)
    uint64_t replaced_       = 0;   // Order Replaced ('U') migrations applied (#386)
    uint64_t desyncs_        = 0;   // responses naming an UNKNOWN token (#426)
    double   session_fill_notional_ = 0.0;  // Σ exec shares × price across all orders (#410)
    int64_t  bought_shares_ = 0;   // executed shares on BUY orders (#458)
    int64_t  sold_shares_   = 0;   // executed shares on SELL orders (#458)
    double   buy_notional_  = 0.0; // Σ exec shares × price on BUY orders (#474)
    double   sell_notional_ = 0.0; // Σ exec shares × price on SELL orders (#474)

    Record* find(const char* token) noexcept {
        auto it = orders_.find(token);
        return (it != orders_.end()) ? &it->second : nullptr;
    }

public:
    // on_new: register a sent Enter Order (token + ordered quantity).
    void on_new(const char* token, int32_t shares) noexcept {
        orders_[token] = Record{OrderState::NEW, shares, shares, 0, 0, false};
        ordered_shares_ += shares;   // #250
    }

    // on_cancel_sent: the client sent a Cancel Order ('X') — mark pending cancel
    // (#159). The 'C' (Cancelled) confirmation clears the flag. Only for active ones.
    void on_cancel_sent(const char* token) noexcept {
        Record* rec = find(token);
        if (rec && (rec->state == OrderState::LIVE || rec->state == OrderState::PARTIAL))
            rec->pending_cancel = true;
    }
    bool is_pending_cancel(const char* token) const noexcept {
        const auto it = orders_.find(token);
        return (it != orders_.end()) && it->second.pending_cancel;
    }

    // on_response: apply an exchange report. Returns the new state (or REJECTED when
    // the report concerns an unknown token — desync).
    OrderState on_response(const OUCHResponse& r) noexcept {
        // Order Replaced ('U') is keyed by the PREVIOUS token, not r.token —
        // the generic lookup below would treat the (still unknown) new token
        // as a desync. Migrate the record to the replacement token (#386):
        // fills stay with the chain, remaining becomes the replacement size,
        // priority-sensitive flags reset (a replace re-queues the order).
        if (std::strcmp(r.type, "REPLACED") == 0) {
            auto pit = orders_.find(r.prev_token);
            if (pit == orders_.end()) { ++rejected_; ++desyncs_; return OrderState::REJECTED; }
            Record moved = pit->second;
            orders_.erase(pit);
            moved.remaining      = r.shares;
            moved.order_ref      = r.order_ref;
            moved.pending_cancel = false;
            moved.price          = r.price;   // #546: the replacement's new price
            moved.state = (moved.filled > 0) ? OrderState::PARTIAL : OrderState::LIVE;
            ++replaced_;
            return (orders_[r.token] = moved).state;
        }
        Record* rec = find(r.token);
        if (!rec) { ++rejected_; ++desyncs_; return OrderState::REJECTED; }

        if (std::strcmp(r.type, "ACCEPTED") == 0) {
            rec->state     = OrderState::LIVE;
            rec->order_ref = r.order_ref;
            rec->side      = (r.side[0] != '\0') ? r.side[0] : ' ';   // #450: "BUY"/"SELL" -> 'B'/'S'
            rec->price     = r.price;   // #546: the working limit price
            ++live_;
        } else if (std::strcmp(r.type, "EXECUTED") == 0) {
            // #554 (audit): an Executed can only apply to a WORKING order.
            // Once a record is terminal, a further 'E' is a duplicate or a
            // late/reordered message — before this guard it re-counted
            // ++filled_ on EVERY duplicate (remaining was already 0, so the
            // <=0 branch re-fired) and RESURRECTED a cancelled order as
            // FILLED, corrupting fills()/order_fill_rate/status_count.
            // The exchange never executes a dead order; never mutate one.
            if (rec->state == OrderState::FILLED || rec->state == OrderState::CANCELLED
                || rec->state == OrderState::REJECTED)
                return rec->state;
            const int32_t exec = (r.shares < rec->remaining) ? r.shares : rec->remaining;
            rec->filled    += exec;
            rec->remaining -= exec;
            if (exec > 0) {
                exec_shares_ += exec; ++exec_count_;   // #328 per-execution
                if (exec > max_exec_shares_) max_exec_shares_ = exec;   // #530 block-fill high-water
                // #410: price the fill — Executed carries the exec price.
                const double notional = r.price * static_cast<double>(exec);
                rec->fill_notional     += notional;
                session_fill_notional_ += notional;
                // #458: realized directional flow, split by the order's side
                // (captured from Accepted #450; ' ' counts on neither).
                if      (rec->side == 'B') { bought_shares_ += exec; buy_notional_  += notional; }
                else if (rec->side == 'S') { sold_shares_   += exec; sell_notional_ += notional; }
            }
            if (rec->remaining <= 0)   { rec->state = OrderState::FILLED; ++filled_; }
            else if (exec > 0)           rec->state = OrderState::PARTIAL;
            // #554: a zero-share 'E' on a LIVE order changes nothing — it must
            // not relabel the order PARTIAL when nothing has filled.
        } else if (std::strcmp(r.type, "CANCELLED") == 0) {
            cancelled_shares_  += rec->remaining;   // #538: the unfilled remainder freed
            rec->state          = OrderState::CANCELLED;
            rec->remaining      = 0;
            rec->pending_cancel = false;   // #159: request confirmed
            ++cancelled_;
        } else if (std::strcmp(r.type, "BROKEN") == 0) {
            // Broken Trade (#134): the exchange invalidates an earlier fill -> reverse
            // the accounting (shares return to "open"). State from FILLED -> LIVE/PARTIAL.
            const int32_t back = (r.shares < rec->filled) ? r.shares : rec->filled;
            // #410: the 'B' report carries NO price, so unwind the notional
            // at the order's AVERAGE fill price (the standard treatment).
            // Session_fill_notional_ stays: the global VWAP is the GROSS
            // as-executed tape; only the per-order average is bust-adjusted.
            if (back > 0 && rec->filled > 0) {
                rec->fill_notional -= rec->fill_notional
                                    * (static_cast<double>(back) / static_cast<double>(rec->filled));
            }
            rec->filled    -= back;
            rec->remaining += back;
            // #458/#474: a bust unwinds realized flow AND its notional on the
            // order's side, scaling the notional proportionally so the side
            // VWAP survives the bust (the #410 treatment, one level up).
            if (rec->side == 'B') {
                if (bought_shares_ > 0)
                    buy_notional_ -= buy_notional_ * (static_cast<double>(back) / static_cast<double>(bought_shares_));
                bought_shares_ -= back;
            } else if (rec->side == 'S') {
                if (sold_shares_ > 0)
                    sell_notional_ -= sell_notional_ * (static_cast<double>(back) / static_cast<double>(sold_shares_));
                sold_shares_ -= back;
            }
            rec->state = (rec->filled > 0) ? OrderState::PARTIAL : OrderState::LIVE;
            ++broken_;
            broken_shares_ += back;   // #320: accumulate unwound volume
        } else if (std::strcmp(r.type, "CXL_PEND") == 0) {
            // Cancel Pending (#378): the exchange ACKNOWLEDGED the cancel and
            // queued it (e.g. a cross is in progress) — the order is still
            // working, so only (re)arm the pending flag; nothing is cancelled
            // yet. Also covers the case where the ack arrives without a local
            // on_cancel_sent (e.g. a cancel issued from another session).
            rec->pending_cancel = true;
        } else if (std::strcmp(r.type, "CXL_REJECT") == 0) {
            // Cancel Reject (#378): the cancel ATTEMPT failed (too late /
            // already executing) — the ORDER itself is untouched and keeps
            // working, so disarm the pending flag and leave the state and the
            // rejected counter alone. Before #378 both cancel-lifecycle
            // reports fell into the final else and wrongly killed a live
            // order as REJECTED.
            rec->pending_cancel = false;
            ++cancel_rejects_;
        } else if (std::strcmp(r.type, "RESTATED") == 0) {
            // Restated (#386): the exchange changed the order WITHOUT a client
            // request (compliance reprice, display reduction). r.shares IS the
            // new open quantity — adopt it, the order keeps working.
            rec->remaining = r.shares;
            rec->price     = r.price;   // #546: a restate can also reprice
        } else if (std::strcmp(r.type, "AIQ_CXL") == 0) {
            // AIQ Canceled (#386): self-match prevention removed PART of the
            // order (r.shares = the decrement). Not an error: reduce the open
            // quantity; only a decrement to zero cancels the order.
            const int32_t dec = (r.shares < rec->remaining) ? r.shares : rec->remaining;
            cancelled_shares_ += dec;   // #538: SMP removed these shares from the book
            rec->remaining -= dec;
            if (rec->remaining == 0) {
                rec->state          = OrderState::CANCELLED;
                rec->pending_cancel = false;
                ++cancelled_;
            }
        } else {  // ERROR / UNKNOWN
            rec->state = OrderState::REJECTED;
            ++rejected_;
        }
        return rec->state;
    }

    // --- Queries ---
    OrderState state(const char* token) const noexcept {
        auto it = orders_.find(token);
        return (it != orders_.end()) ? it->second.state : OrderState::REJECTED;
    }
    int32_t remaining(const char* token) const noexcept {
        auto it = orders_.find(token);
        return (it != orders_.end()) ? it->second.remaining : 0;
    }
    int32_t filled(const char* token) const noexcept {
        auto it = orders_.find(token);
        return (it != orders_.end()) ? it->second.filled : 0;
    }
    // filled_fraction: completion of ONE order = filled / (filled + remaining)
    // (#288), in [0, 1]. Per-order progress (vs the portfolio total_filled/
    // remaining aggregates) — useful for tracking how far a large worked order has
    // got. 0 for an unknown token or a fully-broken order.
    double  filled_fraction(const char* token) const noexcept {
        const auto it = orders_.find(token);
        if (it == orders_.end()) return 0.0;
        const int32_t total = it->second.filled + it->second.remaining;
        return total > 0 ? static_cast<double>(it->second.filled) / static_cast<double>(total) : 0.0;
    }
    // avg_fill_price (#410): the order's volume-weighted average execution
    // price = fill_notional / filled. Bust-ADJUSTED: a Broken Trade carries
    // no price, so it unwinds the notional at the order's average (the
    // standard treatment) and the average survives unchanged. Travels with
    // the chain across a Replaced migration (#386). 0 for an unknown token
    // or nothing filled.
    double avg_fill_price(const char* token) const noexcept {
        const auto it = orders_.find(token);
        if (it == orders_.end() || it->second.filled <= 0) return 0.0;
        return it->second.fill_notional / static_cast<double>(it->second.filled);
    }
    // fill_vwap (#410): the session's GROSS as-executed VWAP across all
    // orders = Σ notional / exec_shares (#328). Busts do NOT adjust it —
    // this is the tape as it printed. The client-side counterpart of itch's
    // executed_vwap (#407) and OMS's blended avg_trade_price (#306).
    // 0 before any execution.
    double fill_vwap() const noexcept {
        return exec_shares_ > 0
            ? session_fill_notional_ / static_cast<double>(exec_shares_)
            : 0.0;
    }
    size_t   order_count() const noexcept { return orders_.size(); }
    // total_filled_shares / total_remaining_shares: volume aggregates over ALL
    // tracked orders (#242). filled = how much has executed in total, remaining =
    // how much is still working. A portfolio view of execution independent of a single
    // token (for sizing / backlog monitoring).
    int32_t total_filled_shares() const noexcept {
        int32_t s = 0;
        for (const auto& [tok, rec] : orders_) s += rec.filled;
        return s;
    }
    int32_t total_remaining_shares() const noexcept {
        int32_t s = 0;
        for (const auto& [tok, rec] : orders_) s += rec.remaining;
        return s;
    }
    // working_shares: open shares truly resting on the exchange RIGHT NOW (#304) —
    // sum of `remaining` over orders in LIVE or PARTIAL state only. Unlike
    // total_remaining_shares (#242), which also counts leftovers on NEW (sent, not
    // yet acked) and CANCELLED records, this is the confirmed live book exposure.
    int32_t working_shares() const noexcept {
        int32_t s = 0;
        for (const auto& [tok, rec] : orders_)
            if (rec.state == OrderState::LIVE || rec.state == OrderState::PARTIAL)
                s += rec.remaining;
        return s;
    }
    // largest_remaining: the biggest single WORKING order by remaining shares (#312),
    // over LIVE/PARTIAL orders. The largest concentration of open exposure in one
    // order — filling or cancelling it moves the book the most, so risk/monitoring
    // watches it. 0 when nothing is working. Complements working_shares (#304, total).
    int32_t largest_remaining() const noexcept {
        int32_t mx = 0;
        for (const auto& [tok, rec] : orders_)
            if ((rec.state == OrderState::LIVE || rec.state == OrderState::PARTIAL)
                && rec.remaining > mx)
                mx = rec.remaining;
        return mx;
    }
    // working_shares_side: the LIVE/PARTIAL open shares on ONE side (#450,
    // MILESTONE 450) — the tracker's first DIRECTION-aware read: everything
    // before this (working_shares #304, largest_remaining #312, pending
    // shares #402...) was direction-blind, yet a book of 500 working buys
    // is the opposite risk of 500 working sells. The side is captured from
    // the Accepted report ('B'/'S'); orders not yet acked carry ' ' and
    // count on neither side (their direction is not confirmed).
    int32_t working_shares_side(char side) const noexcept {
        int32_t s = 0;
        for (const auto& [tok, rec] : orders_)
            if ((rec.state == OrderState::LIVE || rec.state == OrderState::PARTIAL)
                && rec.side == side)
                s += rec.remaining;
        return s;
    }
    // working_notional (#546): the $ VALUE of the confirmed resting book =
    // Σ remaining × working limit price over LIVE/PARTIAL orders — the capital
    // committed to the exchange right now, the dollar face of working_shares
    // (#304) and the OUCH parity of OMS open_order_notional (#180). The price
    // is captured from the Accepted report and follows the order through
    // Replaced (#386 migration adopts the replacement's price) and Restated
    // (a compliance reprice moves it too), so the valuation tracks what is
    // actually on the book, not what was first sent. 500 shares of a $2000
    // name and of a $2 name read identically in shares; not here. 0 when
    // nothing is working.
    double working_notional() const noexcept {
        double s = 0.0;
        for (const auto& [tok, rec] : orders_)
            if (rec.state == OrderState::LIVE || rec.state == OrderState::PARTIAL)
                s += static_cast<double>(rec.remaining) * rec.price;
        return s;
    }
    // bought_shares / sold_shares (#458): EXECUTED shares by order side —
    // the REALIZED directional flow, the filled counterpart to #450's
    // WORKING (resting) split. Side comes from the Accepted report; busts
    // unwind the flow on the same side. net_filled_shares() is the signed
    // realized position delta this tracker has taken on: +N means N more
    // bought than sold. Together with net_working_shares (#450) it is the
    // full directional picture — what has traded plus what is still resting
    // to trade.
    int64_t bought_shares()    const noexcept { return bought_shares_; }
    int64_t sold_shares()      const noexcept { return sold_shares_; }
    int64_t net_filled_shares() const noexcept { return bought_shares_ - sold_shares_; }

    // avg_buy_price / avg_sell_price (#474): the realized VWAP of the BUY
    // and SELL fills separately = side notional / side shares. Where
    // fill_vwap (#410) blends both sides into one gross tape, this splits
    // them — a market maker's buy VWAP should sit BELOW its sell VWAP, and
    // the gap is the edge. Bust-adjusted (the side VWAP survives a Broken
    // Trade). 0 for a side with no fills.
    double avg_buy_price() const noexcept {
        return bought_shares_ > 0 ? buy_notional_ / static_cast<double>(bought_shares_) : 0.0;
    }
    double avg_sell_price() const noexcept {
        return sold_shares_ > 0 ? sell_notional_ / static_cast<double>(sold_shares_) : 0.0;
    }
    // realized_spread_capture (#474): avg_sell_price - avg_buy_price, the
    // per-share edge a two-sided maker actually captured this session
    // (positive = sold higher than bought on average). Only meaningful once
    // BOTH sides have fills; 0 otherwise. The tracker's headline
    // market-making P&L read.
    double realized_spread_capture() const noexcept {
        return (bought_shares_ > 0 && sold_shares_ > 0)
            ? avg_sell_price() - avg_buy_price() : 0.0;
    }

    // realized_spread_capture_bps (#506): the per-share edge (#474) as a
    // fraction of the buy VWAP, in basis points = (avg_sell - avg_buy) /
    // avg_buy * 10000. The $/share capture is not comparable across a $10
    // name and a $1000 name — 10 cents is a lot on one and nothing on the
    // other — so bps normalizes it to the price level, the units a desk
    // actually compares market-making performance in. Only meaningful once
    // both sides have fills and a positive buy VWAP; 0 otherwise.
    double realized_spread_capture_bps() const noexcept {
        const double bp = avg_buy_price();
        return (bought_shares_ > 0 && sold_shares_ > 0 && bp > 0.0)
            ? (avg_sell_price() - bp) / bp * 10000.0 : 0.0;
    }

    // gross_traded_notional (#482): the total $ that changed hands across
    // every execution = the session's turnover. Equal to fill_vwap (#410) x
    // exec_shares (#328); a plain $ accessor for the raw tape value.
    double gross_traded_notional() const noexcept { return session_fill_notional_; }

    // net_cash_flow (#482): sell_notional - buy_notional, the net cash the
    // desk has taken in from this tracker's fills — positive = a net seller
    // of value (more cash in from sells than out for buys). Composed from
    // the #474 side-notional accumulators. When the position has round-
    // tripped to FLAT (net_filled_shares == 0) this IS the realized cash
    // P&L; while the position is still open it is the cash half of the
    // cost basis (net_cash_flow + mark x net_filled_shares = mark-to-market
    // P&L, but the tracker has no mark, so it reports only the cash leg).
    // Bust-adjusted via the #474 proportional notional unwind.
    double net_cash_flow() const noexcept { return sell_notional_ - buy_notional_; }

    // mark_to_market_pnl (#490, MILESTONE 490): the tracker's total P&L at a
    // caller-supplied mark price = net_cash_flow + net_filled_shares * mark.
    // The cash already taken in (#482) plus the value of the still-open
    // inventory (#458) marked to `mark`: a long adds mark*shares, a short
    // subtracts it. When the position is FLAT this collapses to net_cash_
    // flow (the realized cash P&L, mark-independent); while it is open the
    // mark moves it (a long profits as the mark rises). The mark is injected
    // because the tracker is protocol-level and holds no market data — the
    // same pattern as OMS unrealized_pnl (#96). Bust-adjusted via the #474
    // notional unwind. This is the tracker's headline P&L read.
    double mark_to_market_pnl(double mark_price) const noexcept {
        return net_cash_flow()
             + static_cast<double>(net_filled_shares()) * mark_price;
    }

    // realized_pnl (#514): the P&L already BANKED on the round-tripped portion,
    // independent of any mark = matched_shares * (avg_sell_price - avg_buy_
    // price), where matched_shares = min(bought, sold). Where net_cash_flow
    // (#482) still carries the cost of the OPEN inventory and mark_to_market_
    // pnl (#490) needs a mark to value that inventory, this isolates the closed
    // round trips — the money locked in regardless of where the market goes
    // next. It is the $ total whose per-share edge is realized_spread_capture
    // (#474): realized_pnl == matched_shares * that edge. When the book is FLAT
    // (bought == sold) every share is matched and this equals net_cash_flow
    // exactly. 0 until both a buy and a sell have filled (nothing is round-
    // tripped yet). Bust-adjusted through the #474 side accumulators it reads.
    double realized_pnl() const noexcept {
        const int64_t matched = bought_shares_ < sold_shares_ ? bought_shares_ : sold_shares_;
        return matched > 0
            ? static_cast<double>(matched) * (avg_sell_price() - avg_buy_price())
            : 0.0;
    }

    // unrealized_pnl (#522): the P&L on the still-OPEN inventory at a caller-
    // supplied mark = mark_to_market_pnl(mark) - realized_pnl(). It completes
    // the P&L decomposition the tracker now carries: total (#490 mark-to-
    // market) = realized (#514, banked on the round-tripped shares) +
    // unrealized (the open position marked to `mark`). By construction it
    // equals net_filled_shares valued at the gap between `mark` and the open
    // lot's average cost — a long gains as the mark rises above its entry, a
    // short as it falls below. 0 when FLAT (every share matched, nothing open)
    // regardless of the mark: all the P&L is then realized. The mark is
    // injected because the tracker holds no market data, the same pattern as
    // mark_to_market_pnl (#490) and OMS unrealized_pnl (#96).
    double unrealized_pnl(double mark_price) const noexcept {
        return mark_to_market_pnl(mark_price) - realized_pnl();
    }

    // breakeven_mark (#498): the mark price at which mark_to_market_pnl
    // (#490) is exactly zero = -net_cash_flow / net_filled_shares. Solving
    // net_cash_flow + net_shares*mark == 0, this is how far the market must
    // travel for the position to wash out to flat P&L: a long that has
    // already captured some spread has a breakeven BELOW its entry (it can
    // fall a bit and still be even), a short one ABOVE. The risk read that
    // says how much room the position has before it turns red. 0 when FLAT
    // (net_filled_shares == 0) — the P&L is then mark-independent
    // (net_cash_flow) and no breakeven price exists.
    double breakeven_mark() const noexcept {
        const int64_t net = net_filled_shares();
        return net != 0 ? -net_cash_flow() / static_cast<double>(net) : 0.0;
    }

    // net_working_shares: buy-side minus sell-side working shares (#450) —
    // the SIGNED directional tilt of the resting book. 0 is a balanced
    // (market-making) book; a large positive number means the working
    // orders themselves are a long bet before a single fill arrives.
    int32_t net_working_shares() const noexcept {
        return working_shares_side('B') - working_shares_side('S');
    }

    // projected_net_shares: the signed position this tracker would hold if
    // every working order fully filled at its side (#466) =
    // net_filled_shares (#458, realized) + net_working_shares (#450,
    // potential). The pre-trade exposure projection: realized position plus
    // what the resting book would add if it all executes. A desk sizing the
    // next order reads this, not just the realized net, to avoid stacking a
    // long on top of resting buys that are about to fill. Working orders
    // whose side is unconfirmed (unacked, ' ') contribute to neither leg.
    int32_t projected_net_shares() const noexcept {
        return static_cast<int32_t>(net_filled_shares()) + net_working_shares();
    }

    // largest_remaining_token: the TOKEN carrying largest_remaining's biggest
    // working exposure (#394) — the actionable WHICH: that is the order to
    // chase, reprice or pull first. Same LIVE/PARTIAL walk as #312, so the
    // returned value always equals it. Writes the token into out (>= 15
    // bytes, always nul-terminated) and returns the remaining shares;
    // 0 and out[0] == '\0' when nothing is working.
    int32_t largest_remaining_token(char* out) const noexcept {
        int32_t mx = 0;
        const std::string* who = nullptr;
        for (const auto& [tok, rec] : orders_)
            if ((rec.state == OrderState::LIVE || rec.state == OrderState::PARTIAL)
                && rec.remaining > mx) {
                mx  = rec.remaining;
                who = &tok;
            }
        if (who != nullptr) {
            std::strncpy(out, who->c_str(), 14);
            out[14] = '\0';
        } else {
            out[0] = '\0';
        }
        return mx;
    }
    // avg_working_shares: the MEAN remaining size of a working (LIVE/PARTIAL)
    // order (#369) = working_shares / (# working orders). Completes the working-
    // exposure trio with working_shares (#304, total) and largest_remaining
    // (#312, max): the mean vs total vs max distinguishes "one big resting order"
    // from "many small clips" even when working_shares is identical. 0 when
    // nothing is working.
    double avg_working_shares() const noexcept {
        int32_t sum = 0; size_t n = 0;
        for (const auto& [tok, rec] : orders_)
            if (rec.state == OrderState::LIVE || rec.state == OrderState::PARTIAL) {
                sum += rec.remaining; ++n;
            }
        return n > 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
    }
    // total_ordered_shares / fill_rate: cumulative shares ordered (sum of on_new)
    // and executed/ordered ratio (#250) — client-side execution quality. Low =
    // many orders went unfilled (bad limit prices, thin liquidity). 0 when nothing
    // was ordered.
    int64_t total_ordered_shares() const noexcept { return ordered_shares_; }
    // active_count: number of orders still WORKING — non-terminal state (NEW/LIVE/
    // PARTIAL), i.e. not FILLED/CANCELLED/REJECTED (#272). Unlike order_count (every
    // order ever seen) or the event counters, this is the live open-order count the
    // desk is actually exposed to right now.
    size_t active_count() const noexcept {
        size_t c = 0;
        for (const auto& [tok, rec] : orders_)
            if (rec.state != OrderState::FILLED && rec.state != OrderState::CANCELLED
                && rec.state != OrderState::REJECTED) ++c;
        return c;
    }
    // cancel_pending_count: orders with a cancel SENT but not yet confirmed (#280) —
    // the pending_cancel flag (#159), set by on_cancel_sent and cleared by a
    // Cancelled ('C') response. These are in-flight cancels the desk is waiting on;
    // a growing count signals the exchange is slow to ack or cancels are being lost.
    size_t cancel_pending_count() const noexcept {
        size_t c = 0;
        for (const auto& [tok, rec] : orders_) if (rec.pending_cancel) ++c;
        return c;
    }
    // pending_cancel_shares: OPEN shares sitting under an in-flight cancel
    // (#402) — the exposure the desk EXPECTS to free once the exchange acks.
    // The shares axis to cancel_pending_count's (#280) order count: ten
    // 100-lot pending cancels and one 1000-lot read identically on the
    // count but very differently here. Sums `remaining` over pending-cancel
    // records (LIVE/PARTIAL by construction — on_cancel_sent only arms
    // working orders, and every terminal transition clears the flag).
    int32_t pending_cancel_shares() const noexcept {
        int32_t s = 0;
        for (const auto& [tok, rec] : orders_)
            if (rec.pending_cancel) s += rec.remaining;
        return s;
    }
    // pending_cancel_fraction: what part of the live book is already
    // condemned (#402) = pending_cancel_shares / working_shares, in [0,1].
    // Near 1.0 the resting book is an illusion — nearly everything on the
    // exchange is a cancel waiting to be acked. 0 when nothing is working.
    double pending_cancel_fraction() const noexcept {
        const int32_t w = working_shares();
        return w > 0
            ? static_cast<double>(pending_cancel_shares()) / static_cast<double>(w)
            : 0.0;
    }
    // status_count: how many tracked orders are CURRENTLY in a given state (#296).
    // A point-in-time snapshot (vs the cumulative live_/filled_/... event counters,
    // which only ever go up). Mirrors the OMS count_by_status (#290 family) — e.g.
    // status_count(LIVE) is the live resting book right now.
    size_t status_count(OrderState st) const noexcept {
        size_t c = 0;
        for (const auto& [tok, rec] : orders_) if (rec.state == st) ++c;
        return c;
    }
    double  fill_rate() const noexcept {
        return ordered_shares_ > 0
            ? static_cast<double>(total_filled_shares()) / static_cast<double>(ordered_shares_)
            : 0.0;
    }
    uint64_t live()        const noexcept { return live_; }
    uint64_t fills()       const noexcept { return filled_; }
    uint64_t cancels()     const noexcept { return cancelled_; }
    uint64_t rejects()     const noexcept { return rejected_; }
    uint64_t brokens()     const noexcept { return broken_; }
    // cancel_rejects: how many Cancel Reject ('I') reports were applied (#378).
    // Distinct from rejects(): the ORDER survives a cancel reject.
    uint64_t cancel_rejects() const noexcept { return cancel_rejects_; }
    // replaces: how many Order Replaced ('U') token migrations were applied (#386).
    uint64_t replaces() const noexcept { return replaced_; }
    // reset_session: wipe the tracker for a new trading day (#442) — clears
    // the order map (tokens are per-session on NASDAQ; yesterday's tokens
    // colliding with today's would silently corrupt the accounting) and
    // zeroes every counter. The lifecycle parity of OMS
    // reset_session_counters (#204) and risk reset_daily — the tracker was
    // the last stateful component without one, its map growing unbounded
    // across days. Call at the session open, after any end-of-day report.
    void reset_session() noexcept {
        orders_.clear();
        live_ = filled_ = cancelled_ = rejected_ = broken_ = 0;
        ordered_shares_ = 0;
        broken_shares_  = 0;
        exec_count_     = 0;
        exec_shares_    = 0;
        max_exec_shares_ = 0;   // #530
        cancelled_shares_ = 0;  // #538
        cancel_rejects_ = 0;
        replaced_       = 0;
        desyncs_        = 0;
        session_fill_notional_ = 0.0;
        bought_shares_ = 0;
        sold_shares_   = 0;
        buy_notional_  = 0.0;
        sell_notional_ = 0.0;
    }

    // desyncs: responses that named a token the tracker never registered
    // (#426) — including a REPLACED whose PREVIOUS token is unknown. These
    // have always been folded into rejects(), which conflates two very
    // different alarms: the exchange refusing an ORDER (a business event)
    // vs the tracker having LOST STATE (missed messages, a session restart
    // without replay — an ops event). rejects() keeps its historical
    // meaning; desyncs() splits the ops share out: rejects() - desyncs()
    // = true order rejections.
    uint64_t desyncs() const noexcept { return desyncs_; }
    // total_broken_shares: cumulative shares returned to "open" by Broken Trade
    // messages (#320). The exchange rescinds a prior fill — shares move back from
    // filled to remaining. Watching this versus total_filled_shares reveals how much
    // of the executed volume was later invalidated. Distinct from broken_ (event count).
    int64_t total_broken_shares() const noexcept { return broken_shares_; }
    // broken_share_rate: total_broken_shares / ordered_shares (#320). Proportion of
    // ordered volume that was filled and subsequently broken — a feed-quality signal.
    // 0 when nothing has been ordered.
    double broken_share_rate() const noexcept {
        return ordered_shares_ > 0
            ? static_cast<double>(broken_shares_) / static_cast<double>(ordered_shares_)
            : 0.0;
    }
    // total_cancelled_shares: cumulative shares removed from the book by
    // cancels (#538) — the unfilled remainder freed by each CANCELLED report
    // plus every AIQ self-match-prevention decrement. The share-weighted face
    // of the cancel outcome: cancelled_ / cancel_rate (#353) count ORDERS, so a
    // 10-share cancel and a 10,000-share cancel weigh the same there — this is
    // the liquidity actually pulled back, the cancel-side parity of
    // total_filled_shares (#242, taken) and total_broken_shares (#320,
    // unwound). 0 before any cancel; reset by reset_session.
    int64_t total_cancelled_shares() const noexcept { return cancelled_shares_; }
    // cancelled_share_rate: total_cancelled_shares / ordered_shares (#538) —
    // the fraction of everything the desk ORDERED that it later pulled back,
    // in [0,1]. The share-weighted sibling of cancel_rate (#353, per-order)
    // and the mirror of fill_rate (#250, the fraction the market TOOK): the
    // two split the decided volume between taken and withdrawn. High = quote
    // churn in size, not just in message count. 0 when nothing was ordered.
    double cancelled_share_rate() const noexcept {
        return ordered_shares_ > 0
            ? static_cast<double>(cancelled_shares_) / static_cast<double>(ordered_shares_)
            : 0.0;
    }
    // reject_rate: fraction of tracked ORDERS that ended up REJECTED (#345) =
    // rejected_ / order_count. Distinct from fill_rate (#250, a SHARE ratio):
    // this is a per-order rate — e.g. many small rejected clips vs one large
    // rejected order weigh the same here, whereas fill_rate would weigh them by
    // size. A rising reject_rate against a flat fill_rate points at bad order
    // parameters (stale prices, size limits) rather than thin liquidity.
    // 0 when nothing has been tracked yet.
    double reject_rate() const noexcept {
        return order_count() > 0
            ? static_cast<double>(rejected_) / static_cast<double>(order_count())
            : 0.0;
    }
    // cancel_rate: fraction of tracked ORDERS that ended up CANCELLED (#353) =
    // cancelled_ / order_count. Same per-order shape as reject_rate (#345), but
    // for the other terminal non-fill outcome. Together reject_rate + cancel_rate
    // + fill_rate (share-weighted, not order-weighted, so they don't have to sum
    // to 1) sketch WHY orders didn't complete: rejected at entry vs pulled by the
    // client vs simply unfilled. 0 when nothing has been tracked yet.
    double cancel_rate() const noexcept {
        return order_count() > 0
            ? static_cast<double>(cancelled_) / static_cast<double>(order_count())
            : 0.0;
    }
    // order_fill_rate: fraction of tracked ORDERS that reached FILLED = filled_ /
    // order_count (#361). Completes the per-ORDER terminal-outcome trio with
    // reject_rate (#345) and cancel_rate (#353) — all three weight each order
    // equally. Distinct from fill_rate (#250), which is SHARE-weighted (filled
    // shares / ordered shares): a book of many small fully-filled orders next to
    // one huge half-filled one has a high order_fill_rate but a lower fill_rate.
    // 0 when nothing has been tracked yet.
    double order_fill_rate() const noexcept {
        return order_count() > 0
            ? static_cast<double>(filled_) / static_cast<double>(order_count())
            : 0.0;
    }
    // exec_count / avg_exec_shares: number of Executed ('E') reports applied and the
    // average shares per execution (#328). Wire-level execution granularity from the
    // OUCH feed: a small average = the order is being worked in many slices (iceberg /
    // child orders), a large average = block fills. Distinct from fill_rate (#250, a
    // ratio) and total_filled_shares (#242, net of Broken Trades) — this counts
    // execution EVENTS as they arrived, unaffected by later breaks. 0 before any fill.
    uint64_t exec_count() const noexcept { return exec_count_; }
    double   avg_exec_shares() const noexcept {
        return exec_count_ > 0
            ? static_cast<double>(exec_shares_) / static_cast<double>(exec_count_)
            : 0.0;
    }
    // largest_execution (#530): the biggest single Executed report this session,
    // in shares — the block-fill detector, the MAX companion to avg_exec_shares
    // (#328, the mean per execution). Their ratio (largest / avg) is block
    // dominance: one big print among many small child-order clips reads high,
    // an evenly sliced iceberg near 1. The OUCH client-side analog of itch's
    // largest_trade_size (#503) on the public tape. Counts the CLAMPED applied
    // shares (an over-execute past the remaining is trimmed, matching #328), so
    // it never exceeds the order it filled. 0 before any fill; reset by
    // reset_session.
    int64_t largest_execution() const noexcept { return max_exec_shares_; }
    // avg_order_size: mean ordered quantity per tracked order = total_ordered_shares
    // / order_count (#337). The sizing companion to avg_exec_shares (#328, mean
    // shares per execution): together they show how finely the venue slices orders
    // (a large average order filled by a small average execution = heavy
    // fragmentation). 0 when no orders are tracked.
    double avg_order_size() const noexcept {
        const size_t n = orders_.size();
        return n > 0 ? static_cast<double>(ordered_shares_) / static_cast<double>(n) : 0.0;
    }
    // executions_per_order: mean number of Executed reports per tracked order =
    // exec_count / order_count (#337). A direct fill-fragmentation measure: ~1 =
    // clean block fills, high = orders worked in many small slices (iceberg /
    // child orders) — more wire traffic and more signaling/market-impact risk.
    // Distinct from avg_exec_shares (#328, shares per execution): this counts
    // execution EVENTS per order. 0 when no orders are tracked.
    double executions_per_order() const noexcept {
        const size_t n = orders_.size();
        return n > 0 ? static_cast<double>(exec_count_) / static_cast<double>(n) : 0.0;
    }
};

}  // namespace ouch
