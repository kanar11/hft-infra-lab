// Order book latency histogram benchmark
// Measures per-order processing latency and reports p50/p95/p99/p99.9/max
// Build: g++ -O2 -std=c++17 latency_histogram.cpp -o latency_histogram
#include <iostream>
#include <iomanip>
#include <map>
#include <unordered_map>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdint>

// Fixed-point price: stored as integer ticks (1 tick = 0.01)
using Price = std::int64_t;

struct Order {
    int id;
    Price price;
    int quantity;
    bool is_buy;
};

class OrderBook {
    // NOTE: std::map allocates per-node on insert (not ideal for HFT).
    // Production systems use pre-allocated flat arrays indexed by price level.
    std::map<Price, int, std::greater<Price>> bids;
    std::map<Price, int> asks;
    std::uint64_t trades = 0;

public:
    void add_order(const Order& o) noexcept {
        if (o.is_buy) bids[o.price] += o.quantity;
        else          asks[o.price] += o.quantity;
        try_match();
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

    std::uint64_t get_trades() const noexcept { return trades; }
    std::size_t depth() const noexcept { return bids.size() + asks.size(); }
};

static double percentile(std::vector<std::uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return static_cast<double>(v[idx]);
}

int main(int argc, char** argv) {
    const int N = (argc > 1) ? std::atoi(argv[1]) : 1'000'000;

    std::mt19937 rng(42);
    // Price range 99.00-101.00 in ticks (9900-10100)
    std::uniform_int_distribution<Price> price_dist(9900, 10100);
    std::uniform_int_distribution<int>   qty_dist(1, 100);
    std::uniform_int_distribution<int>   side_dist(0, 1);

    std::vector<Order> orders;
    orders.reserve(N);
    for (int i = 0; i < N; i++) {
        orders.push_back({i, price_dist(rng), qty_dist(rng), side_dist(rng) == 1});
    }

    OrderBook book;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(N);

    // Warmup (cache priming, no measurement)
    for (int i = 0; i < 1000 && i < N; i++) book.add_order(orders[i]);

    auto t_wall_start = std::chrono::high_resolution_clock::now();
    for (const auto& o : orders) {
        auto t0 = std::chrono::high_resolution_clock::now();
        book.add_order(o);
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    auto t_wall_end = std::chrono::high_resolution_clock::now();

    double wall_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        t_wall_end - t_wall_start).count() / 1000.0;

    std::uint64_t max_lat = *std::max_element(latencies.begin(), latencies.end());
    double p50   = percentile(latencies, 0.50);
    double p95   = percentile(latencies, 0.95);
    double p99   = percentile(latencies, 0.99);
    double p999  = percentile(latencies, 0.999);

    std::cout << "=== Order Book Latency Histogram ===\n"
              << "Orders processed : " << N << "\n"
              << "Wall time        : " << std::fixed << std::setprecision(2)
              << wall_ms << " ms\n"
              << "Throughput       : " << static_cast<std::uint64_t>(N / (wall_ms / 1000.0))
              << " orders/sec\n"
              << "Trades           : " << book.get_trades() << "\n"
              << "Final depth      : " << book.depth() << "\n"
              << "\nPer-order latency (ns):\n"
              << "  p50    : " << p50   << "\n"
              << "  p95    : " << p95   << "\n"
              << "  p99    : " << p99   << "\n"
              << "  p99.9  : " << p999  << "\n"
              << "  max    : " << max_lat << "\n";
    return 0;
}
