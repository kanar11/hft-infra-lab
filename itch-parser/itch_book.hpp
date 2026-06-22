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
#include <algorithm>

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
    // spread_bps: spread względem mid w punktach bazowych (#131) — miara
    // jakości/płynności rynku niezależna od poziomu ceny.
    double  spread_bps() const noexcept {
        const double m = mid_price();
        return m > 0.0 ? spread() / m * 10000.0 : 0.0;
    }
    // spread_ticks: spread w CALKOWITEJ liczbie ticow ($0.01) (#207). Dla strategii
    // swiadomych wielkosci ticku: 1 = ksiazka najciasniejsza (touch, MM walczy o
    // priorytet), wieksze = jest miejsce na poprawe kwotowania. 0 gdy jednostronna.
    int64_t spread_ticks() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0;
        return asks_.begin()->first - bids_.rbegin()->first;   // ceny w tickach x100
    }
    // is_marketable: would a limit order at limit_price cross immediately against
    // the resting book (#261)? BUY crosses when limit >= best_ask, SELL crosses
    // when limit <= best_bid. Pre-submit check: marketable -> takes liquidity now,
    // otherwise it would rest. false when that side has no resting liquidity.
    bool is_marketable(char side, double limit_price) const noexcept {
        if (side == 'B') { const double a = best_ask(); return a > 0.0 && limit_price >= a; }
        else             { const double b = best_bid(); return b > 0.0 && limit_price <= b; }
    }
    // clear: zresetuj reconstructora do pustego (np. start nowej sesji / re-sync
    // po snapshot recovery). Statystyki feed handlera też zerowane.
    void clear() noexcept {
        orders_.clear(); bids_.clear(); asks_.clear();
        adds_ = executes_ = cancels_ = deletes_ = replaces_ = orphans_ = 0;
    }
    // mid_price: średnia best bid/ask; 0 gdy księga jednostronna.
    double  mid_price() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return (best_bid() + best_ask()) / 2.0;
    }
    int64_t best_bid_qty() const noexcept { return bids_.empty() ? 0 : bids_.rbegin()->second; }
    int64_t best_ask_qty() const noexcept { return asks_.empty() ? 0 : asks_.begin()->second; }
    // imbalance: order-book imbalance top-of-book = (bidQ-askQ)/(bidQ+askQ),
    // zakres [-1,1]. >0 = przewaga kupna (presja w górę), <0 = sprzedaży.
    // Klasyczny krótkoterminowy predyktor kierunku w mikrostrukturze.
    double  imbalance() const noexcept {
        const int64_t b = best_bid_qty(), a = best_ask_qty();
        const int64_t tot = b + a;
        return tot > 0 ? static_cast<double>(b - a) / static_cast<double>(tot) : 0.0;
    }
    // microprice: fair-value wazony rozmiarami (Stoikov). Przy przewadze bidu
    // (Q_bid > Q_ask) ciazy ku ASK (spodziewany ruch w gore) i odwrotnie —
    // lepszy estymator "prawdziwej" ceny niz prosty mid. 0 gdy ksiega jednostronna.
    //   microprice = (P_ask*Q_bid + P_bid*Q_ask) / (Q_bid + Q_ask)
    // liquidity_within: laczna qty w N TICKACH od best po danej stronie (#164).
    // Miara gestosci plynnosci przy szczycie (ile zrealizuje sie blisko touch'a)
    // niezalezna od liczby poziomow. BUY: bidy >= best_bid - ticks; SELL: aski
    // <= best_ask + ticks.
    int64_t liquidity_within(char side, int ticks) const noexcept {
        if (ticks < 0) return 0;
        int64_t sum = 0;
        if (side == 'B') {
            if (bids_.empty()) return 0;
            const int64_t floor = bids_.rbegin()->first - ticks;
            for (auto it = bids_.rbegin(); it != bids_.rend() && it->first >= floor; ++it) sum += it->second;
        } else {
            if (asks_.empty()) return 0;
            const int64_t ceil = asks_.begin()->first + ticks;
            for (auto it = asks_.begin(); it != asks_.end() && it->first <= ceil; ++it) sum += it->second;
        }
        return sum;
    }

    // fillable_shares: ile akcji da sie wykonac do CENY LIMIT (#223). BUY: suma
    // qty asks o cenie <= limit; SELL: suma qty bids o cenie >= limit. Inaczej niz
    // liquidity_within (ticki od best) — tu prog to konkretna cena zlecenia. Sizing
    // zlecenia z limitem: ile sie wykona od reki bez przebicia limitu.
    int64_t fillable_shares(char side, double limit_price) const noexcept {
        const int64_t lim = to_ticks(limit_price);
        int64_t sum = 0;
        if (side == 'B') {                       // kupno: aski rosnaco, bierz <= limit
            for (const auto& [px, qty] : asks_) { if (px > lim) break; sum += qty; }
        } else {                                 // sprzedaz: bidy malejaco od best, bierz >= limit
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (it->first < lim) break;
                sum += it->second;
            }
        }
        return sum;
    }

    // price_to_fill: NAJGORSZA cena (poziom), ktory trzeba dotknac aby wykonac
    // `shares` przechodzac ksiazke (#247). BUY chodzi po askach rosnaco, SELL po
    // bidach malejaco; zwraca cene poziomu, na ktorym skumulowana ilosc pokrywa
    // zlecenie. To LIMIT do sweep'a (inaczej niz expected_fill=VWAP). 0 gdy za malo
    // plynnosci.
    double  price_to_fill(char side, int64_t shares) const noexcept {
        if (shares <= 0) return 0.0;
        int64_t remaining = shares;
        if (side == 'B') {
            for (const auto& [px, qty] : asks_) {
                remaining -= qty;
                if (remaining <= 0) return static_cast<double>(px) / 100.0;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                remaining -= it->second;
                if (remaining <= 0) return static_cast<double>(it->first) / 100.0;
            }
        }
        return 0.0;   // niewystarczajaca plynnosc
    }

    // total_shares: laczna spoczywajaca liczba akcji po danej stronie (#174).
    // Caly rozmiar ksiazki na jednej stronie — surowa miara dostepnej plynnosci
    // (w odroznieniu od liquidity_within, bez ograniczenia do okolic touch'a).
    int64_t total_shares(char side) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        int64_t sum = 0;
        for (const auto& [px, qty] : book) sum += qty;
        return sum;
    }

    // total_notional: laczna WARTOSC ($) spoczywajacej plynnosci po stronie (#191).
    // Suma cena*ilosc po wszystkich poziomach — dopelnia total_shares (akcje):
    // dwie ksiazki o tej samej liczbie akcji moga miec rozny kapital przy roznych
    // cenach. Miara glebokosci w dolarach.
    double total_notional(char side) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        double sum = 0.0;
        for (const auto& [px, qty] : book)
            sum += (static_cast<double>(px) / 100.0) * static_cast<double>(qty);
        return sum;
    }

    // level_count: liczba roznych poziomow cenowych po danej stronie (#174).
    // Grubosc ksiazki — ile odrebnych cen ma plynnosc (rzadka vs gesta ksiazka).
    std::size_t level_count(char side) const noexcept {
        return (side == 'B') ? bids_.size() : asks_.size();
    }

    // is_locked: best_bid == best_ask (#183). Ksiazka "zamknieta" — spread zero.
    // Sygnalizuje nieaktualny/niespojny obraz (feed laggujacy) albo moment przed
    // matchingiem; market-maker zwykle wstrzymuje kwotowanie.
    bool is_locked() const noexcept {
        return !bids_.empty() && !asks_.empty()
            && bids_.rbegin()->first == asks_.begin()->first;
    }

    // is_crossed: best_bid > best_ask (#183). Ksiazka "skrzyzowana" — teoretyczny
    // arbitraz (kup po ask < sprzedaj po bid). Realnie: niespojnosc feedu /
    // brakujacy Delete; konsument powinien odrzucic albo zsnapshotowac.
    bool is_crossed() const noexcept {
        return !bids_.empty() && !asks_.empty()
            && bids_.rbegin()->first > asks_.begin()->first;
    }

    // vwap_depth: volume-weighted cena top-N poziomow po danej stronie (#155).
    // Fair-value uwzgledniajacy GLEBOKOSC (nie tylko touch); im glebiej, tym
    // bardziej odzwierciedla cene realizacji wiekszego zlecenia.
    double  vwap_depth(char side, int n) const noexcept {
        if (n <= 0) return 0.0;
        double notional = 0.0; int64_t qty = 0; int c = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) {
                notional += (it->first / 100.0) * static_cast<double>(it->second);
                qty += it->second;
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) {
                notional += (it->first / 100.0) * static_cast<double>(it->second);
                qty += it->second;
            }
        }
        return qty > 0 ? notional / static_cast<double>(qty) : 0.0;
    }

    // depth_weighted_mid: fair value that averages the depth-VWAP of both sides over
    // the top N levels (#285) = (vwap_depth('B', n) + vwap_depth('S', n)) / 2. Unlike
    // mid_price (touch only) or microprice (size-weighted touch) this folds in N
    // levels of depth on EACH side, so a large resting wall a few levels deep pulls
    // the fair value toward it. 0 when either side lacks liquidity.
    double  depth_weighted_mid(int n) const noexcept {
        const double bv = vwap_depth('B', n);
        const double av = vwap_depth('S', n);
        return (bv > 0.0 && av > 0.0) ? (bv + av) / 2.0 : 0.0;
    }

    // depth_imbalance: order-book imbalance po TOP-N poziomach (#148), uogolnienie
    // top-of-book imbalance (#87, n=1). (Σbid - Σask)/(Σbid+Σask) z n najlepszych
    // poziomow kazdej strony. Glebszy obraz presji niz sam touch.
    double  depth_imbalance(int n) const noexcept {
        if (n <= 0) return 0.0;
        int64_t b = 0, a = 0; int c = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) b += it->second;
        c = 0;
        for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) a += it->second;
        const int64_t tot = b + a;
        return tot > 0 ? static_cast<double>(b - a) / static_cast<double>(tot) : 0.0;
    }
    // notional_imbalance: imbalance wazony WARTOSCIA ($) top-N poziomow (#215).
    // Jak depth_imbalance, ale waga = cena*ilosc zamiast samej ilosci. Duze
    // zlecenia patrza na nominal: 100 szt. po $300 wazy 30x wiecej niz 100 po $10.
    // (Sb_$ - Sa_$)/(Sb_$ + Sa_$), zakres [-1,1]. 0 gdy pusto.
    double  notional_imbalance(int n) const noexcept {
        if (n <= 0) return 0.0;
        double b = 0.0, a = 0.0; int c = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c)
            b += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        c = 0;
        for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c)
            a += (static_cast<double>(it->first) / 100.0) * static_cast<double>(it->second);
        const double tot = b + a;
        return tot > 0.0 ? (b - a) / tot : 0.0;
    }
    // liquidity_imbalance_within: order-book imbalance of the liquidity within N
    // ticks of each touch (#269) — (bid_liq - ask_liq)/(bid_liq + ask_liq) using
    // liquidity_within (#164) on both sides. Unlike depth_imbalance (top-N price
    // LEVELS) this is anchored to a price DISTANCE from the touch, so it's
    // unaffected by how levels are split. [-1, 1]; 0 when both sides empty.
    double  liquidity_imbalance_within(int ticks) const noexcept {
        const int64_t b = liquidity_within('B', ticks);
        const int64_t a = liquidity_within('S', ticks);
        const int64_t tot = b + a;
        return tot > 0 ? static_cast<double>(b - a) / static_cast<double>(tot) : 0.0;
    }
    double  microprice() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        const int64_t qb = best_bid_qty(), qa = best_ask_qty();
        const int64_t tot = qb + qa;
        if (tot <= 0) return 0.0;
        return (best_ask() * static_cast<double>(qb) + best_bid() * static_cast<double>(qa))
               / static_cast<double>(tot);
    }
    // microprice_skew: microprice - mid (#239). Znak i wielkosc presji kierunkowej
    // w jednostkach ceny: >0 = wieksza plynnosc po BIDZIE (presja wzrostowa,
    // microprice ciagnie ku askowi), <0 = po ASKU (spadkowa). 0 dla zbalansowanego
    // lub jednostronnego rynku. Sygnal alpha krotkiego horyzontu.
    double  microprice_skew() const noexcept {
        const double mp = microprice();
        const double m  = mid_price();
        return (mp > 0.0 && m > 0.0) ? mp - m : 0.0;
    }
    // imbalance_signal: discrete directional signal from top-of-book imbalance
    // crossing a threshold (#254). imbalance() = (qbid - qask)/(qbid + qask) in
    // [-1, 1]; returns +1 when bid-heavy beyond +threshold (upward pressure),
    // -1 when ask-heavy beyond -threshold, 0 in between. Ready-to-use entry filter.
    int imbalance_signal(double threshold) const noexcept {
        const double imb = imbalance();
        if (imb >  threshold) return 1;
        if (imb < -threshold) return -1;
        return 0;
    }
    int64_t qty_at(char side, double price) const noexcept {
        const auto& book = (side == 'B') ? bids_ : asks_;
        const auto it = book.find(to_ticks(price));
        return (it != book.end()) ? it->second : 0;
    }
    // expected_fill: pre-trade impact — przejdź odtworzoną księgę i policz VWAP
    // jaki osiągnęłoby marketowe zlecenie `shares` po danej stronie (BUY bierze
    // asks rosnąco, SELL bids malejąco). Zwraca ile akcji wykonalne (≤ shares
    // gdy płynność niewystarczająca); out_vwap = średnia cena (0 gdy 0 fill).
    int64_t expected_fill(char side, int64_t shares, double& out_vwap) const noexcept {
        out_vwap = 0.0;
        if (shares <= 0) return 0;
        int64_t remaining = shares, filled = 0;
        double  notional_ticks = 0.0;
        if (side == 'B') {
            for (auto it = asks_.begin(); it != asks_.end() && remaining > 0; ++it) {
                const int64_t take = std::min<int64_t>(remaining, it->second);
                notional_ticks += static_cast<double>(it->first) * take;
                filled += take; remaining -= take;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend() && remaining > 0; ++it) {
                const int64_t take = std::min<int64_t>(remaining, it->second);
                notional_ticks += static_cast<double>(it->first) * take;
                filled += take; remaining -= take;
            }
        }
        if (filled > 0) out_vwap = notional_ticks / static_cast<double>(filled) / 100.0;
        return filled;
    }

    // slippage_bps: oczekiwany koszt egzekucji w punktach bazowych (#199) — o ile
    // bps gorzej niz mid wykona sie marketowe zlecenie `shares` po danej stronie.
    // BUY placi powyzej mid, SELL dostaje ponizej — w obu wynik DODATNI (koszt).
    // Buduje na expected_fill (VWAP po przejsciu ksiazki). 0 gdy brak fillu/mid.
    // Pre-trade sizing: czy zlecenie nie zje za duzo spreadu/glebokosci.
    double slippage_bps(char side, int64_t shares) const noexcept {
        double vwap = 0.0;
        const int64_t filled = expected_fill(side, shares, vwap);
        const double m = mid_price();
        if (filled <= 0 || m <= 0.0 || vwap <= 0.0) return 0.0;
        const double diff = (side == 'B') ? (vwap - m) : (m - vwap);
        return diff / m * 10000.0;
    }

    // round_trip_cost_bps: pelny koszt transakcyjny round-trip dla `shares` (#231)
    // = slippage kupna + slippage sprzedazy (bps vs mid). Ile zjada glebokosc gdy
    // wejdziesz i wyjdziesz z pozycji tej wielkosci — realny prog rentownosci
    // strategii. 0 gdy ksiega jednostronna (brak mid). Buduje na slippage_bps.
    double round_trip_cost_bps(int64_t shares) const noexcept {
        return slippage_bps('B', shares) + slippage_bps('S', shares);
    }

    // fill_shortfall: how many of `shares` a marketable order would leave UNFILLED
    // after sweeping all available depth on that side (#301) = shares - filled, via
    // expected_fill (#199). 0 means the book fully covers the order; a positive
    // value is the remainder that would rest or go unfilled. Unlike fillable_shares
    // (limit-price bounded) this is bounded only by total displayed depth.
    int64_t fill_shortfall(char side, int64_t shares) const noexcept {
        if (shares <= 0) return 0;
        double vwap = 0.0;
        const int64_t filled = expected_fill(side, shares, vwap);
        return shares - filled;
    }

    // top_levels: skopiuj do n NAJLEPSZYCH poziomow po danej stronie (BUY: bids
    // malejaco od best; SELL: asks rosnaco) — cena + zagregowana qty. Zwraca ile
    // poziomow faktycznie wypelniono (<= n). L2 depth dla strategii/wyswietlania.
    int top_levels(char side, int n, double* out_px, int64_t* out_qty) const noexcept {
        int c = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend() && c < n; ++it, ++c) {
                out_px[c]  = it->first / 100.0;
                out_qty[c] = it->second;
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && c < n; ++it, ++c) {
                out_px[c]  = it->first / 100.0;
                out_qty[c] = it->second;
            }
        }
        return c;
    }

    // nth_level_price / nth_level_qty: random access to the n-th best price level on
    // a side (#277, 0-based; BUY counts bids down from best, SELL counts asks up).
    // Single-level lookup without copying the whole top-N (unlike top_levels). 0
    // when n is out of range. Handy for L2 strategies that reference one level.
    double  nth_level_price(char side, int n) const noexcept {
        if (n < 0) return 0.0;
        int i = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it, ++i)
                if (i == n) return static_cast<double>(it->first) / 100.0;
        } else {
            for (auto it = asks_.begin(); it != asks_.end(); ++it, ++i)
                if (i == n) return static_cast<double>(it->first) / 100.0;
        }
        return 0.0;
    }
    int64_t nth_level_qty(char side, int n) const noexcept {
        if (n < 0) return 0;
        int i = 0;
        if (side == 'B') {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it, ++i)
                if (i == n) return it->second;
        } else {
            for (auto it = asks_.begin(); it != asks_.end(); ++it, ++i)
                if (i == n) return it->second;
        }
        return 0;
    }
    // cumulative_qty: total displayed size across the top N price levels on a side
    // (#293), summing nth_level_qty (#277). How many shares a marketable order would
    // see before walking past depth N — the sizing input for a sweep. Naturally
    // stops at the end of book (out-of-range levels contribute 0).
    int64_t cumulative_qty(char side, int n) const noexcept {
        int64_t sum = 0;
        for (int i = 0; i < n; ++i) sum += nth_level_qty(side, i);
        return sum;
    }
    // depth_notional: total $ notional resting across the top N price levels on a
    // side (#309) = sum(price * qty) over levels 0..N-1 — the capital a sweep would
    // consume, the $ companion to cumulative_qty (#293, shares). Stops at the end of
    // book (an out-of-range level returns price 0). Uses nth_level_price/qty (#277).
    double  depth_notional(char side, int n) const noexcept {
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double px = nth_level_price(side, i);
            if (px <= 0.0) break;
            sum += px * static_cast<double>(nth_level_qty(side, i));
        }
        return sum;
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
