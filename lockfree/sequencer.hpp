/*
 * Sequencer<T, SIZE> — an LMAX Disruptor-style ring, single-producer.
 *
 * Unlike a queue, the Sequencer exposes slots directly: the producer
 * claims a monotonic sequence number, writes *in-place* into the slot,
 * and only then publishes via a store on the cursor. Consumers read
 * slots zero-copy in publication order. Nothing is ever copied "through
 * the queue" — that is exactly the Disruptor's advantage over a
 * traditional queue (a 200-byte struct copy → store into the slot).
 *
 * One producer + many consumers (the most common LMAX variant).
 * A multi-producer Sequencer would need a separate "published" bitmask, because
 * fetch_add does not know which claimed slot has finished its write.
 *
 * API:
 *   producer:  seq = try_claim();   if seq < 0 → ring full
 *              slot(seq) = data;
 *              publish(seq);
 *
 *   consumer:  hi = available();    // the last published seq
 *              for (i = my_next; i <= hi; ++i) use(read(i));
 *              mark_consumed(hi);   // frees slots for the next wrap
 *
 * With many consumers an external aggregator publishes min(all seqs)
 * to mark_consumed — only then can the producer safely wrap.
 *
 * Memory ordering:
 *   cursor_.store(release)  — the producer publishes after writing the slot
 *   cursor_.load(acquire)   — the consumer pairs with release
 *   gating_.store(release)  — the consumer signals "done with seq"
 *   gating_.load(acquire)   — the producer reads before wrapping
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

    T buffer_[SIZE]{};  // value-init — satisfies cppcheck uninitMemberVar
    alignas(64) std::atomic<std::int64_t> cursor_{-1};  // the last published
    alignas(64) std::atomic<std::int64_t> gating_{-1};  // the slowest consumer

    static constexpr std::int64_t MASK = static_cast<std::int64_t>(SIZE - 1);

    std::int64_t next_seq_ = 0;  // producer only — no atomic needed

public:
    Sequencer()                            = default;
    Sequencer(const Sequencer&)            = delete;
    Sequencer& operator=(const Sequencer&) = delete;
    Sequencer(Sequencer&&)                 = delete;
    Sequencer& operator=(Sequencer&&)      = delete;

    // producer side (one thread only)
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

    // consumer side
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
