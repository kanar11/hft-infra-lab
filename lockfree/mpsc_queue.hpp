/*
 * MPSCQueue<T, SIZE> — multiple-producer, single-consumer bounded queue.
 *
 * Vyukov-style slot protocol. Each cell carries its own sequence number;
 * producers race for slots via CAS on head_, the single consumer advances
 * tail_ with a plain relaxed store.
 *
 * Use case: many trading threads → one logger / journal / audit pipeline.
 *
 * Sequence-number protocol
 * ------------------------
 * Each Cell starts with seq == its index. seq encodes both ownership and
 * round number:
 *
 *   producer (claim pos):
 *     cell.seq == pos       → free this round → CAS head_ to claim
 *     cell.seq <  pos       → full (consumer hasn't drained previous wrap)
 *     cell.seq >  pos       → another producer raced; reload head, retry
 *     After write: cell.seq.store(pos + 1, release)  ← publishes data
 *
 *   consumer (drain at tail):
 *     cell.seq == pos + 1   → ready → copy data → advance tail
 *     cell.seq <  pos + 1   → empty
 *     After read: cell.seq.store(pos + SIZE, release)  ← frees slot for next wrap
 *
 * Capacity == SIZE (sequence numbers eliminate the empty/full ambiguity that
 * costs SPSCQueue one slot).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>


namespace lockfree {

template <typename T, std::size_t SIZE>
class MPSCQueue {
    static_assert(SIZE > 0,                       "SIZE must be positive");
    static_assert((SIZE & (SIZE - 1)) == 0,       "SIZE must be a power of two");

    struct alignas(64) Cell {
        std::atomic<std::size_t> seq;
        T                        data;
    };

    Cell buffer_[SIZE]{};  // value-init; ctor body then writes correct seq per slot
    alignas(64) std::atomic<std::size_t> head_{0};  // producers CAS here
    alignas(64) std::atomic<std::size_t> tail_{0};  // single consumer

    static constexpr std::size_t MASK = SIZE - 1;

public:
    MPSCQueue() noexcept {
        for (std::size_t i = 0; i < SIZE; ++i)
            buffer_[i].seq.store(i, std::memory_order_relaxed);
    }

    MPSCQueue(const MPSCQueue&)            = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;
    MPSCQueue(MPSCQueue&&)                 = delete;
    MPSCQueue& operator=(MPSCQueue&&)      = delete;

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
        const std::size_t pos = tail_.load(std::memory_order_relaxed);
        Cell& c = buffer_[pos & MASK];
        const std::size_t seq = c.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<std::intptr_t>(seq)
                        - static_cast<std::intptr_t>(pos + 1);
        if (diff != 0) return false;
        out = c.data;
        c.seq.store(pos + SIZE, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    bool empty() const noexcept {
        const std::size_t pos = tail_.load(std::memory_order_relaxed);
        const std::size_t seq = buffer_[pos & MASK].seq.load(std::memory_order_acquire);
        return seq != pos + 1;
    }

    static constexpr std::size_t capacity() noexcept { return SIZE; }
};

}  // namespace lockfree
