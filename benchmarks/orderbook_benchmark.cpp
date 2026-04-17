/*
 * Orderbook Micro-Benchmark — Add / Cancel / Match Performance
 * Mikro-benchmark orderbooka — wydajność dodawania / anulowania / dopasowywania
 *
 * WHAT THIS MEASURES:
 * Individual operation latencies for the three core orderbook operations:
 *   1. ADD    — insert a new limit order (bid or ask)
 *   2. CANCEL — remove an existing order by ID
 *   3. MATCH  — match a crossing order against resting orders
 *
 * CO TO MIERZY:
 * Indywidualne opóźnienia dla trzech podstawowych operacji orderbooka:
 *   1. ADD    — wstaw nowe zlecenie z limitem (bid lub ask)
 *   2. CANCEL — usuń istniejące zlecenie po ID
 *   3. MATCH  — dopasuj zlecenie krzyżujące się z oczekującymi zleceniami
 *
 * WHY THESE THREE:
 * These are the only operations a matching engine does. Every nanosecond
 * saved here directly translates to better queue position at the exchange.
 * Optiver, Citadel, Jump — they all optimize these exact operations.
 *
 * DLACZEGO TE TRZY:
 * To jedyne operacje silnika dopasowującego. Każda zaoszczędzona nanosekunda
 * przekłada się bezpośrednio na lepszą pozycję w kolejce na giełdzie.
 *
 * Compile / Kompilacja:
 *   g++ -O2 -std=c++17 -Wall -Wextra -pthread -o orderbook_benchmark orderbook_benchmark.cpp
 *
 * Output: per-operation latency percentiles + CSV results
 */

#include <chrono>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>


// === Minimal Orderbook for Benchmarking ===
// === Minimalny Orderbook do Benchmarkowania ===
// This is a simplified version focused on measurable performance.
// For the full implementation see orderbook/orderbook_v2.cpp
// To jest uproszczona wersja skupiona na mierzalnej wydajności.
// Pełna implementacja: orderbook/orderbook_v2.cpp

struct Order {
    uint64_t id;
    bool is_buy;        // true = bid, false = ask
    int64_t price;      // fixed-point: $150.25 → 1502500 (price * 10000)
    uint32_t quantity;
};

class Orderbook {
    // std::map: sorted tree (red-black) — keys are always in order
    // For bids: highest price first (reverse iterator)
    // For asks: lowest price first (forward iterator)
    // std::map: posortowane drzewo (czerwono-czarne) — klucze zawsze w kolejności
    // Dla kupna: najwyższa cena pierwsza (odwrotny iterator)
    // Dla sprzedaży: najniższa cena pierwsza (zwykły iterator)
    std::map<int64_t, std::vector<Order>> bids_;  // price → orders (descending)
    std::map<int64_t, std::vector<Order>> asks_;  // price → orders (ascending)

    // unordered_map: hash table for O(1) lookup by order ID
    // Like a phone book — given an ID, find the order instantly
    // unordered_map: tablica hashująca do wyszukiwania O(1) po ID zlecenia
    // Jak książka telefoniczna — mając ID, znajdź zlecenie natychmiast
    std::unordered_map<uint64_t, std::pair<int64_t, bool>> order_index_;  // id → (price, is_buy)

public:
    // add_order: O(log N) due to map insertion
    // In production: custom allocator + flat arrays for O(1) amortized
    // add_order: O(log N) ze względu na wstawianie do mapy
    // W produkcji: własny alokator + płaskie tablice dla zamortyzowanego O(1)
    void add_order(const Order& order) noexcept {
        auto& book = order.is_buy ? bids_ : asks_;
        book[order.price].push_back(order);
        order_index_[order.id] = {order.price, order.is_buy};
    }

    // cancel_order: find by ID (O(1) hash lookup), then remove from price level
    // cancel_order: znajdź po ID (O(1) wyszukiwanie hash), potem usuń z poziomu cenowego
    bool cancel_order(uint64_t id) noexcept {
        auto it = order_index_.find(id);
        if (it == order_index_.end()) return false;

        auto [price, is_buy] = it->second;
        auto& book = is_buy ? bids_ : asks_;
        auto level_it = book.find(price);
        if (level_it != book.end()) {
            auto& orders = level_it->second;
            for (auto oit = orders.begin(); oit != orders.end(); ++oit) {
                if (oit->id == id) {
                    orders.erase(oit);
                    if (orders.empty()) book.erase(level_it);
                    break;
                }
            }
        }
        order_index_.erase(it);
        return true;
    }

    // try_match: attempt to match an incoming order against resting orders
    // Returns number of shares filled
    // try_match: próba dopasowania przychodzącego zlecenia do oczekujących
    // Zwraca liczbę zrealizowanych akcji
    uint32_t try_match(Order& incoming) noexcept {
        uint32_t filled = 0;
        auto& book = incoming.is_buy ? asks_ : bids_;

        while (incoming.quantity > 0 && !book.empty()) {
            auto it = incoming.is_buy ? book.begin() : std::prev(book.end());

            // Check if prices cross (buy >= ask, or sell <= bid)
            // Sprawdź czy ceny się krzyżują (kupno >= ask, lub sprzedaż <= bid)
            if (incoming.is_buy && incoming.price < it->first) break;
            if (!incoming.is_buy && incoming.price > it->first) break;

            auto& resting = it->second;
            while (incoming.quantity > 0 && !resting.empty()) {
                auto& front = resting.front();
                uint32_t trade_qty = std::min(incoming.quantity, front.quantity);
                incoming.quantity -= trade_qty;
                front.quantity -= trade_qty;
                filled += trade_qty;

                if (front.quantity == 0) {
                    order_index_.erase(front.id);
                    resting.erase(resting.begin());
                }
            }

            if (resting.empty()) book.erase(it);
        }
        return filled;
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        order_index_.clear();
    }
};


// === Benchmark Helpers ===
struct BenchResult {
    std::string name;
    std::vector<int64_t> latencies;
    int64_t min_ns, p50_ns, p90_ns, p99_ns, max_ns;
    double avg_ns;
    double throughput_mops;  // million operations per second

    void compute() {
        std::sort(latencies.begin(), latencies.end());
        int n = latencies.size();
        min_ns = latencies.front();
        p50_ns = latencies[n / 2];
        p90_ns = latencies[(int)(n * 0.90)];
        p99_ns = latencies[(int)(n * 0.99)];
        max_ns = latencies.back();
        int64_t total = std::accumulate(latencies.begin(), latencies.end(), (int64_t)0);
        avg_ns = static_cast<double>(total) / n;
        throughput_mops = 1e9 / avg_ns / 1e6;
    }

    void print() const {
        fprintf(stderr, "  %-12s  avg=%6.0f ns  p50=%4ld  p90=%4ld  p99=%4ld  max=%6ld  (%.1f M ops/sec)\n",
                name.c_str(), avg_ns, p50_ns, p90_ns, p99_ns, max_ns, throughput_mops);
    }
};


int main(int argc, char* argv[]) {
    int ops = 100'000;
    if (argc > 1) {
        ops = std::atoi(argv[1]);
        if (ops <= 0) ops = 100'000;
    }

    fprintf(stderr, "=== Orderbook Micro-Benchmark ===\n");
    fprintf(stderr, "Operations per test: %d\n\n", ops);

    Orderbook book;
    BenchResult add_result{"ADD", {}, 0, 0, 0, 0, 0, 0, 0};
    BenchResult cancel_result{"CANCEL", {}, 0, 0, 0, 0, 0, 0, 0};
    BenchResult match_result{"MATCH", {}, 0, 0, 0, 0, 0, 0, 0};

    add_result.latencies.reserve(ops);
    cancel_result.latencies.reserve(ops);
    match_result.latencies.reserve(ops);

    // === Benchmark: ADD orders ===
    // Pre-populate with some orders, then measure adds
    // Wstępnie wypełnij kilkoma zleceniami, potem mierz dodawanie
    book.clear();
    for (int i = 0; i < ops; ++i) {
        Order order{
            static_cast<uint64_t>(i),
            (i % 2 == 0),                              // alternate buy/sell
            1000000 + (i % 100) * 1000,                // prices around $100
            static_cast<uint32_t>(100 + (i % 50))      // quantity 100-149
        };

        auto start = std::chrono::high_resolution_clock::now();
        book.add_order(order);
        auto end = std::chrono::high_resolution_clock::now();

        add_result.latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    // === Benchmark: CANCEL orders ===
    // Cancel all orders we just added (in random-ish order)
    // Anuluj wszystkie zlecenia które właśnie dodaliśmy (w pseudo-losowej kolejności)
    std::vector<uint64_t> ids_to_cancel;
    ids_to_cancel.reserve(ops);
    for (int i = 0; i < ops; ++i) {
        // Interleave IDs for more realistic access pattern
        // Przemieszaj ID dla bardziej realistycznego wzorca dostępu
        ids_to_cancel.push_back(static_cast<uint64_t>((i * 7 + 13) % ops));
    }

    for (int i = 0; i < ops; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        book.cancel_order(ids_to_cancel[i]);
        auto end = std::chrono::high_resolution_clock::now();

        cancel_result.latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    // === Benchmark: MATCH orders ===
    // Add resting asks, then send matching bids
    // Dodaj oczekujące oferty sprzedaży, potem wyślij pasujące zlecenia kupna
    book.clear();
    uint64_t id_counter = 0;

    // Populate with asks at various prices
    // Wypełnij ofertami sprzedaży po różnych cenach
    for (int i = 0; i < ops; ++i) {
        Order ask{id_counter++, false, 1000000 + (int64_t)(i % 100) * 1000, 100};
        book.add_order(ask);
    }

    // Send matching buy orders
    // Wyślij pasujące zlecenia kupna
    for (int i = 0; i < ops; ++i) {
        Order buy{id_counter++, true, 1000000 + (int64_t)(i % 100) * 1000, 100};

        auto start = std::chrono::high_resolution_clock::now();
        book.try_match(buy);
        auto end = std::chrono::high_resolution_clock::now();

        match_result.latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    // === Compute and print results ===
    add_result.compute();
    cancel_result.compute();
    match_result.compute();

    fprintf(stderr, "--- Results (nanoseconds) ---\n");
    fprintf(stderr, "  %-12s  %6s  %4s  %4s  %4s  %6s  %s\n",
            "Operation", "avg", "p50", "p90", "p99", "max", "throughput");
    add_result.print();
    cancel_result.print();
    match_result.print();

    // Write CSV to stdout
    printf("operation,round,latency_ns\n");
    int step = std::max(1, ops / 5000);
    for (int i = 0; i < ops; i += step) {
        printf("ADD,%d,%ld\n", i, add_result.latencies[i]);
    }
    for (int i = 0; i < ops; i += step) {
        printf("CANCEL,%d,%ld\n", i, cancel_result.latencies[i]);
    }
    for (int i = 0; i < ops; i += step) {
        printf("MATCH,%d,%ld\n", i, match_result.latencies[i]);
    }

    return 0;
}
