/*
 * SPSCQueue<T, SIZE> — a lock-free ring queue, 1 producer / 1 consumer.
 *
 * Assumptions:
 *   - Exactly ONE thread calls push() for the queue's whole lifetime.
 *   - Exactly ONE (other) thread calls pop().
 *   - SIZE must be a positive power of two (bit mask instead of modulo).
 *
 * Memory ordering:
 *   Each side reads its OWN counter (head for the producer, tail for the
 *   consumer) with relaxed — it is the only writer, no sync needed.
 *
 *   The other side's counter is read with acquire, pairing with a release-store:
 *
 *     producer (release on head):     consumer (acquire on head):
 *       buf[h] = item;                    if (t == head.load(acq)) empty;
 *       head.store(next, release);        out = buf[t];
 *                                         tail.store(t+1, release);
 *
 * False sharing:
 *   head_ and tail_ live in separate cache lines (alignas(64)). Without it
 *   every push invalidates the consumer's line — a ~50-100 ns coherence round-trip.
 *   This is the whole point of a lock-free SPSC: producer and consumer never fight
 *   over the same cache line.
 *
 * Why a custom impl when boost::lockfree::spsc_queue exists?
 *   - Education: shows acquire/release pairing in 50 lines.
 *   - No dependencies (boost weighs ~50 MB of headers).
 *   - Full control over the layout (cache line alignment, ABI stable).
 */
#pragma once

#include <atomic>
#include <cstddef>


namespace lockfree {

inline constexpr std::size_t kCacheLine = 64;


template <typename T, std::size_t SIZE>
class SPSCQueue {
    static_assert(SIZE > 0,                       "SIZE must be positive");
    static_assert((SIZE & (SIZE - 1)) == 0,       "SIZE must be a power of two");

    T buffer_[SIZE];
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};   // producer writes
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};   // consumer writes

public:
    // push: producer only. Returns false when the queue is full.
    bool push(const T& item) noexcept {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) & (SIZE - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // pop: consumer only. Returns false when the queue is empty.
    bool pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buffer_[t];
        tail_.store((t + 1) & (SIZE - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    bool full() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        return ((h + 1) & (SIZE - 1)) == tail_.load(std::memory_order_acquire);
    }

    // size: APPROXIMATE (two atomic loads, not a snapshot). Under contention
    // it can be off by ±1. On the hot path use empty()/full().
    std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (SIZE - 1);
    }

    // capacity: usable slots. A power-of-two ring loses 1 slot to
    // distinguish empty from full → capacity == SIZE - 1.
    static constexpr std::size_t capacity() noexcept { return SIZE - 1; }
};

}  // namespace lockfree
