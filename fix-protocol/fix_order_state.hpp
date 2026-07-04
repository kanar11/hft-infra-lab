/*
 * FIXOrderTracker — a client-side FIX order state machine (expansion #111).
 *
 * Symmetric to OUCHOrderTracker (#89): after sending a NewOrderSingle (35=D)
 * the exchange sends back ExecutionReports (35=8) with OrdStatus (39), CumQty (14),
 * LeavesQty (151). The tracker follows the lifecycle per ClOrdID (tag 11): whether the order
 * is New/PartiallyFilled/Filled/Canceled/Rejected and how much is already filled.
 *
 *   on_new(cl_ord_id, qty)       — register a sent order
 *   on_exec_report(FIXMessage)   — apply a 35=8 report (after m.parse())
 *
 * OrdStatus (tag 39): 0=New, 1=PartiallyFilled, 2=Filled, 4=Canceled, 8=Rejected.
 */
#pragma once

#include "fix_parser.hpp"   // FIXMessage

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace fix {

enum class OrdState : uint8_t {
    NEW = 0, PARTIAL = 1, FILLED = 2, CANCELED = 3, REJECTED = 4, UNKNOWN = 5
};

inline const char* ord_state_str(OrdState s) noexcept {
    switch (s) {
        case OrdState::NEW:      return "NEW";
        case OrdState::PARTIAL:  return "PARTIAL";
        case OrdState::FILLED:   return "FILLED";
        case OrdState::CANCELED: return "CANCELED";
        case OrdState::REJECTED: return "REJECTED";
        default:                 return "UNKNOWN";
    }
}

class FIXOrderTracker {
    struct Record {
        OrdState state;
        int32_t  ordered;    // original quantity
        int32_t  cum_qty;    // cumulative fill (tag 14)
        int32_t  leaves_qty; // remaining (tag 151)
        bool     pending_cancel = false;  // OrderCancelRequest in flight (#393)
    };
    std::unordered_map<std::string, Record> orders_;
    uint64_t fills_ = 0, cancels_ = 0, rejects_ = 0;
    uint64_t cancel_rejects_ = 0;   // OrderCancelReject (35=9) applied (#393)
    uint64_t replaces_       = 0;   // OrdStatus=5 ClOrdID migrations applied (#401)

    static OrdState map_status(char s) noexcept {
        switch (s) {
            case '0': return OrdState::NEW;
            case '1': return OrdState::PARTIAL;
            case '2': return OrdState::FILLED;
            case '4': return OrdState::CANCELED;
            case '8': return OrdState::REJECTED;
            default:  return OrdState::UNKNOWN;
        }
    }

public:
    void on_new(const char* cl_ord_id, int32_t qty) {
        orders_[cl_ord_id] = Record{OrdState::NEW, qty, 0, qty};
    }

    // on_exec_report: apply a parsed ExecutionReport (35=8). Returns the new
    // state (UNKNOWN when there is no ClOrdID or an unknown order — desync).
    OrdState on_exec_report(const FIXMessage& m) {
        const char* id = m.get_field(11);            // ClOrdID
        if (!id) return OrdState::UNKNOWN;
        // OrdStatus=5 (Replaced, #401): the report carries the NEW ClOrdID in
        // tag 11 — which the tracker has never registered — and names the
        // working order via tag 41. The generic lookup below would call it a
        // desync and leave a stale record under the old id. Migrate instead
        // (the FIX mirror of the OUCH tracker's token migration #386): fills
        // stay with the chain, 14/151 refresh the quantities, the pending-
        // cancel flag resets (a replace re-queues the order).
        const char* st39 = m.get_field(39);
        if (st39 && st39[0] == '5') {
            const char* orig = m.get_field(41);
            if (!orig) return OrdState::UNKNOWN;
            auto pit = orders_.find(orig);
            if (pit == orders_.end()) return OrdState::UNKNOWN;
            Record moved = pit->second;
            orders_.erase(pit);
            if (const char* cum = m.get_field(14))  moved.cum_qty    = std::atoi(cum);
            if (const char* lv  = m.get_field(151)) moved.leaves_qty = std::atoi(lv);
            moved.pending_cancel = false;
            moved.state = (moved.cum_qty > 0) ? OrdState::PARTIAL : OrdState::NEW;
            ++replaces_;
            return (orders_[id] = moved).state;
        }
        const auto it = orders_.find(id);
        if (it == orders_.end()) return OrdState::UNKNOWN;
        Record& r = it->second;

        if (const char* cum = m.get_field(14))  r.cum_qty    = std::atoi(cum);
        if (const char* lv  = m.get_field(151)) r.leaves_qty = std::atoi(lv);
        const char* st = m.get_field(39);            // OrdStatus
        r.state = st ? map_status(st[0]) : OrdState::UNKNOWN;

        if (r.state == OrdState::FILLED)        ++fills_;
        else if (r.state == OrdState::CANCELED) { ++cancels_; r.pending_cancel = false; }  // #393: confirmed
        else if (r.state == OrdState::REJECTED) ++rejects_;
        return r.state;
    }

    // on_cancel_sent: the client sent an OrderCancelRequest (35=F) — mark the
    // WORKING order pending-cancel (#393; mirrors OUCH #159). Only orders
    // still working (NEW/PARTIAL) can be pending; the 35=8 with OrdStatus=4
    // confirms and clears, on_cancel_reject disarms without cancelling.
    void on_cancel_sent(const char* orig_cl_ord_id) {
        const auto it = orders_.find(orig_cl_ord_id);
        if (it == orders_.end()) return;
        Record& r = it->second;
        if (r.state == OrdState::NEW || r.state == OrdState::PARTIAL)
            r.pending_cancel = true;
    }
    bool is_pending_cancel(const char* cl_ord_id) const {
        const auto it = orders_.find(cl_ord_id);
        return it != orders_.end() && it->second.pending_cancel;
    }

    // on_cancel_reject: apply an OrderCancelReject (35=9) — the tracker half
    // of parse_cancel_reject (#385), mirroring the OUCH tracker's lesson
    // (#378): the CANCEL died, the ORDER did not. The working order is
    // identified by tag 41 (OrigClOrdID) — tag 11 names the refused request
    // itself, which the tracker never registered. Disarms pending_cancel,
    // leaves the state and the rejects counter alone, and counts the event
    // in cancel_rejects_. UNKNOWN when tag 41 is missing or names an
    // unregistered order (desync).
    OrdState on_cancel_reject(const FIXMessage& m) {
        const char* orig = m.get_field(41);
        if (!orig) return OrdState::UNKNOWN;
        const auto it = orders_.find(orig);
        if (it == orders_.end()) return OrdState::UNKNOWN;
        it->second.pending_cancel = false;
        ++cancel_rejects_;
        return it->second.state;
    }

    OrdState state(const char* cl_ord_id) const {
        const auto it = orders_.find(cl_ord_id);
        return (it != orders_.end()) ? it->second.state : OrdState::UNKNOWN;
    }
    int32_t cum_qty(const char* cl_ord_id) const {
        const auto it = orders_.find(cl_ord_id);
        return (it != orders_.end()) ? it->second.cum_qty : 0;
    }
    int32_t leaves_qty(const char* cl_ord_id) const {
        const auto it = orders_.find(cl_ord_id);
        return (it != orders_.end()) ? it->second.leaves_qty : 0;
    }
    size_t   order_count() const noexcept { return orders_.size(); }
    // working_orders: orders still live on the exchange (NEW/PARTIAL) (#409)
    // — the FIX-side parity of the OUCH tracker's active_count (#272).
    // order_count() includes terminal records; this is what the desk still
    // has exposure to.
    size_t working_orders() const noexcept {
        size_t n = 0;
        for (const auto& kv : orders_)
            if (kv.second.state == OrdState::NEW || kv.second.state == OrdState::PARTIAL) ++n;
        return n;
    }
    // working_qty: open shares truly resting on the exchange right now (#409)
    // = sum of leaves_qty over NEW/PARTIAL orders — the FIX-side parity of
    // the OUCH tracker's working_shares (#304). Terminal orders may retain
    // a non-zero leaves_qty snapshot (e.g. the canceled remainder); they are
    // excluded here.
    int32_t working_qty() const noexcept {
        int32_t s = 0;
        for (const auto& kv : orders_)
            if (kv.second.state == OrdState::NEW || kv.second.state == OrdState::PARTIAL)
                s += kv.second.leaves_qty;
        return s;
    }
    // pending_cancel_qty: open shares sitting under an in-flight
    // OrderCancelRequest (#425) — the exposure the desk EXPECTS to free
    // once the exchange acks; the FIX parity of the OUCH tracker's
    // pending_cancel_shares (#402). Sums leaves_qty over pending-cancel
    // records (working by construction: on_cancel_sent arms only
    // NEW/PARTIAL, terminal transitions clear the flag).
    int32_t pending_cancel_qty() const noexcept {
        int32_t s = 0;
        for (const auto& kv : orders_)
            if (kv.second.pending_cancel) s += kv.second.leaves_qty;
        return s;
    }
    // pending_cancel_fraction: what part of the live book is already
    // condemned (#425) = pending_cancel_qty / working_qty, in [0,1].
    // Near 1.0 the resting book is an illusion — nearly everything on the
    // exchange is a cancel waiting to be acked. 0 when nothing is working.
    double pending_cancel_fraction() const noexcept {
        const int32_t w = working_qty();
        return w > 0
            ? static_cast<double>(pending_cancel_qty()) / static_cast<double>(w)
            : 0.0;
    }
    uint64_t fills()       const noexcept { return fills_; }
    uint64_t cancels()     const noexcept { return cancels_; }
    uint64_t rejects()     const noexcept { return rejects_; }
    // cancel_rejects: refused cancel attempts (#393) — distinct from
    // rejects(), which counts orders the exchange rejected outright.
    uint64_t cancel_rejects() const noexcept { return cancel_rejects_; }

    // reset_session: wipe the tracker for a new trading day (#449) — the
    // FIX parity of the OUCH tracker's reset_session (#442). ClOrdIDs are
    // commonly re-issued from 1 each session, so yesterday's terminal
    // records answering for today's fresh ids would silently corrupt the
    // accounting; the map also grows unbounded across days. Clears the
    // order map and zeroes every counter. Call at the session open, after
    // any end-of-day report.
    void reset_session() noexcept {
        orders_.clear();
        fills_ = cancels_ = rejects_ = 0;
        cancel_rejects_ = 0;
        replaces_       = 0;
    }
    // replaces: OrdStatus=5 ClOrdID migrations applied (#401).
    uint64_t replaces() const noexcept { return replaces_; }
};

}  // namespace fix
