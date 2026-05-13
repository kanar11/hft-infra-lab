// SPSC queue benchmark + demo. Class itself lives in spsc_queue.hpp so other
// modules can reuse it. / Klasa w spsc_queue.hpp dla reuse w innych modułach.

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

#include "spsc_queue.hpp"

struct MarketData {
    int seq;
    double price;
    int quantity;
    long long timestamp_ns;
};

// Benchmark queue throughput with producer-consumer pattern
// Benchmark przepustowości kolejki w schemacie producent-konsument
void benchmark_throughput() {
    lockfree::SPSCQueue<MarketData, 65536> queue;
    const int NUM_MESSAGES = 10000000;
    std::atomic<bool> done{false};
    long long total_latency = 0;
    int received = 0;

    // Consumer thread (trading logic)
    // Wątek konsumenta (logika handlu)
    auto consumer = std::thread([&]() {
        MarketData msg{};
        while (!done.load(std::memory_order_relaxed) || !queue.empty()) {
            if (queue.pop(msg)) {
                auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                total_latency += (now - msg.timestamp_ns);
                received++;
            }
        }
    });

    // Producer thread (market data)
    // Wątek producenta (dane rynkowe)
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_MESSAGES; i++) {
        MarketData msg{
            i,
            100.0 + (i % 100) * 0.01,
            (i % 10 + 1) * 100,
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        };
        while (!queue.push(msg)) {}  // spin until space available
        // obracaj się aż do dostępu miejsca
    }

    done = true;
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "=== Lock-Free SPSC Queue Benchmark ===" << std::endl;
    std::cout << "Messages:     " << received << " / " << NUM_MESSAGES << std::endl;
    std::cout << "Time:         " << elapsed_ms << " ms" << std::endl;

    if (received > 0 && elapsed_ms > 0) {
        double msgs_per_sec = (double)received / (elapsed_ms / 1000.0);
        double avg_latency = (double)total_latency / received;
        std::cout << "Throughput:   " << (int)msgs_per_sec << " msg/sec" << std::endl;
        std::cout << "Avg latency:  " << (int)avg_latency << " ns" << std::endl;
    } else {
        std::cout << "Throughput:   N/A (no messages received)" << std::endl;
        std::cout << "Avg latency:  N/A" << std::endl;
    }
    std::cout << "Queue size:   65536 slots" << std::endl;
    std::cout << "Cache lines:  head and tail on separate 64-byte lines (no false sharing)" << std::endl;
}

// Demonstrate basic queue operations
// Zademonstruj podstawowe operacje kolejki
void demo() {
    std::cout << "=== SPSC Queue Demo ===" << std::endl;
    lockfree::SPSCQueue<MarketData, 1024> queue;

    // Simulate market data producer
    // Symuluj producenta danych rynkowych
    for (int i = 0; i < 5; i++) {
        MarketData msg{i, 150.25 + i * 0.01, 100, 0};
        queue.push(msg);
        std::cout << "  PUSH: seq=" << msg.seq << " price=" << msg.price << std::endl;
    }

    std::cout << "  Queue size: " << queue.size() << std::endl;

    // Simulate trading logic consumer
    // Symuluj konsumenta logiki handlu
    MarketData msg;
    while (queue.pop(msg)) {
        std::cout << "  POP:  seq=" << msg.seq << " price=" << msg.price << std::endl;
    }

    std::cout << "  Queue size: " << queue.size() << std::endl;
}

int main() {
    demo();
    std::cout << std::endl;
    benchmark_throughput();
    return 0;
}
