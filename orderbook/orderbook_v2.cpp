#include <iostream>
#include <map>
#include <unordered_map>
#include <chrono>

enum class Side { BUY, SELL };
enum class OrderStatus { ACTIVE, FILLED, CANCELLED, MODIFIED };

struct Order {
    int id;
    double price;
    int quantity;
    Side side;
    OrderStatus status;
};

class OrderBook {
    std::map<double, int, std::greater<double>> bids;
    std::map<double, int> asks;
    std::unordered_map<int, Order> orders;
    int trades = 0;
    int cancels = 0;
    int modifies = 0;

public:
    bool add_order(int id, double price, int qty, Side side) {
        orders[id] = {id, price, qty, side, OrderStatus::ACTIVE};
        if (side == Side::BUY) bids[price] += qty;
        else asks[price] += qty;
        try_match();
        return true;
    }

    bool cancel_order(int id) {
        auto it = orders.find(id);
        if (it == orders.end() || it->second.status != OrderStatus::ACTIVE)
            return false;

        Order& o = it->second;
        if (o.side == Side::BUY) {
            bids[o.price] -= o.quantity;
            if (bids[o.price] <= 0) bids.erase(o.price);
        } else {
            asks[o.price] -= o.quantity;
            if (asks[o.price] <= 0) asks.erase(o.price);
        }
        o.status = OrderStatus::CANCELLED;
        cancels++;
        return true;
    }

    bool modify_order(int id, double new_price, int new_qty) {
        auto it = orders.find(id);
        if (it == orders.end() || it->second.status != OrderStatus::ACTIVE)
            return false;

        Order& o = it->second;
        // Remove old
        if (o.side == Side::BUY) {
            bids[o.price] -= o.quantity;
            if (bids[o.price] <= 0) bids.erase(o.price);
        } else {
            asks[o.price] -= o.quantity;
            if (asks[o.price] <= 0) asks.erase(o.price);
        }
        // Add new
        o.price = new_price;
        o.quantity = new_qty;
        o.status = OrderStatus::MODIFIED;
        if (o.side == Side::BUY) bids[new_price] += new_qty;
        else asks[new_price] += new_qty;
        o.status = OrderStatus::ACTIVE;
        modifies++;
        try_match();
        return true;
    }

    void try_match() {
        while (!bids.empty() && !asks.empty()) {
            auto bb = bids.begin();
            auto ba = asks.begin();
            if (bb->first >= ba->first) {
                int fill = std::min(bb->second, ba->second);
                trades++;
                bb->second -= fill;
                ba->second -= fill;
                if (bb->second == 0) bids.erase(bb);
                if (ba->second == 0) asks.erase(ba);
            } else break;
        }
    }

    void print_book() {
        std::cout << "\n=== ORDER BOOK ===" << std::endl;
        std::cout << "--- ASKS ---" << std::endl;
        for (auto it = asks.rbegin(); it != asks.rend(); ++it)
            std::cout << "  " << it->first << " x " << it->second << std::endl;
        std::cout << "--- BIDS ---" << std::endl;
        for (auto& [p, q] : bids)
            std::cout << "  " << p << " x " << q << std::endl;
        std::cout << "Trades: " << trades << " | Cancels: " << cancels << " | Modifies: " << modifies << std::endl;
    }
};

int main() {
    OrderBook book;
    auto start = std::chrono::high_resolution_clock::now();

    // Add orders
    book.add_order(1, 100.50, 10, Side::BUY);
    book.add_order(2, 100.30, 5, Side::BUY);
    book.add_order(3, 101.00, 8, Side::SELL);
    book.add_order(4, 100.80, 12, Side::SELL);
    std::cout << "After adding 4 orders:";
    book.print_book();

    // Modify order 2: change price to 100.60
    book.modify_order(2, 100.60, 5);
    std::cout << "\nAfter modifying order 2 (100.30 -> 100.60):";
    book.print_book();

    // Cancel order 3
    book.cancel_order(3);
    std::cout << "\nAfter cancelling order 3 (ask @ 101.00):";
    book.print_book();

    // Aggressive buy that crosses the spread
    book.add_order(5, 100.80, 20, Side::BUY);
    std::cout << "\nAfter aggressive buy @ 100.80 x 20:";
    book.print_book();

    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "\nTotal processing: " << ns << " ns" << std::endl;

    return 0;
}
