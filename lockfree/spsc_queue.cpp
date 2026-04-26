/*
 * Single-producer single-consumer lock-free ring buffer.
 *
 * Cache-line discipline:
 *   - Producer touches head (and reads cached_tail to check fullness).
 *   - Consumer touches tail (and reads cached_head to check emptiness).
 *   - Each lives on its own 64-byte line, so producer/consumer never invalidate
 *     each other's lines except when the cached snapshot is refreshed.
 *   - The data buffer is also cache-aligned to avoid false sharing with anything
 *     placed adjacent to the queue object.
 *
 * Used in HFT between the market-data thread and the trading thread.
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

template<typename T, size_t SIZE>
class SPSCQueue {
    static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be power of 2");
    static constexpr size_t MASK = SIZE - 1;
    static constexpr size_t CACHE_LINE = 64;

    alignas(CACHE_LINE) T buffer[SIZE];

    // Producer-side state. cached_tail is a stale read of tail used to skip
    // the expensive atomic load when the queue is known to have room.
    alignas(CACHE_LINE) std::atomic<size_t> head{0};
    alignas(CACHE_LINE) size_t cached_tail{0};

    // Consumer-side state, mirrored.
    alignas(CACHE_LINE) std::atomic<size_t> tail{0};
    alignas(CACHE_LINE) size_t cached_head{0};

public:
    bool push(const T& item) noexcept {
        const size_t h    = head.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;

        if (next == cached_tail) {
            cached_tail = tail.load(std::memory_order_acquire);
            if (next == cached_tail) return false;  // full
        }

        buffer[h] = item;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        const size_t t = tail.load(std::memory_order_relaxed);

        if (t == cached_head) {
            cached_head = head.load(std::memory_order_acquire);
            if (t == cached_head) return false;  // empty
        }

        item = buffer[t];
        tail.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }

    // size(): coarse snapshot — head and tail are loaded separately, so the value
    // is approximate under concurrent push/pop. Use only for diagnostics.
    size_t size() const noexcept {
        return (head.load(std::memory_order_acquire) -
                tail.load(std::memory_order_acquire)) & MASK;
    }

    bool empty() const noexcept {
        return head.load(std::memory_order_acquire) ==
               tail.load(std::memory_order_acquire);
    }
};

struct MarketData {
    int       seq;
    double    price;
    int       quantity;
    long long timestamp_ns;
};

// Benchmark: producer pushes NUM_MESSAGES, consumer measures end-to-end latency.
void benchmark_throughput() {
    SPSCQueue<MarketData, 65536> queue;
    const int NUM_MESSAGES = 10'000'000;
    std::atomic<bool> done{false};
    long long total_latency = 0;
    int received = 0;

    auto consumer = std::thread([&]() {
        MarketData msg;
        // Consumer-side termination: only stop when producer signalled done AND
        // the queue is genuinely empty (checked through the same atomic loads as pop()).
        while (true) {
            if (queue.pop(msg)) {
                const auto now = std::chrono::high_resolution_clock::now()
                                 .time_since_epoch().count();
                total_latency += (now - msg.timestamp_ns);
                received++;
            } else if (done.load(std::memory_order_acquire) && queue.empty()) {
                break;
            }
        }
    });

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_MESSAGES; i++) {
        MarketData msg{
            i,
            100.0 + (i % 100) * 0.01,
            (i % 10 + 1) * 100,
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        };
        while (!queue.push(msg)) {}  // spin until space
    }

    done.store(true, std::memory_order_release);
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "=== Lock-Free SPSC Queue Benchmark ===" << std::endl;
    std::cout << "Messages:     " << received << " / " << NUM_MESSAGES << std::endl;
    std::cout << "Time:         " << elapsed_ms << " ms" << std::endl;

    if (received > 0 && elapsed_ms > 0) {
        const double msgs_per_sec = (double)received / (elapsed_ms / 1000.0);
        const double avg_latency  = (double)total_latency / received;
        std::cout << "Throughput:   " << (long long)msgs_per_sec << " msg/sec" << std::endl;
        std::cout << "Avg latency:  " << (long long)avg_latency << " ns" << std::endl;
    } else {
        std::cout << "Throughput:   N/A" << std::endl;
        std::cout << "Avg latency:  N/A" << std::endl;
    }
    std::cout << "Queue size:   65536 slots" << std::endl;
    std::cout << "Cache lines:  head, tail, and cached snapshots on separate 64-byte lines" << std::endl;
}

void demo() {
    std::cout << "=== SPSC Queue Demo ===" << std::endl;
    SPSCQueue<MarketData, 1024> queue;

    for (int i = 0; i < 5; i++) {
        MarketData msg{i, 150.25 + i * 0.01, 100, 0};
        queue.push(msg);
        std::cout << "  PUSH: seq=" << msg.seq << " price=" << msg.price << std::endl;
    }
    std::cout << "  Queue size: " << queue.size() << std::endl;

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
