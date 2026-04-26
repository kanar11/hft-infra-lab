/*
 * Order Management System (OMS) — header-only.
 *
 * Lifecycle: NEW -> SENT -> FILLED | PARTIAL | CANCELLED | REJECTED
 *
 * Pipeline position: Strategy -> Router -> Risk -> [OMS] -> Exchange
 *
 * Hot-path discipline:
 *   - Fixed-point int64 prices (PRICE_SCALE = 10000) — no FP on hot path.
 *   - char[9] symbol keys — no std::string allocation per call.
 *   - noexcept everywhere on the hot path.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cmath>


// PRICE_SCALE: $150.25 -> 1502500
static constexpr int64_t PRICE_SCALE = 10000;

inline int64_t to_fixed(double price) noexcept {
    return static_cast<int64_t>(price * PRICE_SCALE + 0.5);
}

inline double to_float(int64_t fixed_price) noexcept {
    return static_cast<double>(fixed_price) / PRICE_SCALE;
}


enum class OrderStatus : uint8_t {
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


// SymbolKey: 8-byte ticker stored as uint64 — hashable and comparable in a single op,
// no heap allocation. NASDAQ tickers are <= 8 ASCII chars; we pack-pad with NUL.
struct SymbolKey {
    uint64_t v;

    SymbolKey() noexcept : v(0) {}

    explicit SymbolKey(const char* sym) noexcept : v(0) {
        // Copy up to 8 bytes; remaining bytes stay zero (zero-padded).
        char buf[8] = {0};
        for (int i = 0; i < 8 && sym[i]; ++i) buf[i] = sym[i];
        std::memcpy(&v, buf, 8);
    }

    bool operator==(const SymbolKey& o) const noexcept { return v == o.v; }
};

struct SymbolKeyHash {
    size_t operator()(const SymbolKey& k) const noexcept {
        // Mix bits — uint64 already a good hash for short tickers.
        uint64_t x = k.v;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};


struct Order {
    uint64_t    order_id;
    char        symbol[9];   // 8 + NUL
    Side        side;
    int64_t     price;       // fixed-point
    uint32_t    quantity;
    uint32_t    filled_qty;
    OrderStatus status;
    int64_t     created_ns;
    int64_t     sent_ns;
    int64_t     filled_ns;

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


struct Position {
    char    symbol[9];
    int32_t net_qty;       // can be negative (short)
    int64_t avg_price;     // fixed-point; tracks open-side cost basis
    int64_t realized_pnl;  // fixed-point P&L

    Position() noexcept
        : net_qty(0), avg_price(0), realized_pnl(0) { symbol[0] = '\0'; }

    explicit Position(const char* sym) noexcept
        : net_qty(0), avg_price(0), realized_pnl(0) {
        std::strncpy(symbol, sym, 8);
        symbol[8] = '\0';
    }
};


class OMS {
    std::unordered_map<uint64_t, Order> orders_;
    std::unordered_map<SymbolKey, Position, SymbolKeyHash> positions_;

    uint64_t next_id_;
    int32_t  max_position_;
    int64_t  max_order_value_;  // dollars (NOT fixed-point)

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

public:
    OMS(int32_t max_pos = 1000, double max_val = 100000.0) noexcept
        : next_id_(1),
          max_position_(max_pos),
          max_order_value_(static_cast<int64_t>(max_val)) {}

    // submit_order: validate and register a new order. Returns nullptr on rejection.
    Order* submit_order(const char* symbol, Side side, double price_f,
                        uint32_t quantity) noexcept {
        int64_t t0 = now_ns();

        if (symbol == nullptr || symbol[0] == '\0' || quantity == 0) return nullptr;
        if (std::isnan(price_f) || price_f <= 0.0) return nullptr;

        int64_t price = to_fixed(price_f);
        if (price <= 0) return nullptr;

        // Risk: order notional value (multiply first, divide last — preserves precision).
        int64_t order_value = (price * static_cast<int64_t>(quantity)) / PRICE_SCALE;
        if (order_value > max_order_value_) return nullptr;

        // Risk: per-symbol position limit.
        SymbolKey key(symbol);
        auto pos_it = positions_.find(key);
        int32_t current_qty = (pos_it != positions_.end()) ? pos_it->second.net_qty : 0;
        int32_t delta = (side == Side::BUY)
                        ? static_cast<int32_t>(quantity)
                        : -static_cast<int32_t>(quantity);
        if (std::abs(current_qty + delta) > max_position_) return nullptr;

        uint64_t id = next_id_++;
        Order order(id, symbol, side, price, quantity);
        order.created_ns = t0;
        order.status     = OrderStatus::SENT;
        order.sent_ns    = now_ns();

        auto [it, _] = orders_.emplace(id, order);
        return &it->second;
    }

    // fill_order: process an execution report — update position, average cost, realized P&L.
    // Correctly handles fills that flip a position (long -> short or short -> long).
    void fill_order(uint64_t order_id, uint32_t fill_qty, double fill_price_f) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end() || fill_qty == 0) return;

        Order& order = it->second;
        int64_t fill_price = to_fixed(fill_price_f);

        order.filled_qty += fill_qty;
        order.status = (order.filled_qty >= order.quantity)
                       ? OrderStatus::FILLED
                       : OrderStatus::PARTIAL;
        order.filled_ns = now_ns();

        SymbolKey key(order.symbol);
        auto pos_it = positions_.find(key);
        if (pos_it == positions_.end()) {
            auto [new_it, _] = positions_.emplace(key, Position(order.symbol));
            pos_it = new_it;
        }
        Position& pos = pos_it->second;

        int32_t signed_qty = (order.side == Side::BUY)
                             ? static_cast<int32_t>(fill_qty)
                             : -static_cast<int32_t>(fill_qty);

        // Three cases: opening/extending, closing partially, flipping.
        if (pos.net_qty == 0 || (pos.net_qty > 0) == (signed_qty > 0)) {
            // Opening or extending in the same direction: update weighted avg cost.
            int64_t new_qty = static_cast<int64_t>(pos.net_qty) + signed_qty;
            int64_t old_abs = std::abs(pos.net_qty);
            int64_t add_abs = std::abs(signed_qty);
            pos.avg_price = (pos.avg_price * old_abs + fill_price * add_abs)
                            / (old_abs + add_abs);
            pos.net_qty = static_cast<int32_t>(new_qty);
        } else {
            // Reducing or flipping: realize P&L on the closed portion.
            int32_t closing = std::min(std::abs(signed_qty), std::abs(pos.net_qty));
            int64_t pnl_per_share = (pos.net_qty > 0)
                                    ? (fill_price - pos.avg_price)   // long: sell - cost
                                    : (pos.avg_price - fill_price);  // short: cost - cover
            pos.realized_pnl += pnl_per_share * closing;

            int32_t new_net = pos.net_qty + signed_qty;
            if (new_net == 0) {
                pos.avg_price = 0;
            } else if ((new_net > 0) != (pos.net_qty > 0)) {
                // Flipped through zero: leftover opens a new position at the fill price.
                pos.avg_price = fill_price;
            }
            // else: still on same side, smaller — avg_price unchanged.
            pos.net_qty = new_net;
        }
    }

    void cancel_order(uint64_t order_id) noexcept {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return;
        Order& order = it->second;
        if (order.status == OrderStatus::SENT || order.status == OrderStatus::PARTIAL) {
            order.status = OrderStatus::CANCELLED;
        }
    }

    const Order* get_order(uint64_t id) const noexcept {
        auto it = orders_.find(id);
        return (it != orders_.end()) ? &it->second : nullptr;
    }

    const Position* get_position(const char* symbol) const noexcept {
        auto it = positions_.find(SymbolKey(symbol));
        return (it != positions_.end()) ? &it->second : nullptr;
    }

    size_t order_count() const noexcept { return orders_.size(); }
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
        for (const auto& [_, pos] : positions_) {
            printf("  %s: qty=%d avg=%.2f realized_pnl=$%.2f\n",
                   pos.symbol, pos.net_qty, to_float(pos.avg_price),
                   to_float(pos.realized_pnl));
        }
    }
};
