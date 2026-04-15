#include <iostream>
#include <map>
#include <unordered_map>
#include <chrono>
#include <cstdint>

// Fixed-point price: stored as integer ticks (1 tick = 0.01)
// Stała cena - przechowywana jako liczba całkowita (1 tick = 0,01)
using Price = std::int64_t;

static Price to_ticks(double p) { return static_cast<Price>(p * 100 + 0.5); }
static double to_double(Price p) { return p / 100.0; }

enum class Side { BUY, SELL };
enum class OrderStatus { ACTIVE, FILLED, CANCELLED, MODIFIED };

struct Order {
    int id;
    Price price;
    int quantity;
    Side side;
    OrderStatus status;
};

class OrderBook {
    std::map<Price, int, std::greater<Price>> bids;
    std::map<Price, int> asks;
    std::unordered_map<int, Order> orders;
    int trades = 0;
    int cancels = 0;
    int modifies = 0;

public:
    bool add_order(int id, Price price, int qty, Side side) noexcept {
        orders[id] = {id, price, qty, side, OrderStatus::ACTIVE};
        if (side == Side::BUY) bids[price] += qty;
        else asks[price] += qty;
        try_match();
        return true;
    }

    bool cancel_order(int id) noexcept {
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

    bool modify_order(int id, Price new_price, int new_qty) noexcept {
        auto it = orders.find(id);
        if (it == orders.end() || it->second.status != OrderStatus::ACTIVE)
            return false;

        Order& o = it->second;
        // Remove old order from book
        // Usuń stare zlecenie z księgi
        if (o.side == Side::BUY) {
            bids[o.price] -= o.quantity;
            if (bids[o.price] <= 0) bids.erase(o.price);
        } else {
            asks[o.price] -= o.quantity;
            if (asks[o.price] <= 0) asks.erase(o.price);
        }
        // Add new order
        // Dodaj nowe zlecenie
        o.price = new_price;
        o.quantity = new_qty;
        o.status = OrderStatus::ACTIVE;
        if (o.side == Side::BUY) bids[new_price] += new_qty;
        else asks[new_price] += new_qty;
        modifies++;
        try_match();
        return true;
    }

    void try_match() noexcept {
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

    void print_book() const {
        std::cout << "\n=== ORDER BOOK ===" << std::endl;
        std::cout << "--- ASKS ---" << std::endl;
        for (auto it = asks.rbegin(); it != asks.rend(); ++it)
            std::cout << "  " << to_double(it->first) << " x " << it->second << std::endl;
        std::cout << "--- BIDS ---" << std::endl;
        for (const auto& [p, q] : bids)
            std::cout << "  " << to_double(p) << " x " << q << std::endl;
        std::cout << "Trades: " << trades << " | Cancels: " << cancels << " | Modifies: " << modifies << std::endl;
    }
};

int main() {
    OrderBook book;
    auto start = std::chrono::high_resolution_clock::now();

    // Add orders (prices in ticks: 10050 = $100.50)
    // Dodaj zlecenia (ceny w tickach: 10050 = $100,50)
    book.add_order(1, to_ticks(100.50), 10, Side::BUY);
    book.add_order(2, to_ticks(100.30), 5, Side::BUY);
    book.add_order(3, to_ticks(101.00), 8, Side::SELL);
    book.add_order(4, to_ticks(100.80), 12, Side::SELL);
    std::cout << "After adding 4 orders:";
    book.print_book();

    // Modify order 2: change price to 100.60
    // Zmodyfikuj zlecenie 2: zmień cenę na 100,60
    book.modify_order(2, to_ticks(100.60), 5);
    std::cout << "\nAfter modifying order 2 (100.30 -> 100.60):";
    book.print_book();

    // Cancel order 3
    // Anuluj zlecenie 3
    book.cancel_order(3);
    std::cout << "\nAfter cancelling order 3 (ask @ 101.00):";
    book.print_book();

    // Aggressive buy that crosses the spread
    // Agresywny zakup, który przekracza spread
    book.add_order(5, to_ticks(100.80), 20, Side::BUY);
    std::cout << "\nAfter aggressive buy @ 100.80 x 20:";
    book.print_book();

    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "\nTotal processing: " << ns << " ns" << std::endl;

    return 0;
}
