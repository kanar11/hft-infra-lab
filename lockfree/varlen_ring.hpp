/*
 * VarlenRingBuffer<SIZE> — single-producer single-consumer ring of raw bytes
 *                          for variable-length messages with a 4-byte length
 *                          prefix per record.
 *
 * Distinct from SPSCQueue<T, SIZE> (fixed-size T slots) and Sequencer<T, SIZE>
 * (fixed slots with explicit claim/publish). This is the right primitive when
 * the messages don't have a single fixed shape — log lines, serialized
 * protocol frames, audit records of varying payload size.
 *
 * Wire format inside the ring
 * ---------------------------
 *   [ uint32_t len ][ payload of `len` bytes ][ uint32_t len ][ payload ] ...
 *
 *   - len is stored in native byte order (we're not crossing a wire here).
 *   - A frame never straddles SIZE: if the remaining contiguous space at
 *     the write cursor can't hold the header + payload, the writer wraps
 *     the head to 0 and tries again (no padding sentinel — the consumer
 *     resyncs by following the same wrap rule).
 *
 * Invariants
 * ----------
 *   - SIZE is a power of two ≥ 64 (header is 4 bytes; small SIZE breaks the
 *     wrap heuristic).
 *   - Exactly ONE thread calls write(); exactly ONE (different) thread reads.
 *   - A single message payload must fit in SIZE - sizeof(uint32_t) bytes.
 *
 * Memory ordering
 * ---------------
 *   producer release-stores head_ AFTER writing the payload bytes; consumer
 *   acquire-loads head_ BEFORE reading. Same release/acquire pair as in
 *   SPSCQueue — the ring just happens to store bytes instead of typed slots.
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
        // One slot is "lost" to keep empty (h==t) and full distinguishable.
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

    // write: enqueue payload of `len` bytes. Returns false if the message
    // wouldn't fit in the current free space, or if `len` is zero / too big.
    bool write(const void* payload, LenT len) noexcept {
        if (len == 0 || len + HDR > SIZE - 1) return false;

        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        if (free_space(h, t) < len + HDR) return false;

        // Header.
        write_at(h, &len, HDR);
        // Payload immediately follows.
        write_at((h + HDR) & MASK, payload, len);
        head_.store((h + HDR + len) & MASK, std::memory_order_release);
        return true;
    }

    // read: dequeue into `out` (must be ≥ max_len bytes). Returns the
    // payload length on success, 0 if empty or if the next message is
    // larger than max_len (in which case nothing is consumed).
    LenT read(void* out, LenT max_len) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (available(h, t) < HDR) return 0;

        LenT len = 0;
        read_at(t, &len, HDR);
        if (len == 0 || available(h, t) < HDR + len) return 0;  // partial/corrupt
        if (len > max_len) return 0;                              // caller buffer too small

        read_at((t + HDR) & MASK, out, len);
        tail_.store((t + HDR + len) & MASK, std::memory_order_release);
        return len;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_relaxed)
            == head_.load(std::memory_order_acquire);
    }

    // approximate — two atomic loads, ±HDR-aligned slop under contention.
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
