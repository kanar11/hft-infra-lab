/*
 * MmapTradeLogger — TradeEvent stream backed by mmap()'d disk-backed file.
 *
 * Third variant in the logger family:
 *   TradeLogger          — hand-rolled SPSC ring + async fwrite() flush thread
 *   LockfreeTradeLogger  — lockfree::SPSCQueue + async fwrite() flush thread
 *   MmapTradeLogger      — file mapped into memory; log() writes the event
 *                           bytes directly into the page cache; the kernel
 *                           flushes asynchronously
 *
 * Why mmap
 * --------
 * `log()` becomes a pure memcpy into mapped memory: no syscall, no copy
 * into a userspace queue, no extra thread. The kernel page cache is the
 * audit ring. On systems with persistent memory or fast SSDs, fsync()
 * latency to make a record durable is also lower than fwrite() because
 * the data never crosses a userspace buffer.
 *
 * Layout on disk
 * --------------
 *   [ FlushFileHeader (64 B) | TradeEvent (128 B) | TradeEvent | ... ]
 *
 * Identical to TradeLogger / LockfreeTradeLogger, so the same offline
 * parser reads any of the three. File is sized at construction time
 * (fixed-cap), grows once via ftruncate(), then stays at that size.
 *
 * Caveats
 * -------
 *   - Fixed-capacity. When full, log() returns 0 (no rollover).
 *   - msync() is called from the destructor only; for hard durability
 *     guarantees a caller can invoke flush_sync() at any session
 *     boundary (page-aligned, blocks until kernel writeback).
 *   - Single-writer. The mapped region is a flat array — no synchronisation
 *     between concurrent writers. Trading-thread-pinned, like TradeLogger.
 */
#pragma once

#include "trade_logger.hpp"     // shares TradeEvent / EventType / FlushFileHeader
#include "../common/time_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


namespace mmap_logger {

inline constexpr std::size_t DEFAULT_CAPACITY = 1u << 20;  // 1,048,576 events


class MmapTradeLogger {
    std::size_t   capacity_     = 0;       // max events
    std::size_t   region_bytes_ = 0;       // header + capacity * sizeof(TradeEvent)
    void*         region_       = nullptr;
    TradeEvent*   events_       = nullptr; // points into region_ past the header
    int           fd_           = -1;

    std::uint64_t sequence_     = 0;
    std::uint64_t total_logged_ = 0;
    int           counters_[static_cast<int>(EventType::EVENT_COUNT)]{};
    bool          open_         = false;

public:
    MmapTradeLogger() = default;
    ~MmapTradeLogger() { close(); }

    MmapTradeLogger(const MmapTradeLogger&)            = delete;
    MmapTradeLogger& operator=(const MmapTradeLogger&) = delete;
    MmapTradeLogger(MmapTradeLogger&&)                 = delete;
    MmapTradeLogger& operator=(MmapTradeLogger&&)      = delete;

    // open: mmap a file at `path` sized to hold `capacity` events.
    // Returns false on any failure (open/ftruncate/mmap).
    bool open_file(const char* path, std::size_t capacity = DEFAULT_CAPACITY) noexcept {
        if (open_) return false;

        fd_ = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) return false;

        capacity_     = capacity;
        region_bytes_ = sizeof(FlushFileHeader) + capacity * sizeof(TradeEvent);

        if (::ftruncate(fd_, static_cast<off_t>(region_bytes_)) != 0) {
            ::close(fd_); fd_ = -1;
            return false;
        }

        region_ = ::mmap(nullptr, region_bytes_, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd_, 0);
        if (region_ == MAP_FAILED) {
            region_ = nullptr;
            ::close(fd_); fd_ = -1;
            return false;
        }

        // Write the session header in place.
        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = ::wall_ns();
        std::memcpy(region_, &hdr, sizeof(hdr));

        events_ = reinterpret_cast<TradeEvent*>(
                      static_cast<std::uint8_t*>(region_) + sizeof(FlushFileHeader));
        open_   = true;
        return true;
    }

    // log: write one event directly into the mapped region. Returns the
    // assigned sequence number, or 0 if the region is full / not open.
    std::uint64_t log(EventType type, std::uint64_t order_id = 0,
                      const char* symbol = "", const char* side = "",
                      std::int32_t quantity = 0, double price = 0.0,
                      const char* details = "") noexcept {
        if (!open_ || total_logged_ >= capacity_) return 0;

        TradeEvent& e = events_[total_logged_];
        e.mono_ns     = ::mono_ns();
        e.wall_ns     = ::wall_ns();
        e.sequence_no = ++sequence_;
        e.order_id    = order_id;
        e.price       = price;
        e.quantity    = quantity;
        e.event_type  = type;

        std::strncpy(e.symbol,  symbol,  8);  e.symbol[8]   = '\0';
        std::strncpy(e.side,    side,    4);  e.side[4]     = '\0';
        std::strncpy(e.details, details, 68); e.details[68] = '\0';

        const int idx = static_cast<int>(type);
        if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
            counters_[idx]++;
        ++total_logged_;
        return e.sequence_no;
    }

    // flush_sync: synchronously page out everything written so far.
    // Blocks until the kernel has issued the writeback.
    bool flush_sync() noexcept {
        if (!open_) return false;
        // Only flush the prefix we've actually written — the trailing
        // unused capacity is still mapped but contains zeros.
        const std::size_t used = sizeof(FlushFileHeader)
                               + total_logged_ * sizeof(TradeEvent);
        return ::msync(region_, used, MS_SYNC) == 0;
    }

    // close: msync + munmap + close fd. Idempotent.
    void close() noexcept {
        if (!open_) return;
        flush_sync();
        ::munmap(region_, region_bytes_);
        ::close(fd_);
        region_ = nullptr;
        events_ = nullptr;
        fd_     = -1;
        open_   = false;
    }

    std::uint64_t sequence()      const noexcept { return sequence_; }
    std::uint64_t total_logged()  const noexcept { return total_logged_; }
    std::size_t   capacity()      const noexcept { return capacity_; }
    bool          is_open()       const noexcept { return open_; }
    int           get_counter(EventType type) const noexcept {
        const int idx = static_cast<int>(type);
        return (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
               ? counters_[idx] : 0;
    }
};

}  // namespace mmap_logger
