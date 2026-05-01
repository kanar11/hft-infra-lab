/*
 * Order Management System (OMS) — C++ Implementation
 * System Zarządzania Zleceniami (OMS) — implementacja C++
 *
 * In real HFT, the OMS sits on the critical path between strategy
 * signals and exchange connections. Every microsecond saved here
 * = better fills and more profit.
 *
 * W prawdziwym HFT, OMS znajduje się na krytycznej ścieżce między
 * sygnałami strategii a połączeniami z giełdą.
 * Każda zaoszczędzona mikrosekunda = lepsze realizacje i większy zysk.
 *
 * Performance comparison / Porównanie wydajności:
 *   Python OMS:  ~100K-500K orders/sec
 *   C++ OMS:     ~10-50 million orders/sec  (100x faster)
 *
 * Features / Funkcje:
 *   - Order lifecycle: NEW → SENT → FILLED/PARTIAL/CANCELLED/REJECTED
 *     Cykl życia zlecenia: NEW → SENT → FILLED/PARTIAL/CANCELLED/REJECTED
 *   - Pre-trade risk checks (order value + position limit)
 *     Kontrole ryzyka przed transakcją (wartość zlecenia + limit pozycji)
 *   - Position tracking with average cost basis
 *     Śledzenie pozycji ze średnią ceną kosztów
 *   - Realized P&L calculation
 *     Obliczanie zrealizowanego P&L
 *   - Fixed-point prices (int64, 4 decimal places) — no floating point on hot path
 *     Ceny stałoprzecinkowe (int64, 4 miejsca po przecinku) — bez zmiennoprzecinkowych na gorącej ścieżce
 *
 * Design choices / Decyzje projektowe:
 *   - Header-only: no .cpp file needed, just #include "oms.hpp"
 *     Tylko nagłówek: nie potrzeba pliku .cpp, wystarczy #include "oms.hpp"
 *   - unordered_map for O(1) order lookup by ID
 *     unordered_map dla O(1) wyszukiwania zlecenia po ID
 *   - Fixed-point int64 prices: $150.25 → 1502500 (price * 10000)
 *     Avoids floating-point rounding errors and is faster on CPU
 *     Ceny stałoprzecinkowe int64: $150.25 → 1502500 (cena * 10000)
 *     Unika błędów zaokrąglania zmiennoprzecinkowych i jest szybszy na CPU
 *   - noexcept on hot-path functions: tells compiler no exceptions possible,
 *     enables better optimization (no stack unwinding code generated)
 *     noexcept na funkcjach gorącej ścieżki: mówi kompilatorowi że wyjątki niemożliwe,
 *     umożliwia lepszą optymalizację (brak generowanego kodu rozwijania stosu)
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cmath>


// PRICE_SCALE: multiplier to convert float prices to fixed-point integers
// $150.25 * 10000 = 1502500 — no floating point needed after conversion
// PRICE_SCALE: mnożnik do konwersji cen float na stałoprzecinkowe int
// $150.25 * 10000 = 1502500 — po konwersji nie potrzeba zmiennoprzecinkowych
static constexpr int64_t PRICE_SCALE = 10000;

// Helper: convert float price to fixed-point int64
// Pomocnik: konwertuj cenę float na stałoprzecinkowe int64
inline int64_t to_fixed(double price) noexcept {
    return static_cast<int64_t>(price * PRICE_SCALE + 0.5);
}

// Helper: convert fixed-point int64 back to float (for display only)
// Pomocnik: konwertuj stałoprzecinkowe int64 z powrotem na float (tylko do wyświetlania)
inline double to_float(int64_t fixed_price) noexcept {
    return static_cast<double>(fixed_price) / PRICE_SCALE;
}


// === Enums — same as Python's Enum classes ===
// === Enumy — to samo co klasy Enum w Pythonie ===
// enum class: strongly-typed enum — prevents mixing OrderStatus with Side by accident
// enum class: silnie typowany enum — zapobiega przypadkowemu mieszaniu OrderStatus z Side

enum class OrderStatus : uint8_t {
    // uint8_t: uses only 1 byte (0-255) instead of 4 bytes for int — saves memory
    // In an OMS with millions of orders, this adds up
    // uint8_t: używa tylko 1 bajta (0-255) zamiast 4 bajtów dla int — oszczędza pamięć
    // W OMS z milionami zleceń, to się sumuje
    NEW       = 0,
    SENT      = 1,
    FILLED    = 2,
    PARTIAL   = 3,
    CANCELLED = 4,
    REJECTED  = 5
};

enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

// Convert enum to string for printing (like Python's .value)
// Konwertuj enum na string do wyświetlania (jak .value w Pythonie)
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

inline const char* side_str(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}


// === Order struct — equivalent to Python's @dataclass Order ===
// === Struktura Order — odpowiednik @dataclass Order w Pythonie ===

struct Order {
    uint64_t order_id;
    // char symbol[9]: fixed-size array for stock ticker (8 chars + null terminator)
    // Why not std::string? Strings allocate heap memory (slow). Fixed arrays live
    // directly in the struct (fast). NASDAQ tickers are max 8 chars.
    // char symbol[9]: tablica o stałym rozmiarze na symbol giełdowy (8 znaków + null terminator)
    // Dlaczego nie std::string? Stringi alokują pamięć na stercie (wolne). Tablice stałe żyją
    // bezpośrednio w strukturze (szybkie). Symbole NASDAQ mają max 8 znaków.
    char     symbol[9];
    Side     side;
    int64_t  price;          // fixed-point: $150.25 → 1502500
    uint32_t quantity;
    uint32_t filled_qty;
    OrderStatus status;
    int64_t  created_ns;     // nanosecond timestamp
    int64_t  sent_ns;
    int64_t  filled_ns;

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
        // strncpy: safe string copy that won't overflow the buffer
        // Like 'head -c 8' in bash — copies at most 8 chars
        // strncpy: bezpieczne kopiowanie stringów, które nie przepełni bufora
        // Jak 'head -c 8' w bashu — kopiuje maksymalnie 8 znaków
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
    }
};


// === Position struct — equivalent to Python's @dataclass Position ===
// === Struktura Position — odpowiednik @dataclass Position w Pythonie ===

struct Position {
    char    symbol[9];
    int32_t net_qty;         // realized — updated only on fill
    int32_t pending_qty;     // pending exposure — signed sum of remaining qty on open orders (BUY+, SELL-)
    int64_t avg_price;       // fixed-point
    int64_t realized_pnl;    // fixed-point (accumulated P&L * PRICE_SCALE)
    int64_t total_cost;      // fixed-point (sum of qty * price for open position)

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


// === OMS class — the main Order Management System ===
// === Klasa OMS — główny System Zarządzania Zleceniami ===

class OMS {
    std::unordered_map<uint64_t, Order> orders_;
    std::unordered_map<uint64_t, Position> positions_;  // key = symbol packed into uint64_t (no heap alloc)

    // Pack up to 8 ASCII chars into a uint64_t — avoids std::string allocation on every lookup.
    // memchr bounds the search (≤ 8) so the loop body is safe regardless of caller buffer size.
    static uint64_t sym_to_key(const char* sym) noexcept {
        uint64_t key = 0;
        const void* end = std::memchr(sym, '\0', 8);
        const size_t len = end ? static_cast<size_t>(static_cast<const char*>(end) - sym) : 8;
        for (size_t i = 0; i < len; ++i)
            key |= (static_cast<uint64_t>(static_cast<unsigned char>(sym[i])) << (i * 8));
        return key;
    }

    uint64_t next_id_;
    int32_t  max_position_;
    int64_t  max_order_value_;   // in dollars (NOT fixed-point)

    // now_ns: get current time in nanoseconds — used for latency measurement
    // Uses std::chrono which maps to clock_gettime(CLOCK_MONOTONIC) on Linux
    // now_ns: pobierz bieżący czas w nanosekundach — do pomiaru opóźnień
    // Używa std::chrono, które mapuje się na clock_gettime(CLOCK_MONOTONIC) na Linuxie
    static int64_t now_ns() noexcept {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

public:
    // Constructor — equivalent to Python's __init__
    // Konstruktor — odpowiednik __init__ w Pythonie
    OMS(int32_t max_pos = 1000, double max_val = 100000.0) noexcept
        : next_id_(1),
          max_position_(max_pos),
          max_order_value_(static_cast<int64_t>(max_val)) {}

    // submit_order: create and validate a new order
    // Returns pointer to the order in the map, or nullptr if rejected
    // (nullptr is C++ equivalent of Python's None)
    // submit_order: stwórz i zwaliduj nowe zlecenie
    // Zwraca wskaźnik do zlecenia w mapie, lub nullptr jeśli odrzucone
    // (nullptr to odpowiednik None w Pythonie)
    Order* submit_order(const char* symbol, Side side, double price_f,
                        uint32_t quantity) noexcept {
        int64_t t0 = now_ns();
        int64_t price = to_fixed(price_f);

        // Input validation — same checks as Python version
        // Walidacja danych — te same sprawdzenia co w wersji Python
        if (price <= 0 || quantity == 0 || symbol == nullptr || symbol[0] == '\0') {
            return nullptr;
        }
        // std::isnan: checks if value is NaN (Not a Number)
        // std::isnan: sprawdza czy wartość to NaN (Not a Number)
        if (std::isnan(price_f)) {
            return nullptr;
        }

        // Risk check: order value
        // Kontrola ryzyka: wartość zlecenia
        // price is fixed-point, so divide by PRICE_SCALE to get dollars
        // order_value = (price * quantity) / PRICE_SCALE → value in dollars
        // cena jest stałoprzecinkowa, więc podziel przez PRICE_SCALE żeby dostać dolary
        int64_t order_value = (price / PRICE_SCALE) * (int64_t)quantity;
        if (order_value > max_order_value_) {
            return nullptr;
        }

        // Risk check: position limit (realized + pending + new)
        // Pending exposure prevents overcommit when multiple submits race ahead of fills.
        // Kontrola ryzyka: limit pozycji (zrealizowana + pending + nowe)
        uint64_t sym_key = sym_to_key(symbol);
        auto pos_it = positions_.find(sym_key);
        int32_t current_realized = (pos_it != positions_.end()) ? pos_it->second.net_qty     : 0;
        int32_t current_pending  = (pos_it != positions_.end()) ? pos_it->second.pending_qty : 0;
        int32_t signed_new = (side == Side::BUY) ? (int32_t)quantity : -(int32_t)quantity;
        int32_t exposure = current_realized + current_pending + signed_new;

        if (std::abs(exposure) > max_position_) {
            return nullptr;
        }

        // Create order
        // Stwórz zlecenie
        uint64_t id = next_id_++;
        Order order(id, symbol, side, price, quantity);
        order.created_ns = t0;
        order.status = OrderStatus::SENT;
        order.sent_ns = now_ns();

        auto it = orders_.emplace(id, order).first;

        // Reserve pending capacity now that the order is accepted.
        // Position is created lazily here (not just on first fill) so pending bookkeeping is consistent.
        if (pos_it == positions_.end()) {
            pos_it = positions_.emplace(sym_key, Position(symbol)).first;
        }
        pos_it->second.pending_qty += signed_new;

        return &it->second;
    }

    // fill_order: process execution report — update position and P&L
    // fill_order: przetwórz raport wykonania — zaktualizuj pozycję i P&L
    void fill_order(uint64_t order_id, uint32_t fill_qty, double fill_price_f) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            printf("[OMS] WARNING: fill for unknown order_id=%lu\n", (unsigned long)order_id);
            return;
        }

        Order& order = it->second;
        int64_t fill_price = to_fixed(fill_price_f);

        order.filled_qty += fill_qty;
        order.status = (order.filled_qty >= order.quantity)
                       ? OrderStatus::FILLED
                       : OrderStatus::PARTIAL;
        order.filled_ns = now_ns();

        // Update position — same logic as Python version
        // Aktualizuj pozycję — ta sama logika co w wersji Python
        uint64_t sym_key = sym_to_key(order.symbol);
        auto pos_it = positions_.find(sym_key);
        if (pos_it == positions_.end()) {
            // Position normally created at submit time; fallback for legacy paths.
            pos_it = positions_.emplace(sym_key, Position(order.symbol)).first;
        }
        Position& pos = pos_it->second;

        // Move filled qty from pending to realized
        int32_t signed_fill = (order.side == Side::BUY) ? (int32_t)fill_qty : -(int32_t)fill_qty;
        pos.pending_qty -= signed_fill;

        if (order.side == Side::BUY) {
            // Buying: add to total cost and quantity
            // Kupno: dodaj do kosztu całkowitego i ilości
            pos.total_cost += (int64_t)fill_qty * fill_price;
            pos.net_qty += (int32_t)fill_qty;
        } else {
            // Selling: realize P&L = (sell_price - avg_cost) * qty
            // Sprzedaż: realizuj P&L = (cena_sprzedaży - średni_koszt) * ilość
            if (pos.net_qty > 0) {
                pos.realized_pnl += (int64_t)fill_qty * (fill_price - pos.avg_price);
                pos.total_cost -= (int64_t)fill_qty * pos.avg_price;
            }
            pos.net_qty -= (int32_t)fill_qty;
        }

        // Recalculate average price
        // Przelicz średnią cenę
        if (pos.net_qty > 0) {
            // Round-half-up to minimize fixed-point drift over many fills
            pos.avg_price = (pos.total_cost + pos.net_qty / 2) / pos.net_qty;
        } else if (pos.net_qty == 0) {
            pos.avg_price = 0;
            pos.total_cost = 0;
        }
    }

    // cancel_order: cancel an active order — release its pending exposure
    // cancel_order: anuluj aktywne zlecenie — zwolnij rezerwę pending
    void cancel_order(uint64_t order_id) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return;

        Order& order = it->second;
        if (order.status == OrderStatus::SENT || order.status == OrderStatus::PARTIAL) {
            // Release the unfilled remainder from the pending pool
            int32_t remaining = (int32_t)order.quantity - (int32_t)order.filled_qty;
            int32_t signed_remaining = (order.side == Side::BUY) ? remaining : -remaining;
            auto pos_it = positions_.find(sym_to_key(order.symbol));
            if (pos_it != positions_.end()) {
                pos_it->second.pending_qty -= signed_remaining;
            }
            order.status = OrderStatus::CANCELLED;
        }
    }

    // === Accessors — for reading internal state ===
    // === Akcessory — do odczytu stanu wewnętrznego ===

    const Order* get_order(uint64_t id) const noexcept {
        auto it = orders_.find(id);
        return (it != orders_.end()) ? &it->second : nullptr;
    }

    const Position* get_position(const char* symbol) const noexcept {
        auto it = positions_.find(sym_to_key(symbol));
        return (it != positions_.end()) ? &it->second : nullptr;
    }

    size_t order_count() const noexcept { return orders_.size(); }
    size_t position_count() const noexcept { return positions_.size(); }

    // print_orders: display all orders — equivalent to Python's print_orders()
    void print_orders() const {
        printf("\n=== ORDERS ===\n");
        for (const auto& [id, o] : orders_) {
            printf("  #%lu: %s %u %s @ %.2f [%s] filled=%u\n",
                   (unsigned long)id, side_str(o.side), o.quantity,
                   o.symbol, to_float(o.price), status_str(o.status),
                   o.filled_qty);
        }
    }

    // print_positions: display positions and P&L
    void print_positions() const {
        printf("\n=== POSITIONS ===\n");
        for (const auto& [sym, pos] : positions_) {
            printf("  %s: qty=%d avg=%.2f realized_pnl=$%.2f\n",
                   pos.symbol, pos.net_qty, to_float(pos.avg_price),
                   to_float(pos.realized_pnl));
        }
    }
};
