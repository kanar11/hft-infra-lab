#include <iostream>
#include <map>
#include <chrono>
#include <vector>
#include <random>

struct Order {
    int id;
    double price;
    int quantity;
    bool is_buy;
};

class OrderBook {
    std::map<double, int, std::greater<double>> bids;
    std::map<double, int> asks;
    int trades = 0;

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
                int fill = std::min(best_bid->second, best_ask->second);
                trades++;
                best_bid->second -= fill;
                best_ask->second -= fill;
                if (best_bid->second == 0) bids.erase(best_bid);
                if (best_ask->second == 0) asks.erase(best_ask);
            } else break;
        }
    }

    int get_trades() { return trades; }
    int book_depth() { return bids.size() + asks.size(); }
};

int main() {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> price_dist(99.0, 101.0);
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
            orders.push_back({i, 
                std::round(price_dist(rng) * 100) / 100,
                qty_dist(rng),
                side_dist(rng) == 1});
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (auto& o : orders) {
            book.add_order(o);
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        double ops = n / (ms / 1000.0);

        std::cout << n << "\t\t" 
                  << ms << "\t\t"
                  << static_cast<int>(ops) << "\t\t"
                  << book.get_trades() << "\t"
                  << book.book_depth() << std::endl;
    }

    return 0;
}
