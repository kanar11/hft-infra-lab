/*
 * SPSC Lock-Free Queue — C++ Implementation
 * Bezblokująca kolejka SPSC — implementacja C++
 *
 * Single-Producer Single-Consumer ring buffer. Used in HFT between the
 * market-data thread and the trading thread (or any one-to-one handoff).
 * Bezblokująca kolejka jeden-do-jednego. Używana w HFT między wątkiem
 * danych rynkowych a wątkiem handlu (lub innymi przekazami 1:1).
 *
 * Memory ordering:
 *   producer  → push:  buffer write THEN head.store(release)
 *   consumer  → pop:   head.load(acquire) THEN buffer read
 *   release/acquire pair guarantees the consumer sees a fully-written slot.
 *
 *   Each side reads its own counter (head for producer, tail for consumer)
 *   with relaxed — it is the sole writer, so no synchronisation needed.
 *
 * head and tail sit on separate cache lines (alignas(64)) to avoid false
 * sharing — without it, every producer write would invalidate the consumer's
 * cache line and vice versa, costing 50–100 ns per round-trip.
 */

#pragma once

#include <atomic>
#include <cstddef>


template<typename T, size_t SIZE>
class SPSCQueue {
    static_assert(SIZE > 0,                       "SIZE must be positive");
    static_assert((SIZE & (SIZE - 1)) == 0,       "SIZE must be power of 2");

    T buffer[SIZE];
    alignas(64) std::atomic<size_t> head{0};   // producer writes / producent pisze
    alignas(64) std::atomic<size_t> tail{0};   // consumer reads  / konsument czyta

public:
    // push: producer-only. Returns false if the queue is full.
    // push: tylko producent. Zwraca false jeśli kolejka pełna.
    bool push(const T& item) {
        size_t h    = head.load(std::memory_order_relaxed);
        size_t next = (h + 1) & (SIZE - 1);
        if (next == tail.load(std::memory_order_acquire)) return false;
        buffer[h] = item;
        head.store(next, std::memory_order_release);
        return true;
    }

    // pop: consumer-only. Returns false if the queue is empty.
    // pop: tylko konsument. Zwraca false jeśli kolejka pusta.
    bool pop(T& item) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        item = buffer[t];
        tail.store((t + 1) & (SIZE - 1), std::memory_order_release);
        return true;
    }

    // empty(): consumer-side query. tail is stable (sole writer); acquire
    // on head ensures we observe all committed items.
    bool empty() const noexcept {
        return tail.load(std::memory_order_relaxed)
            == head.load(std::memory_order_acquire);
    }

    // full(): producer-side query. head is stable; acquire on tail ensures
    // we see all slots the consumer has freed.
    bool full() const noexcept {
        size_t h = head.load(std::memory_order_relaxed);
        return ((h + 1) & (SIZE - 1)) == tail.load(std::memory_order_acquire);
    }

    // size(): APPROXIMATE — two separate atomic loads, not a single snapshot.
    // The result can be off by ±1 if either side advances between the loads.
    // Use empty()/full() on the hot path; size() is for monitoring only.
    size_t size() const noexcept {
        size_t h = head.load(std::memory_order_acquire);
        size_t t = tail.load(std::memory_order_acquire);
        return (h - t) & (SIZE - 1);
    }
};
