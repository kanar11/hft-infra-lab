/*
 * Sequencer<T, SIZE> — ring w stylu LMAX Disruptor, single-producer.
 *
 * W przeciwieństwie do kolejki, Sequencer udostępnia sloty bezpośrednio:
 * producent klaimuje monotoniczny numer sekwencyjny, zapisuje *in-place*
 * w slocie, dopiero potem publikuje przez store na cursor. Konsumenci
 * czytają sloty zero-copy w kolejności publikacji. Nic nie jest nigdy
 * kopiowane "przez kolejkę" — to jest właśnie przewaga Disruptora nad
 * tradycyjnym queue (struct po 200 bajtów copy → store w slocie).
 *
 * Jeden producent + wielu konsumentów (najczęstszy wariant LMAX).
 * Multi-producer Sequencer wymagałby osobnej "published" bitmaski, bo
 * fetch_add nie wie który zaklaimowany slot zakończył już write.
 *
 * API:
 *   producent:  seq = try_claim();   if seq < 0 → ring pełny
 *               slot(seq) = data;
 *               publish(seq);
 *
 *   konsument:  hi = available();    // ostatni opublikowany seq
 *               for (i = my_next; i <= hi; ++i) use(read(i));
 *               mark_consumed(hi);   // zwalnia sloty na kolejny wrap
 *
 * Przy wielu konsumentach zewnętrzny aggregator publikuje min(all seqs)
 * do mark_consumed — wtedy producent może bezpiecznie zawinąć.
 *
 * Memory ordering:
 *   cursor_.store(release)  — producent publikuje po zapisaniu slotu
 *   cursor_.load(acquire)   — konsument paruje z release
 *   gating_.store(release)  — konsument sygnalizuje "skończone z seq"
 *   gating_.load(acquire)   — producent czyta przed wrapem
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

    T buffer_[SIZE]{};  // value-init — satysfakcjonuje cppcheck uninitMemberVar
    alignas(64) std::atomic<std::int64_t> cursor_{-1};  // ostatni opublikowany
    alignas(64) std::atomic<std::int64_t> gating_{-1};  // najwolniejszy konsument

    static constexpr std::int64_t MASK = static_cast<std::int64_t>(SIZE - 1);

    std::int64_t next_seq_ = 0;  // tylko producent — atomic zbędny

public:
    Sequencer()                            = default;
    Sequencer(const Sequencer&)            = delete;
    Sequencer& operator=(const Sequencer&) = delete;
    Sequencer(Sequencer&&)                 = delete;
    Sequencer& operator=(Sequencer&&)      = delete;

    // strona producenta (tylko jeden wątek)
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

    // strona konsumenta
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
