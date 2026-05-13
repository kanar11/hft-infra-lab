/*
 * MPMCQueue<T, SIZE> — multiple-producer, multiple-consumer bounded queue.
 *
 * Same per-cell sequence-number protocol as MPSCQueue, but the consumer
 * side also CAS-claims slots on tail_ — any number of consumers may pop
 * concurrently.
 *
 * Use case: work pools, fan-out task dispatch, anywhere both ends scale
 * across cores.
 *
 * Performance note
 * ----------------
 * CAS contention rises with thread count. For 1×1, prefer SPSCQueue (no CAS).
 * For N×1, prefer MPSCQueue (CAS only on head). Reach for MPMCQueue only
 * when both sides genuinely need to scale.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>


namespace lockfree {

template <typename T, std::size_t SIZE>
class MPMCQueue {
    static_assert(SIZE > 0,                       "SIZE must be positive");
    static_assert((SIZE & (SIZE - 1)) == 0,       "SIZE must be a power of two");

    struct alignas(64) Cell {
        std::atomic<std::size_t> seq;
        T                        data;
    };

    Cell buffer_[SIZE]{};  // value-init; ctor body then writes correct seq per slot
    alignas(64) std::atomic<std::size_t> head_{0};  // producers CAS
    alignas(64) std::atomic<std::size_t> tail_{0};  // consumers CAS

    static constexpr std::size_t MASK = SIZE - 1;

public:
    MPMCQueue() noexcept {
        for (std::size_t i = 0; i < SIZE; ++i)
            buffer_[i].seq.store(i, std::memory_order_relaxed);
    }

    MPMCQueue(const MPMCQueue&)            = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&)                 = delete;
    MPMCQueue& operator=(MPMCQueue&&)      = delete;

    bool push(const T& item) noexcept {
        std::size_t pos = head_.load(std::memory_order_relaxed);
        Cell* c = nullptr;
        for (;;) {
            c = &buffer_[pos & MASK];
            const std::size_t seq = c->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
        c->data = item;
        c->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        Cell* c = nullptr;
        for (;;) {
            c = &buffer_[pos & MASK];
            const std::size_t seq = c->seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(seq)
                            - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
        out = c->data;
        c->seq.store(pos + SIZE, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t seq = buffer_[t & MASK].seq.load(std::memory_order_acquire);
        return seq != t + 1;
    }

    static constexpr std::size_t capacity() noexcept { return SIZE; }
};

}  // namespace lockfree
