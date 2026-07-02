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
    };
    std::unordered_map<std::string, Record> orders_;
    uint64_t live_ = 0, filled_ = 0, cancelled_ = 0, rejected_ = 0, broken_ = 0;
    int64_t  ordered_shares_ = 0;   // cumulative shares ever ordered (#250)
    int64_t  broken_shares_  = 0;   // total shares unwound by Broken Trade messages (#320)
    uint64_t exec_count_     = 0;   // number of Executed ('E') reports applied (#328)
    int64_t  exec_shares_    = 0;   // cumulative executed shares, as reported (#328)
    uint64_t cancel_rejects_ = 0;   // Cancel Reject ('I') reports applied (#378)
    uint64_t replaced_       = 0;   // Order Replaced ('U') migrations applied (#386)

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
            if (pit == orders_.end()) { ++rejected_; return OrderState::REJECTED; }
            Record moved = pit->second;
            orders_.erase(pit);
            moved.remaining      = r.shares;
            moved.order_ref      = r.order_ref;
            moved.pending_cancel = false;
            moved.state = (moved.filled > 0) ? OrderState::PARTIAL : OrderState::LIVE;
            ++replaced_;
            return (orders_[r.token] = moved).state;
        }
        Record* rec = find(r.token);
        if (!rec) { ++rejected_; return OrderState::REJECTED; }

        if (std::strcmp(r.type, "ACCEPTED") == 0) {
            rec->state     = OrderState::LIVE;
            rec->order_ref = r.order_ref;
            ++live_;
        } else if (std::strcmp(r.type, "EXECUTED") == 0) {
            const int32_t exec = (r.shares < rec->remaining) ? r.shares : rec->remaining;
            rec->filled    += exec;
            rec->remaining -= exec;
            if (exec > 0) { exec_shares_ += exec; ++exec_count_; }   // #328 per-execution
            if (rec->remaining <= 0) { rec->state = OrderState::FILLED; ++filled_; }
            else                       rec->state = OrderState::PARTIAL;
        } else if (std::strcmp(r.type, "CANCELLED") == 0) {
            rec->state          = OrderState::CANCELLED;
            rec->remaining      = 0;
            rec->pending_cancel = false;   // #159: request confirmed
            ++cancelled_;
        } else if (std::strcmp(r.type, "BROKEN") == 0) {
            // Broken Trade (#134): the exchange invalidates an earlier fill -> reverse
            // the accounting (shares return to "open"). State from FILLED -> LIVE/PARTIAL.
            const int32_t back = (r.shares < rec->filled) ? r.shares : rec->filled;
            rec->filled    -= back;
            rec->remaining += back;
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
        } else if (std::strcmp(r.type, "AIQ_CXL") == 0) {
            // AIQ Canceled (#386): self-match prevention removed PART of the
            // order (r.shares = the decrement). Not an error: reduce the open
            // quantity; only a decrement to zero cancels the order.
            const int32_t dec = (r.shares < rec->remaining) ? r.shares : rec->remaining;
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
