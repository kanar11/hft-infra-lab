/*
 * LockfreeTradeLogger — same audit-trail semantics as TradeLogger, but the
 * internal storage is lockfree::SPSCQueue<TradeEvent, CAPACITY> from the
 * library in lockfree/ instead of a hand-rolled ring of atomics.
 *
 * Why both variants live in the repo
 * ----------------------------------
 * TradeLogger (trade_logger.hpp) is the "from scratch" implementation —
 * cache-aligned head_/tail_ pair, custom flush-thread protocol, optional
 * ring_mode_ that overwrites the oldest record when full. It's the proof
 * that we understand the SPSC release/acquire pattern at the lowest level.
 *
 * LockfreeTradeLogger (this file) is the "reusable building block" version
 * — drop in lockfree::SPSCQueue and you get the same correctness guarantees
 * for free. No ring_mode_ here: SPSCQueue is fail-on-full. Use it when
 * audit completeness matters more than absorbing transient bursts.
 *
 * The two share TradeEvent / EventType / FlushFileHeader from
 * trade_logger.hpp, so the binary file format is identical and an offline
 * parser can't tell which logger produced a given session.
 */
#pragma once

#include "trade_logger.hpp"            // shares TradeEvent / EventType / FlushFileHeader

#include "../lockfree/spsc_queue.hpp"
#include "../common/time_utils.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>


namespace lockfree_logger {

// Default capacity matches MAX_EVENTS in trade_logger.hpp (1M).
// Must be a power of two (SPSCQueue static_assert).
inline constexpr std::size_t DEFAULT_CAPACITY = 1u << 20;  // 1,048,576


template <std::size_t CAPACITY = DEFAULT_CAPACITY>
class LockfreeTradeLogger {
    using Queue = lockfree::SPSCQueue<TradeEvent, CAPACITY>;

    // Heap-allocated (CAPACITY * 128 B + alignment padding can be 128 MB).
    std::unique_ptr<Queue> queue_;

    // Trading-thread-local counters (only written here, only read from
    // the same thread or between sessions).
    std::uint64_t sequence_     = 0;
    std::uint64_t total_logged_ = 0;
    int counters_[static_cast<int>(EventType::EVENT_COUNT)]{};

    // Async flush state.
    std::thread       flush_thread_;
    std::atomic<bool> flush_running_{false};
    std::FILE*        flush_file_ = nullptr;

    void flush_loop() noexcept {
        TradeEvent ev{};
        while (true) {
            // Drain everything currently visible to the consumer side.
            while (queue_->pop(ev)) {
                std::fwrite(&ev, sizeof(TradeEvent), 1, flush_file_);
            }
            if (!flush_running_.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        // Final drain after stop signal.
        while (queue_->pop(ev)) {
            std::fwrite(&ev, sizeof(TradeEvent), 1, flush_file_);
        }
        std::fflush(flush_file_);
    }

public:
    LockfreeTradeLogger()
        : queue_(std::make_unique<Queue>()) {}

    ~LockfreeTradeLogger() { stop_async_flush(); }

    LockfreeTradeLogger(const LockfreeTradeLogger&)            = delete;
    LockfreeTradeLogger& operator=(const LockfreeTradeLogger&) = delete;
    LockfreeTradeLogger(LockfreeTradeLogger&&)                 = delete;
    LockfreeTradeLogger& operator=(LockfreeTradeLogger&&)      = delete;

    // log: build the TradeEvent on the stack, push into the queue.
    // Returns the sequence number, or 0 if the queue is full.
    std::uint64_t log(EventType type, std::uint64_t order_id = 0,
                      const char* symbol = "", const char* side = "",
                      std::int32_t quantity = 0, double price = 0.0,
                      const char* details = "") noexcept {
        TradeEvent e{};
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

        if (!queue_->push(e)) {
            // Full — roll the sequence back so the counter stays meaningful.
            --sequence_;
            return 0;
        }
        ++total_logged_;
        const int idx = static_cast<int>(type);
        if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
            counters_[idx]++;
        return e.sequence_no;
    }

    bool start_async_flush(const char* filepath) noexcept {
        flush_file_ = std::fopen(filepath, "ab");
        if (!flush_file_) return false;

        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = ::wall_ns();
        std::fwrite(&hdr, sizeof(hdr), 1, flush_file_);
        std::fflush(flush_file_);

        flush_running_.store(true, std::memory_order_relaxed);
        flush_thread_ = std::thread([this]() { flush_loop(); });
        return true;
    }

    void stop_async_flush() noexcept {
        if (flush_thread_.joinable()) {
            flush_running_.store(false, std::memory_order_relaxed);
            flush_thread_.join();
        }
        if (flush_file_) {
            std::fclose(flush_file_);
            flush_file_ = nullptr;
        }
    }

    // Read-only accessors — safe between sessions (not concurrently with log()).
    std::uint64_t sequence()     const noexcept { return sequence_; }
    std::uint64_t total_logged() const noexcept { return total_logged_; }
    bool empty()                 const noexcept { return queue_->empty(); }
    static constexpr std::size_t capacity() noexcept { return CAPACITY; }
    int get_counter(EventType type) const noexcept {
        const int idx = static_cast<int>(type);
        return (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
               ? counters_[idx] : 0;
    }
};

}  // namespace lockfree_logger
