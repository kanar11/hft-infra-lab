#include <iostream>
#include <map>
#include <chrono>

struct Order {
    int id;
    double price;
    int quantity;
    bool is_buy;
};

class OrderBook {
    std::map<double, int, std::greater<double>> bids;  // highest first
    std::map<double, int> asks;  // lowest first

public:
    void add_order(const Order& order) {
        if (order.is_buy) {
            bids[order.price] += order.quantity;
        } else {
            asks[order.price] += order.quantity;
        }
        try_match();
    }

    void try_match() {
        while (!bids.empty() && !asks.empty()) {
            auto best_bid = bids.begin();
            auto best_ask = asks.begin();

            if (best_bid->first >= best_ask->first) {
                int fill_qty = std::min(best_bid->second, best_ask->second);
                std::cout << "TRADE: " << fill_qty << " @ " 
                          << best_ask->first << std::endl;

                best_bid->second -= fill_qty;
                best_ask->second -= fill_qty;

                if (best_bid->second == 0) bids.erase(best_bid);
                if (best_ask->second == 0) asks.erase(best_ask);
            } else {
                break;
            }
        }
    }

    void print_book() {
        std::cout << "\n=== ORDER BOOK ===" << std::endl;
        std::cout << "--- ASKS ---" << std::endl;
        for (auto it = asks.rbegin(); it != asks.rend(); ++it)
            std::cout << "  " << it->first << " x " << it->second << std::endl;
        std::cout << "--- BIDS ---" << std::endl;
        for (auto& [price, qty] : bids)
            std::cout << "  " << price << " x " << qty << std::endl;
    }
};

int main() {
    OrderBook book;

    auto start = std::chrono::high_resolution_clock::now();

    book.add_order({1, 100.50, 10, true});
    book.add_order({2, 100.30, 5, true});
    book.add_order({3, 101.00, 8, false});
    book.add_order({4, 100.80, 12, false});
    book.print_book();

    book.add_order({5, 100.80, 15, true});
    book.print_book();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    std::cout << "\nProcessing time: " << duration.count() << " ns" << std::endl;

    return 0;
}
