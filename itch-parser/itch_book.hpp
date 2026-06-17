/*
 * ITCHOrderBook — rekonstrukcja księgi L3 ze strumienia NASDAQ ITCH
 * (expansion #82).
 *
 * itch_parser.hpp dekoduje POJEDYNCZE wiadomości; nikt nie składał z nich
 * KSIĘGI. To jest brakujący most: feed handler, który z Add/Execute/Cancel/
 * Delete/Replace odtwarza pełny obraz rynku (best bid/ask, głębokość, resting
 * orders po order_ref).
 *
 * To NIE jest silnik dopasowań (orderbook_pro) — reconstructor tylko ODTWARZA
 * to, co mówi feed; nigdy nie matchuje i nigdy nie krzyżuje. Prawdziwy ITCH to
 * już zmatchowana księga, więc Add'y po obu stronach nie przecinają się — a
 * egzekucje/anulacje przychodzą wprost jako wiadomości, nie wynikają z matchu.
 *
 * Mapowanie zdarzeń:
 *   ADD_ORDER       → resting order (ref → side/price/qty) + agregat poziomu
 *   ORDER_EXECUTED  → redukcja qty resting o exec_shares (0 → usuń)
 *   ORDER_CANCELLED → redukcja qty o cancelled_shares (partial cancel)
 *   DELETE_ORDER    → usuń resting w całości
 *   REPLACE_ORDER   → delete(orig) + add(new) z nową ceną/ilością
 *   (TRADE/SYSTEM/STOCK_DIR — nie zmieniają księgi limit-order)
 *
 * Ceny w tickach int64 (× 100 = $0.01), agregaty per poziom w dwóch std::map.
 * best_bid = max klucz bids_, best_ask = min klucz asks_.
 */
#pragma once

#include "itch_parser.hpp"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <cmath>

namespace itch {

class ITCHOrderBook {
    struct Resting {
        char     side;          // 'B' / 'S'
        int64_t  price_ticks;
        uint32_t shares;
    };

    std::unordered_map<int64_t, Resting> orders_;  // order_ref → resting
    std::map<int64_t, int64_t> bids_;              // price_ticks → Σ qty (BUY)
    std::map<int64_t, int64_t> asks_;              // price_ticks → Σ qty (SELL)

    // Statystyki / diagnostyka feed handlera.
    uint64_t adds_ = 0, executes_ = 0, cancels_ = 0, deletes_ = 0, replaces_ = 0;
    uint64_t orphans_ = 0;   // event dla nieznanego ref (luka w feedzie / desync)

    static int64_t to_ticks(double price) noexcept {
        return static_cast<int64_t>(price * 100.0 + (price >= 0 ? 0.5 : -0.5));
    }
    std::map<int64_t, int64_t>& side_book(char side) noexcept {
        return (side == 'B') ? bids_ : asks_;
    }

    // reduce_: zdejmij qty z resting (execute/cancel). Czyści poziom i order
    // gdy schodzą do zera. Nieznany ref = orphan (gap recovery to sygnał).
    void reduce_(int64_t ref, uint32_t qty) noexcept {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++orphans_; return; }
        Resting& r = it->second;
        const uint32_t dec = (qty < r.shares) ? qty : r.shares;
        r.shares -= dec;
        auto& book = side_book(r.side);
        auto lvl = book.find(r.price_ticks);
        if (lvl != book.end()) {
            lvl->second -= static_cast<int64_t>(dec);
            if (lvl->second <= 0) book.erase(lvl);
        }
        if (r.shares == 0) orders_.erase(it);
    }

public:
    // --- Pojedyncze zdarzenia (gdy wołane bezpośrednio) ---
    void on_add(int64_t ref, char side, double price, uint32_t shares) noexcept {
        if (shares == 0) return;
        const int64_t px = to_ticks(price);
        orders_[ref] = Resting{side, px, shares};
        side_book(side)[px] += static_cast<int64_t>(shares);
        ++adds_;
    }
    void on_execute(int64_t ref, uint32_t exec_shares) noexcept { reduce_(ref, exec_shares); ++executes_; }
    void on_cancel(int64_t ref, uint32_t cancelled_shares) noexcept { reduce_(ref, cancelled_shares); ++cancels_; }
    void on_delete(int64_t ref) noexcept {
        auto it = orders_.find(ref);
        if (it == orders_.end()) { ++orphans_; ++deletes_; return; }
        reduce_(ref, it->second.shares);   // zdejmij całość
        ++deletes_;
    }
    void on_replace(int64_t orig_ref, int64_t new_ref, double new_price, uint32_t new_shares) noexcept {
        auto it = orders_.find(orig_ref);
        if (it == orders_.end()) { ++orphans_; ++replaces_; return; }
        const char side = it->second.side;   // replace zachowuje stronę
        reduce_(orig_ref, it->second.shares);
        on_add(new_ref, side, new_price, new_shares);
        --adds_;                              // on_add policzył add; replace to nie add
        ++replaces_;
    }

    // apply: dispatch z ParsedMessage (główne wejście — karm strumieniem z parsera).
    void apply(const ParsedMessage& pm) noexcept {
        switch (pm.type) {
            case MsgType::ADD_ORDER:
                on_add(pm.data.add_order.order_ref, pm.data.add_order.side,
                       pm.data.add_order.price, pm.data.add_order.shares);
                break;
            case MsgType::ADD_ORDER_MPID:
                on_add(pm.data.add_order_mpid.order_ref, pm.data.add_order_mpid.side,
                       pm.data.add_order_mpid.price, pm.data.add_order_mpid.shares);
                break;
            case MsgType::ORDER_EXECUTED:
                on_execute(pm.data.order_executed.order_ref, pm.data.order_executed.exec_shares);
                break;
            case MsgType::ORDER_CANCELLED:
                on_cancel(pm.data.order_cancelled.order_ref, pm.data.order_cancelled.cancelled_shares);
                break;
            case MsgType::DELETE_ORDER:
                on_delete(pm.data.delete_order.order_ref);
                break;
            case MsgType::REPLACE_ORDER:
                on_replace(pm.data.replace_order.orig_order_ref, pm.data.replace_order.new_order_ref,
                           pm.data.replace_order.new_price, pm.data.replace_order.new_shares);
                break;
            default: break;   // TRADE / SYSTEM_EVENT / STOCK_DIRECTORY — nie ruszają księgi
        }
    }

    // --- Zapytania o stan księgi ---
    double  best_bid()  const noexcept { return bids_.empty() ? 0.0 : bids_.rbegin()->first / 100.0; }
    double  best_ask()  const noexcept { return asks_.empty() ? 0.0 : asks_.begin()->first  / 100.0; }
    double  spread()    const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return best_ask() - best_bid();
    }
    int64_t qty_at(char side, double price) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        const auto it = book.find(to_ticks(price));
        return (it != book.end()) ? it->second : 0;
    }
    size_t  bid_levels()     const noexcept { return bids_.size(); }
    size_t  ask_levels()     const noexcept { return asks_.size(); }
    size_t  resting_orders() const noexcept { return orders_.size(); }
    int64_t total_bid_qty()  const noexcept { int64_t s = 0; for (auto& l : bids_) s += l.second; return s; }
    int64_t total_ask_qty()  const noexcept { int64_t s = 0; for (auto& l : asks_) s += l.second; return s; }

    // --- Statystyki feed handlera ---
    uint64_t adds()     const noexcept { return adds_; }
    uint64_t executes() const noexcept { return executes_; }
    uint64_t cancels()  const noexcept { return cancels_; }
    uint64_t deletes()  const noexcept { return deletes_; }
    uint64_t replaces() const noexcept { return replaces_; }
    uint64_t orphans()  const noexcept { return orphans_; }   // sygnał desync / luki feedu
};

}  // namespace itch
