#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

template<typename T, size_t SIZE>
class SPSCQueue {
    // Single Producer Single Consumer lock-free queue
    // Used in HFT between market data thread and trading thread
    
    static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be power of 2");
    
    T buffer[SIZE];
    alignas(64) std::atomic<size_t> head{0};  // producer writes here
    alignas(64) std::atomic<size_t> tail{0};  // consumer reads here
    // alignas(64) prevents false sharing between CPU cache lines

public:
    bool push(const T& item) {
        size_t h = head.load(std::memory_order_relaxed);
        size_t next = (h + 1) & (SIZE - 1);  // bitwise AND instead of modulo
        
        if (next == tail.load(std::memory_order_acquire))
            return false;  // queue full
        
        buffer[h] = item;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t t = tail.load(std::memory_order_relaxed);
        
        if (t == head.load(std::memory_order_acquire))
            return false;  // queue empty
        
        item = buffer[t];
        tail.store((t + 1) & (SIZE - 1), std::memory_order_release);
        return true;
    }

    size_t size() const {
        return (head.load() - tail.load()) & (SIZE - 1);
    }
};

struct MarketData {
    int seq;
    double price;
    int quantity;
    long long timestamp_ns;
};

void benchmark_throughput() {
    SPSCQueue<MarketData, 65536> queue;
    const int NUM_MESSAGES = 10000000;
    std::atomic<bool> done{false};
    long long total_latency = 0;
    int received = 0;

    // Consumer thread (trading logic)
    auto consumer = std::thread([&]() {
        MarketData msg;
        while (!done || queue.size() > 0) {
            if (queue.pop(msg)) {
                auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                total_latency += (now - msg.timestamp_ns);
                received++;
            }
        }
    });

    // Producer thread (market data)
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_MESSAGES; i++) {
        MarketData msg{
            i,
            100.0 + (i % 100) * 0.01,
            (i % 10 + 1) * 100,
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        };
        while (!queue.push(msg)) {}  // spin until space available
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

void demo() {
    std::cout << "=== SPSC Queue Demo ===" << std::endl;
    SPSCQueue<MarketData, 1024> queue;
    
    // Simulate market data producer
    for (int i = 0; i < 5; i++) {
        MarketData msg{i, 150.25 + i * 0.01, 100, 0};
        queue.push(msg);
        std::cout << "  PUSH: seq=" << msg.seq << " price=" << msg.price << std::endl;
    }
    
    std::cout << "  Queue size: " << queue.size() << std::endl;
    
    // Simulate trading logic consumer
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
