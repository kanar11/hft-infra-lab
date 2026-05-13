/*
 * Sequencer<T, SIZE> — single-producer LMAX Disruptor-style ring.
 *
 * Unlike a queue, the Sequencer exposes the slots directly: producer claims
 * a monotonic sequence number, writes into the underlying slot in place,
 * then publishes by storing to the cursor. Consumers read slots zero-copy
 * in published order. No data is ever copied through the queue — the speed
 * advantage of LMAX Disruptor over a queue.
 *
 * Single producer + multiple consumers (the most common LMAX flavour).
 * A multi-producer Sequencer would need a separate "published" bitmask
 * because fetch_add can't tell which claimed slots have completed writing.
 *
 * API
 * ---
 *   producer:  seq = try_claim();   if seq < 0 → ring full
 *              slot(seq) = data;
 *              publish(seq);
 *
 *   consumer:  hi = available();    // latest published seq
 *              for (i = my_next; i <= hi; ++i) use(read(i));
 *              mark_consumed(hi);   // free slots for next wrap
 *
 * With multiple consumers, an external aggregator publishes the min of all
 * their sequences to mark_consumed (the producer can then wrap safely).
 *
 * Memory ordering
 * ---------------
 *   cursor_.store(seq, release)  — producer publishes after writing slot
 *   cursor_.load(acquire)        — consumer pairs with the release
 *   gating_.store(seq, release)  — consumer signals "done with seq" (frees wrap)
 *   gating_.load(acquire)        — producer checks before wrap
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>


namespace lockfree {

template <typename T, std::size_t SIZE>
class Sequencer {
    static_assert(SIZE > 0,                       "SIZE must be positive");
    static_assert((SIZE & (SIZE - 1)) == 0,       "SIZE must be a power of two");

    T buffer_[SIZE]{};  // value-init satisfies cppcheck uninitMemberVar
    alignas(64) std::atomic<std::int64_t> cursor_{-1};  // last published
    alignas(64) std::atomic<std::int64_t> gating_{-1};  // slowest consumer

    static constexpr std::int64_t MASK = static_cast<std::int64_t>(SIZE - 1);

    std::int64_t next_seq_ = 0;  // producer-only — no atomic needed

public:
    Sequencer()                            = default;
    Sequencer(const Sequencer&)            = delete;
    Sequencer& operator=(const Sequencer&) = delete;
    Sequencer(Sequencer&&)                 = delete;
    Sequencer& operator=(Sequencer&&)      = delete;

    // --- producer side (one thread only) ---

    std::int64_t try_claim() noexcept {
        const std::int64_t seq      = next_seq_;
        const std::int64_t wrap_min = seq - static_cast<std::int64_t>(SIZE);
        if (wrap_min > gating_.load(std::memory_order_acquire))
            return -1;
        next_seq_ = seq + 1;
        return seq;
    }

    T& slot(std::int64_t seq) noexcept { return buffer_[seq & MASK]; }

    void publish(std::int64_t seq) noexcept {
        cursor_.store(seq, std::memory_order_release);
    }

    // --- consumer side ---

    std::int64_t available() const noexcept {
        return cursor_.load(std::memory_order_acquire);
    }

    const T& read(std::int64_t seq) const noexcept {
        return buffer_[seq & MASK];
    }

    void mark_consumed(std::int64_t seq) noexcept {
        gating_.store(seq, std::memory_order_release);
    }

    static constexpr std::size_t capacity() noexcept { return SIZE; }
};

}  // namespace lockfree
