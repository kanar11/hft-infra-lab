/*
 * Order Management System (OMS) — C++.
 *
 * OMS leży na krytycznej ścieżce między strategią a giełdą: każde zlecenie
 * jest tutaj walidowane, pre-trade-checkowane, zapisywane do mapy po ID,
 * a po fillu uaktualnia pozycję i realizowane P&L.
 *
 * Wydajność: ~10-50 M zleceń/sek (Python equivalent ~100-500 K/s).
 *
 * Cykl życia: NEW → SENT → FILLED | PARTIAL | CANCELLED | REJECTED.
 *
 * Kluczowe decyzje projektowe:
 *   - Header-only — wystarczy #include "oms.hpp".
 *   - Ceny stałoprzecinkowe int64 (× 10000) — bez float na hot-pathie,
 *     unika błędów zaokrąglania.
 *   - Tickery jako char[9] (max 8 znaków + null) — bez alokacji
 *     std::string.
 *   - Map kluczowane przez sym_to_key(symbol) — packed uint64_t,
 *     ten sam schemat co RiskManager.
 *   - Pending exposure (Position::pending_qty) trackowany w submit /
 *     fill / cancel — pre-trade check liczy realized + pending + new,
 *     zapobiega over-commit gdy wiele submitów ściga się z fillami.
 *   - noexcept na hot-pathie — kompilator nie generuje stack-unwind.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cstdio>
#include <cmath>

#include "../common/types.hpp"
#include "../common/symbol_key.hpp"
#include "../common/time_utils.hpp"


// Mnożnik dla cen stałoprzecinkowych: $150.25 → 1502500.
static constexpr int64_t PRICE_SCALE = 10000;

// to_fixed: cena double → int64 fixed-point. Zaokrąglenie half-up.
inline int64_t to_fixed(double price) noexcept {
    return static_cast<int64_t>(price * PRICE_SCALE + 0.5);
}

// to_float: int64 fixed-point → double (tylko do wyświetlania).
inline double to_float(int64_t fixed_price) noexcept {
    return static_cast<double>(fixed_price) / PRICE_SCALE;
}


// Status w cyklu życia zlecenia. uint8_t — w OMS z milionami zleceń
// oszczędność 3 bajtów per Order się sumuje.
enum class OrderStatus : uint8_t {
    NEW       = 0,   // utworzone lokalnie, jeszcze nie wysłane
    SENT      = 1,   // wysłane na giełdę (lub do symulatora)
    FILLED    = 2,   // w pełni zrealizowane
    PARTIAL   = 3,   // część zrealizowana, reszta wciąż aktywna
    CANCELLED = 4,   // anulowane (przez nas)
    REJECTED  = 5    // odrzucone (przez giełdę / risk manager)
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

// (Side enum + side_str są w common/types.hpp.)


// OMSReject — powód odrzucenia w submit_order (#88). Wcześniej submit zwracał
// tylko nullptr bez informacji DLACZEGO — caller nie mógł rozróżnić błędu inputu
// od limitu wartości czy pozycji. Mirror RejectReason z orderbook_pro.
enum class OMSReject : uint8_t {
    NONE           = 0,   // brak odrzucenia (sukces)
    INVALID_INPUT  = 1,   // NaN / cena≤0 / qty=0 / pusty symbol
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


// Order — pojedyncze zlecenie. Trzymane w OMS::orders_ pod kluczem order_id.
//
// Wszystkie czasy w nanosekundach z CLOCK_MONOTONIC (mono_ns). Pomiar
// latency od submit do fill wystarczy odjąć filled_ns - sent_ns.
struct Order {
    uint64_t    order_id;
    char        symbol[9];      // fixed-size, NASDAQ tickery to max 8 znaków
    Side        side;
    int64_t     price;          // fixed-point: $150.25 → 1502500
    uint32_t    quantity;
    uint32_t    filled_qty;     // ile do tej pory zrealizowano
    OrderStatus status;
    int64_t     created_ns;     // moment przyjęcia w submit_order
    int64_t     sent_ns;        // moment wysłania (status → SENT)
    int64_t     filled_ns;      // moment ostatniego fillu
    int64_t     fill_notional;  // Σ fill_qty × fill_price (#141) — do avg fill price
    int64_t     expire_ns;      // GTD: wygasniecie (#172); 0 = bez wygasniecia (DAY/GTC)

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

    // avg_fill_price: srednia cena wykonania (fixed-point) — zlecenie moze byc
    // wypelniane po roznych cenach; 0 gdy brak fillu. (#141)
    int64_t avg_fill_price() const noexcept {
        return filled_qty > 0 ? fill_notional / static_cast<int64_t>(filled_qty) : 0;
    }
};


// Position — agregowana ekspozycja na jeden symbol. Trzymana w OMS::positions_
// pod kluczem sym_to_key(symbol).
//
// net_qty aktualizuje się tylko w fill_order; pending_qty rośnie w submit
// i maleje w fill/cancel. Suma (net + pending) to "całkowity zaangażowany
// limit", którego pilnujemy w pre-trade checku.
struct Position {
    char    symbol[9];
    int32_t net_qty;         // zrealizowane (dodatnie = long, ujemne = short)
    int32_t pending_qty;     // w locie (BUY+, SELL-)
    int64_t avg_price;       // średnia cena zrealizowanego long'a, fixed-point
    int64_t realized_pnl;    // skumulowany P&L (brutto) × PRICE_SCALE
    int64_t total_cost;      // suma qty × cena dla aktualnego long'a
    int64_t fees;            // skumulowane prowizje × PRICE_SCALE (#83)

    Position() noexcept
        : net_qty(0), pending_qty(0), avg_price(0), realized_pnl(0), total_cost(0), fees(0) {
        symbol[0] = '\0';
    }

    explicit Position(const char* sym) noexcept
        : net_qty(0), pending_qty(0), avg_price(0), realized_pnl(0), total_cost(0), fees(0) {
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
    }

    // net_pnl: P&L po odjęciu prowizji (to co realnie zostaje w kieszeni).
    int64_t net_pnl() const noexcept { return realized_pnl - fees; }

    // unrealized_pnl: mark-to-market otwartej pozycji przy cenie mark (#96).
    // Signed: long (net>0) zarabia gdy mark>avg; short (net<0) gdy mark<avg —
    // oba pokrywa net_qty * (mark - avg). Fixed-point (×PRICE_SCALE).
    int64_t unrealized_pnl(int64_t mark_price) const noexcept {
        return static_cast<int64_t>(net_qty) * (mark_price - avg_price);
    }

    // total_pnl: realized (brutto) + unrealized - fees przy danej cenie mark.
    int64_t total_pnl(int64_t mark_price) const noexcept {
        return realized_pnl + unrealized_pnl(mark_price) - fees;
    }
};


// OMS — główna klasa. Hot-path API:
//   submit_order  — pre-trade check + utworzenie + rezerwacja pending
//   fill_order    — przepływ pending → realized + przeliczenie P&L
//   cancel_order  — zwolnienie pending dla niezrealizowanej reszty
//
// Read-only accessors: get_order, get_position, order_count, position_count.
class OMS {
    std::unordered_map<uint64_t, Order>    orders_;     // ID → Order
    std::unordered_map<uint64_t, Position> positions_;  // sym_to_key → Position

    uint64_t next_id_;
    int32_t  max_position_;     // limit |net + pending + new| per symbol
    int64_t  max_order_value_;  // limit price × qty na jedno zlecenie (w dolarach)
    int64_t  commission_fp_;    // prowizja per akcja, fixed-point (×PRICE_SCALE)
    int64_t  total_fees_;       // skumulowane prowizje całego OMS, fixed-point
    OMSReject last_reject_ = OMSReject::NONE;  // powód ostatniego odrzucenia (#88)
    uint64_t  reject_counts_[4] = {0, 0, 0, 0}; // licznik odrzuceń per OMSReject (#136)
    uint64_t  total_submitted_ = 0;  // przyjete submity (#160)
    uint64_t  total_fills_    = 0;   // liczniki operacji cyklu zycia (#151)
    uint64_t  total_cancels_  = 0;
    uint64_t  total_replaces_ = 0;

public:
    // commission_per_share: np. 0.0035 = $0.0035/akcja (typowy taker fee).
    OMS(int32_t max_pos = 1000, double max_val = 100000.0,
        double commission_per_share = 0.0) noexcept
        : next_id_(1),
          max_position_(max_pos),
          max_order_value_(static_cast<int64_t>(max_val)),
          commission_fp_(static_cast<int64_t>(commission_per_share * PRICE_SCALE + 0.5)),
          total_fees_(0) {}

    // submit_order: walidacja inputów, dwa pre-trade checki, utworzenie
    // Order'a, rezerwacja pending w Position. Zwraca pointer do zlecenia
    // w mapie (stabilny dopóki OMS żyje) lub nullptr przy odrzuceniu.
    // out_reason (opcjonalny, #88): przy nullptr dostaje powód odrzucenia.
    Order* submit_order(const char* symbol, Side side, double price_f,
                        uint32_t quantity, OMSReject* out_reason = nullptr,
                        int64_t expire_ns = 0) noexcept {
        const int64_t t0    = mono_ns();
        const int64_t price = to_fixed(price_f);
        auto fail = [&](OMSReject r) -> Order* {
            last_reject_ = r;
            ++reject_counts_[static_cast<int>(r)];   // #136 statystyki per powód
            if (out_reason) *out_reason = r;
            return nullptr;
        };

        // Walidacja inputów. NaN, ujemna/zerowa cena, zero shares,
        // pusty/null symbol — odrzuć od razu.
        if (std::isnan(price_f) || price <= 0 || quantity == 0
            || symbol == nullptr || symbol[0] == '\0') {
            return fail(OMSReject::INVALID_INPUT);
        }

        // Pre-trade check #1: wartość zlecenia w dolarach.
        const int64_t order_value = (price / PRICE_SCALE) * static_cast<int64_t>(quantity);
        if (order_value > max_order_value_) return fail(OMSReject::ORDER_VALUE);

        // Pre-trade check #2: limit pozycji (realized + pending + new).
        const uint64_t sym_key  = sym_to_key(symbol);
        const int32_t  signed_n = signed_qty(side, quantity);
        auto           pos_it   = positions_.find(sym_key);
        const int32_t  cur_real = (pos_it != positions_.end()) ? pos_it->second.net_qty     : 0;
        const int32_t  cur_pend = (pos_it != positions_.end()) ? pos_it->second.pending_qty : 0;
        if (std::abs(cur_real + cur_pend + signed_n) > max_position_)
            return fail(OMSReject::POSITION_LIMIT);

        last_reject_ = OMSReject::NONE;
        if (out_reason) *out_reason = OMSReject::NONE;

        // Utwórz Order, ustaw status SENT, zapisz w mapie.
        const uint64_t id = next_id_++;
        Order order(id, symbol, side, price, quantity);
        order.created_ns = t0;
        order.sent_ns    = mono_ns();
        order.status     = OrderStatus::SENT;
        order.expire_ns  = expire_ns;   // #172 GTD
        auto it = orders_.emplace(id, order).first;

        // Zarezerwuj pending. Tworzymy Position leniwie tutaj (a nie dopiero
        // przy pierwszym fillu) żeby księgowanie pending było spójne na
        // każdej ścieżce.
        if (pos_it == positions_.end()) {
            pos_it = positions_.emplace(sym_key, Position(symbol)).first;
        }
        pos_it->second.pending_qty += signed_n;
        ++total_submitted_;   // #160
        return &it->second;
    }

    // fill_order: raport wykonania z giełdy. Aktualizuje filled_qty + status
    // w Order, przepływa qty z pending do net w Position, przelicza avg_price
    // (BUY) lub realized_pnl (SELL). Zwraca rzeczywiście zaaplikowane qty
    // (0 gdy zlecenie nieznane / już wypełnione / fill_qty=0); może być MNIEJSZE
    // niż żądane gdy próba over-fill (clamp do remaining + warn).
    uint32_t fill_order(uint64_t order_id, uint32_t fill_qty, double fill_price_f) noexcept {
        if (fill_qty == 0) return 0;
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: fill for unknown order_id=%lu\n", (unsigned long)order_id);
            return 0;
        }
        Order& order = it->second;
        const int64_t fill_price = to_fixed(fill_price_f);

        // Over-fill protection — venue ack > pozostałe qty = bug po stronie
        // giełdy / drugi raz ten sam fill / strategy bug. Clamp do remaining,
        // wpisz tylko valid część, loguj jako anomalię (audit + regression).
        const uint32_t remaining = (order.filled_qty < order.quantity)
            ? (order.quantity - order.filled_qty) : 0;
        if (fill_qty > remaining) {
            printf("[OMS] WARNING: over-fill on order_id=%lu (req=%u, remaining=%u) — clamping\n",
                   (unsigned long)order_id, fill_qty, remaining);
            fill_qty = remaining;
            if (fill_qty == 0) return 0;  // już całkowicie wypełnione
        }

        order.filled_qty    += fill_qty;
        order.fill_notional += static_cast<int64_t>(fill_qty) * fill_price;  // #141
        ++total_fills_;   // #151
        order.status      = (order.filled_qty >= order.quantity)
                            ? OrderStatus::FILLED
                            : OrderStatus::PARTIAL;
        order.filled_ns   = mono_ns();

        // Pobierz / utwórz Position dla tego symbolu. Normalnie Position
        // już istnieje (stworzony w submit_order), ale fallback dla
        // ścieżek omijających submit (np. legacy testy).
        const uint64_t sym_key = sym_to_key(order.symbol);
        auto pos_it = positions_.find(sym_key);
        if (pos_it == positions_.end()) {
            pos_it = positions_.emplace(sym_key, Position(order.symbol)).first;
        }
        Position& pos = pos_it->second;

        // Przepływ qty z pending do net. pos.net_qty + pos.pending_qty
        // pozostaje niezmienione, więc niezmiennik łącznej ekspozycji jest
        // automatycznie zachowany.
        const int32_t signed_fill = signed_qty(order.side, fill_qty);
        pos.pending_qty -= signed_fill;
        apply_fill_to_position(pos, signed_fill, static_cast<int64_t>(fill_qty), fill_price);

        // Prowizja: naliczana od każdej zrealizowanej akcji (taker fee). Zmniejsza
        // P&L netto, nie brutto — realized_pnl pozostaje "czysty" do atrybucji.
        if (commission_fp_ != 0) {
            const int64_t fee = static_cast<int64_t>(fill_qty) * commission_fp_;
            pos.fees    += fee;
            total_fees_ += fee;
        }
        return fill_qty;  // ile faktycznie zaaplikowano (po clampie)
    }

    // cancel_order: zwalnia niezrealizowaną resztę z pending. Tylko dla
    // statusów SENT lub PARTIAL — pozostałe są błędem i zostają zgłoszone.
    void cancel_order(uint64_t order_id) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: cancel for unknown order_id=%lu\n", (unsigned long)order_id);
            return;
        }
        Order& order = it->second;
        if (order.status != OrderStatus::SENT && order.status != OrderStatus::PARTIAL) {
            // FILLED / CANCELLED / REJECTED — milcząca zgoda ukryłaby
            // double-cancel bug po stronie wywołującego. Zaloguj.
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
        order.status = OrderStatus::CANCELLED;
        ++total_cancels_;   // #151
    }

    // replace_order: amend (cancel/replace) ceny i/lub ilości otwartego zlecenia.
    // Mapuje na OUCH 'U' (Replace) i FIX 'G' (OrderCancelReplaceRequest).
    //
    // Reguły:
    //   - tylko SENT / PARTIAL (jak cancel)
    //   - nowa ilość ≥ filled_qty (nie można "od-wypełnić"); ==filled → FILLED
    //   - re-walidacja pre-trade: wartość zlecenia + limit pozycji na NOWEJ
    //     pozostałej ekspozycji; breach → amend odrzucony, zlecenie bez zmian
    //   - pending przesuwany o (new_remaining - old_remaining)
    //
    // Zwraca true gdy amend zaaplikowany, false przy odrzuceniu / błędzie.
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

        // Pre-trade #1: wartość zlecenia na nowej cenie/ilości.
        const int64_t order_value = (new_price / PRICE_SCALE) * static_cast<int64_t>(new_quantity);
        if (order_value > max_order_value_) return false;

        // Pre-trade #2: limit pozycji na zamienionej pozostałej ekspozycji.
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

        // Commit: przesuń pending, podmień cenę/ilość, przelicz status.
        if (pos_it != positions_.end()) pos_it->second.pending_qty += (new_pend - old_pend);
        order.price    = new_price;
        order.quantity = new_quantity;
        order.status   = (order.filled_qty >= order.quantity) ? OrderStatus::FILLED
                       : (order.filled_qty > 0)               ? OrderStatus::PARTIAL
                                                              : OrderStatus::SENT;
        ++total_replaces_;   // #151
        return true;
    }

    // amend_quantity: REDUKCJA ilosci w miejscu, ta sama cena (#188). Reduce-only
    // amend zwykle ZACHOWUJE priorytet w kolejce na gieldzie (inaczej niz pelny
    // cancel/replace ze zmiana ceny). Wymusza: zlecenie aktywne, nowa ilosc
    // mniejsza od biezacej i nie ponizej juz wykonanej. Deleguje do replace_order
    // (ta sama cena), wiec ksiegowosc pending/pozycji jest spojna.
    bool amend_quantity(uint64_t order_id, uint32_t new_quantity) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return false;
        const Order& order = it->second;
        if (order.status != OrderStatus::SENT && order.status != OrderStatus::PARTIAL) return false;
        if (new_quantity >= order.quantity)   return false;   // tylko redukcja
        if (new_quantity <  order.filled_qty) return false;   // nie ponizej wykonanego
        return replace_order(order_id, to_float(order.price), new_quantity);
    }

    // cancel_all: masowe anulowanie wszystkich AKTYWNYCH zleceń (#100). Risk-off
    // / panic button — przy kill switchu albo końcu sesji zdejmujemy wszystkie
    // otwarte zlecenia (zwalnia pending). Zwraca ile anulowano. Iteracja po
    // orders_ jest bezpieczna: cancel_order zmienia tylko status + pending_,
    // nie strukturę mapy orders_.
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

    // purge_expired: anuluj wszystkie AKTYWNE zlecenia GTD ktorych czas
    // wygasniecia minal (#172; expire_ns > 0 && <= now_ns). Zwraca ile anulowano.
    // Wolaj z petli sesji z biezacym mono_ns().
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

    // open_order_notional: laczny nominal ($) NIEZREALIZOWANEJ czesci wszystkich
    // pracujacych zlecen (SENT/PARTIAL) (#180). Kapital zwiazany w ksiazce zlecen
    // — widok pre-trade exposure niezalezny od pozycji (te pokrywa unrealized_pnl).
    // Liczy tylko reszte (quantity - filled_qty), wiec partiale licza sie uczciwie.
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

    // open_position_count: liczba symboli z NIEZEROWA pozycja netto (#196).
    // Inaczej niz position_count() (= liczba wpisow w mapie, w tym zamknietych na
    // 0): to faktyczna szerokosc portfela — ile instrumentow realnie trzymamy.
    size_t open_position_count() const noexcept {
        size_t c = 0;
        for (const auto& [key, p] : positions_) if (p.net_qty != 0) ++c;
        return c;
    }

    // is_flat: brak otwartych pozycji ORAZ brak pracujacych zlecen (#196).
    // Kontrola end-of-day / rekoncyliacji: czy desk jest w pelni zamkniety.
    bool is_flat() const noexcept {
        if (open_position_count() != 0) return false;
        for (const auto& [id, o] : orders_)
            if (o.status == OrderStatus::SENT || o.status == OrderStatus::PARTIAL) return false;
        return true;
    }

    // cancel_all_symbol: jak cancel_all, ale tylko dla jednego tickera (np. po
    // halt'cie konkretnego symbolu).
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

    // Accessory (read-only).
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
    // count_by_status: ile zlecen w danym stanie (#128) — observability/monitoring
    // (np. ile aktywnych SENT/PARTIAL, ile odrzuconych).
    size_t count_by_status(OrderStatus st) const noexcept {
        size_t n = 0;
        for (const auto& kv : orders_) if (kv.second.status == st) ++n;
        return n;
    }
    // total_fees: skumulowane prowizje całego OMS (fixed-point ×PRICE_SCALE).
    int64_t total_fees() const noexcept { return total_fees_; }
    // Runtime zmiana prowizji (#166) — harmonogram oplat moze sie zmienic w sesji
    // (tier po wolumenie, promocja). Kolejne fille uzywaja nowej stawki.
    void   set_commission(double per_share) noexcept {
        commission_fp_ = static_cast<int64_t>(per_share * PRICE_SCALE + 0.5);
    }
    double commission_per_share() const noexcept {
        return static_cast<double>(commission_fp_) / PRICE_SCALE;
    }

    // Agregaty P&L portfela (#120) — suma po wszystkich pozycjach. Fixed-point
    // (×PRICE_SCALE); to_float dla dolarow. Zastepuje reczne petle po stronie
    // wolajacego (sim/backtest sumowal pozycje sam).
    int64_t total_realized_pnl() const noexcept {
        int64_t s = 0;
        for (const auto& kv : positions_) s += kv.second.realized_pnl;
        return s;
    }
    int64_t total_net_pnl() const noexcept {     // realized - fees po portfelu
        int64_t s = 0;
        for (const auto& kv : positions_) s += kv.second.net_pnl();
        return s;
    }
    // last_reject: powód ostatniego odrzucenia submit_order (#88).
    OMSReject last_reject() const noexcept { return last_reject_; }
    // reject_count: ile zlecen odrzucono z danego powodu (#136, observability).
    uint64_t reject_count(OMSReject r) const noexcept { return reject_counts_[static_cast<int>(r)]; }
    // Liczniki operacji cyklu zycia (#151/#160, observability/dashboard).
    uint64_t total_submitted() const noexcept { return total_submitted_; }
    uint64_t total_fills()    const noexcept { return total_fills_; }
    uint64_t total_cancels()  const noexcept { return total_cancels_; }
    uint64_t total_replaces() const noexcept { return total_replaces_; }

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
    // signed_qty: zamiana (side, qty) na sygnowaną ilość. BUY → +qty, SELL → -qty.
    // Używane 3× w klasie (submit, fill, cancel) — wyciągnięte żeby nie powtarzać.
    static int32_t signed_qty(Side side, uint32_t qty) noexcept {
        return (side == Side::BUY) ? static_cast<int32_t>(qty) : -static_cast<int32_t>(qty);
    }

    // apply_fill_to_position: jeden, symetryczny model księgowania dla long,
    // short i FLIP (przejście long↔short jednym fillem). avg_price trzyma
    // średnią cenę WEJŚCIA bieżącej nogi (dodatniej lub ujemnej); realized_pnl
    // księgowany dopiero przy redukcji/zamknięciu.
    //
    //   - otwarcie / powiększenie tej samej strony → ważona średnia wejścia
    //   - redukcja / zamknięcie → realize: long zamknięty sprzedażą = (sell-avg),
    //     short zamknięty kupnem = (avg-buy), na min(|net|, fill) akcjach
    //   - flip: resztka po zamknięciu otwiera nową nogę po cenie fillu
    //
    // Wszystko fixed-point (×PRICE_SCALE); realized_pnl w dolarach×PRICE_SCALE.
    // total_cost utrzymywany jako basis bieżącej nogi (= avg×|net|) — pole
    // wystawiane przez Python binding, więc musi pozostać spójne.
    static void apply_fill_to_position(Position& pos, int32_t signed_fill,
                                       int64_t fill_qty, int64_t fill_price) noexcept {
        const int32_t old_net = pos.net_qty;
        const int32_t new_net = old_net + signed_fill;

        const bool same_dir = (old_net == 0) || ((old_net > 0) == (signed_fill > 0));
        if (same_dir) {
            // Ważona średnia wejścia po stronie powiększanej nogi.
            const int64_t old_abs = std::abs(old_net);
            const int64_t tot_abs = old_abs + fill_qty;
            pos.avg_price = (pos.avg_price * old_abs + fill_price * fill_qty + tot_abs / 2) / tot_abs;
            pos.net_qty   = new_net;
        } else {
            // Redukcja/zamknięcie/flip — realize na zamykanej części.
            const int64_t close_qty = std::min<int64_t>(std::abs(old_net), fill_qty);
            if (old_net > 0) pos.realized_pnl += close_qty * (fill_price - pos.avg_price);
            else             pos.realized_pnl += close_qty * (pos.avg_price - fill_price);
            pos.net_qty = new_net;
            if (new_net == 0) {
                pos.avg_price = 0;                       // płasko — czysta średnia
            } else if ((old_net > 0) != (new_net > 0)) {
                pos.avg_price = fill_price;              // flip — nowa noga po cenie fillu
            }
            // inaczej: ta sama strona, tylko zmniejszona → avg_price bez zmian
        }
        pos.total_cost = pos.avg_price * std::abs(pos.net_qty);
    }
};
