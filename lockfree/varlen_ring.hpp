/*
 * VarlenRingBuffer<SIZE> — an SPSC byte ring for VARIABLE-length messages,
 *                          with a 4-byte length prefix per record.
 *
 * The difference from SPSCQueue<T, SIZE> (fixed-size T slots) and Sequencer<T, SIZE>
 * (fixed slots + explicit claim/publish): VarlenRing is the right primitive when
 * messages don't share one fixed shape — log lines, serialized protocol
 * frames, audit records with a payload of varying length.
 *
 * Wire format inside the ring:
 *   [ uint32_t len ][ payload `len` bytes ][ uint32_t len ][ payload ] ...
 *
 *   - len is kept in native byte order (we don't go over the network, host-only).
 *   - A frame does NOT straddle the SIZE boundary — on a wrap the memcpy splits into 2
 *     pieces (the first to the end of the buffer, the second from 0). Both write_at
 *     and read_at handle the wrap symmetrically.
 *
 * Assumptions:
 *   - SIZE is a power of two ≥ 64 (the header is 4 bytes; with a small SIZE the
 *     wrap heuristic breaks down).
 *   - Exactly ONE thread calls write(); exactly ONE (other) — read().
 *   - A single payload must fit in SIZE - sizeof(uint32_t) - 1 bytes
 *     (minus 1 because SPSC loses a slot to empty/full).
 *
 * Memory ordering:
 *   The producer release-stores head_ AFTER writing the payload; the consumer
 *   acquire-loads head_ BEFORE reading. The same release/acquire pair as
 *   in SPSCQueue — the ring simply holds bytes instead of typed slots.
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
        // One slot is "lost" so that empty (h==t) and full are distinguishable.
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

    // write: enqueue a payload of `len` bytes. Returns false when the message
    // does not fit in the currently free space, or when `len` is 0 / too large.
    bool write(const void* payload, LenT len) noexcept {
        if (len == 0 || len + HDR > SIZE - 1) return false;

        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (free_space(h, t) < len + HDR) return false;

        // Header first, then the payload right after it.
        write_at(h, &len, HDR);
        write_at((h + HDR) & MASK, payload, len);
        head_.store((h + HDR + len) & MASK, std::memory_order_release);
        return true;
    }

    // read: read into `out` (the buffer must have ≥ max_len bytes). Returns
    // the payload length, 0 when empty or when the next message is larger
    // than max_len (then NOTHING is consumed — tail unchanged).
    LenT read(void* out, LenT max_len) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (available(h, t) < HDR) return 0;

        LenT len = 0;
        read_at(t, &len, HDR);
        if (len == 0 || available(h, t) < HDR + len) return 0;  // partial/corrupt
        if (len > max_len) return 0;                              // the caller's buffer is too small

        read_at((t + HDR) & MASK, out, len);
        tail_.store((t + HDR + len) & MASK, std::memory_order_release);
        return len;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    // approximate — two atomic loads, ±HDR slop under contention.
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
