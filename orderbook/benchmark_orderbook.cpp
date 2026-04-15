#include <iostream>
#include <map>
#include <chrono>
#include <vector>
#include <random>
#include <cstdint>

// Fixed-point price: stored as integer ticks (1 tick = 0.01)
// Stała cena - przechowywana jako liczba całkowita (1 tick = 0,01)
using Price = std::int64_t;

struct Order {
    int id;
    Price price;
    int quantity;
    bool is_buy;
};

class OrderBook {
    // NOTE: std::map allocates per-node on insert (not ideal for HFT).
    // UWAGA: std::map przydziela pamięć dla każdego węzła przy wstawianiu (nieidealne dla HFT)
    // Production systems use pre-allocated flat arrays indexed by price level.
    // Systemy produkcyjne używają wstępnie przydzielonych płaskich tablic indeksowanych poziomem ceny
    // Using std::map here for clarity; see lockfree/spsc_queue.cpp for
    // Używamy std::map tutaj dla jasności; zobacz lockfree/spsc_queue.cpp na przykład
    // an example of pre-allocated, cache-friendly data structures.
    // wstępnie przydzielonych, przyjaznych dla cache'u struktur danych
    std::map<Price, int, std::greater<Price>> bids;
    std::map<Price, int> asks;
    std::uint64_t trades = 0;

public:
    void add_order(const Order& order) noexcept {
        if (order.is_buy) {
            bids[order.price] += order.quantity;
        } else {
            asks[order.price] += order.quantity;
        }
        try_match();
    }

    void try_match() noexcept {
        while (!bids.empty() && !asks.empty()) {
            auto best_bid = bids.begin();
            auto best_ask = asks.begin();
            if (best_bid->first >= best_ask->first) {
                int fill = std::min(best_bid->second, best_ask->second);
                trades++;
                best_bid->second -= fill;
                best_ask->second -= fill;
                if (best_bid->second == 0) bids.erase(best_bid);
                if (best_ask->second == 0) asks.erase(best_ask);
            } else break;
        }
    }

    std::uint64_t get_trades() const noexcept { return trades; }
    std::size_t book_depth() const noexcept { return bids.size() + asks.size(); }
};

int main() {
    std::mt19937 rng(42);
    // Price range 99.00-101.00 in ticks (9900-10100)
    // Zakres cen 99,00-101,00 w tickach (9900-10100)
    std::uniform_int_distribution<Price> price_dist(9900, 10100);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::vector<int> test_sizes = {1000, 10000, 100000, 1000000};

    std::cout << "=== Order Book Benchmark ===" << std::endl;
    std::cout << "Orders\t\tTime(ms)\tOrders/sec\tTrades\tDepth" << std::endl;
    std::cout << "------\t\t--------\t----------\t------\t-----" << std::endl;

    for (int n : test_sizes) {
        OrderBook book;
        std::vector<Order> orders;
        orders.reserve(n);

        for (int i = 0; i < n; i++) {
            orders.push_back({i, price_dist(rng), qty_dist(rng), side_dist(rng) == 1});
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (const auto& o : orders) {
            book.add_order(o);
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        double ops = n / (ms / 1000.0);

        std::cout << n << "\t\t"
                  << ms << "\t\t"
                  << static_cast<std::uint64_t>(ops) << "\t\t"
                  << book.get_trades() << "\t"
                  << book.book_depth() << std::endl;
    }

    return 0;
}
