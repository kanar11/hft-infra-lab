/*
 * VarlenRingBuffer<SIZE> — ring SPSC bajtów dla wiadomości o ZMIENNEJ długości,
 *                          z 4-bajtowym prefiksem długości na rekord.
 *
 * Różnica względem SPSCQueue<T, SIZE> (sloty fixed-size T) i Sequencer<T, SIZE>
 * (fixed slots + explicit claim/publish): VarlenRing to właściwy prymityw gdy
 * wiadomości nie mają jednego stałego kształtu — linie loga, serializowane
 * ramki protokołu, rekordy audit o różnej długości payloadu.
 *
 * Format wire wewnątrz ringu:
 *   [ uint32_t len ][ payload `len` bajtów ][ uint32_t len ][ payload ] ...
 *
 *   - len trzymane w native byte order (nie idziemy przez sieć, host-only).
 *   - Ramka NIE straddle'uje granicy SIZE — przy wrapie memcpy dzieli na 2
 *     kawałki (pierwszy do końca buforu, drugi od 0). Zarówno write_at jak
 *     i read_at obsługują wrap symetrycznie.
 *
 * Założenia:
 *   - SIZE to potęga dwójki ≥ 64 (header ma 4 bajty; przy małym SIZE heurystyka
 *     wrapu się rozjeżdża).
 *   - Dokładnie JEDEN wątek wywołuje write(); dokładnie JEDEN (inny) — read().
 *   - Pojedynczy payload musi mieścić się w SIZE - sizeof(uint32_t) - 1 bajtach
 *     (minus 1 bo SPSC traci slot na empty/full).
 *
 * Memory ordering:
 *   Producent release-store'uje head_ PO zapisaniu payloadu; konsument
 *   acquire-load'uje head_ PRZED czytaniem. Ta sama para release/acquire co
 *   w SPSCQueue — ring trzyma po prostu bajty zamiast typowanych slotów.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>


namespace lockfree {

inline constexpr std::size_t kCacheLineVarlen = 64;


template <std::size_t SIZE>
class VarlenRingBuffer {
    static_assert(SIZE >= 64,                          "SIZE must be ≥ 64");
    static_assert((SIZE & (SIZE - 1)) == 0,            "SIZE must be a power of two");

    using LenT = std::uint32_t;
    static constexpr std::size_t HDR = sizeof(LenT);
    static constexpr std::size_t MASK = SIZE - 1;

    std::uint8_t buffer_[SIZE]{};
    alignas(kCacheLineVarlen) std::atomic<std::size_t> head_{0};   // producer
    alignas(kCacheLineVarlen) std::atomic<std::size_t> tail_{0};   // consumer

    static std::size_t free_space(std::size_t h, std::size_t t) noexcept {
        // Jeden slot "tracony" żeby empty (h==t) i full były rozróżnialne.
        return (t - h - 1) & MASK;
    }

    static std::size_t available(std::size_t h, std::size_t t) noexcept {
        return (h - t) & MASK;
    }

public:
    VarlenRingBuffer() = default;

    VarlenRingBuffer(const VarlenRingBuffer&)            = delete;
    VarlenRingBuffer& operator=(const VarlenRingBuffer&) = delete;
    VarlenRingBuffer(VarlenRingBuffer&&)                 = delete;
    VarlenRingBuffer& operator=(VarlenRingBuffer&&)      = delete;

    // write: zakolejkuj payload `len` bajtów. Zwraca false gdy wiadomość
    // nie zmieści się w aktualnie wolnej przestrzeni, lub gdy `len` to 0 / za duże.
    bool write(const void* payload, LenT len) noexcept {
        if (len == 0 || len + HDR > SIZE - 1) return false;

        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (free_space(h, t) < len + HDR) return false;

        // Najpierw header, potem payload tuż za nim.
        write_at(h, &len, HDR);
        write_at((h + HDR) & MASK, payload, len);
        head_.store((h + HDR + len) & MASK, std::memory_order_release);
        return true;
    }

    // read: odczytaj do `out` (bufor musi mieć ≥ max_len bajtów). Zwraca
    // długość payloadu, 0 gdy pusty albo gdy następna wiadomość jest większa
    // niż max_len (wtedy NIC nie jest konsumowane — tail bez zmiany).
    LenT read(void* out, LenT max_len) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (available(h, t) < HDR) return 0;

        LenT len = 0;
        read_at(t, &len, HDR);
        if (len == 0 || available(h, t) < HDR + len) return 0;  // partial/corrupt
        if (len > max_len) return 0;                              // bufor wywołującego za mały

        read_at((t + HDR) & MASK, out, len);
        tail_.store((t + HDR + len) & MASK, std::memory_order_release);
        return len;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    // przybliżone — dwa load'y atomic, ±HDR slop przy kontencji.
    std::size_t bytes_used() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return available(h, t);
    }

    static constexpr std::size_t capacity() noexcept { return SIZE - 1; }

private:
    void write_at(std::size_t pos, const void* src, std::size_t n) noexcept {
        const std::uint8_t* s = static_cast<const std::uint8_t*>(src);
        if (pos + n <= SIZE) {
            std::memcpy(buffer_ + pos, s, n);
        } else {
            const std::size_t first = SIZE - pos;
            std::memcpy(buffer_ + pos, s, first);
            std::memcpy(buffer_,       s + first, n - first);
        }
    }

    void read_at(std::size_t pos, void* dst, std::size_t n) const noexcept {
        std::uint8_t* d = static_cast<std::uint8_t*>(dst);
        if (pos + n <= SIZE) {
            std::memcpy(d, buffer_ + pos, n);
        } else {
            const std::size_t first = SIZE - pos;
            std::memcpy(d,         buffer_ + pos, first);
            std::memcpy(d + first, buffer_,       n - first);
        }
    }
};

}  // namespace lockfree
