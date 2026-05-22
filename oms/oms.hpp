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

    Order() noexcept
        : order_id(0), side(Side::BUY), price(0), quantity(0),
          filled_qty(0), status(OrderStatus::NEW),
          created_ns(0), sent_ns(0), filled_ns(0) {
        symbol[0] = '\0';
    }

    Order(uint64_t id, const char* sym, Side s, int64_t px, uint32_t qty) noexcept
        : order_id(id), side(s), price(px), quantity(qty),
          filled_qty(0), status(OrderStatus::NEW),
          created_ns(0), sent_ns(0), filled_ns(0) {
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
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
    int64_t realized_pnl;    // skumulowany P&L × PRICE_SCALE
    int64_t total_cost;      // suma qty × cena dla aktualnego long'a

    Position() noexcept
        : net_qty(0), pending_qty(0), avg_price(0), realized_pnl(0), total_cost(0) {
        symbol[0] = '\0';
    }

    explicit Position(const char* sym) noexcept
        : net_qty(0), pending_qty(0), avg_price(0), realized_pnl(0), total_cost(0) {
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
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

public:
    OMS(int32_t max_pos = 1000, double max_val = 100000.0) noexcept
        : next_id_(1),
          max_position_(max_pos),
          max_order_value_(static_cast<int64_t>(max_val)) {}

    // submit_order: walidacja inputów, dwa pre-trade checki, utworzenie
    // Order'a, rezerwacja pending w Position. Zwraca pointer do zlecenia
    // w mapie (stabilny dopóki OMS żyje) lub nullptr przy odrzuceniu.
    Order* submit_order(const char* symbol, Side side, double price_f,
                        uint32_t quantity) noexcept {
        const int64_t t0    = mono_ns();
        const int64_t price = to_fixed(price_f);

        // Walidacja inputów. NaN, ujemna/zerowa cena, zero shares,
        // pusty/null symbol — odrzuć od razu.
        if (std::isnan(price_f) || price <= 0 || quantity == 0
            || symbol == nullptr || symbol[0] == '\0') {
            return nullptr;
        }

        // Pre-trade check #1: wartość zlecenia w dolarach.
        const int64_t order_value = (price / PRICE_SCALE) * static_cast<int64_t>(quantity);
        if (order_value > max_order_value_) return nullptr;

        // Pre-trade check #2: limit pozycji (realized + pending + new).
        const uint64_t sym_key  = sym_to_key(symbol);
        const int32_t  signed_n = signed_qty(side, quantity);
        auto           pos_it   = positions_.find(sym_key);
        const int32_t  cur_real = (pos_it != positions_.end()) ? pos_it->second.net_qty     : 0;
        const int32_t  cur_pend = (pos_it != positions_.end()) ? pos_it->second.pending_qty : 0;
        if (std::abs(cur_real + cur_pend + signed_n) > max_position_) return nullptr;

        // Utwórz Order, ustaw status SENT, zapisz w mapie.
        const uint64_t id = next_id_++;
        Order order(id, symbol, side, price, quantity);
        order.created_ns = t0;
        order.sent_ns    = mono_ns();
        order.status     = OrderStatus::SENT;
        auto it = orders_.emplace(id, order).first;

        // Zarezerwuj pending. Tworzymy Position leniwie tutaj (a nie dopiero
        // przy pierwszym fillu) żeby księgowanie pending było spójne na
        // każdej ścieżce.
        if (pos_it == positions_.end()) {
            pos_it = positions_.emplace(sym_key, Position(symbol)).first;
        }
        pos_it->second.pending_qty += signed_n;
        return &it->second;
    }

    // fill_order: raport wykonania z giełdy. Aktualizuje filled_qty + status
    // w Order, przepływa qty z pending do net w Position, przelicza avg_price
    // (BUY) lub realized_pnl (SELL).
    void fill_order(uint64_t order_id, uint32_t fill_qty, double fill_price_f) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: fill for unknown order_id=%lu\n", (unsigned long)order_id);
            return;
        }
        Order& order = it->second;
        const int64_t fill_price = to_fixed(fill_price_f);

        order.filled_qty += fill_qty;
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

        if (order.side == Side::BUY) {
            // Kupno: zwiększ koszt całkowity i ilość. Realizowany P&L
            // pojawi się dopiero przy SELL.
            pos.total_cost += static_cast<int64_t>(fill_qty) * fill_price;
            pos.net_qty    += static_cast<int32_t>(fill_qty);
        } else {
            // Sprzedaż long'a: realize P&L = (sell - avg_cost) * qty.
            // Dla short'a (net_qty ≤ 0) nie liczymy P&L tutaj — to wymaga
            // znanego avg_short_price, którego dla uproszczenia nie trzymamy.
            if (pos.net_qty > 0) {
                pos.realized_pnl += static_cast<int64_t>(fill_qty) * (fill_price - pos.avg_price);
                pos.total_cost   -= static_cast<int64_t>(fill_qty) * pos.avg_price;
            }
            pos.net_qty -= static_cast<int32_t>(fill_qty);
        }
        recompute_avg_price(pos);
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

    // recompute_avg_price: po każdym fillu odśwież średnią cenę long'a.
    // Round-half-up żeby zminimalizować drift fixed-point przy wielu fillach.
    // Gdy pozycja schodzi do zera — resetuj avg i total_cost (żeby kolejny
    // BUY zaczął od czystej średniej).
    static void recompute_avg_price(Position& pos) noexcept {
        if (pos.net_qty > 0) {
            pos.avg_price = (pos.total_cost + pos.net_qty / 2) / pos.net_qty;
        } else if (pos.net_qty == 0) {
            pos.avg_price  = 0;
            pos.total_cost = 0;
        }
    }
};
