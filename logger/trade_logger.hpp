/*
 * TradeLogger — an order audit trail (SPSC ring + asynchronous flush).
 *
 * Every event (submit, fill, cancel, risk decision, kill switch,
 * system event) is recorded as a fixed TradeEvent record (128 bytes,
 * 2 cache lines) with two timestamps — CLOCK_MONOTONIC for
 * latency measurement and CLOCK_REALTIME for regulators (MiFID II RTS 25
 * Article 2 requires a UTC epoch).
 *
 * Architecture:
 *   - The trading thread writes via log()       — hot path, O(1), no allocation
 *   - The flush_thread_ thread drains the ring to a file — off-CPU, batched fwrite
 *   - A third thread may read a snapshot          — only between sessions,
 *                                                  not concurrently with log()
 *
 * Performance: ~15-25 M events/sec for log(), ~5-8 M writes/sec to the file.
 *
 * File format: FlushFileHeader (64 B) + N × TradeEvent (128 B). The file
 * is appendable (open "ab") — each session adds its own header
 * + its records, and the offline parser recognizes them by the magic "HFTLOG\0\0".
 *
 * Three logger variants in the repo (different trade-offs):
 *   - TradeLogger (this file)    — a hand-rolled SPSC ring + flush thread
 *                                  + a ring_mode mode (overwrites the oldest)
 *   - LockfreeTradeLogger        — uses lockfree::SPSCQueue as the backing
 *                                  store; no ring_mode (fail-on-full)
 *   - MmapTradeLogger            — the file is mmap'd into RAM, log() is a memcpy,
 *                                  the kernel flushes asynchronously
 *
 * Pipeline: Strategy → Router → Risk → OMS → Logger → audit file.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unistd.h>     // ::fsync (durability after power loss)
#include <string>

#include "../common/time_utils.hpp"


// Max events in the ring buffer. 1M × 128 B = 128 MB —
// allocated once in the constructor.
static constexpr int MAX_EVENTS = 1'000'000;

// Limit for reserve() in unique_orders/symbols — to avoid allocating
// a large hash map when the buffer is only partially filled.
static constexpr int MAX_TRACKED_IDS = 4096;


// What happened to the order. uint8_t — 1 byte vs 4, in TradeEvent
// (128 B exactly) every byte counts.
enum class EventType : uint8_t {
    ORDER_SUBMIT  = 0,   // the strategy sent an order to the OMS
    RISK_ACCEPT   = 1,   // the RiskManager let it through
    RISK_REJECT   = 2,   // the RiskManager rejected it (reason code in `details`)
    ORDER_FILL    = 3,   // the exchange fully executed it
    ORDER_PARTIAL = 4,   // a partial execution
    ORDER_CANCEL  = 5,   // cancelled
    KILL_SWITCH   = 6,   // emergency mode — trading halted
    SYSTEM_START  = 7,   // session start
    SYSTEM_STOP   = 8,   // session end
    EVENT_COUNT   = 9    // sentinel for iterating over the counter
};

inline const char* event_type_str(EventType t) noexcept {
    static const char* names[] = {
        "ORDER_SUBMIT", "RISK_ACCEPT", "RISK_REJECT",
        "ORDER_FILL", "ORDER_PARTIAL", "ORDER_CANCEL",
        "KILL_SWITCH", "SYSTEM_START", "SYSTEM_STOP"
    };
    int idx = static_cast<int>(t);
    if (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
        return names[idx];
    return "UNKNOWN";
}


// A single immutable audit-trail record. Exactly 128 bytes
// (2 cache lines) — fields laid out to avoid padding.
// alignas(64): each record starts on a cache-line boundary, so the
// flush thread reading slot K does not invalidate the line in which the
// trading thread writes slot K+1.
struct alignas(64) TradeEvent {
    // 8-byte fields first — the compiler inserts no padding
    int64_t   mono_ns;       // CLOCK_MONOTONIC, ns — for latency
    int64_t   wall_ns;       // CLOCK_REALTIME, ns  — UTC epoch for the regulator
    uint64_t  sequence_no;   // monotonic counter — the regulator detects gaps
    uint64_t  order_id;      // matches OMS::Order::order_id
    double    price;         // float for display (OMS keeps fixed-point)
    // 4-byte
    int32_t   quantity;
    // 1-byte + char[] — no padding between them
    EventType event_type;
    char      symbol[9];     // "AAPL" + null
    char      side[5];       // "BUY"/"SELL" + null
    char      details[69];   // free field for context (venue, reject reason, ...)
    // Layout: 8+8+8+8+8 + 4 + 1+9+5+69 = 128 bytes
};
static_assert(sizeof(TradeEvent) == 128, "TradeEvent must be exactly 128 bytes");


// Audit file header. 64 bytes, once per session, before the records.
struct FlushFileHeader {
    char     magic[8];          // "HFTLOG\0\0" — format identification
    uint32_t version;           // = 1
    uint32_t record_size;       // sizeof(TradeEvent) = 128
    int64_t  created_wall_ns;   // UTC wall clock at session start
    char     padding[40];       // reserved for the future (8+4+4+8+40 = 64)
};
static_assert(sizeof(FlushFileHeader) == 64, "FlushFileHeader must be 64 bytes");


class TradeLogger {
    // Ring buffer — allocated once, never reallocated
    std::unique_ptr<TradeEvent[]> events_;
    int max_events_;

    // SPSC atomics — the trading thread writes head_, the flush thread reads head_
    // (acquire) and writes flush_tail_ (relaxed — it is the only writer).
    // The release/acquire pair guarantees the flush_thread sees the complete
    // record data only once it sees head_+1.
    alignas(64) std::atomic<int> head_{0};
    alignas(64) std::atomic<int> flush_tail_{0};

    // Counters — trading thread only (or offline query). 64-bit, because
    // at 25 M evt/s an int32 wraps in ~85 s — a long session would destroy
    // the sequence numbers the regulator uses to detect gaps.
    uint64_t sequence_{0};
    uint64_t total_logged_{0};
    bool     ring_mode_;        // true = overwrite the oldest, false = stop when full

    // Per-type counters — a quick "how many submits / fills / rejects"
    int counters_[static_cast<int>(EventType::EVENT_COUNT)]{};

    // Flush thread state
    std::thread       flush_thread_;
    std::atomic<bool> flush_running_{false};
    FILE*             flush_file_{nullptr};

    // drain_to_file: writes contiguous spans [t..h) to flush_file_.
    // Batched fwrite — one syscall per contiguous range instead of N. For
    // ring_mode_ it splits into at most 2 spans (before wrap, after wrap).
    int drain_to_file(int t, int h) noexcept {
        while (t != h) {
            int slot   = ring_mode_ ? (t % max_events_) : t;
            int contig = ring_mode_ ? std::min(max_events_ - slot, h - t) : (h - t);
            std::fwrite(&events_[slot], sizeof(TradeEvent), contig, flush_file_);
            t += contig;
        }
        return t;
    }

    // flush_loop: the flush thread's loop. Each iteration: reads head_ (acquire),
    // drains [flush_tail_..head_), then sleeps 50 µs so as not to spin the
    // CPU. After flush_running_ = false, one more drain (the producer
    // may have managed to append) and fflush.
    //
    // In production the 50 µs sleep is replaced with a futex / condvar to
    // shorten the wakeup latency. Here a simpler version for readability.
    void flush_loop() noexcept {
        while (true) {
            int h = head_.load(std::memory_order_acquire);
            int t = flush_tail_.load(std::memory_order_relaxed);
            if (t != h) {
                t = drain_to_file(t, h);
                flush_tail_.store(t, std::memory_order_relaxed);
            }
            if (!flush_running_.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        // Final drain after the stop signal — the producer may have pushed
        // something between testing flush_running_ and break.
        int h = head_.load(std::memory_order_acquire);
        int t = drain_to_file(flush_tail_.load(std::memory_order_relaxed), h);
        flush_tail_.store(t, std::memory_order_relaxed);
        std::fflush(flush_file_);
        // fsync — durability guarantee after power loss. Compliance (SEC/MiFID II
        // audit trail) requires the last minute of orders to survive a machine crash.
        // Without it the data sits in the kernel page cache and is lost on power cut.
        if (flush_file_) ::fsync(::fileno(flush_file_));
    }

public:
    // ring_mode=false (default): a linear buffer, log() returns 0 when full.
    // ring_mode=true:             a ring buffer, log() overwrites
    //                              the oldest events on overflow.
    explicit TradeLogger(bool ring_mode = false, int capacity = MAX_EVENTS)
        : events_(std::make_unique<TradeEvent[]>(capacity)),
          max_events_(capacity), ring_mode_(ring_mode) {
        std::memset(counters_, 0, sizeof(counters_));
    }

    ~TradeLogger() { stop_async_flush(); }

    TradeLogger(const TradeLogger&)            = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;
    // A move would be complicated — flush_thread_/flush_file_/flush_running_
    // would need a transfer + an atomic resync of head_/flush_tail_. The logger is
    // pinned to the trading thread, not worth it.
    TradeLogger(TradeLogger&&)                 = delete;
    TradeLogger& operator=(TradeLogger&&)      = delete;

    // start_async_flush: open the file (append) and start the flush thread.
    // Returns false if fopen failed. The file appends a header — you can open
    // the same file repeatedly, each run starts with its own HFTLOG header.
    bool start_async_flush(const char* filepath) noexcept {
        flush_file_ = std::fopen(filepath, "ab");
        if (!flush_file_) return false;

        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = wall_ns();
        std::fwrite(&hdr, sizeof(hdr), 1, flush_file_);
        std::fflush(flush_file_);

        flush_running_.store(true, std::memory_order_relaxed);
        flush_thread_ = std::thread([this]() { flush_loop(); });
        return true;
    }

    // stop_async_flush: signal the flush thread to finish, wait for the
    // join, close the file. Blocks until all pending events
    // are written (and fsynced — see flush_loop).
    void stop_async_flush() noexcept {
        if (flush_thread_.joinable()) {
            flush_running_.store(false, std::memory_order_relaxed);
            flush_thread_.join();
        }
        if (flush_file_) {
            // An extra fsync when someone close()s without the async flush (legacy/sync mode).
            std::fflush(flush_file_);
            ::fsync(::fileno(flush_file_));
            std::fclose(flush_file_);
            flush_file_ = nullptr;
        }
    }

    // log: hot path. Builds a TradeEvent in the chosen slot, increments
    // sequence_/total_logged_/counters_, release-stores head_ (read
    // by the flush thread via acquire).
    //
    // Returns sequence_no (>0) on success or 0 if the linear buffer
    // is full. We don't return a pointer to the slot — in ring_mode the slot
    // would be overwritten by the next log()s and the caller could read
    // stale data.
    uint64_t log(EventType type, uint64_t order_id = 0,
                 const char* symbol = "", const char* side = "",
                 int32_t quantity = 0, double price = 0.0,
                 const char* details = "") noexcept {
        int head = head_.load(std::memory_order_relaxed);
        if (!ring_mode_ && head >= max_events_) return 0;

        int slot = ring_mode_ ? (head % max_events_) : head;
        TradeEvent& e = events_[slot];
        e.mono_ns     = mono_ns();
        e.wall_ns     = wall_ns();
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
        total_logged_++;

        // Release store: the flush thread will read this slot only once it sees
        // head+1. All writes to TradeEvent are before it in program
        // order, so after the release they are visible to the acquire-reader.
        head_.store(head + 1, std::memory_order_release);
        return e.sequence_no;
    }

    // Accessors (simple read-only).
    int      total_events()  const noexcept {
        int h = head_.load(std::memory_order_relaxed);
        return ring_mode_ ? std::min(h, max_events_) : h;
    }
    uint64_t total_logged()  const noexcept { return total_logged_; }
    uint64_t sequence()      const noexcept { return sequence_; }
    int      capacity()      const noexcept { return max_events_; }
    bool     is_ring_mode()  const noexcept { return ring_mode_; }
    bool     buffer_full()   const noexcept {
        return head_.load(std::memory_order_relaxed) >= max_events_;
    }
    int get_counter(EventType type) const noexcept {
        const int idx = static_cast<int>(type);
        return (idx >= 0 && idx < static_cast<int>(EventType::EVENT_COUNT))
               ? counters_[idx] : 0;
    }

    // Query — all safe only between sessions (not concurrently
    // with log()). They iterate over the full buffer, O(N).
    int get_events(const TradeEvent** out) const noexcept {
        *out = events_.get();
        return total_events();
    }

    // get_events_filtered: copies into `out` the records that satisfy the filters.
    // The filters are optional (pass 0 / -1 / nullptr to skip).
    int get_events_filtered(TradeEvent* out, int max_out,
                            uint64_t filter_order_id = 0,
                            int filter_type = -1,
                            const char* filter_symbol = nullptr) const noexcept {
        int count = 0;
        const int n = total_events();
        for (int i = 0; i < n && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (filter_order_id > 0 && e.order_id != filter_order_id) continue;
            if (filter_type    >= 0 && static_cast<int>(e.event_type) != filter_type) continue;
            if (filter_symbol  && std::strcmp(e.symbol, filter_symbol) != 0) continue;
            out[count++] = e;
        }
        return count;
    }

    int get_order_trail(uint64_t order_id, TradeEvent* out, int max_out) const noexcept {
        return get_events_filtered(out, max_out, order_id);
    }

    // unique_orders/symbols: counts how many *distinct* order_id / symbols appeared
    // in the buffer. Allocates a hash set — call offline.
    int unique_orders() const {
        std::unordered_set<uint64_t> ids;
        ids.reserve(std::min(total_events(), MAX_TRACKED_IDS));
        const int n = total_events();
        for (int i = 0; i < n; ++i)
            if (events_[i].order_id > 0) ids.insert(events_[i].order_id);
        return static_cast<int>(ids.size());
    }

    int unique_symbols() const {
        std::unordered_set<std::string> syms;
        syms.reserve(std::min(total_events(), MAX_TRACKED_IDS));
        const int n = total_events();
        for (int i = 0; i < n; ++i)
            if (events_[i].symbol[0] != '\0') syms.emplace(events_[i].symbol);
        return static_cast<int>(syms.size());
    }

    // time_span_ms: the time span between the first and last event
    // (from mono_ns — resistant to NTP corrections).
    double time_span_ms() const noexcept {
        const int n = total_events();
        if (n < 2) return 0.0;
        return static_cast<double>(events_[n - 1].mono_ns - events_[0].mono_ns) / 1'000'000.0;
    }

    // get_events_in_range: a query by wall_ns (UTC), e.g. "all events
    // between 09:30:00.000 and 09:30:00.100" — useful for regulators
    // that demand a replay of a specific time window.
    int get_events_in_range(TradeEvent* out, int max_out,
                            int64_t start_wall_ns, int64_t end_wall_ns) const noexcept {
        int count = 0;
        const int n = total_events();
        for (int i = 0; i < n && count < max_out; ++i) {
            const TradeEvent& e = events_[i];
            if (e.wall_ns >= start_wall_ns && e.wall_ns <= end_wall_ns)
                out[count++] = e;
        }
        return count;
    }

    // Inter-event latency statistics (the mono_ns gap between consecutive ones).
    struct LatencyStats {
        int     count;
        int64_t min_ns, max_ns, mean_ns, p50_ns, p90_ns, p99_ns;
    };

    LatencyStats get_latency_stats() const noexcept {
        LatencyStats stats{};
        const int n = total_events();
        if (n < 2) return stats;

        const int cnt = n - 1;
        std::vector<int64_t> latencies(cnt);
        int64_t sum = 0;
        for (int i = 0; i < cnt; ++i) {
            latencies[i] = events_[i + 1].mono_ns - events_[i].mono_ns;
            sum += latencies[i];
        }
        std::sort(latencies.begin(), latencies.end());

        stats.count   = cnt;
        stats.min_ns  = latencies[0];
        stats.max_ns  = latencies[cnt - 1];
        stats.mean_ns = sum / cnt;
        stats.p50_ns  = latencies[cnt / 2];
        stats.p90_ns  = latencies[static_cast<int>(cnt * 0.90)];
        stats.p99_ns  = latencies[std::min(static_cast<int>(cnt * 0.99), cnt - 1)];
        return stats;
    }

    // Reports — offline only (they allocate, do printfs).
    void print_summary() const {
        printf("\n=== Logger Summary ===\n");
        printf("  Total events:   %d\n",          total_events());
        printf("  Total logged:   %lu\n",         (unsigned long)total_logged_);
        printf("  Unique orders:  %d\n",          unique_orders());
        printf("  Unique symbols: %d\n",          unique_symbols());
        printf("  Time span:      %.3f ms\n",     time_span_ms());
        printf("  Event counts:\n");
        for (int i = 0; i < static_cast<int>(EventType::EVENT_COUNT); ++i)
            if (counters_[i] > 0)
                printf("    %-20s %d\n",
                       event_type_str(static_cast<EventType>(i)), counters_[i]);
    }

    void print_order_trail(uint64_t order_id) const {
        printf("--- Order #%lu Audit Trail ---\n", (unsigned long)order_id);
        const int n = total_events();
        for (int i = 0; i < n; ++i) {
            if (events_[i].order_id != order_id) continue;
            const TradeEvent& e = events_[i];
            printf("  seq=%-8lu  %-15s  %s  %s  qty=%d  px=%.4f  %s\n",
                   (unsigned long)e.sequence_no,
                   event_type_str(e.event_type),
                   e.symbol, e.side, e.quantity, e.price, e.details);
        }
    }

    // export_binary: a synchronous dump of the whole buffer to a file.
    // Use when you want to archive without a prior start_async_flush.
    bool export_binary(const char* filepath) const {
        FILE* f = std::fopen(filepath, "ab");
        if (!f) return false;
        FlushFileHeader hdr{};
        std::memcpy(hdr.magic, "HFTLOG\0\0", 8);
        hdr.version         = 1;
        hdr.record_size     = sizeof(TradeEvent);
        hdr.created_wall_ns = wall_ns();
        std::fwrite(&hdr, sizeof(hdr), 1, f);
        const int n = total_events();
        std::fwrite(events_.get(), sizeof(TradeEvent), n, f);
        std::fclose(f);
        return true;
    }

    // CSV / JSON to stdout or a file. All 4 paths use
    // write_csv_row / write_json_obj as shared formatters.
    void export_csv()                              const { export_csv_to(stdout); }
    void export_json()                             const { export_json_to(stdout); }
    bool export_csv_to_file(const char* filepath)  const { return write_to_file(filepath, &TradeLogger::export_csv_to); }
    bool export_json_to_file(const char* filepath) const { return write_to_file(filepath, &TradeLogger::export_json_to); }

private:
    static void write_csv_row(FILE* f, const TradeEvent& e) noexcept {
        fprintf(f, "%lu,%ld,%ld,%s,%lu,%s,%s,%d,%.4f,%s\n",
                (unsigned long)e.sequence_no, e.mono_ns, e.wall_ns,
                event_type_str(e.event_type),
                (unsigned long)e.order_id, e.symbol, e.side,
                e.quantity, e.price, e.details);
    }
    static void write_json_obj(FILE* f, const TradeEvent& e, bool last) noexcept {
        fprintf(f, "    {\"seq\": %lu, \"mono_ns\": %ld, \"wall_ns\": %ld, "
                "\"event_type\": \"%s\", \"order_id\": %lu, "
                "\"symbol\": \"%s\", \"side\": \"%s\", "
                "\"quantity\": %d, \"price\": %.4f, \"details\": \"%s\"}%s\n",
                (unsigned long)e.sequence_no, e.mono_ns, e.wall_ns,
                event_type_str(e.event_type), (unsigned long)e.order_id,
                e.symbol, e.side, e.quantity, e.price, e.details,
                last ? "" : ",");
    }

    void export_csv_to(FILE* f) const {
        fprintf(f, "seq,mono_ns,wall_ns,event_type,order_id,symbol,side,quantity,price,details\n");
        const int n = total_events();
        for (int i = 0; i < n; ++i) write_csv_row(f, events_[i]);
    }
    void export_json_to(FILE* f) const {
        const int n = total_events();
        fprintf(f, "{\n  \"total_logged\": %lu,\n  \"events_in_buffer\": %d,\n  \"events\": [\n",
                (unsigned long)total_logged_, n);
        for (int i = 0; i < n; ++i) write_json_obj(f, events_[i], i == n - 1);
        fprintf(f, "  ]\n}\n");
    }

    // write_to_file: open in "w", call the member writer, close.
    // A shared skeleton for export_csv_to_file / export_json_to_file.
    bool write_to_file(const char* filepath,
                       void (TradeLogger::*writer)(FILE*) const) const {
        FILE* f = std::fopen(filepath, "w");
        if (!f) return false;
        (this->*writer)(f);
        std::fclose(f);
        return true;
    }
};
