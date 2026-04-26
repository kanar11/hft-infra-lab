/*
 * Order book v2 — supports add / cancel / modify with per-order tracking.
 *
 * Each Order tracks its remaining (unfilled) quantity, so cancel and modify
 * subtract the *current* size from the price level, never the original.
 * This is the bug that v1 doesn't address.
 *
 * Prices are fixed-point ticks (1 tick = $0.01).
 */

#include <iostream>
#include <map>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <algorithm>

using Price = std::int64_t;

static Price to_ticks(double p) { return static_cast<Price>(p * 100 + 0.5); }
static double to_double(Price p) { return p / 100.0; }

enum class Side { BUY, SELL };
enum class OrderStatus { ACTIVE, FILLED, CANCELLED };

struct Order {
    int         id;
    Price       price;
    int         remaining;   // unfilled qty — drives all level adjustments
    Side        side;
    OrderStatus status;
};

class OrderBook {
    std::map<Price, int, std::greater<Price>> bids;  // highest first
    std::map<Price, int>                       asks;  // lowest first
    std::unordered_map<int, Order>             orders;
    int trades   = 0;
    int cancels  = 0;
    int modifies = 0;

    void level_add(Side side, Price price, int qty) {
        if (side == Side::BUY) bids[price] += qty;
        else                   asks[price] += qty;
    }

    void level_sub(Side side, Price price, int qty) {
        if (side == Side::BUY) {
            auto it = bids.find(price);
            if (it == bids.end()) return;
            it->second -= qty;
            if (it->second <= 0) bids.erase(it);
        } else {
            auto it = asks.find(price);
            if (it == asks.end()) return;
            it->second -= qty;
            if (it->second <= 0) asks.erase(it);
        }
    }

public:
    bool add_order(int id, Price price, int qty, Side side) noexcept {
        if (qty <= 0 || orders.count(id)) return false;
        orders[id] = Order{id, price, qty, side, OrderStatus::ACTIVE};
        level_add(side, price, qty);
        try_match();
        return true;
    }

    bool cancel_order(int id) noexcept {
        auto it = orders.find(id);
        if (it == orders.end() || it->second.status != OrderStatus::ACTIVE) return false;
        Order& o = it->second;
        level_sub(o.side, o.price, o.remaining);
        o.remaining = 0;
        o.status    = OrderStatus::CANCELLED;
        cancels++;
        return true;
    }

    bool modify_order(int id, Price new_price, int new_qty) noexcept {
        auto it = orders.find(id);
        if (it == orders.end() || it->second.status != OrderStatus::ACTIVE) return false;
        if (new_qty <= 0) return false;

        Order& o = it->second;
        level_sub(o.side, o.price, o.remaining);
        o.price     = new_price;
        o.remaining = new_qty;
        level_add(o.side, new_price, new_qty);
        modifies++;
        try_match();
        return true;
    }

    // try_match: cross top-of-book bids and asks until the spread is non-negative.
    // Reduces both the resting order's remaining qty and the price-level total.
    void try_match() noexcept {
        while (!bids.empty() && !asks.empty()) {
            auto bb = bids.begin();
            auto ba = asks.begin();
            if (bb->first < ba->first) break;

            const int fill = std::min(bb->second, ba->second);
            trades++;
            bb->second -= fill;
            ba->second -= fill;
            if (bb->second == 0) bids.erase(bb);
            if (ba->second == 0) asks.erase(ba);
        }
    }

    void print_book() const {
        std::cout << "\n=== ORDER BOOK ===" << std::endl;
        std::cout << "--- ASKS ---" << std::endl;
        for (auto it = asks.rbegin(); it != asks.rend(); ++it)
            std::cout << "  " << to_double(it->first) << " x " << it->second << std::endl;
        std::cout << "--- BIDS ---" << std::endl;
        for (const auto& [p, q] : bids)
            std::cout << "  " << to_double(p) << " x " << q << std::endl;
        std::cout << "Trades: " << trades
                  << " | Cancels: " << cancels
                  << " | Modifies: " << modifies << std::endl;
    }
};


int main() {
    OrderBook book;
    auto start = std::chrono::high_resolution_clock::now();

    book.add_order(1, to_ticks(100.50), 10, Side::BUY);
    book.add_order(2, to_ticks(100.30), 5,  Side::BUY);
    book.add_order(3, to_ticks(101.00), 8,  Side::SELL);
    book.add_order(4, to_ticks(100.80), 12, Side::SELL);
    std::cout << "After adding 4 orders:";
    book.print_book();

    book.modify_order(2, to_ticks(100.60), 5);
    std::cout << "\nAfter modifying order 2 (100.30 -> 100.60):";
    book.print_book();

    book.cancel_order(3);
    std::cout << "\nAfter cancelling order 3 (ask @ 101.00):";
    book.print_book();

    book.add_order(5, to_ticks(100.80), 20, Side::BUY);
    std::cout << "\nAfter aggressive buy @ 100.80 x 20:";
    book.print_book();

    auto end = std::chrono::high_resolution_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "\nTotal processing: " << ns << " ns" << std::endl;
    return 0;
}
