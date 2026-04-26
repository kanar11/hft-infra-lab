/*
 * Order book v1 — minimal price-level matching.
 *
 * Aggregates qty per price level. No per-order tracking (see orderbook_v2.cpp
 * for that). Intended as the simplest possible reference implementation.
 *
 * Prices are fixed-point ticks (1 tick = $0.01).
 */

#include <iostream>
#include <map>
#include <chrono>
#include <cstdint>
#include <algorithm>

using Price = std::int64_t;

static Price to_ticks(double p)  { return static_cast<Price>(p * 100 + 0.5); }
static double to_double(Price p) { return p / 100.0; }

struct Order {
    int   id;
    Price price;
    int   quantity;
    bool  is_buy;
};

class OrderBook {
    std::map<Price, int, std::greater<Price>> bids;  // highest first
    std::map<Price, int>                       asks;  // lowest first
    int trades = 0;

public:
    void add_order(const Order& order) noexcept {
        if (order.is_buy) bids[order.price] += order.quantity;
        else              asks[order.price] += order.quantity;
        try_match();
    }

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
        for (const auto& [price, qty] : bids)
            std::cout << "  " << to_double(price) << " x " << qty << std::endl;
        std::cout << "Trades: " << trades << std::endl;
    }
};

int main() {
    OrderBook book;
    auto start = std::chrono::high_resolution_clock::now();

    book.add_order({1, to_ticks(100.50), 10, true});
    book.add_order({2, to_ticks(100.30), 5,  true});
    book.add_order({3, to_ticks(101.00), 8,  false});
    book.add_order({4, to_ticks(100.80), 12, false});
    book.print_book();

    book.add_order({5, to_ticks(100.80), 15, true});
    book.print_book();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    std::cout << "\nProcessing time: " << duration.count() << " ns" << std::endl;
    return 0;
}
