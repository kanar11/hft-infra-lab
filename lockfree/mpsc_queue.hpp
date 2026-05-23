/*
 * MPSCQueue<T, SIZE> — kolejka ograniczona N producentów / 1 konsument.
 *
 * Protokół slotu w stylu Vyukova. Każda komórka (Cell) ma własny numer
 * sekwencyjny; producenci ścigają się o sloty przez CAS na head_, jedyny
 * konsument przesuwa tail_ zwykłym relaxed-store.
 *
 * Use case: wiele wątków tradingowych → jeden logger / journal / audit.
 *
 * Protokół seq:
 *   Każda Cell startuje z seq == swój indeks. seq koduje jednocześnie
 *   właściciela i numer "okrążenia" ring buffera:
 *
 *   producent (claim pos):
 *     cell.seq == pos       → wolny w tym okrążeniu → CAS head_
 *     cell.seq <  pos       → pełna (konsument nie wybrał poprzedniego wrapu)
 *     cell.seq >  pos       → wyścig z innym producentem; reload head, retry
 *     po zapisie: cell.seq.store(pos+1, release)   ← publikuje dane
 *
 *   konsument (drain at tail):
 *     cell.seq == pos+1     → gotowe → kopiuj dane → advance tail
 *     cell.seq <  pos+1     → pusty
 *     po odczycie: cell.seq.store(pos+SIZE, release)  ← zwalnia slot na kolejny wrap
 *
 * Capacity == SIZE — numery sekwencyjne eliminują niejednoznaczność
 * empty/full która w SPSCQueue kosztowała 1 slot.
 *
 * Dlaczego CAS_weak? Na ARM/POWER CAS_strong kosztuje dodatkowy retry-loop
 * w mikrokodzie; CAS_weak może spurious-fail ale i tak retrujemy.
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

    Cell buffer_[SIZE]{};  // value-init; konstruktor zaraz nadpisuje seq na poprawne
    alignas(64) std::atomic<std::size_t> head_{0};  // producenci CAS-ują tu
    alignas(64) std::atomic<std::size_t> tail_{0};  // jeden konsument

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
