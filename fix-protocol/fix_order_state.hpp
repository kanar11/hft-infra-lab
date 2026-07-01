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
    };
    std::unordered_map<std::string, Record> orders_;
    uint64_t fills_ = 0, cancels_ = 0, rejects_ = 0;

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
        const auto it = orders_.find(id);
        if (it == orders_.end()) return OrdState::UNKNOWN;
        Record& r = it->second;

        if (const char* cum = m.get_field(14))  r.cum_qty    = std::atoi(cum);
        if (const char* lv  = m.get_field(151)) r.leaves_qty = std::atoi(lv);
        const char* st = m.get_field(39);            // OrdStatus
        r.state = st ? map_status(st[0]) : OrdState::UNKNOWN;

        if (r.state == OrdState::FILLED)        ++fills_;
        else if (r.state == OrdState::CANCELED) ++cancels_;
        else if (r.state == OrdState::REJECTED) ++rejects_;
        return r.state;
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
    uint64_t fills()       const noexcept { return fills_; }
    uint64_t cancels()     const noexcept { return cancels_; }
    uint64_t rejects()     const noexcept { return rejects_; }
};

}  // namespace fix
