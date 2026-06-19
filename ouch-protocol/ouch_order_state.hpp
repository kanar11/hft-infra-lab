/*
 * OUCHOrderTracker — kliencka maszyna stanu zlecenia OUCH (expansion #89).
 *
 * OUCHMessage encode/decode obsluguje POJEDYNCZE wiadomosci; nikt nie sledzil
 * CYKLU ZYCIA zlecenia po stronie klienta (token -> live/partial/filled/
 * cancelled/rejected + ile akcji jeszcze otwarte). To brakujacy element: po
 * wyslaniu Enter Order klient musi wiedziec co sie z nim dzieje, gdy przychodza
 * raporty Accepted/Executed/Cancelled.
 *
 * Cykl: NEW (wyslane) -> LIVE (Accepted) -> [PARTIAL po czesciowym Executed] ->
 *       FILLED (remaining=0) | CANCELLED (Cancelled) | REJECTED (Error)
 *
 * Karm on_new() przy wysylce, on_response() kazdym sparsowanym OUCHResponse.
 */
#pragma once

#include "ouch_protocol.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ouch {

enum class OrderState : uint8_t {
    NEW       = 0,   // wyslane, czekamy na Accepted
    LIVE      = 1,   // Accepted — zlecenie aktywne w ksiedze
    PARTIAL   = 2,   // czesciowo wypelnione, reszta aktywna
    FILLED    = 3,   // calkowicie wypelnione
    CANCELLED = 4,   // anulowane
    REJECTED  = 5,   // odrzucone (Error)
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
        int32_t    original;     // pierwotna ilosc
        int32_t    remaining;    // ile jeszcze otwarte
        int32_t    filled;       // ile wypelnione
        int64_t    order_ref;    // z Accepted (0 dopoki nieznane)
    };
    std::unordered_map<std::string, Record> orders_;
    uint64_t live_ = 0, filled_ = 0, cancelled_ = 0, rejected_ = 0, broken_ = 0;

    Record* find(const char* token) noexcept {
        auto it = orders_.find(token);
        return (it != orders_.end()) ? &it->second : nullptr;
    }

public:
    // on_new: zarejestruj wyslane Enter Order (token + zlecona ilosc).
    void on_new(const char* token, int32_t shares) noexcept {
        orders_[token] = Record{OrderState::NEW, shares, shares, 0, 0};
    }

    // on_response: zaaplikuj raport gieldy. Zwraca nowy stan (lub REJECTED gdy
    // raport dotyczy nieznanego tokenu — desync).
    OrderState on_response(const OUCHResponse& r) noexcept {
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
            if (rec->remaining <= 0) { rec->state = OrderState::FILLED; ++filled_; }
            else                       rec->state = OrderState::PARTIAL;
        } else if (std::strcmp(r.type, "CANCELLED") == 0) {
            rec->state     = OrderState::CANCELLED;
            rec->remaining = 0;
            ++cancelled_;
        } else if (std::strcmp(r.type, "BROKEN") == 0) {
            // Broken Trade (#134): gielda uniewaznia wczesniejszy fill -> odwroc
            // ksiegowanie (akcje wracaja do "otwarte"). Stan z FILLED -> LIVE/PARTIAL.
            const int32_t back = (r.shares < rec->filled) ? r.shares : rec->filled;
            rec->filled    -= back;
            rec->remaining += back;
            rec->state = (rec->filled > 0) ? OrderState::PARTIAL : OrderState::LIVE;
            ++broken_;
        } else {  // ERROR / UNKNOWN
            rec->state = OrderState::REJECTED;
            ++rejected_;
        }
        return rec->state;
    }

    // --- Zapytania ---
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
    size_t   order_count() const noexcept { return orders_.size(); }
    uint64_t live()        const noexcept { return live_; }
    uint64_t fills()       const noexcept { return filled_; }
    uint64_t cancels()     const noexcept { return cancelled_; }
    uint64_t rejects()     const noexcept { return rejected_; }
    uint64_t brokens()     const noexcept { return broken_; }
};

}  // namespace ouch
