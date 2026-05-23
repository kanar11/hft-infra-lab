/*
 * SPSCQueue<T, SIZE> — kolejka lock-free ring 1 producent / 1 konsument.
 *
 * Założenia:
 *   - Dokładnie JEDEN wątek wywołuje push() przez cały czas życia kolejki.
 *   - Dokładnie JEDEN (inny) wątek wywołuje pop().
 *   - SIZE musi być dodatnią potęgą dwójki (maska bitowa zamiast modulo).
 *
 * Memory ordering:
 *   Każda strona czyta swój WŁASNY licznik (head dla producenta, tail dla
 *   konsumenta) z relaxed — jest jedynym pisarzem, sync zbędny.
 *
 *   Licznik drugiej strony czyta się z acquire, parując z release-store:
 *
 *     producent (release on head):     konsument (acquire on head):
 *       buf[h] = item;                    if (t == head.load(acq)) empty;
 *       head.store(next, release);        out = buf[t];
 *                                         tail.store(t+1, release);
 *
 * False sharing:
 *   head_ i tail_ leżą w osobnych liniach cache (alignas(64)). Bez tego
 *   każdy push invaliduje linię konsumenta — ~50-100 ns coherence round-trip.
 *   To jest cały sens lock-free SPSC: producent i konsument nigdy nie biją się
 *   o tę samą linię cache.
 *
 * Po co własna impl skoro istnieje boost::lockfree::spsc_queue?
 *   - Edukacja: pokazuje paring acquire/release w 50 liniach.
 *   - Brak zależności (boost waży ~50 MB headers).
 *   - Pełna kontrola nad layoutem (cache line alignment, ABI stable).
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
    // push: tylko producent. Zwraca false gdy kolejka pełna.
    bool push(const T& item) noexcept {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) & (SIZE - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // pop: tylko konsument. Zwraca false gdy kolejka pusta.
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

    // size: PRZYBLIŻONA (dwa load'y atomic, nie snapshot). Pod kontencją
    // może być off o ±1. Na hot path używaj empty()/full().
    std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (SIZE - 1);
    }

    // capacity: użyteczne sloty. Power-of-two ring traci 1 slot żeby
    // odróżnić empty od full → capacity == SIZE - 1.
    static constexpr std::size_t capacity() noexcept { return SIZE - 1; }
};

}  // namespace lockfree
