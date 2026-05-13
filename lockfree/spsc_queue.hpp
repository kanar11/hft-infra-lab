/*
 * SPSCQueue<T, SIZE> — single-producer single-consumer lock-free ring buffer.
 *
 * Invariants
 * ----------
 *   - Exactly ONE thread calls push() over the queue's lifetime.
 *   - Exactly ONE (different) thread calls pop().
 *   - SIZE must be a positive power of two.
 *
 * Memory ordering
 * ---------------
 * Each side reads its OWN counter (head for producer, tail for consumer)
 * with std::memory_order_relaxed — sole writer, no sync needed.
 *
 * Each side reads the OTHER side's counter with std::memory_order_acquire,
 * paired with a release-store from the other thread:
 *
 *   ┌──────────────────┐  release       acquire   ┌──────────────────┐
 *   │ producer:        │ ───────► head ────────►  │ consumer:        │
 *   │ buf[h] = item    │                          │ item = buf[t]    │
 *   │ head.store(...)  │                          │ tail.store(...)  │
 *   └──────────────────┘ ◄─────── tail ◄───────── └──────────────────┘
 *                          acquire       release
 *
 * False sharing prevention
 * ------------------------
 * head_ and tail_ each occupy their own 64-byte cache line (alignas(64))
 * so the producer writing head_ does not invalidate the consumer's copy
 * of tail_. Without that separation, every push pays ~50-100 ns of cache-
 * coherence round-trip.
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
    // push: producer-only. Returns false if the queue is full.
    bool push(const T& item) noexcept {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) & (SIZE - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // pop: consumer-only. Returns false if the queue is empty.
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

    // size: APPROXIMATE (two atomic loads, not a snapshot). Off by ±1 under
    // contention. Use empty()/full() on the hot path.
    std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (SIZE - 1);
    }

    // capacity: usable slots. A power-of-two ring loses one slot to
    // distinguish empty from full, so capacity == SIZE - 1.
    static constexpr std::size_t capacity() noexcept { return SIZE - 1; }
};

}  // namespace lockfree
